#include <gtest/gtest.h>

#include "MockConsole.hpp"
#include "protocol/ConnectionProbe.hpp"
#include "protocol/LatencyStats.hpp"

using namespace veloplay;
using namespace veloplay::protocol;
using namespace std::chrono_literals;

TEST(LatencyStats, EmptyWindow) {
    const LatencySnapshot s = LatencyStats().snapshot();
    EXPECT_EQ(s.sent, 0);
    EXPECT_EQ(s.lossPercent(), 0.0f);
    EXPECT_EQ(rateLink(s), LinkQuality::Unknown);
}

TEST(LatencyStats, ComputesAverageRangeAndJitter) {
    LatencyStats stats;
    for (float ms : {4.0f, 6.0f, 4.0f, 6.0f, 10.0f}) stats.add(ms);
    const LatencySnapshot s = stats.snapshot();
    EXPECT_EQ(s.sent, 5);
    EXPECT_EQ(s.lost, 0);
    EXPECT_FLOAT_EQ(s.average, 6.0f);
    EXPECT_FLOAT_EQ(s.minimum, 4.0f);
    EXPECT_FLOAT_EQ(s.maximum, 10.0f);
    EXPECT_FLOAT_EQ(s.p95, 10.0f);
    EXPECT_FLOAT_EQ(s.jitter, (2.0f + 2.0f + 2.0f + 4.0f) / 4.0f);
    EXPECT_EQ(s.history.size(), 5u);
}

TEST(LatencyStats, CountsLossAndDoesNotBridgeGapsForJitter) {
    LatencyStats stats;
    stats.add(5.0f);
    stats.add(std::nullopt);
    stats.add(50.0f); // no jitter against the 5 ms before the gap
    stats.add(52.0f);
    const LatencySnapshot s = stats.snapshot();
    EXPECT_EQ(s.lost, 1);
    EXPECT_FLOAT_EQ(s.lossPercent(), 25.0f);
    EXPECT_FLOAT_EQ(s.jitter, 2.0f);
    EXPECT_LT(s.history[1], 0.0f);
}

TEST(LatencyStats, KeepsOnlyTheNewestProbes) {
    LatencyStats stats(3);
    for (float ms : {100.0f, 1.0f, 2.0f, 3.0f}) stats.add(ms);
    const LatencySnapshot s = stats.snapshot();
    EXPECT_EQ(s.sent, 3);
    EXPECT_FLOAT_EQ(s.maximum, 3.0f);
}

TEST(LatencyStats, P95IgnoresTheRareSpike) {
    LatencyStats stats;
    for (int i = 0; i < 99; ++i) stats.add(5.0f);
    stats.add(400.0f);
    EXPECT_FLOAT_EQ(stats.snapshot().p95, 5.0f);
    EXPECT_FLOAT_EQ(stats.snapshot().maximum, 400.0f);
}

namespace {

LatencySnapshot linkOf(std::initializer_list<float> pattern, int repeats) {
    LatencyStats stats(1000);
    for (int i = 0; i < repeats; ++i) {
        for (float ms : pattern) stats.add(ms < 0.0f ? std::nullopt : std::optional<float>(ms));
    }
    return stats.snapshot();
}

} // namespace

TEST(LinkRating, NeedsEnoughProbes) {
    EXPECT_EQ(rateLink(linkOf({3.0f}, kMinProbesForRating - 1)), LinkQuality::Unknown);
    EXPECT_EQ(rateLink(linkOf({3.0f}, kMinProbesForRating)), LinkQuality::Excellent);
}

TEST(LinkRating, GradesTypicalLinks) {
    EXPECT_EQ(rateLink(linkOf({3.0f, 3.5f, 3.2f}, 20)), LinkQuality::Excellent);   // cable
    EXPECT_EQ(rateLink(linkOf({6.0f, 10.0f, 7.0f, 12.0f}, 20)), LinkQuality::Good); // solid Wi-Fi
    EXPECT_EQ(rateLink(linkOf({10.0f, 18.0f, 12.0f, 22.0f}, 20)), LinkQuality::Fair); // weak Wi-Fi
    EXPECT_EQ(rateLink(linkOf({10.0f, 90.0f, 20.0f, 140.0f}, 20)), LinkQuality::Poor);
}

TEST(LinkRating, LossAlonePullsTheRatingDown) {
    // Perfect timing, but every 50th packet is lost: 2 % loss is "fair" at best.
    LatencyStats stats(1000);
    for (int i = 0; i < 100; ++i) stats.add(i % 50 == 0 ? std::nullopt : std::optional<float>(3.0f));
    EXPECT_EQ(rateLink(stats.snapshot()), LinkQuality::Fair);
    EXPECT_EQ(rateLink(linkOf({-1.0f}, 20)), LinkQuality::Poor); // nothing answers
}

TEST(LinkRating, AdviceGetsMoreCarefulAsTheLinkGetsWorse) {
    const StreamAdvice excellent = adviceFor(LinkQuality::Excellent);
    const StreamAdvice good = adviceFor(LinkQuality::Good);
    const StreamAdvice fair = adviceFor(LinkQuality::Fair);
    const StreamAdvice poor = adviceFor(LinkQuality::Poor);
    EXPECT_GT(excellent.bitrateKbps, good.bitrateKbps);
    EXPECT_GT(good.bitrateKbps, fair.bitrateKbps);
    EXPECT_GT(fair.bitrateKbps, poor.bitrateKbps);
    EXPECT_FALSE(good.stableBuffer);
    EXPECT_TRUE(fair.stableBuffer);
    EXPECT_GE(poor.bitrateKbps, 5'000); // stays inside the range the settings accept
}

namespace {

ConnectionProbe::Config fastProbe(std::uint16_t port) {
    ConnectionProbe::Config config;
    config.consolePort = port;
    config.interval = 15ms;
    config.timeout = 60ms;
    return config;
}

} // namespace

TEST(ConnectionProbe, MeasuresASteadyLink) {
    test::MockConsole console({});
    ConnectionProbe probe(fastProbe(console.discoveryPort()));
    ASSERT_TRUE(probe.start(net::kLoopbackAddress));
    ASSERT_TRUE(test::waitFor([&] { return probe.snapshot().sent >= 20; }));
    probe.stop();

    const LatencySnapshot s = probe.snapshot();
    EXPECT_EQ(s.lost, 0);
    EXPECT_GT(s.average, 0.0f);
    EXPECT_LT(s.average, 50.0f);
    EXPECT_FALSE(probe.running());
}

TEST(ConnectionProbe, SeesDelay) {
    test::MockConsole::Config config;
    config.answerDelay = 40ms;
    test::MockConsole console(config);
    auto probeConfig = fastProbe(console.discoveryPort());
    probeConfig.timeout = 200ms;
    ConnectionProbe probe(probeConfig);
    ASSERT_TRUE(probe.start(net::kLoopbackAddress));
    ASSERT_TRUE(test::waitFor([&] { return probe.snapshot().answered() >= 10; }, 6000ms));
    // Operating systems round sleeps to their timer tick (about 16 ms on
    // Windows), so the bound is well below the configured delay. Without the
    // delay an answer on loopback takes a fraction of a millisecond.
    EXPECT_GE(probe.snapshot().minimum, 15.0f);
}

TEST(ConnectionProbe, CountsLostPackets) {
    test::MockConsole::Config config;
    config.dropEveryNthSearch = 3;
    test::MockConsole console(config);
    ConnectionProbe probe(fastProbe(console.discoveryPort()));
    ASSERT_TRUE(probe.start(net::kLoopbackAddress));
    ASSERT_TRUE(test::waitFor([&] { return probe.snapshot().sent >= 15; }, 5000ms));
    probe.stop();

    const LatencySnapshot s = probe.snapshot();
    EXPECT_GE(s.lost, 3);
    EXPECT_GT(s.answered(), s.lost);
}

TEST(ConnectionProbe, SilentHostIsAllLoss) {
    auto unused = net::UdpSocket::open({{net::kLoopbackAddress, 0}, false});
    ASSERT_TRUE(unused);
    ConnectionProbe probe(fastProbe(unused->localEndpoint().port));
    ASSERT_TRUE(probe.start(net::kLoopbackAddress));
    ASSERT_TRUE(test::waitFor([&] { return probe.snapshot().sent >= kMinProbesForRating; }, 5000ms));
    const LatencySnapshot s = probe.snapshot();
    EXPECT_EQ(s.answered(), 0);
    EXPECT_EQ(rateLink(s), LinkQuality::Poor);
}

TEST(ConnectionProbe, RestartClearsOldResults) {
    test::MockConsole console({});
    ConnectionProbe probe(fastProbe(console.discoveryPort()));
    ASSERT_TRUE(probe.start(net::kLoopbackAddress));
    ASSERT_TRUE(test::waitFor([&] { return probe.snapshot().sent >= 8; }));
    ASSERT_TRUE(probe.start(net::kLoopbackAddress));
    EXPECT_LT(probe.snapshot().sent, 8);
}

TEST(LinkRating, IgnoresTheTimeAConsoleTakesToAnswer) {
    // Measured at a real PS5: it answers a search packet after about 100 ms
    // even on a link where ping takes 1 to 4 ms. Steady answers mean a
    // steady link, however long each one takes.
    EXPECT_EQ(rateLink(linkOf({100.7f, 101.4f, 100.9f, 101.8f}, 20)), LinkQuality::Excellent);
    EXPECT_EQ(rateLink(linkOf({101.0f, 104.0f, 102.0f, 105.0f}, 20)), LinkQuality::Good);
    // Answers that are held up count as before.
    EXPECT_EQ(rateLink(linkOf({101.0f, 190.0f, 120.0f, 240.0f}, 20)), LinkQuality::Poor);
}

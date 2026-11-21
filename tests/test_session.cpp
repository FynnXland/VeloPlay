#include <gtest/gtest.h>

#include <thread>

#include "MockConsole.hpp"
#include "net/Socket.hpp"
#include "session/ISession.hpp"
#include "session/StreamMeter.hpp"

using namespace veloplay;
using namespace std::chrono_literals;

namespace {

using MeterClock = session::StreamMeter::Clock;

// The port a console listens on for session requests. libchiaki has it built in.
constexpr std::uint16_t kSessionPort = 9295;

session::SessionConfig loopbackConfig() {
    session::SessionConfig config;
    config.host = "127.0.0.1";
    config.registKey = "1b3d4f5a";
    for (std::size_t i = 0; i < config.rpKey.size(); ++i) config.rpKey[i] = static_cast<std::uint8_t>(i);
    config.stream.bitrateKbps = 12'000;
    return config;
}

std::optional<session::SessionEvent> waitForQuit(session::ISession& session, std::chrono::milliseconds timeout) {
    std::optional<session::SessionEvent> quit;
    test::waitFor(
        [&] {
            for (session::SessionEvent& event : session.pollEvents()) {
                if (event.type == session::SessionEvent::Type::Quit) quit = std::move(event);
            }
            return quit.has_value();
        },
        timeout);
    return quit;
}

} // namespace

TEST(StreamMeter, TurnsCountersIntoRates) {
    session::StreamMeter meter;
    const MeterClock::time_point start{};

    EXPECT_EQ(meter.rates(start).framesPerSecond, 0.0);

    // 30 frames of 50 000 bytes in one second: 30 fps and 12 Mbit/s.
    for (int i = 0; i < 30; ++i) meter.addVideo(50'000, 0);
    const auto rates = meter.rates(start + 1s);
    EXPECT_NEAR(rates.framesPerSecond, 30.0, 0.01);
    EXPECT_NEAR(rates.megabitsPerSecond, 12.0, 0.01);
    EXPECT_EQ(rates.videoFrames, 30u);
    EXPECT_EQ(rates.videoBytes, 1'500'000u);
}

TEST(StreamMeter, KeepsTheValueBetweenWindows) {
    session::StreamMeter meter;
    const MeterClock::time_point start{};
    meter.rates(start);
    for (int i = 0; i < 60; ++i) meter.addVideo(1000, 0);
    EXPECT_NEAR(meter.rates(start + 1s).framesPerSecond, 60.0, 0.01);

    // Asked again a moment later, before a new window is full: same answer.
    meter.addVideo(1000, 0);
    EXPECT_NEAR(meter.rates(start + 1100ms).framesPerSecond, 60.0, 0.01);
}

TEST(StreamMeter, DropsToZeroWhenNothingArrives) {
    session::StreamMeter meter;
    const MeterClock::time_point start{};
    meter.rates(start);
    for (int i = 0; i < 30; ++i) meter.addVideo(1000, 0);
    EXPECT_GT(meter.rates(start + 1s).framesPerSecond, 0.0);

    // A short gap keeps the last value, a long one does not.
    EXPECT_GT(meter.rates(start + 2s).framesPerSecond, 0.0);
    EXPECT_EQ(meter.rates(start + 4s).framesPerSecond, 0.0);
    EXPECT_EQ(meter.rates(start + 4s).megabitsPerSecond, 0.0);
}

TEST(StreamMeter, CountsLossAndAudio) {
    session::StreamMeter meter;
    meter.addVideo(100, 2);
    meter.addVideo(100, 0);
    meter.addVideo(100, -1); // "unknown" must not count backwards
    meter.addAudio();
    meter.addAudio();
    const auto rates = meter.rates(MeterClock::time_point{});
    EXPECT_EQ(rates.framesLost, 2u);
    EXPECT_EQ(rates.audioPackets, 2u);
    EXPECT_EQ(rates.videoFrames, 3u);
}

TEST(QuitReason, KnowsWhatIsWorthRetrying) {
    EXPECT_TRUE(session::isTransient(session::QuitReason::ConnectionLost));
    EXPECT_TRUE(session::isTransient(session::QuitReason::ConnectionRefused));
    EXPECT_FALSE(session::isTransient(session::QuitReason::InUse));
    EXPECT_FALSE(session::isTransient(session::QuitReason::VersionMismatch));
    EXPECT_FALSE(session::isTransient(session::QuitReason::Stopped));
    EXPECT_EQ(session::toString(session::QuitReason::InUse), "in_use");
}

// The tests below run the real library against this machine. They need the
// console's session port on loopback, so they step aside if it is taken.

TEST(ChiakiSession, RejectsUnusableInput) {
    auto session = session::chiakiSessionFactory()();
    auto config = loopbackConfig();
    config.host = "not-an-address";
    EXPECT_EQ(session->start(config, nullptr).error().code, ErrorCode::InvalidArgument);

    config = loopbackConfig();
    config.registKey.clear();
    EXPECT_EQ(session->start(config, nullptr).error().code, ErrorCode::InvalidArgument);

    config = loopbackConfig();
    config.registKey = std::string(17, 'a');
    EXPECT_EQ(session->start(config, nullptr).error().code, ErrorCode::InvalidArgument);

    // Nothing was started, so these are harmless.
    session->stop();
    EXPECT_FALSE(session->submitLoginPin("1234").ok());
    EXPECT_FALSE(session->sleepConsole().ok());
    EXPECT_TRUE(session->pollEvents().empty());
}

TEST(ChiakiSession, ReportsRefusalWhenNothingListens) {
    {
        auto probe = net::TcpListener::listen({net::kLoopbackAddress, kSessionPort});
        if (!probe) GTEST_SKIP() << "port " << kSessionPort << " is in use on this machine";
    }

    auto session = session::chiakiSessionFactory()();
    ASSERT_TRUE(session->start(loopbackConfig(), nullptr).ok());
    const auto quit = waitForQuit(*session, 15s);
    ASSERT_TRUE(quit.has_value());
#ifdef _WIN32
    // Winsock reports a refused non-blocking connect differently, and the
    // library then sees its own five-second timeout instead of the refusal.
    EXPECT_TRUE(quit->reason == session::QuitReason::ConnectionRefused ||
                quit->reason == session::QuitReason::SetupFailed)
        << quit->detail;
#else
    EXPECT_EQ(quit->reason, session::QuitReason::ConnectionRefused) << quit->detail;
#endif
    EXPECT_TRUE(session::isTransient(quit->reason));
    EXPECT_FALSE(quit->detail.empty());
}

TEST(ChiakiSession, SendsTheSessionRequestAndReportsConsoleInUse) {
    auto listener = net::TcpListener::listen({net::kLoopbackAddress, kSessionPort});
    if (!listener) GTEST_SKIP() << "port " << kSessionPort << " is in use on this machine";

    // Plays the console: reads the request, answers "Remote Play is in use".
    std::string request;
    std::jthread console([&] {
        auto accepted = listener->accept(10'000ms);
        if (!accepted || !accepted->has_value()) return;
        net::TcpSocket& socket = **accepted;
        std::array<std::uint8_t, 1024> buffer{};
        while (request.find("\r\n\r\n") == std::string::npos) {
            const auto received = socket.receiveSome(buffer, 5000ms);
            if (!received || *received == 0) break;
            request.append(reinterpret_cast<const char*>(buffer.data()), *received);
        }
        const std::string answer = "HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: 80108b10\r\nContent-Length: 0\r\n\r\n";
        (void)socket.sendAll(asBytes(answer), 5000ms);
        // Keep the socket open until the client has read the answer.
        (void)socket.receiveSome(buffer, 2000ms);
    });

    auto session = session::chiakiSessionFactory()();
    ASSERT_TRUE(session->start(loopbackConfig(), nullptr).ok());
    const auto quit = waitForQuit(*session, 15s);
    console.join();

    ASSERT_TRUE(quit.has_value());
    EXPECT_EQ(quit->reason, session::QuitReason::InUse) << quit->detail;

    // What went over the wire is what a PS5 expects from a Remote Play client.
    EXPECT_EQ(request.rfind("GET /sie/ps5/rp/sess/init HTTP/1.1\r\n", 0), 0u) << request;
    EXPECT_NE(request.find("\r\nHost: 127.0.0.1:9295\r\n"), std::string::npos) << request;
    // The regist key travels as hex of its ASCII form: "1b3d4f5a" -> 3162336434663561.
    EXPECT_NE(request.find("\r\nRP-Registkey: 3162336434663561\r\n"), std::string::npos) << request;
    EXPECT_NE(request.find("\r\nRp-Version: 1.0\r\n"), std::string::npos) << request;
}

TEST(ChiakiSession, StopsWhileWaitingForTheConsole) {
    auto listener = net::TcpListener::listen({net::kLoopbackAddress, kSessionPort});
    if (!listener) GTEST_SKIP() << "port " << kSessionPort << " is in use on this machine";

    // The "console" accepts and then says nothing.
    auto session = session::chiakiSessionFactory()();
    ASSERT_TRUE(session->start(loopbackConfig(), nullptr).ok());
    auto accepted = listener->accept(10'000ms);
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(accepted->has_value());

    session->stop();
    const auto quit = waitForQuit(*session, 10s);
    ASSERT_TRUE(quit.has_value());
    EXPECT_EQ(quit->reason, session::QuitReason::Stopped);

    // Destroying a finished session must not hang.
    session.reset();
}

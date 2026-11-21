#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "util/LogFiles.hpp"
#include "util/StallWatch.hpp"
#include <gtest/gtest.h>

#include "util/Bytes.hpp"

using namespace veloplay;

TEST(Hex, EncodesLowercase) {
    const Bytes bytes{0x00, 0x1f, 0xab, 0xff};
    EXPECT_EQ(toHex(bytes), "001fabff");
}

TEST(Hex, DecodesBothCases) {
    EXPECT_EQ(fromHex("001FabFF"), (Bytes{0x00, 0x1f, 0xab, 0xff}));
    EXPECT_EQ(fromHex(""), Bytes{});
}

TEST(Hex, RejectsMalformedInput) {
    EXPECT_FALSE(fromHex("abc"));
    EXPECT_FALSE(fromHex("zz"));
    EXPECT_FALSE(fromHex("0x12"));
}

TEST(Base64, MatchesRfc4648Vectors) {
    EXPECT_EQ(base64Encode(asBytes("")), "");
    EXPECT_EQ(base64Encode(asBytes("f")), "Zg==");
    EXPECT_EQ(base64Encode(asBytes("fo")), "Zm8=");
    EXPECT_EQ(base64Encode(asBytes("foo")), "Zm9v");
    EXPECT_EQ(base64Encode(asBytes("foobar")), "Zm9vYmFy");
}

TEST(Base64, DecodesWithAndWithoutPadding) {
    EXPECT_EQ(asText(*base64Decode("Zm9vYg==")), "foob");
    EXPECT_EQ(asText(*base64Decode("Zm9vYg")), "foob");
    EXPECT_EQ(asText(*base64Decode("Zm9vYmE=")), "fooba");
}

TEST(Base64, RejectsInvalidCharacters) {
    EXPECT_FALSE(base64Decode("Zm9v!A=="));
    EXPECT_FALSE(base64Decode("Zm9v Yg=="));
    EXPECT_FALSE(base64Decode("Z"));
}

TEST(Text, TrimAndCompare) {
    EXPECT_EQ(trim("  a b \r\n"), "a b");
    EXPECT_EQ(trim(" \t "), "");
    EXPECT_TRUE(equalsIgnoreCase("Content-Length", "content-length"));
    EXPECT_FALSE(equalsIgnoreCase("abc", "abcd"));
}

// ---- StallWatch ------------------------------------------------------------

namespace {

using StallClock = veloplay::diag::StallDetector::Clock;

veloplay::diag::StallDetector::Sample turn(std::uint64_t turns, StallClock::time_point at, const char* step) {
    veloplay::diag::StallDetector::Sample sample;
    sample.turns = turns;
    sample.lastTurn = at;
    sample.step = step;
    return sample;
}

} // namespace

TEST(StallDetector, SaysNothingWhileTheLoopComesAround) {
    using namespace std::chrono_literals;
    veloplay::diag::StallDetector detector(1500ms);
    const StallClock::time_point start{};
    // Nothing is known before the loop has run once, however much time passes.
    EXPECT_FALSE(detector.check(turn(0, start, nullptr), start + 60s));
    for (int i = 1; i <= 100; ++i) {
        const auto at = start + i * 50ms;
        EXPECT_FALSE(detector.check(turn(static_cast<std::uint64_t>(i), at, "idle"), at + 20ms));
    }
}

TEST(StallDetector, ReportsALoopThatStandsStillEvenWhenStepsChange) {
    using namespace std::chrono_literals;
    veloplay::diag::StallDetector detector(1500ms, 5000ms);
    const StallClock::time_point start{};
    EXPECT_FALSE(detector.check(turn(7, start, "session"), start + 1400ms));

    // The case from issue 3: the step keeps changing, but the loop never gets
    // back to asking Windows for messages.
    auto report = detector.check(turn(7, start, "reading the controllers"), start + 1600ms);
    ASSERT_TRUE(report);
    EXPECT_STREQ(report->step, "reading the controllers");
    EXPECT_EQ(report->duration, 1600ms);
    EXPECT_FALSE(report->finished);

    // Not again right away ...
    EXPECT_FALSE(detector.check(turn(7, start, "session"), start + 3000ms));
    // ... but every five seconds, with the step it is in by then.
    report = detector.check(turn(7, start, "session"), start + 6600ms);
    ASSERT_TRUE(report);
    EXPECT_STREQ(report->step, "session");
    EXPECT_EQ(report->duration, 6600ms);
    EXPECT_FALSE(report->finished);

    // The loop comes around again: one last report with the full length.
    report = detector.check(turn(8, start + 9000ms, "idle"), start + 9100ms);
    ASSERT_TRUE(report);
    EXPECT_STREQ(report->step, "session");
    EXPECT_EQ(report->duration, 9000ms);
    EXPECT_TRUE(report->finished);

    // And it starts from scratch.
    EXPECT_FALSE(detector.check(turn(9, start + 9200ms, "idle"), start + 9300ms));
    report = detector.check(turn(9, start + 9200ms, nullptr), start + 11000ms);
    ASSERT_TRUE(report);
    EXPECT_STREQ(report->step, "an unnamed step");
}

TEST(StallWatch, ReportsFromItsOwnThread) {
    using namespace std::chrono_literals;
    std::mutex mutex;
    std::vector<std::pair<std::string, bool>> reports;
    const auto count = [&] {
        std::lock_guard lock(mutex);
        return reports.size();
    };
    const auto waitForReports = [&](std::size_t wanted) {
        for (int i = 0; i < 400 && count() < wanted; ++i) std::this_thread::sleep_for(10ms);
        return count() >= wanted;
    };

    // Generous margins: sleeping on Windows can overshoot by 15 ms and more.
    veloplay::diag::StallWatch watch;
    watch.start(400ms, [&](const veloplay::diag::StallReport& report) {
        std::lock_guard lock(mutex);
        reports.emplace_back(report.step, report.finished);
    }, 10ms, 60s);

    // A loop that turns: nothing to report.
    for (int i = 0; i < 10; ++i) {
        veloplay::diag::loopTurned();
        veloplay::diag::setStep("quick");
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(count(), 0u);

    // One that stops turning is reported while it is stuck ...
    veloplay::diag::loopTurned();
    veloplay::diag::setStep("slow step");
    ASSERT_TRUE(waitForReports(1));
    // ... and once more when it is over.
    veloplay::diag::loopTurned();
    ASSERT_TRUE(waitForReports(2));
    watch.stop();

    std::lock_guard lock(mutex);
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[0].first, "slow step");
    EXPECT_FALSE(reports[0].second);
    EXPECT_EQ(reports[1].first, "slow step");
    EXPECT_TRUE(reports[1].second);
}

// ---- Log files -------------------------------------------------------------

namespace {

void writeFile(const std::filesystem::path& path, const std::string& text) { std::ofstream(path) << text; }

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST(LogFiles, KeepsTheLogsOfEarlierRuns) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "veloplay-logfiles-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // First start: nothing there yet.
    EXPECT_EQ(veloplay::rotateLogFiles(dir, "veloplay", 3), dir / "veloplay.log");
    EXPECT_FALSE(std::filesystem::exists(dir / "veloplay.1.log"));

    // Five runs, each writes its number. The log of the run that hung is
    // still there after the restart, and the oldest ones go.
    for (int run = 1; run <= 5; ++run) {
        const std::filesystem::path current = veloplay::rotateLogFiles(dir, "veloplay", 3);
        EXPECT_FALSE(std::filesystem::exists(current));
        writeFile(current, "run " + std::to_string(run));
    }
    EXPECT_EQ(readFile(dir / "veloplay.log"), "run 5");
    EXPECT_EQ(readFile(dir / "veloplay.1.log"), "run 4");
    EXPECT_EQ(readFile(dir / "veloplay.2.log"), "run 3");
    EXPECT_EQ(readFile(dir / "veloplay.3.log"), "run 2");
    EXPECT_FALSE(std::filesystem::exists(dir / "veloplay.4.log"));
    std::filesystem::remove_all(dir);
}

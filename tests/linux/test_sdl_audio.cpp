// Sound through SDL's "dummy" driver, which plays and records silence in real
// time without a sound card, as on the CI machines.
#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "linux/SdlAudio.hpp"

using namespace veloplay;
using namespace std::chrono_literals;

namespace {

class LinuxSound : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Only takes effect if no other test started SDL's audio already; any driver will do then.
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        initialised_ = SDL_InitSubSystem(SDL_INIT_AUDIO);
    }
    static void TearDownTestSuite() {
        if (initialised_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    void SetUp() override {
        if (!initialised_) GTEST_SKIP() << "SDL audio does not start here: " << SDL_GetError();
    }

    template <class Condition> static bool waitFor(Condition condition, std::chrono::milliseconds limit = 3000ms) {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > until) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    static inline bool initialised_ = false;
};

} // namespace

// What the tap hears is what went to the device: the samples written, in order, in the shape asked for.
TEST_F(LinuxSound, PlaysWhatIsWrittenAndTapsExactlyThat) {
    linuxos::SdlAudioOutput output;
    std::mutex mutex;
    std::vector<std::int16_t> heard;
    int tapRate = 0;
    int tapChannels = 0;
    output.setTap([&](std::span<const std::int16_t> block, int rate, int channels) {
        std::lock_guard lock(mutex);
        heard.insert(heard.end(), block.begin(), block.end());
        tapRate = rate;
        tapChannels = channels;
    });
    ASSERT_TRUE(output.start(48000, 2, 40ms));
    ASSERT_TRUE(output.waitUntilOpened(3s));
    EXPECT_TRUE(output.running());

    // 100 ms of a ramp, left and right apart, so the order shows.
    std::vector<std::int16_t> ramp;
    for (int i = 0; i < 4800; ++i) {
        ramp.push_back(static_cast<std::int16_t>(i + 1));
        ramp.push_back(static_cast<std::int16_t>(-(i + 1)));
    }
    output.write(ramp);
    ASSERT_TRUE(waitFor([&] {
        std::lock_guard lock(mutex);
        return std::find(heard.begin(), heard.end(), static_cast<std::int16_t>(4800)) != heard.end();
    })) << "the ramp never came out";
    std::this_thread::sleep_for(50ms);
    std::lock_guard lock(mutex);
    EXPECT_EQ(tapRate, 48000);
    EXPECT_EQ(tapChannels, 2);
    ASSERT_EQ(heard.size() % 2, 0u);
    // The buffer may leave out or repeat a single frame (one in 200) to hold its reserve; nothing else changes.
    int last = 0;
    int distinct = 0;
    for (std::size_t i = 0; i < heard.size(); i += 2) {
        const int left = heard[i];
        EXPECT_EQ(heard[i + 1], -left) << "frame " << i / 2 << " came apart";
        if (left == 0) continue; // silence before and after
        EXPECT_GE(left, last) << "out of order at frame " << i / 2;
        if (left != last) ++distinct;
        last = left;
    }
    EXPECT_GE(distinct, 4800 - 4800 / 100);
}

TEST_F(LinuxSound, CountsEachGapWhenTheNetworkFallsBehind) {
    linuxos::SdlAudioOutput output;
    ASSERT_TRUE(output.start(48000, 2, 20ms));
    ASSERT_TRUE(output.waitUntilOpened(3s));
    EXPECT_EQ(output.underruns(), 0u);
    const std::vector<std::int16_t> burst(2 * 2400, 100); // 50 ms, then nothing
    output.write(burst);
    EXPECT_TRUE(waitFor([&] { return output.underruns() >= 1; })) << "no gap was counted";
    const std::uint64_t first = output.underruns();
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(output.underruns(), first); // still the same gap, not one per empty read
    output.write(burst);
    EXPECT_TRUE(waitFor([&] { return output.underruns() >= first + 1; }));
    output.setDelay(60ms); // moves the reserve while playing; must not disturb anything
    output.stop();
    EXPECT_FALSE(output.running());
    EXPECT_EQ(output.underruns(), 0u);
    output.write(burst); // after stop: ignored
}

TEST_F(LinuxSound, OpensADeviceByNameOnlyWhenItHasTheChannels) {
    linuxos::SdlAudioOutput controller(std::vector<std::string>{"DualSense", "Wireless Controller"});
    ASSERT_TRUE(controller.start(48000, 4, 20ms));
    EXPECT_FALSE(controller.waitUntilOpened(3s)); // no controller on this machine
    EXPECT_FALSE(controller.running());
    controller.stop();

    const std::vector<std::string> names = linuxos::SdlAudioOutput::deviceNames();
    if (names.empty()) GTEST_SKIP() << "this SDL driver lists no outputs by name";
    linuxos::SdlAudioOutput named(std::vector<std::string>{names.front()});
    ASSERT_TRUE(named.start(48000, 1, 20ms));
    EXPECT_TRUE(named.waitUntilOpened(3s)) << names.front();
    named.stop();
    linuxos::SdlAudioOutput tooFew(std::vector<std::string>{names.front()});
    ASSERT_TRUE(tooFew.start(48000, 64, 20ms)); // more channels than any device has
    EXPECT_FALSE(tooFew.waitUntilOpened(3s));
}

TEST_F(LinuxSound, RecordsBlocksInTheShapeTheConsoleWants) {
    linuxos::SdlAudioInput microphone;
    std::atomic<int> blocks{0};
    std::atomic<std::size_t> wrongSize{0};
    ASSERT_TRUE(microphone.start([&](std::span<const std::int16_t> block) {
        if (block.size() != static_cast<std::size_t>(media::kMicFrameSamples * media::kMicChannels)) ++wrongSize;
        ++blocks;
    }));
    EXPECT_TRUE(microphone.running());
    ASSERT_TRUE(waitFor([&] { return blocks.load() >= 10; })) << "no sound from the microphone";
    EXPECT_TRUE(microphone.recording());
    EXPECT_EQ(wrongSize.load(), 0u);
    microphone.stop();
    EXPECT_FALSE(microphone.running());
    EXPECT_FALSE(microphone.recording());
    const int after = blocks.load();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(blocks.load(), after); // none after stop()
}

// stop() comes back only after a call to the sink that was running has ended.
TEST_F(LinuxSound, StopWaitsForTheSinkToReturn) {
    linuxos::SdlAudioInput microphone;
    std::atomic<bool> inside{false};
    std::atomic<bool> entered{false};
    ASSERT_TRUE(microphone.start([&](std::span<const std::int16_t>) {
        inside = true;
        entered = true;
        std::this_thread::sleep_for(50ms);
        inside = false;
    }));
    ASSERT_TRUE(waitFor([&] { return entered.load(); }));
    microphone.stop();
    EXPECT_FALSE(inside.load());
    EXPECT_FALSE(microphone.start({})); // no sink, nothing to start
}

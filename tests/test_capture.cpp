#include <gtest/gtest.h>

#include "app/CaptureMatch.hpp"

using namespace veloplay;

TEST(CaptureMatch, FindsTheSoundInputOfTheSameCard) {
    const std::vector<std::string> sounds = {"Microphone (Realtek High Definition Audio)",
                                             "Digital Audio Interface (USB3 Capture)", "Line In (Realtek Audio)"};
    EXPECT_EQ(pickSoundFor("USB3 Capture", sounds), 1);

    const std::vector<std::string> elgato = {"Headset Microphone (DualSense Wireless Controller)",
                                             "Game Capture HD60 S+ Audio"};
    EXPECT_EQ(pickSoundFor("Game Capture HD60 S+", elgato), 1);

    // More shared words win over fewer.
    const std::vector<std::string> two = {"Capture Audio", "Live Gamer Capture Audio"};
    EXPECT_EQ(pickSoundFor("Live Gamer Capture", two), 1);
}

TEST(CaptureMatch, LeavesEverythingAloneWhenNothingBelongsToIt) {
    // A webcam without its own microphone must not take the PC's.
    const std::vector<std::string> sounds = {"Microphone (Realtek High Definition Audio)", "Line In (USB Audio Device)"};
    EXPECT_EQ(pickSoundFor("Integrated Camera", sounds), -1);
    EXPECT_EQ(pickSoundFor("USB Video Device", sounds), -1); // only common words in common
    EXPECT_EQ(pickSoundFor("Anything", {}), -1);
}

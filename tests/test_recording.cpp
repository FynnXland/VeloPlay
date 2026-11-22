#include <gtest/gtest.h>

#include "media/Recording.hpp"

using namespace veloplay::media;

TEST(Recording, TheLogSaysWhatEveryPictureInTheFileIs) {
    FrameLog log;
    for (const ShownKind kind : {ShownKind::Real, ShownKind::Computed, ShownKind::Real, ShownKind::Repeated}) log.add(kind);
    EXPECT_EQ(log.size(), 4u);
    EXPECT_EQ(log.count(ShownKind::Real), 2u);
    EXPECT_EQ(log.count(ShownKind::Computed), 1u);
    EXPECT_EQ(log.text(), "rcrp\n");
}

TEST(Recording, SilenceKeepsTheSoundWithThePictures) {
    // Sound arrives as it should: nothing to add.
    EXPECT_EQ(silenceToAdd(48'000, 1.0, 48'000), 0);
    EXPECT_EQ(silenceToAdd(40'000, 1.0, 48'000), 0); // a sixth of a second behind: still waiting
    // No sound at all: after a second, silence up to a tenth of a second behind the clock.
    EXPECT_EQ(silenceToAdd(0, 1.0, 48'000), 43'200);
    // Filled once, nothing more until it falls behind again.
    EXPECT_EQ(silenceToAdd(43'200, 1.1, 48'000), 0);
    EXPECT_EQ(silenceToAdd(43'200, 1.5, 48'000), 24'000); // 0.6 s behind, filled to 0.1 s behind
    EXPECT_EQ(silenceToAdd(0, 0.0, 48'000), 0);
    EXPECT_EQ(silenceToAdd(0, 1.0, 0), 0);
}

TEST(Recording, SamplesAreTimedInHundredsOfNanoseconds) {
    EXPECT_EQ(sampleTime(0, 48'000), 0);
    EXPECT_EQ(sampleTime(48'000, 48'000), 10'000'000);
    EXPECT_EQ(sampleTime(480, 48'000), 100'000);
    // Three hours of sound do not overflow and land exactly.
    EXPECT_EQ(sampleTime(48'000LL * 3 * 3600, 48'000), 3LL * 3600 * 10'000'000);
}

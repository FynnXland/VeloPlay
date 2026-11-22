#pragma once

#include <chrono>
#include <cstdint>

#include "app/StreamProfile.hpp"

// What "automatic" asks the console for. The user does not pick a frame
// rate, a resolution, a codec or a bitrate: the program does, and it keeps
// what it learned about the link to each console.

namespace veloplay {

inline constexpr int kAutoStartBitrateKbps = 15'000;
inline constexpr int kAutoMaxBitrateKbps = 30'000;

/// What earlier sessions with one console showed. Zero means nothing yet.
struct LinkMemory {
    int bitrateKbps = 0; // what to ask for next time
    int ceilingKbps = 0; // a rate that turned out too high lies above this

    friend bool operator==(const LinkMemory&, const LinkMemory&) = default;
};

/// How one session went, as far as the bitrate is concerned.
struct SessionOutcome {
    int bitrateKbps = 0;               // what the console was asked for
    std::chrono::seconds streamed{0};  // time with a picture
    std::uint64_t frames = 0;          // pictures that arrived
    std::uint64_t framesLost = 0;      // pictures that did not
    bool stalled = false;              // the stream stood still and was started again
    /// The mark was already lowered once in this session (an evening with reconnects counts as one):
    /// a second hitch lowers the rate, not the mark again, or one bad evening ends at 720p for good.
    bool markLoweredBefore = false;
};

/// The settings "automatic" uses with a console it knows this much about.
///
/// 1080p with HEVC is the best the console offers over Remote Play. 60 fps
/// are asked for whatever the game does: a 60 fps game arrives as it is, and
/// a 30 fps game is recognised by its repeated pictures (CadenceDetector).
/// HDR stays off until there is a display path for it.
[[nodiscard]] StreamSettings automaticStream(const LinkMemory& memory);

/// What to remember after a session. A stream that stood still or lost
/// pictures lowers the rate and marks where the trouble began. A clean
/// session of some length raises it a step, up to that mark; one that ran
/// at the mark without trouble moves the mark up a little, so a bad Wi-Fi
/// evening is not remembered for ever.
[[nodiscard]] LinkMemory learn(const LinkMemory& memory, const SessionOutcome& outcome);

} // namespace veloplay

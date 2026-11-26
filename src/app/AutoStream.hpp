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
/// Away from home (VPN, another network, Sony's servers) the way runs over two internet lines, one of
/// them the upload at home. A guess, not measured: a careful start at 720p, as chiaki-ng does it from
/// away, that one clean session raises to 1080p.
inline constexpr int kAwayStartBitrateKbps = 6'000;
/// Below this much arriving, 1080p shows more blocks than 720p would. Only taken as a sign when the
/// link was in trouble too: a quiet scene or a menu sends little on any link.
inline constexpr int kFullHdDeliveredKbps = 5'000;

/// What earlier sessions with one console showed. Zero means nothing yet.
struct LinkMemory {
    int bitrateKbps = 0; // what to ask for next time
    int ceilingKbps = 0; // a rate that turned out too high lies above this
    /// The console sent too little for 1080p on a link in trouble: ask for 720p until two sessions in
    /// a row went cleanly at 720p.
    bool lowResolution = false;
    int cleanAtLow = 0;

    friend bool operator==(const LinkMemory&, const LinkMemory&) = default;
};

/// How one session went, as far as the bitrate is concerned.
struct SessionOutcome {
    int bitrateKbps = 0;               // what the console was asked for
    std::chrono::seconds streamed{0};  // time with a picture
    std::uint64_t frames = 0;          // pictures that arrived
    std::uint64_t framesLost = 0;      // pictures that did not
    bool stalled = false;              // the stream stood still and was started again
    bool fullHd = true;                // 1080p was asked for
    /// What arrived, averaged over the time with a picture; 0 when unknown. The console lowers its rate
    /// by itself when it hears of losses, so this can lie far below what was asked for.
    int deliveredKbps = 0;
    bool unsteady = false;             // pictures came unevenly for a while (the note about the link)
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
/// `away`: the console is reached from somewhere else; with nothing learned yet it starts lower.
/// `upTo720p`: never more than 720p, for a processor that unpacks the video itself and is slow. What
/// is learned stays as it is, so switching the limit off brings back what the link allows.
[[nodiscard]] StreamSettings automaticStream(const LinkMemory& memory, bool away = false, bool upTo720p = false);

/// What to remember after a session. A stream that stood still or lost
/// pictures lowers the rate and marks where the trouble began. A clean
/// session of some length raises it a step, up to that mark; one that ran
/// at the mark without trouble moves the mark up a little, so a bad Wi-Fi
/// evening is not remembered for ever.
[[nodiscard]] LinkMemory learn(const LinkMemory& memory, const SessionOutcome& outcome);

} // namespace veloplay

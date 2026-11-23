#include "app/AutoStream.hpp"

#include <algorithm>

namespace veloplay {

namespace {

// Too short to say anything about the link.
constexpr std::chrono::seconds kShortestSession{120};
// Lost pictures per picture: above this the rate was too high, below that one it was fine.
constexpr double kLossTooHigh = 0.005;
constexpr double kLossFine = 0.0005;
// Below this, 1080p shows more blocks than 720p hides.
constexpr int kFullHdFromKbps = 7'000;

int rounded(double kbps) { return static_cast<int>(kbps / 500.0 + 0.5) * 500; }

} // namespace

StreamSettings automaticStream(const LinkMemory& memory, bool away) {
    StreamSettings stream;
    stream.fps = 60;
    stream.codec = VideoCodec::Hevc;
    stream.hdr = false;
    const int start = away ? kAwayStartBitrateKbps : kAutoStartBitrateKbps;
    stream.bitrateKbps = std::clamp(memory.bitrateKbps > 0 ? memory.bitrateKbps : start,
                                    kMinBitrateKbps, kAutoMaxBitrateKbps);
    const bool fullHd = stream.bitrateKbps >= kFullHdFromKbps;
    stream.width = fullHd ? 1920 : 1280;
    stream.height = fullHd ? 1080 : 720;
    return stream;
}

LinkMemory learn(const LinkMemory& memory, const SessionOutcome& outcome) {
    LinkMemory next = memory;
    const int used = outcome.bitrateKbps;
    if (used <= 0) return next;

    const auto lower = [&](double factor) {
        // The trouble began somewhere below the rate that failed.
        if (!outcome.markLoweredBefore) {
            const int ceiling = std::max(kMinBitrateKbps, rounded(used * 0.9));
            next.ceilingKbps = next.ceilingKbps > 0 ? std::min(next.ceilingKbps, ceiling) : ceiling;
        }
        next.bitrateKbps = std::max(kMinBitrateKbps, rounded(used * factor));
    };

    if (outcome.stalled) {
        lower(0.5);
        return next;
    }
    const std::uint64_t total = outcome.frames + outcome.framesLost;
    if (outcome.streamed < kShortestSession || total == 0) return next;

    const double loss = static_cast<double>(outcome.framesLost) / static_cast<double>(total);
    if (loss > kLossTooHigh) {
        lower(0.75);
    } else if (loss < kLossFine) {
        // Clean at the mark itself: the trouble back then may have been the evening, not the link.
        // The mark moves up a tenth, and goes once it reaches the most automatic asks for.
        if (next.ceilingKbps > 0 && used >= next.ceilingKbps * 9 / 10) {
            next.ceilingKbps = rounded(next.ceilingKbps * 1.1);
            if (next.ceilingKbps >= kAutoMaxBitrateKbps) next.ceilingKbps = 0;
        }
        int raised = std::min(kAutoMaxBitrateKbps, rounded(used * 1.25));
        if (next.ceilingKbps > 0) raised = std::min(raised, next.ceilingKbps);
        next.bitrateKbps = std::max(used, raised);
    } else {
        next.bitrateKbps = used;
    }
    return next;
}

} // namespace veloplay

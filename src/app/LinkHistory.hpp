#pragma once

#include <cstdint>
#include <deque>
#include <vector>

// How the link to the console went, second by second, for the page "Network". It keeps the
// last half hour in detail for the chart and the whole session in a few sums for the figures.

namespace veloplay {

/// One second of a stream.
struct LinkSample {
    int second = 0;             // since the stream began
    float mbps = 0.0f;          // video that arrived
    float fps = 0.0f;           // pictures that arrived
    float unevenMs = 0.0f;      // how much pictures were held up on the way (FramePacer::unevenness)
    std::uint32_t lost = 0;     // pictures lost in this second
    bool stalled = false;       // no picture for the whole second

    friend bool operator==(const LinkSample&, const LinkSample&) = default;
};

/// Held up by at least this much counts as unsteady: the same mark as the note over the picture.
inline constexpr float kUnsteadyMs = 6.0f;

/// What the whole session showed so far.
struct LinkSummary {
    int seconds = 0;            // samples taken (the first seconds of a stream are left out)
    float averageMbps = 0.0f;
    float peakMbps = 0.0f;
    float lowestMbps = 0.0f;    // the worst second that still had pictures
    int lowestAt = -1;          // its second, -1 when none
    float worstUnevenMs = 0.0f;
    int worstUnevenAt = -1;
    int steadySeconds = 0;      // under kUnsteadyMs and nothing lost
    std::uint64_t lostPictures = 0;
    int stalls = 0;             // runs of seconds without a picture
    float averageFps = 0.0f;

    [[nodiscard]] int steadyPercent() const { return seconds > 0 ? steadySeconds * 100 / seconds : 0; }
};

class LinkHistory {
public:
    /// How much of the session the chart keeps; older seconds stay only in the summary.
    static constexpr int kKeptSeconds = 30 * 60;
    /// The first seconds of a stream are the console finding its rate; they would spoil the lowest value.
    static constexpr int kSettleSeconds = 3;

    void clear();
    void add(const LinkSample& sample);

    [[nodiscard]] const std::deque<LinkSample>& samples() const { return samples_; }
    [[nodiscard]] const LinkSummary& summary() const { return summary_; }
    [[nodiscard]] bool empty() const { return samples_.empty(); }

private:
    std::deque<LinkSample> samples_;
    LinkSummary summary_;
    double mbpsSum_ = 0.0;
    double fpsSum_ = 0.0;
    bool inStall_ = false;
};

} // namespace veloplay

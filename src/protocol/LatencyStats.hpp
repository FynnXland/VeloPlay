#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

// Numbers that describe how steady the link to a console is. They are computed
// from response times of discovery packets, so they include a little processing
// time on the console. What matters for playing is how much the times vary and
// how many packets get lost, and both show up reliably.

namespace veloplay::protocol {

struct LatencySnapshot {
    int sent = 0;
    int lost = 0;
    float average = 0.0f; // ms, answered probes only
    float minimum = 0.0f;
    float maximum = 0.0f;
    float p95 = 0.0f;     // 95 % of the answers were at least this fast
    float jitter = 0.0f;  // mean difference between consecutive answers, ms
    /// Oldest first. A negative value marks a lost probe.
    std::vector<float> history;

    [[nodiscard]] float lossPercent() const { return sent > 0 ? 100.0f * static_cast<float>(lost) / static_cast<float>(sent) : 0.0f; }
    [[nodiscard]] int answered() const { return sent - lost; }
};

/// Rolling window over the most recent probes.
class LatencyStats {
public:
    explicit LatencyStats(std::size_t capacity = 120) : capacity_(capacity) {}

    /// An empty optional records a probe that got no answer.
    void add(std::optional<float> milliseconds);
    void clear() { samples_.clear(); }

    [[nodiscard]] LatencySnapshot snapshot() const;

private:
    std::size_t capacity_;
    std::deque<float> samples_; // negative = lost
};

enum class LinkQuality {
    Unknown,   // not enough probes yet
    Excellent, // as steady as a cable
    Good,
    Fair,      // playable, with hiccups now and then
    Poor,
};

inline constexpr int kMinProbesForRating = 10;

/// A plain rating of the link. The worst of the three criteria decides.
LinkQuality rateLink(const LatencySnapshot& snapshot);

/// Stream settings that suit a link of the given quality, as a starting point.
struct StreamAdvice {
    int bitrateKbps;
    bool stableBuffer;
};
StreamAdvice adviceFor(LinkQuality quality);

} // namespace veloplay::protocol

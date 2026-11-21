#include "protocol/LatencyStats.hpp"

#include <algorithm>
#include <cmath>

namespace veloplay::protocol {

void LatencyStats::add(std::optional<float> milliseconds) {
    samples_.push_back(milliseconds ? std::max(0.0f, *milliseconds) : -1.0f);
    while (samples_.size() > capacity_) samples_.pop_front();
}

LatencySnapshot LatencyStats::snapshot() const {
    LatencySnapshot result;
    result.history.assign(samples_.begin(), samples_.end());
    result.sent = static_cast<int>(samples_.size());

    std::vector<float> answered;
    float jitterSum = 0.0f;
    int jitterCount = 0;
    std::optional<float> previous;
    for (float sample : samples_) {
        if (sample < 0.0f) {
            ++result.lost;
            previous.reset(); // do not bridge a gap when measuring jitter
            continue;
        }
        answered.push_back(sample);
        if (previous) {
            jitterSum += std::abs(sample - *previous);
            ++jitterCount;
        }
        previous = sample;
    }
    if (answered.empty()) return result;

    float sum = 0.0f;
    for (float value : answered) sum += value;
    result.average = sum / static_cast<float>(answered.size());
    if (jitterCount > 0) result.jitter = jitterSum / static_cast<float>(jitterCount);

    std::sort(answered.begin(), answered.end());
    result.minimum = answered.front();
    result.maximum = answered.back();
    const std::size_t index = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(answered.size()))) - 1;
    result.p95 = answered[std::min(index, answered.size() - 1)];
    return result;
}

LinkQuality rateLink(const LatencySnapshot& s) {
    if (s.sent < kMinProbesForRating) return LinkQuality::Unknown;
    if (s.answered() == 0) return LinkQuality::Poor;

    // What counts is how much answers are held up, not how long the fastest
    // one takes: a PS5 lets about 100 ms pass before it answers a search
    // packet at all (measured on a link where ping takes 1 to 4 ms). So the
    // best answer is taken as the console's own delay and everything above
    // it as the network's.
    const float loss = s.lossPercent();
    const float delay = std::max(0.0f, s.p95 - s.minimum);
    if (loss <= 0.0f && s.jitter <= 2.0f && delay <= 12.0f) return LinkQuality::Excellent;
    if (loss <= 1.0f && s.jitter <= 5.0f && delay <= 30.0f) return LinkQuality::Good;
    if (loss <= 4.0f && s.jitter <= 12.0f && delay <= 70.0f) return LinkQuality::Fair;
    return LinkQuality::Poor;
}

StreamAdvice adviceFor(LinkQuality quality) {
    switch (quality) {
        // The measurement sees delay and loss, not how much the link can
        // carry, so even a perfect result does not justify a very high rate.
        case LinkQuality::Excellent: return {30'000, false};
        case LinkQuality::Good: return {20'000, false};
        case LinkQuality::Fair: return {10'000, true};
        case LinkQuality::Poor: return {6'000, true};
        case LinkQuality::Unknown: break;
    }
    return {15'000, true};
}

} // namespace veloplay::protocol

#include "app/LinkHistory.hpp"

namespace veloplay {

void LinkHistory::clear() {
    samples_.clear();
    summary_ = {};
    mbpsSum_ = 0.0;
    fpsSum_ = 0.0;
    inStall_ = false;
}

void LinkHistory::add(const LinkSample& sample) {
    samples_.push_back(sample);
    while (samples_.size() > static_cast<std::size_t>(kKeptSeconds)) samples_.pop_front();
    if (sample.second < kSettleSeconds) return;

    LinkSummary& s = summary_;
    ++s.seconds;
    mbpsSum_ += sample.mbps;
    fpsSum_ += sample.fps;
    s.averageMbps = static_cast<float>(mbpsSum_ / s.seconds);
    s.averageFps = static_cast<float>(fpsSum_ / s.seconds);
    if (sample.mbps > s.peakMbps) s.peakMbps = sample.mbps;
    // A second without a picture is a stall and counted as one; the lowest rate is one that still carried pictures.
    if (!sample.stalled && (s.lowestAt < 0 || sample.mbps < s.lowestMbps)) {
        s.lowestMbps = sample.mbps;
        s.lowestAt = sample.second;
    }
    if (sample.unevenMs > s.worstUnevenMs) {
        s.worstUnevenMs = sample.unevenMs;
        s.worstUnevenAt = sample.second;
    }
    if (!sample.stalled && sample.lost == 0 && sample.unevenMs < kUnsteadyMs) ++s.steadySeconds;
    s.lostPictures += sample.lost;
    if (sample.stalled && !inStall_) ++s.stalls;
    inStall_ = sample.stalled;
}

} // namespace veloplay

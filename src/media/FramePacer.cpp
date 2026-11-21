#include "media/FramePacer.hpp"

#include <algorithm>
#include <cmath>

namespace veloplay::media {

void InputRateDetector::reset(int nominalFps) {
    fps_ = nominalFps >= 45 ? 60 : 30;
    against_ = 0;
}

void InputRateDetector::addSample(double fps) {
    // A stream that stands still says nothing about its rate.
    if (fps < 10.0) return;
    // Between 40 and 45 it could be either; such a second does not count.
    const int vote = fps > 45.0 ? 60 : fps < 40.0 ? 30 : fps_;
    if (vote == fps_) {
        against_ = 0;
        return;
    }
    if (++against_ >= 3) {
        fps_ = vote;
        against_ = 0;
    }
}

int frameGenerationFactorFor(int setting, int inputFps, double refreshHz) {
    int factor = setting <= 0 ? (inputFps <= 30 ? 2 : 1) : std::clamp(setting, 1, 4);
    if (refreshHz > 1.0 && inputFps > 0) {
        // A 59.94 Hz screen still fits two pictures per picture of a 30 fps stream.
        const int fits = static_cast<int>((refreshHz + 3.0) / inputFps);
        factor = std::min(factor, std::max(1, fits));
    }
    return factor;
}

void CadenceDetector::reset() { *this = CadenceDetector{}; }

void CadenceDetector::add(std::uint64_t picture, double changed) {
    const std::size_t at = static_cast<std::size_t>(picture % kWindow);
    kinds_[at] = changed < kRepeatBelow ? Kind::Repeat : changed > kNewAbove ? Kind::New : Kind::Unknown;
    numbers_[at] = picture;
    filled_[at] = true;

    // Count over the pictures right before this one, by even and odd number.
    int repeats[2] = {0, 0};
    int news[2] = {0, 0};
    int seen[2] = {0, 0};
    int lately[2][2] = {{0, 0}, {0, 0}}; // the last four pictures: [side][0] repeats, [side][1] new ones
    for (std::size_t i = 0; i < kWindow; ++i) {
        if (!filled_[i] || numbers_[i] > picture || picture - numbers_[i] >= kWindow) continue;
        const std::size_t side = static_cast<std::size_t>(numbers_[i] & 1U);
        ++seen[side];
        if (kinds_[i] == Kind::Repeat) ++repeats[side];
        if (kinds_[i] == Kind::New) ++news[side];
        if (picture - numbers_[i] < 4 && kinds_[i] != Kind::Unknown) ++lately[side][kinds_[i] == Kind::New ? 1 : 0];
    }
    const int half = static_cast<int>(kWindow / 2);
    for (std::size_t side = 0; side < 2; ++side) {
        // All of one side repeat, and the other side moves: that is the rhythm.
        if (seen[side] == half && repeats[side] == half && news[1 - side] >= half - 2) {
            halved_ = true;
            parity_ = side;
            return;
        }
    }
    if (!halved_) return;
    const std::size_t was = static_cast<std::size_t>(parity_);
    if (lately[was][1] == 2 && lately[1 - was][0] == 2) {
        // The rhythm is still there, shifted by one: a picture got lost on the way.
        parity_ = 1 - was;
    } else if (news[was] >= 2) {
        // New pictures where repeats were expected: the game runs at the full rate again.
        halved_ = false;
    }
}

void FramePacer::configure(int factor, Clock::duration period) {
    factor = std::clamp(factor, 1, 4);
    if (factor == factor_ && period == period_) return;
    factor_ = factor;
    period_ = period;
}

void FramePacer::reset() {
    previous_ = {};
    current_ = {};
}

FramePacer::Clock::duration FramePacer::addedDelay() const {
    return period_ * (factor_ - 1) / factor_;
}

void FramePacer::onFrame(std::uint64_t id, Clock::time_point arrival) {
    Slot slot;
    slot.valid = true;
    slot.id = id;
    slot.due = arrival + addedDelay();
    if (current_.valid && factor_ > 1) {
        // Even distances: 97 % of a period after the picture before, which
        // takes about a second to make up for one that came 10 ms late.
        const Clock::time_point even = current_.due + period_ * 97 / 100;
        // Far behind (a long pause, or a pile of pictures at once): start afresh instead of trailing.
        if (even > slot.due && even - slot.due <= 2 * period_) slot.due = even;
    }
    // Pictures never go backwards in time, also when the factor was just lowered.
    if (current_.valid && slot.due < current_.due) slot.due = current_.due;
    previous_ = current_;
    current_ = slot;
}

FrameChoice FramePacer::choose(Clock::time_point now, Clock::duration slack) const {
    // No more slack than half a step, or a picture could go out a whole step early.
    now += std::clamp(slack, Clock::duration::zero(), period_ / (2 * factor_));
    FrameChoice choice;
    if (!current_.valid) return choice;
    choice.valid = true;
    choice.steps = factor_;
    if (!previous_.valid || factor_ <= 1 || now >= current_.due || current_.due <= previous_.due) {
        choice.base = current_.id;
        return choice;
    }
    // Between the two: how far along the way from the previous picture's
    // moment to the current one's. Before the previous one's moment (two
    // pictures arrived in a burst) the previous one is shown.
    const double span = std::chrono::duration<double>(current_.due - previous_.due).count();
    const double along = std::chrono::duration<double>(now - previous_.due).count() / span;
    // The small amount on top keeps a moment that falls exactly on a step
    // from being rounded down to the step before.
    const int step = std::clamp(static_cast<int>(std::floor(along * factor_ + 1e-3)), 0, factor_ - 1);
    choice.base = previous_.id;
    choice.next = current_.id;
    choice.step = step;
    return choice;
}

} // namespace veloplay::media

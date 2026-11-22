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

void CadenceDetector::add(std::uint64_t picture, double change) {
    // A picture is judged against the one before and the one after it, so one picture late.
    if (filled_ == 3) {
        changes_[0] = changes_[1];
        changes_[1] = changes_[2];
        numbers_[0] = numbers_[1];
        numbers_[1] = numbers_[2];
        filled_ = 2;
    }
    // A gap in the numbers: what came before does not border on this picture.
    if (filled_ > 0 && numbers_[filled_ - 1] + 1 != picture) filled_ = 0;
    changes_[filled_] = change;
    numbers_[filled_] = picture;
    ++filled_;
    if (filled_ == 3) judge(numbers_[1], changes_[0], changes_[1], changes_[2]);
}

void CadenceDetector::judge(std::uint64_t picture, double before, double here, double after) {
    const double less = std::min(before, after);
    const double more = std::max(before, after);
    const std::size_t side = static_cast<std::size_t>(picture & 1U);

    const auto speaksFor = [&](std::size_t repeats) {
        score_[repeats] = std::min(score_[repeats] + 1, kMost);
        score_[1 - repeats] = std::max(score_[1 - repeats] - 2, 0);
    };
    if (less >= kMoving && here <= less * kClearlyLess) {
        // A calm picture between two that move: this one repeats.
        speaksFor(side);
    } else if (here >= kMoving && more <= here * kClearlyLess) {
        // A picture that moves between two calm ones: its neighbours repeat.
        speaksFor(1 - side);
    } else if (less >= kMoving && here >= kMoving) {
        // Three in a row that move alike: the game runs at the rate of the stream.
        score_[0] = std::max(score_[0] - 1, 0);
        score_[1] = std::max(score_[1] - 1, 0);
    }
    // Anything else, standing still above all, says nothing.

    if (!halved_) {
        for (std::size_t repeats = 0; repeats < 2; ++repeats) {
            if (score_[repeats] >= kEnter && score_[repeats] > score_[1 - repeats]) {
                halved_ = true;
                parity_ = repeats;
            }
        }
        return;
    }
    const std::size_t was = static_cast<std::size_t>(parity_);
    if (score_[1 - was] >= kEnter && score_[1 - was] > score_[was]) {
        // The rhythm is still there, shifted by one: a picture got lost or the game stumbled.
        parity_ = 1 - was;
    } else if (score_[0] == 0 && score_[1] == 0) {
        halved_ = false;
    }
}

void FramePacer::configure(int factor, Clock::duration period, Clock::duration refresh) {
    factor = std::clamp(factor, 1, 4);
    refresh_ = refresh;
    if (factor == factor_ && period == period_) return;
    // Lateness is measured against the period; at another rate the old figures say nothing.
    if (period != period_) {
        latenessCount_ = 0;
        latenessNext_ = 0;
        madeUpCount_ = 0;
        madeUpNext_ = 0;
        owed_ = 0.0f;
        lastArrival_ = {};
    }
    factor_ = factor;
    period_ = period;
}

void FramePacer::reset() {
    previous_ = {};
    current_ = {};
    latenessCount_ = 0;
    latenessNext_ = 0;
    lastArrival_ = {};
    reserve_ = {};
    madeUpCount_ = 0;
    madeUpNext_ = 0;
    owed_ = 0.0f;
    unevennessMs_ = 0.0f;
}

FramePacer::Clock::duration FramePacer::addedDelay() const {
    return period_ * (factor_ - 1) / factor_ + reserve_;
}

void FramePacer::measureLateness(Clock::time_point arrival) {
    using Ms = std::chrono::duration<float, std::milli>;
    // Up to 10 ms for pictures that come late: a picture later than that is a stall, not
    // unevenness, and waiting for it would cost every picture that much.
    constexpr float kMostForLateness = 10.0f;
    constexpr float kLeastForLateness = 1.0f; // the loop's own wobble, always
    if (lastArrival_ != Clock::time_point{}) {
        const float late = Ms(arrival - lastArrival_ - period_).count();
        // A picture right behind a late one comes early; that is no lateness. A long gap is a stall.
        if (late < Ms(2 * period_).count()) {
            lateness_[latenessNext_] = std::max(late, 0.0f);
            latenessNext_ = (latenessNext_ + 1) % lateness_.size();
            latenessCount_ = std::min(latenessCount_ + 1, lateness_.size());
        }
        // Held up on the way, the late picture is followed by one that comes early; a picture that was
        // never sent is followed by one on time. Only the first is the link's doing.
        constexpr float kMargin = 0.5f; // the loop's and the clock's own wobble
        float madeUp = 0.0f;
        if (late > 2 * Ms(period_).count()) {
            owed_ = 0.0f; // a stall: something else says so
        } else if (late > kMargin) {
            owed_ = std::max(owed_, late);
        } else if (late < -kMargin && owed_ > 0.0f) {
            madeUp = std::min(owed_, -late);
            owed_ -= madeUp;
        } else {
            owed_ = 0.0f; // on time again without catching up: whatever was late was not held up
        }
        madeUp_[madeUpNext_] = madeUp;
        madeUpNext_ = (madeUpNext_ + 1) % madeUp_.size();
        madeUpCount_ = std::min(madeUpCount_ + 1, madeUp_.size());
        if (madeUpCount_ >= 16) {
            std::array<float, 256> sorted{};
            std::copy_n(madeUp_.begin(), madeUpCount_, sorted.begin());
            const std::size_t rank = madeUpCount_ - 1 - madeUpCount_ / 50;
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(rank),
                             sorted.begin() + static_cast<std::ptrdiff_t>(madeUpCount_));
            unevennessMs_ = sorted[rank];
        }
    }
    lastArrival_ = arrival;

    // Nearly all of them: the 98th of a hundred, so a rare late picture is covered too.
    float covered = kLeastForLateness;
    if (latenessCount_ >= 16) {
        std::array<float, 256> sorted{};
        std::copy_n(lateness_.begin(), latenessCount_, sorted.begin());
        const std::size_t rank = latenessCount_ - 1 - latenessCount_ / 50;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(rank),
                         sorted.begin() + static_cast<std::ptrdiff_t>(latenessCount_));
        covered = std::clamp(sorted[rank], kLeastForLateness, kMostForLateness);
    }
    // Half a refresh, the most choose() lets a picture go out early (60 Hz until it is measured).
    const Clock::duration halfRefresh = std::min<Clock::duration>(
        refresh_ > Clock::duration::zero() ? refresh_ / 2 : std::chrono::microseconds{8'333}, period_ / (2 * factor_));
    const auto target = halfRefresh + std::chrono::duration_cast<Clock::duration>(Ms(covered));
    // Up at once, so the next late picture is covered; down slowly, about 1.5 ms per second
    // at 30 pictures, so the rhythm does not jump.
    if (target >= reserve_) reserve_ = target;
    else reserve_ = std::max(target, reserve_ - std::chrono::duration_cast<Clock::duration>(Ms(0.05f)));
}

void FramePacer::onFrame(std::uint64_t id, Clock::time_point arrival) {
    measureLateness(arrival);
    Slot slot;
    slot.valid = true;
    slot.id = id;
    slot.due = arrival + addedDelay();
    if (current_.valid) {
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

bool FramePacer::wantsNext(Clock::time_point now, Clock::duration slack) const {
    if (!current_.valid) return true;
    now += std::clamp(slack, Clock::duration::zero(), period_ / (2 * factor_));
    return now >= current_.due + period_ / factor_;
}

FrameChoice FramePacer::choose(Clock::time_point now, Clock::duration slack) const {
    // No more slack than half a step, or a picture could go out a whole step early.
    now += std::clamp(slack, Clock::duration::zero(), period_ / (2 * factor_));
    FrameChoice choice;
    if (!current_.valid) return choice;
    choice.valid = true;
    choice.steps = factor_;
    if (!previous_.valid || now >= current_.due || current_.due <= previous_.due) {
        choice.base = current_.id;
        return choice;
    }
    // Without pictures in between, the one before stays until the newest one's turn.
    if (factor_ <= 1) {
        choice.base = previous_.id;
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

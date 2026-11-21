#include "util/StallWatch.hpp"

#include <atomic>

namespace veloplay::diag {

namespace {

std::atomic<const char*> gStep{nullptr};
std::atomic<std::int64_t> gLastTurn{0}; // steady clock, nanoseconds
std::atomic<std::uint64_t> gTurns{0};

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(StallWatch::Clock::now().time_since_epoch()).count();
}

} // namespace

void loopTurned() noexcept {
    gLastTurn.store(nowNs(), std::memory_order_relaxed);
    gTurns.fetch_add(1, std::memory_order_relaxed);
}

void setStep(const char* step) noexcept { gStep.store(step, std::memory_order_relaxed); }

std::chrono::milliseconds sinceLoopTurned() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds(nowNs() - gLastTurn.load(std::memory_order_relaxed)));
}

const char* currentStep() noexcept { return gStep.load(std::memory_order_relaxed); }

std::optional<StallReport> StallDetector::check(const Sample& sample, Clock::time_point now) {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    if (stalled_) {
        if (sample.turns != stalledTurn_) {
            // It moved on. The turn that came next marks the end of the stall.
            stalled_ = false;
            return StallReport{step_, duration_cast<milliseconds>(sample.lastTurn - stalledSince_), true};
        }
        // The step can still change while the loop does not come around, for
        // example when one pass through the loop hangs in several places.
        if (sample.step) step_ = sample.step;
        if (now - lastReport_ >= repeatEvery_) {
            lastReport_ = now;
            return StallReport{step_, duration_cast<milliseconds>(now - stalledSince_), false};
        }
        return std::nullopt;
    }
    if (sample.turns == 0 || now - sample.lastTurn < threshold_) return std::nullopt;
    stalled_ = true;
    stalledTurn_ = sample.turns;
    stalledSince_ = sample.lastTurn;
    lastReport_ = now;
    step_ = sample.step ? sample.step : "an unnamed step";
    return StallReport{step_, duration_cast<milliseconds>(now - stalledSince_), false};
}

void StallWatch::start(std::chrono::milliseconds threshold, Report report, std::chrono::milliseconds checkEvery,
                       std::chrono::milliseconds repeatEvery) {
    stop();
    // A watch that starts counts from now, not from what an earlier loop left behind.
    gTurns.store(0, std::memory_order_relaxed);
    thread_ = std::jthread([threshold, checkEvery, repeatEvery, report = std::move(report)](std::stop_token stop) {
        StallDetector detector(threshold, repeatEvery);
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(checkEvery);
            StallDetector::Sample sample;
            sample.turns = gTurns.load(std::memory_order_relaxed);
            sample.lastTurn = Clock::time_point(std::chrono::duration_cast<Clock::duration>(
                std::chrono::nanoseconds(gLastTurn.load(std::memory_order_relaxed))));
            sample.step = gStep.load(std::memory_order_relaxed);
            if (auto found = detector.check(sample, Clock::now())) report(*found);
        }
    });
}

void StallWatch::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

} // namespace veloplay::diag

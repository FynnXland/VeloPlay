#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>

// Finds out where the window's thread gets stuck. The thread says each time
// its loop comes around and names the step it is about to do; a second
// thread notices when the loop has not come around for far too long and
// reports the step. That is what turns "the program did not respond" into a
// line in the log that says where.

namespace veloplay::diag {

/// The window's loop came around: it asked Windows for messages again. This
/// is what Windows itself goes by when it decides that a window hangs.
void loopTurned() noexcept;

/// Names what the calling thread is about to do. Pass a string literal: the
/// pointer is kept, not the text.
void setStep(const char* step) noexcept;

/// How long ago the loop last came around, and the step named last (may be
/// null). For diagnostics from other threads.
[[nodiscard]] std::chrono::milliseconds sinceLoopTurned() noexcept;
[[nodiscard]] const char* currentStep() noexcept;

struct StallReport {
    const char* step = nullptr;           // what the thread was doing; never null
    std::chrono::milliseconds duration{}; // for how long the loop has not come around
    bool finished = false;                // false while it is stuck, true once it moved on
};

/// Decides from what the loop last said. Time is passed in, so tests need no clock.
class StallDetector {
public:
    using Clock = std::chrono::steady_clock;

    struct Sample {
        std::uint64_t turns = 0;      // how often the loop has come around
        Clock::time_point lastTurn{}; // when it last did
        const char* step = nullptr;   // the step named last
    };

    /// Reports once the loop is `threshold` late, again every `repeatEvery`
    /// while it stays stuck, and once more when it moves on.
    explicit StallDetector(std::chrono::milliseconds threshold,
                           std::chrono::milliseconds repeatEvery = std::chrono::milliseconds{5000})
        : threshold_(threshold), repeatEvery_(repeatEvery) {}

    std::optional<StallReport> check(const Sample& sample, Clock::time_point now);

private:
    std::chrono::milliseconds threshold_;
    std::chrono::milliseconds repeatEvery_;
    bool stalled_ = false;
    std::uint64_t stalledTurn_ = 0;
    Clock::time_point stalledSince_{};
    Clock::time_point lastReport_{};
    const char* step_ = nullptr;
};

class StallWatch {
public:
    using Clock = StallDetector::Clock;
    /// Called from the watching thread.
    using Report = std::function<void(const StallReport& report)>;

    StallWatch() = default;
    ~StallWatch() { stop(); }
    StallWatch(const StallWatch&) = delete;
    StallWatch& operator=(const StallWatch&) = delete;

    /// Watches the thread that calls loopTurned(). Nothing is reported before its first call.
    void start(std::chrono::milliseconds threshold, Report report,
               std::chrono::milliseconds checkEvery = std::chrono::milliseconds{250},
               std::chrono::milliseconds repeatEvery = std::chrono::milliseconds{5000});
    void stop();

private:
    std::jthread thread_;
};

} // namespace veloplay::diag

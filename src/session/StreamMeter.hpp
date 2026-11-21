#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace veloplay::session {

/// Counts what arrives and turns the counters into rates. add*() may be called
/// from any thread; rates() belongs to one reader.
class StreamMeter {
public:
    using Clock = std::chrono::steady_clock;

    struct Rates {
        double framesPerSecond = 0.0;
        double megabitsPerSecond = 0.0;
        std::uint64_t videoFrames = 0;
        std::uint64_t videoBytes = 0;
        std::uint64_t audioPackets = 0;
        std::uint64_t framesLost = 0;
    };

    void addVideo(std::size_t bytes, int framesLost);
    void addAudio();

    /// Rates over the time since the window last closed. A window closes once
    /// it is at least kWindow long, so frequent calls return a steady value.
    Rates rates(Clock::time_point now);

    static constexpr std::chrono::milliseconds kWindow{500};
    /// Without a single frame for this long the rates drop to zero.
    static constexpr std::chrono::milliseconds kStale{2000};

private:
    std::atomic<std::uint64_t> videoFrames_{0};
    std::atomic<std::uint64_t> videoBytes_{0};
    std::atomic<std::uint64_t> audioPackets_{0};
    std::atomic<std::uint64_t> framesLost_{0};

    bool started_ = false;
    Clock::time_point windowStart_{};
    std::uint64_t windowFrames_ = 0;
    std::uint64_t windowBytes_ = 0;
    double framesPerSecond_ = 0.0;
    double megabitsPerSecond_ = 0.0;
};

} // namespace veloplay::session

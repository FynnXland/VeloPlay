#include "session/StreamMeter.hpp"

namespace veloplay::session {

void StreamMeter::addVideo(std::size_t bytes, int framesLost) {
    videoFrames_.fetch_add(1, std::memory_order_relaxed);
    videoBytes_.fetch_add(bytes, std::memory_order_relaxed);
    if (framesLost > 0) framesLost_.fetch_add(static_cast<std::uint64_t>(framesLost), std::memory_order_relaxed);
}

void StreamMeter::addAudio() { audioPackets_.fetch_add(1, std::memory_order_relaxed); }

StreamMeter::Rates StreamMeter::rates(Clock::time_point now) {
    const std::uint64_t frames = videoFrames_.load(std::memory_order_relaxed);
    const std::uint64_t bytes = videoBytes_.load(std::memory_order_relaxed);

    if (!started_) {
        started_ = true;
        windowStart_ = now;
        windowFrames_ = frames;
        windowBytes_ = bytes;
    } else if (now - windowStart_ >= kWindow) {
        const double seconds = std::chrono::duration<double>(now - windowStart_).count();
        const std::uint64_t newFrames = frames - windowFrames_;
        if (newFrames > 0 || now - windowStart_ >= kStale) {
            framesPerSecond_ = static_cast<double>(newFrames) / seconds;
            megabitsPerSecond_ = static_cast<double>(bytes - windowBytes_) * 8.0 / seconds / 1'000'000.0;
            windowStart_ = now;
            windowFrames_ = frames;
            windowBytes_ = bytes;
        }
    }

    Rates out;
    out.framesPerSecond = framesPerSecond_;
    out.megabitsPerSecond = megabitsPerSecond_;
    out.videoFrames = frames;
    out.videoBytes = bytes;
    out.audioPackets = audioPackets_.load(std::memory_order_relaxed);
    out.framesLost = framesLost_.load(std::memory_order_relaxed);
    return out;
}

} // namespace veloplay::session

#include "media/Recording.hpp"

#include <algorithm>
#include <cmath>

namespace veloplay::media {

void FrameLog::add(ShownKind kind) { letters_.push_back(static_cast<char>(kind)); }

std::size_t FrameLog::count(ShownKind kind) const {
    return static_cast<std::size_t>(std::count(letters_.begin(), letters_.end(), static_cast<char>(kind)));
}

std::int64_t silenceToAdd(std::int64_t samplesSoFar, double secondsSinceStart, int sampleRate, double allowedLag,
                          double keepBehind) {
    if (sampleRate <= 0 || secondsSinceStart <= 0.0) return 0;
    const double behind = secondsSinceStart - static_cast<double>(samplesSoFar) / sampleRate;
    if (behind <= allowedLag) return 0;
    return std::max<std::int64_t>(0, std::llround((behind - keepBehind) * sampleRate));
}

std::int64_t sampleTime(std::int64_t sample, int sampleRate) {
    if (sampleRate <= 0) return 0;
    // Exact for whole seconds and without overflow for recordings of many hours.
    return sample / sampleRate * 10'000'000 + sample % sampleRate * 10'000'000 / sampleRate;
}

} // namespace veloplay::media

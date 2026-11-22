#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace veloplay::media {

/// What a picture in a recording is. A recording takes one picture per
/// refresh of the screen, so the same picture can go in twice.
enum class ShownKind : char {
    Real = 'r',     // a picture the console sent
    Computed = 'c', // a picture VeloPlay computed between two of them
    Repeated = 'p', // the picture before, shown once more
};

/// One letter per picture that went into the file, in order. It describes
/// the file, not the screen: a picture dropped on its way to the file is
/// not in it. With it a recording can be turned into the same scene without
/// computed pictures, by leaving out the "c" ones.
class FrameLog {
public:
    void add(ShownKind kind);
    [[nodiscard]] std::size_t size() const { return letters_.size(); }
    [[nodiscard]] std::size_t count(ShownKind kind) const;
    /// The letters, then a newline: what goes into the file beside the recording.
    [[nodiscard]] std::string text() const { return letters_ + "\n"; }

private:
    std::string letters_;
};

/// The sound of a recording is timed by counting its samples. When none
/// arrive (no sound device, a game in silence that sends nothing) the pictures
/// would run ahead and the file would stall waiting for sound. So once the
/// sound is more than `allowedLag` seconds behind the clock, silence fills it
/// up to `keepBehind` seconds behind the clock. Gives back how many samples
/// of silence to add, per channel.
[[nodiscard]] std::int64_t silenceToAdd(std::int64_t samplesSoFar, double secondsSinceStart, int sampleRate,
                                        double allowedLag = 0.25, double keepBehind = 0.10);

/// The time of a sample in a recording, in the units Media Foundation counts in (100 ns).
[[nodiscard]] std::int64_t sampleTime(std::int64_t sample, int sampleRate);

} // namespace veloplay::media

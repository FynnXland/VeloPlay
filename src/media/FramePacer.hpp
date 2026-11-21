#pragma once

#include <chrono>
#include <cstdint>

// When which picture goes to the screen. The console sends 30 or 60 pictures
// a second; with frame generation the PC shows more than arrive. This file
// holds the decisions for that, without any graphics: which rate arrives,
// how many pictures to compute in between, and which one is due right now.

namespace veloplay::media {

/// Tells a 30 fps stream from a 60 fps one by what actually arrives. The
/// setting says what was asked for; a console can deliver something else.
class InputRateDetector {
public:
    explicit InputRateDetector(int nominalFps = 30) { reset(nominalFps); }

    /// Starts over with what the session asked for.
    void reset(int nominalFps);

    /// One measurement of the pictures that arrived per second. Feed about
    /// one a second. A single odd second changes nothing: on Wi-Fi a 60 fps
    /// stream drops to 35 for a moment without becoming a 30 fps stream.
    void addSample(double fps);

    [[nodiscard]] int fps() const { return fps_; } // 30 or 60

private:
    int fps_ = 30;
    int against_ = 0; // measurements in a row that speak for the other rate
};

/// How many pictures reach the screen per picture from the console.
/// `setting` 0 means automatic: a 30 fps stream is doubled, a 60 fps stream
/// is shown as it is. 1 is off, 2 to 4 are fixed factors.
[[nodiscard]] int frameGenerationFactorFor(int setting, int inputFps);

/// What to show at one moment.
struct FrameChoice {
    bool valid = false;     // false until the first picture has arrived
    std::uint64_t base = 0; // the picture from the console to show or to start from
    std::uint64_t next = 0; // the one after it; only meaningful when step > 0
    int step = 0;           // 0: show `base` itself. k: the k-th of the pictures between base and next
    int steps = 1;          // pictures per console picture; the phase between the two is step / steps

    friend bool operator==(const FrameChoice&, const FrameChoice&) = default;
};

/// Lays the pictures out in time. A picture between A and B can only be
/// computed once B is there, so everything is shown a little late: with a
/// factor n and a period T between console pictures, B itself appears
/// T * (n - 1) / n after it arrived, and the pictures between A and B fill
/// the time before that. The first of them is due the moment B arrives.
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;

    /// `factor` 1 switches generation off: every picture is shown when it arrives.
    void configure(int factor, Clock::duration period);
    void reset();

    /// A picture from the console is ready. Ids must grow.
    void onFrame(std::uint64_t id, Clock::time_point arrival);

    /// What goes out with the refresh that happens at `now`. A screen shows
    /// pictures only at its refreshes, and a picture that is due a hair
    /// after one would wait for the next: with a 30 fps stream on a 60 Hz
    /// screen every step falls right on a refresh, so that hair decides each
    /// time. `slack` is how much too early a picture may go out instead;
    /// half the time between two refreshes makes each picture go out with
    /// the refresh nearest to its moment.
    [[nodiscard]] FrameChoice choose(Clock::time_point now, Clock::duration slack = Clock::duration::zero()) const;

    /// How much later than without generation a picture appears. The sound
    /// has to wait this long as well to stay in step.
    [[nodiscard]] Clock::duration addedDelay() const;

    [[nodiscard]] int factor() const { return factor_; }

private:
    struct Slot {
        bool valid = false;
        std::uint64_t id = 0;
        Clock::time_point due{}; // when the picture itself goes to the screen
    };

    int factor_ = 1;
    Clock::duration period_ = std::chrono::microseconds{33'333};
    Slot previous_;
    Slot current_;
};

} // namespace veloplay::media

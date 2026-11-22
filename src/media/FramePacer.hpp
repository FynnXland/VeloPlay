#pragma once

#include <array>
#include <chrono>
#include <cstddef>
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
///
/// A screen cannot show more pictures than it has refreshes. With
/// `refreshHz` given, the factor stays within refreshes per picture from the
/// console: 3x on a 60 Hz screen would only add delay. 0 means unknown.
[[nodiscard]] int frameGenerationFactorFor(int setting, int inputFps, double refreshHz = 0.0);

/// Notices a game that runs at 30 fps inside a 60 fps stream. The console
/// then sends every picture twice, and computing a picture between two that
/// are the same gains nothing. Fed with how much each picture differs from
/// the one before it.
///
/// A repeat is not the same picture bit for bit: the encoder uses it to
/// touch up what it had no data for the first time, the more the faster the
/// picture moves. So no fixed limit says "repeat". What counts is the
/// rhythm: every other picture changes clearly less than the ones around
/// it. Each picture that fits the rhythm adds to a score, each one that
/// speaks against it takes away, and the answer only changes when the score
/// has gone all the way. A game that stands still says nothing; a moment
/// that does not fit, such as a stumble of the game or a hard cut, changes
/// nothing.
class CadenceDetector {
public:
    void reset();

    /// `picture` counts every picture from the console, shown or not.
    /// `change` is how much it differs from the picture before, as the mean
    /// difference in brightness levels (0 to 255).
    void add(std::uint64_t picture, double change);

    /// True while every other picture repeats the one before it.
    [[nodiscard]] bool halved() const { return halved_; }

    /// With halved(): whether this picture is one of the repeats.
    [[nodiscard]] bool isRepeat(std::uint64_t picture) const { return halved_ && (picture & 1U) == parity_; }

    /// The change of the newest picture and of the two before it, newest last. For the log.
    [[nodiscard]] const double* recent() const { return changes_; }

private:
    // Below this, nothing moves and nothing can be told.
    static constexpr double kMoving = 0.5;
    // A repeat changes at most this much of what its neighbours change.
    static constexpr double kClearlyLess = 0.6;
    // Pictures in the rhythm it takes to believe it, and the most that are remembered.
    static constexpr int kEnter = 10;
    static constexpr int kMost = 40;

    void judge(std::uint64_t picture, double before, double here, double after);

    double changes_[3]{};          // of the last three pictures, oldest first
    std::uint64_t numbers_[3]{};
    int filled_ = 0;
    int score_[2] = {0, 0};        // how much speaks for even, and for odd, numbers being the repeats
    bool halved_ = false;
    std::uint64_t parity_ = 0;     // numbers with this lowest bit are the repeats
};

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

    /// `factor` 1 switches generation off: every picture is shown when it arrives. `refresh` is
    /// the time between two refreshes of the screen, zero when not known yet.
    void configure(int factor, Clock::duration period, Clock::duration refresh = Clock::duration::zero());
    void reset();

    /// A picture from the console is ready. Ids must grow.
    ///
    /// With generation on, pictures are laid out at even distances and not
    /// at the distances they arrived at. Wi-Fi delivers unevenly: one
    /// picture late, the next right behind it. Shown as they come, motion
    /// would stutter in that rhythm. So a picture is due one period after
    /// the one before it, or when it can be shown at the earliest, whichever
    /// is later; a slightly shortened period lets the display catch up
    /// again after a late picture.
    void onFrame(std::uint64_t id, Clock::time_point arrival);

    /// When the newest picture itself goes to the screen. For tests and the log.
    [[nodiscard]] Clock::time_point newestDue() const { return current_.due; }

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

    /// The part of addedDelay() that waits for pictures that come late, beyond the half period.
    ///
    /// A computed picture needs the next real one, and is due the moment that one is decoded.
    /// A screen shows pictures only at its refreshes, and choose() lets one go out up to half a
    /// refresh early, so without a reserve the computed picture was lost whenever the real one
    /// came a moment after a refresh, and again for every picture Wi-Fi brought a few
    /// milliseconds late: the display dropped below 60 now and then. The reserve is half a
    /// refresh plus how late pictures have come lately (nearly all of them, measured), so it is
    /// small on a steady link.
    [[nodiscard]] Clock::duration reserve() const { return factor_ > 1 ? reserve_ : Clock::duration::zero(); }

    [[nodiscard]] int factor() const { return factor_; }

private:
    struct Slot {
        bool valid = false;
        std::uint64_t id = 0;
        Clock::time_point due{}; // when the picture itself goes to the screen
    };

    void measureLateness(Clock::time_point arrival);

    int factor_ = 1;
    Clock::duration period_ = std::chrono::microseconds{33'333};
    Clock::duration refresh_{};
    Slot previous_;
    Slot current_;
    // How late pictures came, in milliseconds past one period after the one before: the newest few seconds.
    std::array<float, 256> lateness_{};
    std::size_t latenessCount_ = 0;
    std::size_t latenessNext_ = 0;
    Clock::time_point lastArrival_{};
    Clock::duration reserve_{};
};

} // namespace veloplay::media

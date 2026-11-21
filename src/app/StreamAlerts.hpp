#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

// Short notes over the running picture: the connection stalls, pictures get
// lost, no controller is there. They say what is wrong while it is wrong,
// so a hitch in the game is not mistaken for one in the network or the
// other way round. This file decides when a note shows; it draws nothing.

namespace veloplay {

/// What can be wrong during a stream, most pressing first.
enum class StreamAlert {
    NoController,     // nothing to steer with is connected
    ConnectionStalls, // no picture has arrived for a noticeable moment
    PicturesLost,     // pictures went missing on the way
    DecoderBehind,    // this PC threw pictures away because unpacking fell behind
    FewerPictures,    // the console sends clearly fewer pictures than asked for, without losses
};

/// What is known about the stream at one moment.
struct StreamObservation {
    std::chrono::milliseconds sincePicture{0}; // since the last picture arrived
    std::uint64_t framesLost = 0;              // lost on the way, counted since the session began
    std::uint64_t framesDropped = 0;           // thrown away by the decoder, counted the same way
    double streamFps = 0.0;                    // pictures arriving per second
    int requestedFps = 60;
    bool controllerConnected = true;
    /// False once something other than a controller steers (keyboard and mouse, later).
    bool controllerWanted = true;
};

/// Turns observations into notes that do not flicker: a note appears when
/// its cause is clear and stays a few seconds after the cause is gone.
class StreamAlerts {
public:
    using Clock = std::chrono::steady_clock;

    /// A new session or a new attempt. Counters start from zero again.
    void reset(Clock::time_point now);

    /// Call often; several times a second at least.
    void update(const StreamObservation& observation, Clock::time_point now);

    /// The notes to show right now, most pressing first.
    [[nodiscard]] std::vector<StreamAlert> active(Clock::time_point now) const;

    /// Notes that appeared with the last update(), for the log.
    [[nodiscard]] const std::vector<StreamAlert>& raised() const { return raised_; }

private:
    static constexpr int kKinds = 5;
    struct Slot {
        bool on = false;
        Clock::time_point until{}; // shown up to here
    };

    void raise(StreamAlert alert, Clock::time_point until);

    Slot slots_[kKinds];
    std::vector<StreamAlert> raised_;
    Clock::time_point started_{};
    bool primed_ = false; // the counters below hold values of this session
    std::uint64_t framesLost_ = 0;
    std::uint64_t framesDropped_ = 0;
    std::vector<Clock::time_point> losses_; // when pictures were lost lately
    Clock::time_point controllerGone_{};    // since when there is no controller; zero = there is one
    Clock::time_point fewerSince_{};        // since when too few pictures arrive; zero = enough
    Clock::time_point fewerQuietUntil_{};   // said once; not again before this
};

} // namespace veloplay

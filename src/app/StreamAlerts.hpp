#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>
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
    BatteryLow,       // the controller's battery is nearly empty and it is not charging
    GraphicsRestored, // the graphics card had a hiccup and the picture was set up again
    UnsteadyLink,     // the console's pictures come unevenly for a while: its Wi-Fi, most likely
};

/// Every kind, in the order above.
inline constexpr std::array<StreamAlert, 8> kAllStreamAlerts = {
    StreamAlert::NoController,  StreamAlert::ConnectionStalls, StreamAlert::PicturesLost, StreamAlert::DecoderBehind,
    StreamAlert::FewerPictures, StreamAlert::BatteryLow,       StreamAlert::GraphicsRestored, StreamAlert::UnsteadyLink,
};

/// The kind's name in the settings file; stays the same when the enum changes.
std::string_view alertKey(StreamAlert alert);

struct NoteSettings;
/// Whether a note of this kind is shown over the picture. Hidden ones still go to the log.
bool alertShown(const NoteSettings& settings, StreamAlert alert);

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
    int batteryPercent = -1; // of the controller in use; -1 = unknown or none
    bool charging = false;
    /// How late the console's pictures come lately, in milliseconds (FramePacer::lateness); 0 = unknown.
    double pictureLatenessMs = 0.0;
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

    /// The graphics device was lost and made anew; the next update() says so for a few seconds.
    void graphicsRestored(Clock::time_point now) { restoredAt_ = now; }

    /// Notes that appeared with the last update(), for the log.
    [[nodiscard]] const std::vector<StreamAlert>& raised() const { return raised_; }

private:
    static constexpr int kKinds = 8;
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
    int batteryToldAt_ = 101;               // the charge the last note was about; 101 = none yet
    Clock::time_point restoredAt_{};        // when the graphics device was made anew; zero = not since the last update
    Clock::time_point unsteadySince_{};     // since when pictures come unevenly; zero = they do not
    Clock::time_point unsteadyQuietUntil_{}; // said once; not again before this
};

} // namespace veloplay

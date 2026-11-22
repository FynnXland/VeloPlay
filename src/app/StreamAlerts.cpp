#include "app/StreamAlerts.hpp"

#include <algorithm>

#include "app/Config.hpp"

namespace veloplay {

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

// The first moments of a stream have no steady rate yet.
constexpr seconds kGrace{3};
// A gap this long between pictures is seen as a hitch. At 30 fps pictures are 33 ms apart.
constexpr milliseconds kStall{300};
// One lost picture now and then is normal on Wi-Fi and repaired unseen. Several in a short time show.
constexpr int kLossesThatShow = 3;
constexpr seconds kLossWindow{5};
// A controller that reconnects takes a moment to come back.
constexpr milliseconds kControllerPatience{1500};
// Clearly fewer pictures than asked for, for this long, without anything being lost.
constexpr double kFewerBelow = 0.8;
constexpr seconds kFewerAfter{3};
// How long a note stays after its cause was seen last.
constexpr milliseconds kHold{2500};
constexpr seconds kHoldLong{4};
// A console that sends fewer pictures for good is said once, not for the whole evening.
constexpr seconds kFewerShown{6};
constexpr seconds kFewerQuiet{60};
// The battery is mentioned at these two marks, each once, so there is time to reach for the cable.
constexpr int kBatteryLow = 15;
constexpr int kBatteryVeryLow = 5;
constexpr seconds kBatteryShown{8};
// Long enough to read after the picture came back; the picture is back within a second.
constexpr seconds kRestoredShown{6};
// Pictures more than 6 ms late for nearly all of them, for 20 seconds on end: the link is uneven, not
// a passing moment. Below 4 ms it counts as steady again. Said for 10 seconds, then not for half an hour:
// it is advice, and the log has the details.
constexpr double kUnsteadyFromMs = 6.0;
constexpr double kSteadyBelowMs = 4.0;
constexpr seconds kUnsteadyAfter{20};
constexpr seconds kUnsteadyShown{10};
constexpr seconds kUnsteadyQuiet{1800};

} // namespace

void StreamAlerts::reset(Clock::time_point now) {
    *this = StreamAlerts{};
    started_ = now;
}

void StreamAlerts::raise(StreamAlert alert, Clock::time_point until) {
    Slot& slot = slots_[static_cast<int>(alert)];
    if (!slot.on) raised_.push_back(alert);
    slot.on = true;
    slot.until = std::max(slot.until, until);
}

void StreamAlerts::update(const StreamObservation& o, Clock::time_point now) {
    raised_.clear();
    for (Slot& slot : slots_) {
        if (slot.on && now >= slot.until) slot.on = false;
    }

    // The controller matters from the first moment on.
    if (o.controllerConnected || !o.controllerWanted) {
        controllerGone_ = {};
        slots_[static_cast<int>(StreamAlert::NoController)].on = false;
    } else {
        if (controllerGone_ == Clock::time_point{}) controllerGone_ = now;
        // Shown for as long as there is none.
        if (now - controllerGone_ >= kControllerPatience) raise(StreamAlert::NoController, now + kHold);
    }

    // A hiccup of the graphics card counts at any time, the first seconds too.
    if (restoredAt_ != Clock::time_point{}) {
        raise(StreamAlert::GraphicsRestored, restoredAt_ + kRestoredShown);
        restoredAt_ = {};
    }

    // So does its battery. On the cable or above the mark nothing is owed, and the next low is news again.
    if (!o.controllerConnected || o.batteryPercent < 0 || o.charging || o.batteryPercent > kBatteryLow) {
        batteryToldAt_ = 101;
    } else {
        const int mark = o.batteryPercent <= kBatteryVeryLow ? kBatteryVeryLow : kBatteryLow;
        if (mark < batteryToldAt_) {
            raise(StreamAlert::BatteryLow, now + kBatteryShown);
            batteryToldAt_ = mark;
        }
    }

    if (!primed_) {
        primed_ = true;
        framesLost_ = o.framesLost;
        framesDropped_ = o.framesDropped;
    }
    const std::uint64_t lost = o.framesLost > framesLost_ ? o.framesLost - framesLost_ : 0;
    const std::uint64_t dropped = o.framesDropped > framesDropped_ ? o.framesDropped - framesDropped_ : 0;
    framesLost_ = o.framesLost;
    framesDropped_ = o.framesDropped;
    if (now - started_ < kGrace) return;

    // Uneven pictures for a while: the console's Wi-Fi, most likely (VeloPlay cannot see its band).
    if (o.pictureLatenessMs < kSteadyBelowMs) {
        unsteadySince_ = {};
    } else if (o.pictureLatenessMs >= kUnsteadyFromMs) {
        if (unsteadySince_ == Clock::time_point{}) unsteadySince_ = now;
        if (now - unsteadySince_ >= kUnsteadyAfter && now >= unsteadyQuietUntil_) {
            raise(StreamAlert::UnsteadyLink, now + kUnsteadyShown);
            unsteadyQuietUntil_ = now + kUnsteadyQuiet;
        }
    }

    const bool stalled = o.sincePicture >= kStall;
    if (stalled) raise(StreamAlert::ConnectionStalls, now + kHold);

    for (std::uint64_t i = 0; i < std::min<std::uint64_t>(lost, kLossesThatShow); ++i) losses_.push_back(now);
    std::erase_if(losses_, [&](Clock::time_point at) { return now - at > kLossWindow; });
    if (static_cast<int>(losses_.size()) >= kLossesThatShow) raise(StreamAlert::PicturesLost, now + kHoldLong);

    if (dropped > 0) raise(StreamAlert::DecoderBehind, now + kHoldLong);

    // Fewer pictures with nothing lost and nothing standing: the console
    // itself sends less, because of the game or because it is busy.
    const bool fewer = o.requestedFps > 0 && o.streamFps > 1.0 && o.streamFps < o.requestedFps * kFewerBelow &&
                       !stalled && losses_.empty();
    if (!fewer) {
        fewerSince_ = {};
    } else {
        if (fewerSince_ == Clock::time_point{}) fewerSince_ = now;
        if (now - fewerSince_ >= kFewerAfter && now >= fewerQuietUntil_) {
            raise(StreamAlert::FewerPictures, now + kFewerShown);
            fewerQuietUntil_ = now + kFewerQuiet;
        }
    }
}

std::vector<StreamAlert> StreamAlerts::active(Clock::time_point now) const {
    std::vector<StreamAlert> result;
    for (int i = 0; i < kKinds; ++i) {
        if (slots_[i].on && now < slots_[i].until) result.push_back(static_cast<StreamAlert>(i));
    }
    return result;
}

std::string_view alertKey(StreamAlert alert) {
    switch (alert) {
        case StreamAlert::NoController: return "no_controller";
        case StreamAlert::ConnectionStalls: return "connection_stalls";
        case StreamAlert::PicturesLost: return "pictures_lost";
        case StreamAlert::DecoderBehind: return "decoder_behind";
        case StreamAlert::FewerPictures: return "fewer_pictures";
        case StreamAlert::BatteryLow: return "battery_low";
        case StreamAlert::GraphicsRestored: return "graphics_restored";
        case StreamAlert::UnsteadyLink: return "unsteady_link";
    }
    return "unknown";
}

bool alertShown(const NoteSettings& settings, StreamAlert alert) {
    if (!settings.enabled) return false;
    return std::find(settings.hidden.begin(), settings.hidden.end(), alertKey(alert)) == settings.hidden.end();
}

} // namespace veloplay

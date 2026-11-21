#pragma once

#include <chrono>
#include <mutex>
#include <vector>

#include "input/ControllerState.hpp"

namespace veloplay::input {

/// Input that comes from scripts instead of a physical controller. Everything
/// set here expires on its own, so a script that crashes cannot leave a button
/// held down. Thread-safe.
class VirtualController {
public:
    using Clock = std::chrono::steady_clock;

    /// Replaces the held state. It stays valid for `ttl`; a script that wants
    /// to hold a stick keeps sending it.
    void setState(const ControllerState& state, Clock::duration ttl, Clock::time_point now);

    /// Presses buttons for a fixed time, on top of the held state.
    void press(std::uint32_t buttons, Clock::duration duration, Clock::time_point now);

    void releaseAll();

    /// The combined scripted input at `now`.
    [[nodiscard]] ControllerState state(Clock::time_point now);

private:
    struct TimedPress {
        std::uint32_t buttons;
        Clock::time_point until;
    };

    std::mutex mutex_;
    ControllerState held_{};
    Clock::time_point heldUntil_{};
    std::vector<TimedPress> presses_;
};

} // namespace veloplay::input

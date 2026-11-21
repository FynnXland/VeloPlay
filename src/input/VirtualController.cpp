#include "input/VirtualController.hpp"

#include <algorithm>

namespace veloplay::input {

void VirtualController::setState(const ControllerState& state, Clock::duration ttl, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    held_ = state;
    heldUntil_ = now + ttl;
}

void VirtualController::press(std::uint32_t buttons, Clock::duration duration, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    presses_.push_back({buttons, now + duration});
}

void VirtualController::releaseAll() {
    std::lock_guard lock(mutex_);
    held_ = {};
    heldUntil_ = {};
    presses_.clear();
}

ControllerState VirtualController::state(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    if (now >= heldUntil_) held_ = {};
    std::erase_if(presses_, [&](const TimedPress& press) { return now >= press.until; });

    ControllerState result = held_;
    for (const TimedPress& press : presses_) result.buttons |= press.buttons;
    return result;
}

} // namespace veloplay::input

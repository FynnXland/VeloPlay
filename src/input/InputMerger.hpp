#pragma once

#include "input/ControllerState.hpp"

namespace veloplay::input {

struct StickPosition {
    std::int16_t x = 0;
    std::int16_t y = 0;

    friend bool operator==(const StickPosition&, const StickPosition&) = default;
};

/// Radial dead zone. Inside the zone the stick reads as centered; outside, the
/// remaining travel is stretched back to the full range so there is no jump at
/// the edge. `deadzone` is a fraction of full deflection (0.0 .. 0.9).
StickPosition applyDeadzone(StickPosition stick, float deadzone);

ControllerState applyDeadzoneToSticks(const ControllerState& state, float deadzone);

/// Combines the physical controller with scripted input from the remote
/// control API. The person holding the controller always wins:
///  - buttons are combined, so a script can add presses but never block one
///  - sticks, triggers, touches and motion come from the physical controller
///    whenever it is not neutral, otherwise from the script
ControllerState mergeInputs(const ControllerState& physical, const ControllerState& scripted);

} // namespace veloplay::input

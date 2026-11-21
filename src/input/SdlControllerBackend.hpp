#pragma once

#include <memory>

#include "input/IControllerBackend.hpp"

namespace veloplay::input {

/// Controllers through SDL3. SDL talks to the DualSense directly over HID,
/// which gives touchpad, motion sensors and rumble over USB and Bluetooth, and
/// it covers DualShock 4, Xbox and generic pads as well.
///
/// Returns nullptr if SDL's gamepad subsystem cannot start.
std::unique_ptr<IControllerBackend> createSdlControllerBackend();

} // namespace veloplay::input

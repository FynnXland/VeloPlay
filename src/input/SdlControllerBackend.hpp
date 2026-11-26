#pragma once

#include <memory>

#include "input/IControllerBackend.hpp"

namespace veloplay::input {

/// Controllers through SDL3. SDL talks to the DualSense directly over HID,
/// which gives touchpad, motion sensors and rumble over USB and Bluetooth, and
/// it covers DualShock 4, Xbox and generic pads as well.
///
/// Returns nullptr if SDL's gamepad subsystem cannot start.
///
/// With `fetchesEvents` false the backend leaves SDL's event queue alone: a
/// program whose window lives in SDL too fetches every event itself and hands
/// those about controllers coming and going to handleSdlControllerEvent().
std::unique_ptr<IControllerBackend> createSdlControllerBackend(bool fetchesEvents = true);

/// One SDL_Event (passed as a pointer, so this header needs no SDL) for a backend made with
/// `fetchesEvents` false. Events that are not about controllers are ignored.
void handleSdlControllerEvent(IControllerBackend& backend, const void* sdlEvent);

} // namespace veloplay::input

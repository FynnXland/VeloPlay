#include "input/SdlControllerBackend.hpp"

#include <algorithm>
#include <cmath>
#include <map>

// The client has its own WinMain; SDL only handles controllers here.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <spdlog/spdlog.h>

namespace veloplay::input {

namespace {

ControllerType toType(SDL_GamepadType type, Uint16 vendor, Uint16 product) {
    constexpr Uint16 kSonyVendor = 0x054c;
    constexpr Uint16 kDualSenseEdge = 0x0df2;
    switch (type) {
        case SDL_GAMEPAD_TYPE_PS5:
            return (vendor == kSonyVendor && product == kDualSenseEdge) ? ControllerType::DualSenseEdge
                                                                         : ControllerType::DualSense;
        case SDL_GAMEPAD_TYPE_PS4: return ControllerType::DualShock4;
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE: return ControllerType::Xbox;
        case SDL_GAMEPAD_TYPE_UNKNOWN: return ControllerType::Unknown;
        default: return ControllerType::Other;
    }
}

ConnectionType toConnection(SDL_JoystickConnectionState state) {
    switch (state) {
        case SDL_JOYSTICK_CONNECTION_WIRED: return ConnectionType::Usb;
        case SDL_JOYSTICK_CONNECTION_WIRELESS: return ConnectionType::Bluetooth;
        default: return ConnectionType::Unknown;
    }
}

std::uint8_t toTrigger(Sint16 value) {
    // SDL reports triggers as 0..32767.
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(value), 0, 32767) * 255 / 32767);
}

class SdlControllerBackend final : public IControllerBackend {
public:
    ~SdlControllerBackend() override {
        for (auto& [id, pad] : pads_) SDL_CloseGamepad(pad.handle);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }

    void poll() override {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) open(event.gdevice.which);
            if (event.type == SDL_EVENT_GAMEPAD_REMOVED) close(event.gdevice.which);
        }
    }

    std::vector<ControllerInfo> controllers() const override {
        std::vector<ControllerInfo> result;
        for (const auto& [id, pad] : pads_) result.push_back(pad.info);
        return result;
    }

    std::optional<ControllerState> read(std::uint32_t id) override {
        const auto it = pads_.find(id);
        if (it == pads_.end()) return std::nullopt;
        SDL_Gamepad* pad = it->second.handle;

        ControllerState state;
        const auto map = [&](SDL_GamepadButton button, Button target) {
            if (SDL_GetGamepadButton(pad, button)) state.buttons |= mask(target);
        };
        map(SDL_GAMEPAD_BUTTON_SOUTH, Button::Cross);
        map(SDL_GAMEPAD_BUTTON_EAST, Button::Circle);
        map(SDL_GAMEPAD_BUTTON_WEST, Button::Square);
        map(SDL_GAMEPAD_BUTTON_NORTH, Button::Triangle);
        map(SDL_GAMEPAD_BUTTON_DPAD_LEFT, Button::DpadLeft);
        map(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Button::DpadRight);
        map(SDL_GAMEPAD_BUTTON_DPAD_UP, Button::DpadUp);
        map(SDL_GAMEPAD_BUTTON_DPAD_DOWN, Button::DpadDown);
        map(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Button::L1);
        map(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Button::R1);
        map(SDL_GAMEPAD_BUTTON_LEFT_STICK, Button::L3);
        map(SDL_GAMEPAD_BUTTON_RIGHT_STICK, Button::R3);
        map(SDL_GAMEPAD_BUTTON_START, Button::Options);
        map(SDL_GAMEPAD_BUTTON_BACK, Button::Create);
        map(SDL_GAMEPAD_BUTTON_GUIDE, Button::Ps);
        map(SDL_GAMEPAD_BUTTON_TOUCHPAD, Button::Touchpad);
        map(SDL_GAMEPAD_BUTTON_MISC1, Button::Mute);

        state.leftX = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        state.leftY = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        state.rightX = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
        state.rightY = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
        state.l2 = toTrigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        state.r2 = toTrigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));

        if (it->second.info.hasTouchpad) {
            const int fingers = std::min<int>(SDL_GetNumGamepadTouchpadFingers(pad, 0), static_cast<int>(state.touches.size()));
            for (int finger = 0; finger < fingers; ++finger) {
                bool down = false;
                float x = 0.0f, y = 0.0f, pressure = 0.0f;
                if (!SDL_GetGamepadTouchpadFinger(pad, 0, finger, &down, &x, &y, &pressure) || !down) continue;
                TouchPoint& touch = state.touches[static_cast<std::size_t>(finger)];
                touch.active = true;
                touch.id = static_cast<std::uint8_t>(finger);
                touch.x = static_cast<std::uint16_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * (kTouchpadWidth - 1)));
                touch.y = static_cast<std::uint16_t>(std::lround(std::clamp(y, 0.0f, 1.0f) * (kTouchpadHeight - 1)));
            }
        }

        if (it->second.info.hasMotion) {
            const bool gyro = SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, state.motion.gyro.data(), 3);
            const bool accel = SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, state.motion.accel.data(), 3);
            state.motion.valid = gyro && accel;
        }
        return state;
    }

    bool rumble(std::uint32_t id, std::uint16_t low, std::uint16_t high, std::chrono::milliseconds duration) override {
        const auto it = pads_.find(id);
        if (it == pads_.end()) return false;
        return SDL_RumbleGamepad(it->second.handle, low, high, static_cast<Uint32>(duration.count()));
    }

    bool sendEffect(std::uint32_t id, std::span<const std::uint8_t> block) override {
        const auto it = pads_.find(id);
        if (it == pads_.end()) return false;
        return SDL_SendGamepadEffect(it->second.handle, block.data(), static_cast<int>(block.size()));
    }

    /// Controllers that were plugged in before the program started.
    void openConnected() {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count; ++i) open(ids[i]);
        SDL_free(ids);
    }

private:
    struct Pad {
        SDL_Gamepad* handle = nullptr;
        ControllerInfo info;
    };

    void open(SDL_JoystickID id) {
        if (pads_.contains(id)) return;
        SDL_Gamepad* handle = SDL_OpenGamepad(id);
        if (!handle) {
            spdlog::warn("Could not open controller {}: {}", id, SDL_GetError());
            return;
        }

        Pad pad;
        pad.handle = handle;
        pad.info.id = id;
        const char* name = SDL_GetGamepadName(handle);
        pad.info.name = name ? name : "Controller";
        pad.info.type = toType(SDL_GetGamepadType(handle), SDL_GetGamepadVendor(handle), SDL_GetGamepadProduct(handle));
        pad.info.connection = toConnection(SDL_GetGamepadConnectionState(handle));
        pad.info.hasTouchpad = SDL_GetNumGamepadTouchpads(handle) > 0;
        pad.info.hasRumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(handle), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);

        // Sensors are off by default to save power; a PS5 game may need them.
        const bool gyro = SDL_GamepadHasSensor(handle, SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(handle, SDL_SENSOR_GYRO, true);
        const bool accel = SDL_GamepadHasSensor(handle, SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(handle, SDL_SENSOR_ACCEL, true);
        pad.info.hasMotion = gyro && accel;

        pads_.emplace(id, std::move(pad));
    }

    void close(SDL_JoystickID id) {
        const auto it = pads_.find(id);
        if (it == pads_.end()) return;
        SDL_CloseGamepad(it->second.handle);
        pads_.erase(it);
    }

    std::map<std::uint32_t, Pad> pads_;
};

} // namespace

std::unique_ptr<IControllerBackend> createSdlControllerBackend() {
    // Keep reading the controller while the window is in the background, and
    // use SDL's own DualSense driver so touchpad, sensors and rumble work.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");

    SDL_SetMainReady();
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        spdlog::error("SDL gamepad support could not start: {}", SDL_GetError());
        return nullptr;
    }
    auto backend = std::make_unique<SdlControllerBackend>();
    backend->openConnected();
    return backend;
}

} // namespace veloplay::input

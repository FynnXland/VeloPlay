#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// The controller model used everywhere inside the client. It follows the
// DualSense, because that is what the console expects, but any gamepad maps
// onto it. Conversion to the console's wire format happens in the session code.

namespace veloplay::input {

enum class Button : std::uint32_t {
    Cross = 1u << 0,
    Circle = 1u << 1,
    Square = 1u << 2,
    Triangle = 1u << 3,
    DpadLeft = 1u << 4,
    DpadRight = 1u << 5,
    DpadUp = 1u << 6,
    DpadDown = 1u << 7,
    L1 = 1u << 8,
    R1 = 1u << 9,
    L3 = 1u << 10,
    R3 = 1u << 11,
    Options = 1u << 12,
    Create = 1u << 13,
    Touchpad = 1u << 14,
    Ps = 1u << 15,
    Mute = 1u << 16,
};

inline constexpr std::array kAllButtons = {
    Button::Cross,   Button::Circle,   Button::Square, Button::Triangle, Button::DpadLeft, Button::DpadRight,
    Button::DpadUp,  Button::DpadDown, Button::L1,     Button::R1,       Button::L3,       Button::R3,
    Button::Options, Button::Create,   Button::Touchpad, Button::Ps,     Button::Mute,
};

constexpr std::uint32_t mask(Button button) { return static_cast<std::uint32_t>(button); }

/// Lowercase identifier used in the remote control API ("cross", "dpad_up", ...).
std::string_view buttonName(Button button);
std::optional<Button> parseButton(std::string_view name);

inline constexpr std::uint16_t kTouchpadWidth = 1920;
inline constexpr std::uint16_t kTouchpadHeight = 1080;

struct TouchPoint {
    bool active = false;
    std::uint8_t id = 0;
    std::uint16_t x = 0; // 0 .. kTouchpadWidth - 1
    std::uint16_t y = 0; // 0 .. kTouchpadHeight - 1

    friend bool operator==(const TouchPoint&, const TouchPoint&) = default;
};

struct MotionState {
    bool valid = false;
    std::array<float, 3> gyro{};  // rad/s
    std::array<float, 3> accel{}; // m/s², includes gravity

    friend bool operator==(const MotionState&, const MotionState&) = default;
};

struct ControllerState {
    std::uint32_t buttons = 0; // bitmask of Button
    std::uint8_t l2 = 0;       // 0 = released, 255 = fully pressed
    std::uint8_t r2 = 0;
    std::int16_t leftX = 0;    // right is positive
    std::int16_t leftY = 0;    // down is positive
    std::int16_t rightX = 0;
    std::int16_t rightY = 0;
    std::array<TouchPoint, 2> touches{};
    MotionState motion{};

    [[nodiscard]] bool pressed(Button button) const { return (buttons & mask(button)) != 0; }
    void set(Button button, bool down) {
        if (down) buttons |= mask(button);
        else buttons &= ~mask(button);
    }

    friend bool operator==(const ControllerState&, const ControllerState&) = default;
};

} // namespace veloplay::input

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "input/ControllerState.hpp"

namespace veloplay::input {

/// A joystick that is no gamepad, as the system reports it: a steering
/// wheel with pedals, a flight stick. Axes run from -32768 to 32767.
struct RawJoystick {
    std::vector<std::int16_t> axes;
    std::uint64_t buttons = 0; // one bit per button, the first 64
    std::uint8_t hat = 0;      // first hat: 1 up, 2 right, 4 down, 8 left
};

struct JoystickInfo {
    std::uint32_t id = 0;
    std::string name;
    bool force = false; // it can push back (force feedback)

    friend bool operator==(const JoystickInfo&, const JoystickInfo&) = default;
};

/// What a wheel can do for the console, which only knows a DualSense: the
/// wheel turns the left stick, the pedals press R2 and L2, its buttons
/// press buttons. The console gets no wheel; to the game it is a controller.
enum class WheelTarget : int {
    Steer,
    Gas,
    Brake,
    Cross,
    Circle,
    Square,
    Triangle,
    L1,
    R1,
    L3,
    R3,
    Options,
    Create,
    Touchpad,
    Ps,
    Count
};
inline constexpr int kWheelButtonTargets = static_cast<int>(WheelTarget::Count) - static_cast<int>(WheelTarget::Cross);

struct PedalAxis {
    int axis = -1;
    int released = 0; // the axis's value with the foot off
    int pressed = 0;  // and pressed all the way

    friend bool operator==(const PedalAxis&, const PedalAxis&) = default;
};

struct WheelSettings {
    bool enabled = false;
    std::string device; // the wheel's name, to find it again when it is plugged in later
    int steerAxis = -1;
    int steerCenter = 0;     // the axis's value with the wheel straight
    float steering = 1.0f;   // 1: full lock is full stick; 2: half the turn already is
    float deadzone = 0.02f;  // around the centre, as part of full lock
    PedalAxis gas;
    PedalAxis brake;
    std::array<int, kWheelButtonTargets> buttons{}; // the wheel's button for each of Cross..Ps, -1 none
    float centering = 0.3f;  // how hard the wheel pulls back to the middle while streaming, 0..1

    WheelSettings() { buttons.fill(-1); }
    friend bool operator==(const WheelSettings&, const WheelSettings&) = default;
};

/// The controller state a wheel stands for. Unassigned parts stay neutral.
[[nodiscard]] ControllerState mapWheel(const WheelSettings& settings, const RawJoystick& raw);

/// Finds out which axis or button of a wheel is meant, by watching the user
/// move it: turn the wheel, press a pedal all the way and let go, press a
/// button. Axis numbers differ from wheel to wheel and are never guessed.
class WheelCapture {
public:
    struct Result {
        WheelTarget target = WheelTarget::Steer;
        int index = -1;   // axis or button
        int rest = 0;     // axes: the value at rest (centre, pedal released)
        int extreme = 0;  // pedals: the value pressed all the way
    };

    void start(WheelTarget target, const RawJoystick& atRest);
    void cancel() { active_ = false; }
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] WheelTarget target() const { return target_; }
    /// The next reading; a result once the movement is clear.
    std::optional<Result> feed(const RawJoystick& now);

private:
    bool active_ = false;
    WheelTarget target_ = WheelTarget::Steer;
    RawJoystick rest_;
    int axis_ = -1;   // pedal: the axis found, while waiting for the foot to come off
    int extreme_ = 0;
};

} // namespace veloplay::input

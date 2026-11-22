#include "input/Wheel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace veloplay::input {

namespace {

// How far an axis has to move before it counts as the one meant: more than a
// wobble of the other axes, less than any wheel's or pedal's full travel.
constexpr int kAxisMoved = 12'000;
constexpr int kPedalMoved = 16'000;

constexpr std::array<Button, kWheelButtonTargets> kButtons = {
    Button::Cross, Button::Circle, Button::Square, Button::Triangle, Button::L1,       Button::R1,
    Button::L3,    Button::R3,     Button::Options, Button::Create,  Button::Touchpad, Button::Ps};

int axisValue(const RawJoystick& raw, int axis) {
    if (axis < 0 || axis >= static_cast<int>(raw.axes.size())) return 0;
    return raw.axes[static_cast<std::size_t>(axis)];
}

bool button(const RawJoystick& raw, int index) {
    return index >= 0 && index < 64 && ((raw.buttons >> index) & 1u) != 0;
}

std::uint8_t pedal(const PedalAxis& axis, const RawJoystick& raw) {
    if (axis.axis < 0 || axis.pressed == axis.released) return 0;
    const float t = static_cast<float>(axisValue(raw, axis.axis) - axis.released) /
                    static_cast<float>(axis.pressed - axis.released);
    // A little at the start is the foot resting on the pedal.
    const float pressed = std::clamp((t - 0.03f) / 0.97f, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(pressed * 255.0f));
}

} // namespace

ControllerState mapWheel(const WheelSettings& settings, const RawJoystick& raw) {
    ControllerState state;
    if (settings.steerAxis >= 0) {
        const float span = 32767.0f;
        float turn = static_cast<float>(axisValue(raw, settings.steerAxis) - settings.steerCenter) / span;
        turn = std::clamp(turn * std::max(settings.steering, 0.1f), -1.0f, 1.0f);
        const float dead = std::clamp(settings.deadzone, 0.0f, 0.5f);
        const float magnitude = std::abs(turn);
        turn = magnitude <= dead ? 0.0f : std::copysign((magnitude - dead) / (1.0f - dead), turn);
        state.leftX = static_cast<std::int16_t>(std::lround(turn * 32767.0f));
    }
    state.r2 = pedal(settings.gas, raw);
    state.l2 = pedal(settings.brake, raw);
    for (int i = 0; i < kWheelButtonTargets; ++i) {
        if (button(raw, settings.buttons[static_cast<std::size_t>(i)])) state.set(kButtons[static_cast<std::size_t>(i)], true);
    }
    // The hat is the D-pad on every wheel that has one.
    state.set(Button::DpadUp, (raw.hat & 1u) != 0);
    state.set(Button::DpadRight, (raw.hat & 2u) != 0);
    state.set(Button::DpadDown, (raw.hat & 4u) != 0);
    state.set(Button::DpadLeft, (raw.hat & 8u) != 0);
    return state;
}

void WheelCapture::start(WheelTarget target, const RawJoystick& atRest) {
    active_ = true;
    target_ = target;
    rest_ = atRest;
    axis_ = -1;
    extreme_ = 0;
}

std::optional<WheelCapture::Result> WheelCapture::feed(const RawJoystick& now) {
    if (!active_) return std::nullopt;
    Result result;
    result.target = target_;
    if (target_ == WheelTarget::Steer) {
        int best = -1;
        int moved = kAxisMoved;
        for (int i = 0; i < static_cast<int>(now.axes.size()); ++i) {
            const int distance = std::abs(axisValue(now, i) - axisValue(rest_, i));
            if (distance >= moved) {
                moved = distance;
                best = i;
            }
        }
        if (best < 0) return std::nullopt;
        active_ = false;
        result.index = best;
        result.rest = axisValue(rest_, best);
        return result;
    }
    if (target_ == WheelTarget::Gas || target_ == WheelTarget::Brake) {
        if (axis_ < 0) {
            int moved = kPedalMoved;
            for (int i = 0; i < static_cast<int>(now.axes.size()); ++i) {
                const int distance = std::abs(axisValue(now, i) - axisValue(rest_, i));
                if (distance >= moved) {
                    moved = distance;
                    axis_ = i;
                }
            }
            if (axis_ < 0) return std::nullopt;
            extreme_ = axisValue(now, axis_);
        }
        const int rest = axisValue(rest_, axis_);
        const int value = axisValue(now, axis_);
        if (std::abs(value - rest) > std::abs(extreme_ - rest)) extreme_ = value;
        // Done once the foot comes off again: then the deepest point is known.
        if (std::abs(value - rest) * 10 > std::abs(extreme_ - rest) * 3) return std::nullopt;
        active_ = false;
        result.index = axis_;
        result.rest = rest;
        result.extreme = extreme_;
        return result;
    }
    // A button: the first one that is down now and was not at the start.
    const std::uint64_t pressed = now.buttons & ~rest_.buttons;
    if (pressed == 0) return std::nullopt;
    for (int i = 0; i < 64; ++i) {
        if ((pressed >> i) & 1u) {
            active_ = false;
            result.index = i;
            return result;
        }
    }
    return std::nullopt;
}

} // namespace veloplay::input

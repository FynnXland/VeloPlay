#include "input/InputMerger.hpp"

#include <algorithm>
#include <cmath>

namespace veloplay::input {

namespace {

constexpr float kStickMax = 32767.0f;

std::int16_t toAxis(float value) {
    return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * kStickMax));
}

} // namespace

StickPosition applyDeadzone(StickPosition stick, float deadzone) {
    deadzone = std::clamp(deadzone, 0.0f, 0.9f);
    const float x = std::max(static_cast<float>(stick.x), -kStickMax) / kStickMax;
    const float y = std::max(static_cast<float>(stick.y), -kStickMax) / kStickMax;
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= deadzone || magnitude == 0.0f) return {};

    const float scaled = std::min(1.0f, (magnitude - deadzone) / (1.0f - deadzone));
    const float factor = scaled / magnitude;
    return {toAxis(x * factor), toAxis(y * factor)};
}

ControllerState applyDeadzoneToSticks(const ControllerState& state, float deadzone) {
    ControllerState result = state;
    const StickPosition left = applyDeadzone(StickPosition{state.leftX, state.leftY}, deadzone);
    const StickPosition right = applyDeadzone(StickPosition{state.rightX, state.rightY}, deadzone);
    result.leftX = left.x;
    result.leftY = left.y;
    result.rightX = right.x;
    result.rightY = right.y;
    return result;
}

ControllerState mergeInputs(const ControllerState& physical, const ControllerState& scripted) {
    ControllerState merged = physical;
    merged.buttons = physical.buttons | scripted.buttons;

    if (physical.l2 == 0) merged.l2 = scripted.l2;
    if (physical.r2 == 0) merged.r2 = scripted.r2;

    if (physical.leftX == 0 && physical.leftY == 0) {
        merged.leftX = scripted.leftX;
        merged.leftY = scripted.leftY;
    }
    if (physical.rightX == 0 && physical.rightY == 0) {
        merged.rightX = scripted.rightX;
        merged.rightY = scripted.rightY;
    }

    for (std::size_t i = 0; i < merged.touches.size(); ++i) {
        if (!physical.touches[i].active) merged.touches[i] = scripted.touches[i];
    }
    if (!physical.motion.valid) merged.motion = scripted.motion;
    return merged;
}

} // namespace veloplay::input

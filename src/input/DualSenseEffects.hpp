#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace veloplay::input {

inline constexpr std::size_t kTriggerEffectParameters = 10;

/// Size of the effects block SDL's DualSense driver accepts through
/// SDL_SendGamepadEffect().
inline constexpr std::size_t kDualSenseEffectSize = 47;

/// Effect type that releases a trigger, with all parameters zero.
inline constexpr std::uint8_t kTriggerEffectOff = 0x05;

/// Builds the effects block that sets both adaptive triggers and leaves
/// everything else on the controller (rumble, lights, audio) untouched.
/// `left` and `right` are the ten parameter bytes the console sends per trigger.
std::array<std::uint8_t, kDualSenseEffectSize> buildTriggerEffect(std::uint8_t typeLeft,
                                                                  std::span<const std::uint8_t> left,
                                                                  std::uint8_t typeRight,
                                                                  std::span<const std::uint8_t> right);

} // namespace veloplay::input

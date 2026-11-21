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

/// How strong the console wants an effect of the controller to be. The user
/// sets it on the console (Accessories > Controllers); the console passes it
/// on, and the controller has to be told, or it keeps whatever strength the
/// program before left it at.
enum class EffectStrength { Off, Strong, Medium, Weak };

/// The controller's power setting for both kinds of effect in one byte: how
/// far the motors of the triggers (upper half) and of vibration and haptics
/// (lower half) are held back. Zero is full strength.
[[nodiscard]] std::uint8_t motorPower(EffectStrength haptics, EffectStrength triggers);

/// Builds the effects block that sets both adaptive triggers and leaves
/// everything else on the controller (rumble, lights, audio) untouched.
/// `left` and `right` are the ten parameter bytes the console sends per trigger.
/// `power` is motorPower(); it goes along with every block.
std::array<std::uint8_t, kDualSenseEffectSize> buildTriggerEffect(std::uint8_t typeLeft,
                                                                  std::span<const std::uint8_t> left,
                                                                  std::uint8_t typeRight,
                                                                  std::span<const std::uint8_t> right,
                                                                  std::uint8_t power = 0);

/// A block that sets only the strength of the effects and touches nothing else.
std::array<std::uint8_t, kDualSenseEffectSize> buildMotorPower(std::uint8_t power);

} // namespace veloplay::input

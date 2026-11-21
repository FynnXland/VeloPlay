#include "input/DualSenseEffects.hpp"

#include <algorithm>

namespace veloplay::input {

namespace {

// Layout of DS5EffectsState_t from SDL's testcontroller.c (zlib license),
// which is what SDL_SendGamepadEffect() expects for a DualSense:
//
//   0  enable bits 1      10  right trigger effect: type, 10 parameters
//   1  enable bits 2      21  left trigger effect:  type, 10 parameters
//   2  rumble right       32  six bytes; the fifth (36) is the motor power
//   3  rumble left        38  enable bits 3
//   4..9 audio settings   39..46 lights
//
// The motor power byte, the bit that makes the controller take it, and the
// values per strength are those of chiaki-ng (gui/src/controllermanager.cpp
// and streamsession.cpp), which sends them with every effects block.
constexpr std::size_t kEnableBits1 = 0;
constexpr std::size_t kEnableBits2 = 1;
constexpr std::size_t kMotorPower = 36;
constexpr std::uint8_t kEnableMotorPower = 0x40;
constexpr std::size_t kRightTrigger = 10;
constexpr std::size_t kLeftTrigger = 21;
constexpr std::uint8_t kEnableRightTrigger = 0x04;
constexpr std::uint8_t kEnableLeftTrigger = 0x08;

} // namespace

std::uint8_t motorPower(EffectStrength haptics, EffectStrength triggers) {
    std::uint8_t power = 0;
    switch (haptics) {
        case EffectStrength::Strong: break;
        case EffectStrength::Medium: power |= 0x02; break;
        case EffectStrength::Weak: power |= 0x03; break;
        case EffectStrength::Off: power |= 0x0F; break;
    }
    switch (triggers) {
        case EffectStrength::Strong: break;
        case EffectStrength::Medium: power |= 0x60; break;
        case EffectStrength::Weak: power |= 0x90; break;
        case EffectStrength::Off: power |= 0xF0; break;
    }
    return power;
}

std::array<std::uint8_t, kDualSenseEffectSize> buildMotorPower(std::uint8_t power) {
    std::array<std::uint8_t, kDualSenseEffectSize> state{};
    state[kEnableBits2] = kEnableMotorPower;
    state[kMotorPower] = power;
    return state;
}

std::array<std::uint8_t, kDualSenseEffectSize> buildTriggerEffect(std::uint8_t typeLeft,
                                                                  std::span<const std::uint8_t> left,
                                                                  std::uint8_t typeRight,
                                                                  std::span<const std::uint8_t> right,
                                                                  std::uint8_t power) {
    std::array<std::uint8_t, kDualSenseEffectSize> state = buildMotorPower(power);
    state[kEnableBits1] = kEnableRightTrigger | kEnableLeftTrigger;
    state[kRightTrigger] = typeRight;
    std::copy_n(right.begin(), std::min(right.size(), kTriggerEffectParameters), state.begin() + kRightTrigger + 1);
    state[kLeftTrigger] = typeLeft;
    std::copy_n(left.begin(), std::min(left.size(), kTriggerEffectParameters), state.begin() + kLeftTrigger + 1);
    return state;
}

namespace {

/// Zones `position` to 9 switched on, each with `value` in three bits. Shared by resistance and shaking.
void fillZones(TriggerEffect& effect, int position, int value) {
    std::uint16_t active = 0;
    std::uint32_t levels = 0;
    for (int zone = position; zone < 10; ++zone) {
        active = static_cast<std::uint16_t>(active | (1u << zone));
        levels |= static_cast<std::uint32_t>(value & 0x07) << (3 * zone);
    }
    effect.parameters[0] = static_cast<std::uint8_t>(active & 0xff);
    effect.parameters[1] = static_cast<std::uint8_t>(active >> 8);
    effect.parameters[2] = static_cast<std::uint8_t>(levels & 0xff);
    effect.parameters[3] = static_cast<std::uint8_t>((levels >> 8) & 0xff);
    effect.parameters[4] = static_cast<std::uint8_t>((levels >> 16) & 0xff);
    effect.parameters[5] = static_cast<std::uint8_t>((levels >> 24) & 0xff);
}

} // namespace

TriggerEffect feedbackEffect(int position, int strength) {
    TriggerEffect effect;
    if (position < 0 || position > 9 || strength < 1 || strength > 8) return effect; // released
    effect.type = 0x21;
    fillZones(effect, position, strength - 1);
    return effect;
}

TriggerEffect weaponEffect(int start, int end, int strength) {
    TriggerEffect effect;
    if (start < 2 || start > 7 || end <= start || end > 8 || strength < 1 || strength > 8) return effect;
    effect.type = 0x25;
    const auto zones = static_cast<std::uint16_t>((1u << start) | (1u << end));
    effect.parameters[0] = static_cast<std::uint8_t>(zones & 0xff);
    effect.parameters[1] = static_cast<std::uint8_t>(zones >> 8);
    effect.parameters[2] = static_cast<std::uint8_t>(strength - 1);
    return effect;
}

TriggerEffect vibrationEffect(int position, int amplitude, int frequency) {
    TriggerEffect effect;
    if (position < 0 || position > 9 || amplitude < 1 || amplitude > 8 || frequency < 1 || frequency > 255) return effect;
    effect.type = 0x26;
    fillZones(effect, position, amplitude - 1);
    effect.parameters[8] = static_cast<std::uint8_t>(frequency);
    return effect;
}

} // namespace veloplay::input

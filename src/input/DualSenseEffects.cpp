#include "input/DualSenseEffects.hpp"

#include <algorithm>

namespace veloplay::input {

namespace {

// Layout of DS5EffectsState_t from SDL's testcontroller.c (zlib license),
// which is what SDL_SendGamepadEffect() expects for a DualSense:
//
//   0  enable bits 1      10  right trigger effect: type, 10 parameters
//   1  enable bits 2      21  left trigger effect:  type, 10 parameters
//   2  rumble right       32  six bytes not used here
//   3  rumble left        38  enable bits 3
//   4..9 audio settings   39..46 lights
constexpr std::size_t kEnableBits1 = 0;
constexpr std::size_t kRightTrigger = 10;
constexpr std::size_t kLeftTrigger = 21;
constexpr std::uint8_t kEnableRightTrigger = 0x04;
constexpr std::uint8_t kEnableLeftTrigger = 0x08;

} // namespace

std::array<std::uint8_t, kDualSenseEffectSize> buildTriggerEffect(std::uint8_t typeLeft,
                                                                  std::span<const std::uint8_t> left,
                                                                  std::uint8_t typeRight,
                                                                  std::span<const std::uint8_t> right) {
    std::array<std::uint8_t, kDualSenseEffectSize> state{};
    state[kEnableBits1] = kEnableRightTrigger | kEnableLeftTrigger;
    state[kRightTrigger] = typeRight;
    std::copy_n(right.begin(), std::min(right.size(), kTriggerEffectParameters), state.begin() + kRightTrigger + 1);
    state[kLeftTrigger] = typeLeft;
    std::copy_n(left.begin(), std::min(left.size(), kTriggerEffectParameters), state.begin() + kLeftTrigger + 1);
    return state;
}

} // namespace veloplay::input

#include <gtest/gtest.h>

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include "input/KeyboardMouse.hpp"
#include "linux/KeyMap.hpp"

using namespace veloplay;

TEST(LinuxKeys, GiveTheCodesTheLayoutsUse) {
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_W), 'W');
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_A), 'A');
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_0), '0');
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_1), '1');
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_9), '9');
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_SPACE), input::key::Space);
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_LSHIFT), input::key::Shift);
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_RSHIFT), input::key::Shift);
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_RCTRL), input::key::Ctrl);
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_TAB), input::key::Tab);
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_ESCAPE), input::key::Escape);
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_F1), input::key::F1);
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_F11), input::key::F11);
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_KP_0), 0x60);
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_KP_9), 0x69);
    EXPECT_EQ(linuxos::keyCodeFor(SDL_SCANCODE_UNKNOWN), 0);
    // The keys VeloPlay keeps for itself are the same ones as on Windows.
    EXPECT_TRUE(input::isReservedKey(linuxos::keyCodeFor(SDL_SCANCODE_ESCAPE)));
    EXPECT_TRUE(input::isReservedKey(linuxos::keyCodeFor(SDL_SCANCODE_F11)));
    EXPECT_FALSE(input::isReservedKey(linuxos::keyCodeFor(SDL_SCANCODE_W)));
}

TEST(LinuxKeys, GiveMouseButtonsTheirCodes) {
    EXPECT_EQ(linuxos::mouseButtonCode(SDL_BUTTON_LEFT), input::key::MouseLeft);
    EXPECT_EQ(linuxos::mouseButtonCode(SDL_BUTTON_RIGHT), input::key::MouseRight);
    EXPECT_EQ(linuxos::mouseButtonCode(SDL_BUTTON_MIDDLE), input::key::MouseMiddle);
    EXPECT_EQ(linuxos::mouseButtonCode(SDL_BUTTON_X1), input::key::MouseBack);
    EXPECT_EQ(linuxos::mouseButtonCode(SDL_BUTTON_X2), input::key::MouseForward);
    EXPECT_EQ(linuxos::mouseButtonCode(99), 0);
}

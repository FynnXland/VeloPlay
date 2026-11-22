#include <gtest/gtest.h>

#include "input/Wheel.hpp"

using namespace veloplay::input;

namespace {

RawJoystick atRest() {
    RawJoystick raw;
    // Wheel straight, gas released at the top of its range, brake released at the bottom.
    raw.axes = {0, 32767, -32768, 0};
    return raw;
}

} // namespace

TEST(Wheel, FindsTheSteeringAxisByWatchingTheWheelTurn) {
    WheelCapture capture;
    capture.start(WheelTarget::Steer, atRest());
    RawJoystick now = atRest();
    now.axes[3] = 3000; // a little wobble elsewhere is not it
    now.axes[0] = 5000;
    EXPECT_FALSE(capture.feed(now));
    now.axes[0] = -20000;
    const auto found = capture.feed(now);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->index, 0);
    EXPECT_EQ(found->rest, 0);
    EXPECT_FALSE(capture.active());
}

TEST(Wheel, LearnsAPedalsWholeTravelWhateverWayItRuns) {
    WheelCapture capture;
    capture.start(WheelTarget::Gas, atRest());
    RawJoystick now = atRest();
    // This pedal runs from 32767 released down to -32768 pressed.
    for (const int value : {20000, -10000, -32000, -31000}) {
        now.axes[1] = static_cast<std::int16_t>(value);
        EXPECT_FALSE(capture.feed(now)) << value;
    }
    now.axes[1] = 30000; // foot off
    const auto found = capture.feed(now);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->index, 1);
    EXPECT_EQ(found->rest, 32767);
    EXPECT_EQ(found->extreme, -32000);
}

TEST(Wheel, TakesTheButtonThatWentDown) {
    WheelCapture capture;
    RawJoystick rest = atRest();
    rest.buttons = 1u << 2; // held from before: not this one
    capture.start(WheelTarget::Cross, rest);
    RawJoystick now = rest;
    EXPECT_FALSE(capture.feed(now));
    now.buttons |= 1u << 7;
    const auto found = capture.feed(now);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->index, 7);
}

TEST(Wheel, TurnsTheWheelIntoAController) {
    WheelSettings settings;
    settings.steerAxis = 0;
    settings.gas = {1, 32767, -32768};
    settings.brake = {2, -32768, 32767};
    settings.buttons[0] = 7; // Cross
    settings.buttons[3] = 9; // Triangle

    RawJoystick raw = atRest();
    ControllerState state = mapWheel(settings, raw);
    EXPECT_EQ(state.leftX, 0);
    EXPECT_EQ(state.r2, 0);
    EXPECT_EQ(state.l2, 0);

    raw.axes[0] = 32767;  // full lock right
    raw.axes[1] = -32768; // gas all the way
    raw.axes[2] = 0;      // brake half
    raw.buttons = 1u << 7;
    raw.hat = 8; // left
    state = mapWheel(settings, raw);
    EXPECT_EQ(state.leftX, 32767);
    EXPECT_EQ(state.r2, 255);
    EXPECT_NEAR(state.l2, 124, 3);
    EXPECT_TRUE(state.pressed(Button::Cross));
    EXPECT_FALSE(state.pressed(Button::Triangle));
    EXPECT_TRUE(state.pressed(Button::DpadLeft));

    // More steering: half the turn is already full stick. Small movements around the middle are nothing.
    settings.steering = 2.0f;
    raw.axes[0] = 16384;
    EXPECT_EQ(mapWheel(settings, raw).leftX, 32767);
    raw.axes[0] = 300;
    EXPECT_EQ(mapWheel(settings, raw).leftX, 0);
    // Nothing assigned: no button pressed, whatever the wheel's buttons do.
    raw.hat = 0;
    EXPECT_EQ(mapWheel(WheelSettings{}, raw).buttons, 0u);
}

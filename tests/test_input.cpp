#include <gtest/gtest.h>

#include <cmath>
#include <map>

#include "input/DualSenseEffects.hpp"
#include "input/ControllerManager.hpp"
#include "input/InputMerger.hpp"
#include "input/VirtualController.hpp"

using namespace veloplay::input;
using namespace std::chrono_literals;

TEST(Buttons, NamesRoundTripAndAreUnique) {
    std::uint32_t seen = 0;
    for (Button button : kAllButtons) {
        EXPECT_EQ(parseButton(buttonName(button)), button);
        EXPECT_EQ(seen & mask(button), 0u);
        seen |= mask(button);
    }
    EXPECT_FALSE(parseButton("x"));
    EXPECT_FALSE(parseButton("Cross")); // names are lowercase
}

TEST(ControllerState, SetAndQueryButtons) {
    ControllerState state;
    state.set(Button::Cross, true);
    state.set(Button::Ps, true);
    EXPECT_TRUE(state.pressed(Button::Cross));
    EXPECT_FALSE(state.pressed(Button::Circle));
    state.set(Button::Cross, false);
    EXPECT_EQ(state.buttons, mask(Button::Ps));
}

TEST(Deadzone, CentersSmallDeflection) {
    EXPECT_EQ(applyDeadzone({2000, -1500}, 0.10f), (StickPosition{0, 0}));
    EXPECT_EQ(applyDeadzone({0, 0}, 0.10f), (StickPosition{0, 0}));
}

TEST(Deadzone, KeepsFullDeflection) {
    EXPECT_EQ(applyDeadzone({32767, 0}, 0.10f), (StickPosition{32767, 0}));
    EXPECT_EQ(applyDeadzone({0, -32768}, 0.10f), (StickPosition{0, -32767}));
}

TEST(Deadzone, RescalesWithoutJumpAtTheEdge) {
    // Just outside a 10 % zone the output starts near zero, not at 10 %.
    const StickPosition justOutside = applyDeadzone({3600, 0}, 0.10f);
    EXPECT_GT(justOutside.x, 0);
    EXPECT_LT(justOutside.x, 600);

    // Halfway between the zone edge and full deflection maps to half.
    const StickPosition half = applyDeadzone({static_cast<std::int16_t>(32767 * 0.55f), 0}, 0.10f);
    EXPECT_NEAR(half.x, 32767 / 2, 40);
}

TEST(Deadzone, IsRadialAndKeepsDirection) {
    const StickPosition diagonal = applyDeadzone({12000, 12000}, 0.20f);
    EXPECT_EQ(diagonal.x, diagonal.y);
    EXPECT_GT(diagonal.x, 0);
    // Each axis alone is inside a 40 % square zone, but the radial distance is not.
    EXPECT_NE(applyDeadzone({12000, 12000}, 0.40f), (StickPosition{0, 0}));
    EXPECT_EQ(applyDeadzone({12000, 0}, 0.40f), (StickPosition{0, 0}));
}

TEST(Deadzone, ZeroDisablesIt) {
    EXPECT_EQ(applyDeadzone({100, -100}, 0.0f), (StickPosition{100, -100}));
}

TEST(MergeInputs, ScriptAddsButtonsButCannotRemoveThem) {
    ControllerState physical;
    physical.set(Button::Cross, true);
    ControllerState scripted;
    scripted.set(Button::Ps, true);

    const ControllerState merged = mergeInputs(physical, scripted);
    EXPECT_TRUE(merged.pressed(Button::Cross));
    EXPECT_TRUE(merged.pressed(Button::Ps));
}

TEST(MergeInputs, PhysicalAnalogInputWins) {
    ControllerState physical;
    physical.leftX = 9000;
    physical.r2 = 40;
    ControllerState scripted;
    scripted.leftX = -32767;
    scripted.leftY = 32767;
    scripted.rightX = 1234;
    scripted.l2 = 255;
    scripted.r2 = 255;

    const ControllerState merged = mergeInputs(physical, scripted);
    EXPECT_EQ(merged.leftX, 9000); // the hand on the stick decides
    EXPECT_EQ(merged.leftY, 0);
    EXPECT_EQ(merged.rightX, 1234); // untouched stick comes from the script
    EXPECT_EQ(merged.l2, 255);
    EXPECT_EQ(merged.r2, 40);
}

TEST(MergeInputs, TouchAndMotionFallBackToScript) {
    ControllerState physical;
    physical.touches[0] = {true, 1, 100, 200};
    ControllerState scripted;
    scripted.touches[0] = {true, 7, 5, 5};
    scripted.touches[1] = {true, 8, 6, 6};
    scripted.motion.valid = true;
    scripted.motion.gyro = {1.0f, 2.0f, 3.0f};

    const ControllerState merged = mergeInputs(physical, scripted);
    EXPECT_EQ(merged.touches[0].id, 1);
    EXPECT_EQ(merged.touches[1].id, 8);
    EXPECT_TRUE(merged.motion.valid);

    physical.motion.valid = true;
    EXPECT_EQ(mergeInputs(physical, scripted).motion.gyro[0], 0.0f);
}

TEST(VirtualController, HeldStateExpires) {
    VirtualController controller;
    const VirtualController::Clock::time_point t0{};
    ControllerState state;
    state.set(Button::Cross, true);
    state.leftX = 5000;

    controller.setState(state, 250ms, t0);
    EXPECT_EQ(controller.state(t0 + 249ms), state);
    EXPECT_EQ(controller.state(t0 + 250ms), ControllerState{});
}

TEST(VirtualController, TimedPressesOverlapAndEndIndependently) {
    VirtualController controller;
    const VirtualController::Clock::time_point t0{};
    controller.press(mask(Button::Cross), 100ms, t0);
    controller.press(mask(Button::Ps), 300ms, t0 + 50ms);

    EXPECT_EQ(controller.state(t0 + 60ms).buttons, mask(Button::Cross) | mask(Button::Ps));
    EXPECT_EQ(controller.state(t0 + 100ms).buttons, mask(Button::Ps));
    EXPECT_EQ(controller.state(t0 + 350ms).buttons, 0u);
}

TEST(VirtualController, ReleaseAllClearsEverything) {
    VirtualController controller;
    const VirtualController::Clock::time_point t0{};
    ControllerState state;
    state.r2 = 200;
    controller.setState(state, 5s, t0);
    controller.press(mask(Button::Circle), 5s, t0);
    controller.releaseAll();
    EXPECT_EQ(controller.state(t0 + 1ms), ControllerState{});
}

namespace {

/// Backend driven by the test: controllers appear and vanish on command.
class FakeBackend final : public IControllerBackend {
public:
    struct Shared {
        std::vector<ControllerInfo> controllers;
        std::map<std::uint32_t, ControllerState> states;
        int rumbles = 0;
        std::uint32_t rumbledId = 0;
        std::uint32_t effectId = 0;
        std::vector<std::vector<std::uint8_t>> effects;
    };

    explicit FakeBackend(Shared& shared) : shared_(shared) {}

    void poll() override {}
    std::vector<ControllerInfo> controllers() const override { return shared_.controllers; }
    std::optional<ControllerState> read(std::uint32_t id) override {
        const auto it = shared_.states.find(id);
        if (it == shared_.states.end()) return std::nullopt;
        return it->second;
    }
    bool rumble(std::uint32_t id, std::uint16_t, std::uint16_t, std::chrono::milliseconds) override {
        ++shared_.rumbles;
        shared_.rumbledId = id;
        return true;
    }
    bool sendEffect(std::uint32_t id, std::span<const std::uint8_t> block) override {
        shared_.effectId = id;
        shared_.effects.emplace_back(block.begin(), block.end());
        return true;
    }

private:
    Shared& shared_;
};

ControllerInfo makeInfo(std::uint32_t id, std::string name, ControllerType type, ConnectionType connection) {
    ControllerInfo info;
    info.id = id;
    info.name = std::move(name);
    info.type = type;
    info.connection = connection;
    return info;
}

} // namespace

TEST(ControllerRank, PrefersDualSenseAndCable) {
    const auto dualSenseUsb = makeInfo(1, "DualSense", ControllerType::DualSense, ConnectionType::Usb);
    const auto dualSenseBt = makeInfo(2, "DualSense", ControllerType::DualSense, ConnectionType::Bluetooth);
    const auto dualShock = makeInfo(3, "DualShock 4", ControllerType::DualShock4, ConnectionType::Usb);
    const auto xbox = makeInfo(4, "Xbox", ControllerType::Xbox, ConnectionType::Usb);
    EXPECT_GT(controllerRank(dualSenseUsb), controllerRank(dualSenseBt));
    EXPECT_GT(controllerRank(dualSenseBt), controllerRank(dualShock));
    EXPECT_GT(controllerRank(dualShock), controllerRank(xbox));
}

TEST(ControllerManager, WorksWithoutBackend) {
    ControllerManager manager(nullptr);
    manager.update();
    EXPECT_FALSE(manager.active());
    EXPECT_EQ(manager.state(), ControllerState{});
    EXPECT_FALSE(manager.rumble(1, 1, 10ms));
}

TEST(ControllerManager, ReportsHotPlugAndPicksBestController) {
    FakeBackend::Shared shared;
    ControllerManager manager(std::make_unique<FakeBackend>(shared));

    manager.update();
    EXPECT_FALSE(manager.active());
    EXPECT_TRUE(manager.takeEvents().empty());

    shared.controllers = {makeInfo(1, "Xbox Controller", ControllerType::Xbox, ConnectionType::Usb),
                          makeInfo(2, "DualSense", ControllerType::DualSense, ConnectionType::Usb)};
    manager.update();
    auto events = manager.takeEvents();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].kind, ControllerManager::Event::Kind::Connected);
    ASSERT_TRUE(manager.active());
    EXPECT_EQ(manager.active()->id, 2u);

    shared.controllers.pop_back(); // DualSense unplugged
    manager.update();
    events = manager.takeEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, ControllerManager::Event::Kind::Disconnected);
    EXPECT_EQ(events[0].info.name, "DualSense");
    EXPECT_EQ(manager.active()->id, 1u); // falls back to what is left

    shared.controllers.clear();
    manager.update();
    EXPECT_FALSE(manager.active());
    EXPECT_EQ(manager.state(), ControllerState{});
}

TEST(ControllerManager, KeepsActiveControllerWhenABetterOneAppears) {
    FakeBackend::Shared shared;
    ControllerManager manager(std::make_unique<FakeBackend>(shared));
    shared.controllers = {makeInfo(1, "Xbox Controller", ControllerType::Xbox, ConnectionType::Usb)};
    manager.update();
    shared.controllers.push_back(makeInfo(2, "DualSense", ControllerType::DualSense, ConnectionType::Usb));
    manager.update();
    EXPECT_EQ(manager.active()->id, 1u);
}

TEST(ControllerManager, HonoursPreferredController) {
    FakeBackend::Shared shared;
    ControllerManager manager(std::make_unique<FakeBackend>(shared));
    shared.controllers = {makeInfo(1, "Xbox Controller", ControllerType::Xbox, ConnectionType::Usb),
                          makeInfo(2, "DualSense", ControllerType::DualSense, ConnectionType::Usb)};
    manager.update();
    EXPECT_EQ(manager.active()->id, 2u);

    manager.setPreferredName("Xbox Controller");
    manager.update();
    EXPECT_EQ(manager.active()->id, 1u);

    manager.setPreferredName(""); // back to automatic: stays put, no surprise switch
    manager.update();
    EXPECT_EQ(manager.active()->id, 1u);
}

TEST(ControllerManager, AppliesDeadzoneAndForwardsRumble) {
    FakeBackend::Shared shared;
    ControllerManager manager(std::make_unique<FakeBackend>(shared));
    shared.controllers = {makeInfo(7, "DualSense", ControllerType::DualSense, ConnectionType::Usb)};
    ControllerState raw;
    raw.leftX = 1500; // stick drift
    raw.rightX = 32767;
    raw.set(Button::Cross, true);
    shared.states[7] = raw;

    manager.setDeadzone(0.10f);
    manager.update();
    EXPECT_EQ(manager.state().leftX, 0);
    EXPECT_EQ(manager.state().rightX, 32767);
    EXPECT_TRUE(manager.state().pressed(Button::Cross));

    EXPECT_TRUE(manager.rumble(1000, 2000, 100ms));
    EXPECT_EQ(shared.rumbles, 1);
    EXPECT_EQ(shared.rumbledId, 7u);
}

TEST(DualSenseEffects, PlacesBothTriggersInTheEffectsBlock) {
    const std::array<std::uint8_t, 10> left = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const std::array<std::uint8_t, 10> right = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    const auto block = buildTriggerEffect(0x21, left, 0x26, right);

    ASSERT_EQ(block.size(), 47u);
    EXPECT_EQ(block[0], 0x0c); // change both triggers, nothing else
    EXPECT_EQ(block[1], 0x00);
    EXPECT_EQ(block[2], 0x00); // rumble stays as it is
    EXPECT_EQ(block[3], 0x00);
    EXPECT_EQ(block[10], 0x26); // right trigger: type, then its parameters
    EXPECT_EQ(block[11], 11);
    EXPECT_EQ(block[20], 20);
    EXPECT_EQ(block[21], 0x21); // left trigger
    EXPECT_EQ(block[22], 1);
    EXPECT_EQ(block[31], 10);
    for (std::size_t i = 32; i < block.size(); ++i) EXPECT_EQ(block[i], 0) << i;
}

TEST(ControllerManager, SendsTriggerEffectsToADualSenseOnly) {
    FakeBackend::Shared shared;
    ControllerManager manager(std::make_unique<FakeBackend>(shared));
    const std::array<std::uint8_t, 10> parameters = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0};

    EXPECT_FALSE(manager.triggerEffects(0x21, parameters, 0x21, parameters)); // no controller

    shared.controllers = {makeInfo(3, "Xbox", ControllerType::Xbox, ConnectionType::Usb)};
    manager.update();
    EXPECT_FALSE(manager.triggerEffects(0x21, parameters, 0x21, parameters));
    EXPECT_TRUE(shared.effects.empty());

    shared.controllers = {makeInfo(7, "DualSense", ControllerType::DualSense, ConnectionType::Usb)};
    manager.update();
    EXPECT_TRUE(manager.triggerEffects(0x21, parameters, 0x02, parameters));
    ASSERT_EQ(shared.effects.size(), 1u);
    EXPECT_EQ(shared.effectId, 7u);
    EXPECT_EQ(shared.effects[0][21], 0x21);
    EXPECT_EQ(shared.effects[0][10], 0x02);

    // Releasing the triggers is effect 0x05 with empty parameters.
    EXPECT_TRUE(manager.clearTriggerEffects());
    ASSERT_EQ(shared.effects.size(), 2u);
    EXPECT_EQ(shared.effects[1][10], kTriggerEffectOff);
    EXPECT_EQ(shared.effects[1][21], kTriggerEffectOff);
    EXPECT_EQ(shared.effects[1][11], 0);
}

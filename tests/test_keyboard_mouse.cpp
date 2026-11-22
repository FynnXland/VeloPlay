#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <set>

#include "input/KeyboardMouse.hpp"

using namespace veloplay::input;
using namespace std::chrono_literals;

namespace {

using Clock = KeyboardMouseMapper::Clock;

KeyboardMouseSettings enabled() {
    KeyboardMouseSettings settings;
    settings.enabled = true;
    return settings;
}

/// Moves the mouse evenly for a while, read as often as the program reads it.
ControllerState moveFor(KeyboardMouseMapper& mapper, Clock::time_point& now, std::chrono::milliseconds duration,
                        double countsPerSecondX, double countsPerSecondY, int reportsPerSecond = 1000) {
    ControllerState state;
    double owedX = 0.0;
    double owedY = 0.0;
    const auto step = 2ms;
    const int reportEvery = 1000 / reportsPerSecond; // in milliseconds
    int elapsed = 0;
    for (auto t = 0ms; t < duration; t += step, elapsed += 2) {
        owedX += countsPerSecondX * 0.002;
        owedY += countsPerSecondY * 0.002;
        if (elapsed % reportEvery == 0) {
            const int dx = static_cast<int>(owedX);
            const int dy = static_cast<int>(owedY);
            mapper.mouseMoved(dx, dy);
            owedX -= dx;
            owedY -= dy;
        }
        now += step;
        state = mapper.state(now);
    }
    return state;
}

} // namespace

TEST(KeyboardMouse, KeepsTheProgramsOwnKeysAwayFromTheGame) {
    // Menu, upscaling, display, full screen: these work while playing and never reach the console.
    for (const int code : {key::Escape, key::F1, key::F8, key::F9, key::F10, key::F11, key::F12}) EXPECT_TRUE(isReservedKey(code)) << code;
    EXPECT_FALSE(isReservedKey('W'));
    // A layout that names one of them loses that entry instead of swallowing the key.
    KeyboardMouseSettings settings;
    settings.keys = defaultKeyBindings();
    settings.keys[static_cast<std::size_t>(PadControl::Cross)] = key::F8;
    sanitize(settings);
    EXPECT_EQ(settings.keys[static_cast<std::size_t>(PadControl::Cross)], key::None);
}

TEST(KeyboardMouse, EveryPresetUsesEachKeyOnce) {
    for (const auto& keys : {presetKeyBindings(KeyPreset::Standard), presetKeyBindings(KeyPreset::OpenWorld), firstKeyBindings()}) {
        std::set<int> seen;
        for (const int code : keys) {
            if (code == key::None) continue;
            EXPECT_TRUE(seen.insert(code).second) << "key " << code << " stands for two controls";
            EXPECT_FALSE(isReservedKey(code));
        }
        // Walking is on W A S D, shooting and aiming on the mouse buttons.
        EXPECT_EQ(keys[static_cast<std::size_t>(PadControl::LeftStickUp)], 'W');
        EXPECT_EQ(keys[static_cast<std::size_t>(PadControl::R2)], key::MouseLeft);
        EXPECT_EQ(keys[static_cast<std::size_t>(PadControl::L2)], key::MouseRight);
    }
}

TEST(KeyboardMouse, StartsWithKeysWherePcGamesHaveThem) {
    const auto at = [](const std::array<int, kPadControlCount>& keys, PadControl control) {
        return keys[static_cast<std::size_t>(control)];
    };
    // Space jumps and jumping is Cross; Shift runs and running is a press on the left stick.
    const auto standard = defaultKeyBindings();
    EXPECT_EQ(standard, presetKeyBindings(KeyPreset::Standard));
    EXPECT_EQ(at(standard, PadControl::Cross), key::Space);
    EXPECT_EQ(at(standard, PadControl::L3), key::Shift);
    EXPECT_EQ(at(standard, PadControl::Circle), key::Ctrl);
    EXPECT_EQ(at(standard, PadControl::Square), 'R');
    // The other preset keeps those and moves what a game with walking and driving is known for.
    const auto openWorld = presetKeyBindings(KeyPreset::OpenWorld);
    EXPECT_EQ(at(openWorld, PadControl::Cross), key::Space);
    EXPECT_EQ(at(openWorld, PadControl::L1), key::Tab);
    EXPECT_EQ(at(openWorld, PadControl::Options), 'P');

    // A layout says which preset it is, until one key changes.
    EXPECT_EQ(presetOf(standard), KeyPreset::Standard);
    EXPECT_EQ(presetOf(openWorld), KeyPreset::OpenWorld);
    auto changed = standard;
    changed[static_cast<std::size_t>(PadControl::Triangle)] = 'G';
    EXPECT_FALSE(presetOf(changed).has_value());
    EXPECT_FALSE(presetOf(firstKeyBindings()).has_value());
}

TEST(KeyboardMouse, DoesNothingWhileSwitchedOff) {
    KeyboardMouseMapper mapper;
    mapper.configure(KeyboardMouseSettings{});
    mapper.key('W', true);
    mapper.key(key::MouseLeft, true);
    mapper.mouseMoved(500, 0);
    Clock::time_point now{1s};
    EXPECT_EQ(mapper.state(now), ControllerState{});
}

TEST(KeyboardMouse, KeysPressButtonsAndTriggers) {
    KeyboardMouseMapper mapper;
    mapper.configure(enabled());
    Clock::time_point now{1s};
    mapper.key(key::Space, true);
    mapper.key(key::MouseLeft, true);
    ControllerState state = mapper.state(now);
    EXPECT_TRUE(state.pressed(Button::Cross));
    EXPECT_FALSE(state.pressed(Button::Square));
    EXPECT_EQ(state.r2, 255);
    EXPECT_EQ(state.l2, 0);

    mapper.key(key::Space, false);
    mapper.key(key::MouseLeft, false);
    mapper.key(key::MouseRight, true);
    state = mapper.state(now);
    EXPECT_FALSE(state.pressed(Button::Cross));
    EXPECT_EQ(state.r2, 0);
    EXPECT_EQ(state.l2, 255);

    // A key nothing is bound to changes nothing.
    mapper.key('Z', true);
    mapper.key(key::MouseRight, false);
    EXPECT_EQ(mapper.state(now), ControllerState{});
}

TEST(KeyboardMouse, FourKeysMakeAStick) {
    KeyboardMouseMapper mapper;
    mapper.configure(enabled());
    Clock::time_point now{1s};
    mapper.key('W', true);
    ControllerState state = mapper.state(now);
    EXPECT_EQ(state.leftX, 0);
    EXPECT_EQ(state.leftY, -32767); // up is negative

    // Forward and right: between the two, and no further out than one alone.
    mapper.key('D', true);
    state = mapper.state(now);
    EXPECT_NEAR(state.leftX, 23170, 2);
    EXPECT_NEAR(state.leftY, -23170, 2);
    EXPECT_NEAR(std::hypot(state.leftX, state.leftY), 32767.0, 3.0);

    // Opposite keys cancel out.
    mapper.key('A', true);
    state = mapper.state(now);
    EXPECT_EQ(state.leftX, 0);
    EXPECT_EQ(state.leftY, -32767);
}

TEST(KeyboardMouse, TheSpeedOfTheMouseIsHowFarTheStickIsPushed) {
    KeyboardMouseMapper mapper;
    mapper.configure(enabled());
    Clock::time_point now{1s};
    (void)mapper.state(now);

    // A slow, steady move to the right.
    const ControllerState slow = moveFor(mapper, now, 300ms, 600.0, 0.0);
    EXPECT_GT(slow.rightX, 6000);
    EXPECT_LT(slow.rightX, 16000);
    EXPECT_EQ(slow.rightY, 0);
    // Twice as fast pushes clearly further.
    const ControllerState faster = moveFor(mapper, now, 300ms, 1200.0, 0.0);
    EXPECT_GT(faster.rightX, slow.rightX + 5000);
    // A flick: all the way, and not beyond.
    const ControllerState flick = moveFor(mapper, now, 200ms, 9000.0, 0.0);
    EXPECT_EQ(flick.rightX, 32767);

    // The hand stops: so does the stick, within a few pictures.
    const ControllerState stopped = moveFor(mapper, now, 120ms, 0.0, 0.0);
    EXPECT_EQ(stopped.rightX, 0);
    EXPECT_EQ(stopped.rightY, 0);

    // Up is up: the mouse away from the player pushes the stick up, which is negative.
    const ControllerState up = moveFor(mapper, now, 300ms, 0.0, -800.0);
    EXPECT_LT(up.rightY, -6000);
    EXPECT_EQ(up.rightX, 0);
}

TEST(KeyboardMouse, StaysSteadyWithAMouseThatReportsRarely) {
    // Office mice report 125 times a second, the state is read 500 times a
    // second. The stick must not flicker between pushed and centred.
    KeyboardMouseMapper mapper;
    mapper.configure(enabled());
    Clock::time_point now{1s};
    (void)mapper.state(now);
    (void)moveFor(mapper, now, 200ms, 1000.0, 0.0, 125);
    int lowest = 32767;
    int highest = 0;
    for (int i = 0; i < 100; ++i) {
        const ControllerState state = moveFor(mapper, now, 2ms, 1000.0, 0.0, 125);
        lowest = std::min<int>(lowest, state.rightX);
        highest = std::max<int>(highest, state.rightX);
    }
    EXPECT_GT(lowest, 8000);
    EXPECT_LT(highest - lowest, 9000);
}

TEST(KeyboardMouse, FollowsSensitivityAndTheInvertedAxis) {
    KeyboardMouseSettings settings = enabled();
    KeyboardMouseMapper normal;
    normal.configure(settings);
    settings.mouseSensitivity = 2.0f;
    settings.invertY = true;
    KeyboardMouseMapper changed;
    changed.configure(settings);

    Clock::time_point a{1s};
    Clock::time_point b{1s};
    (void)normal.state(a);
    (void)changed.state(b);
    const ControllerState plain = moveFor(normal, a, 300ms, 500.0, 500.0);
    const ControllerState other = moveFor(changed, b, 300ms, 500.0, 500.0);
    EXPECT_GT(other.rightX, plain.rightX + 3000);
    EXPECT_GT(plain.rightY, 0);
    EXPECT_LT(other.rightY, 0);
}

TEST(KeyboardMouse, AWheelNotchIsAShortPress) {
    KeyboardMouseSettings settings = enabled();
    settings.keys[static_cast<std::size_t>(PadControl::DpadRight)] = key::WheelUp;
    settings.keys[static_cast<std::size_t>(PadControl::DpadLeft)] = key::WheelDown;
    KeyboardMouseMapper mapper;
    mapper.configure(settings);
    Clock::time_point now{1s};
    mapper.wheel(1, now);
    EXPECT_TRUE(mapper.state(now).pressed(Button::DpadRight));
    EXPECT_FALSE(mapper.state(now).pressed(Button::DpadLeft));
    EXPECT_TRUE(mapper.state(now + 50ms).pressed(Button::DpadRight));
    EXPECT_FALSE(mapper.state(now + 100ms).pressed(Button::DpadRight));
    mapper.wheel(-2, now + 200ms);
    EXPECT_TRUE(mapper.state(now + 210ms).pressed(Button::DpadLeft));
}

TEST(KeyboardMouse, LetsGoOfEverythingWhenTheWindowLosesTheKeyboard) {
    KeyboardMouseMapper mapper;
    mapper.configure(enabled());
    Clock::time_point now{1s};
    (void)mapper.state(now);
    mapper.key('W', true);
    mapper.key(key::MouseLeft, true);
    (void)moveFor(mapper, now, 100ms, 2000.0, 0.0);
    mapper.releaseAll();
    now += 2ms;
    EXPECT_EQ(mapper.state(now), ControllerState{});
}

TEST(KeyboardMouse, SettingsKeepOneControlPerKey) {
    KeyboardMouseSettings settings = enabled();
    settings.mouseSensitivity = 40.0f;
    settings.keys[static_cast<std::size_t>(PadControl::Circle)] = 'W';          // already walking forward
    settings.keys[static_cast<std::size_t>(PadControl::Triangle)] = key::Escape; // VeloPlay's own menu
    settings.keys[static_cast<std::size_t>(PadControl::Cross)] = -5;
    sanitize(settings);
    EXPECT_FLOAT_EQ(settings.mouseSensitivity, 4.0f);
    EXPECT_EQ(settings.keys[static_cast<std::size_t>(PadControl::Triangle)], key::None);
    EXPECT_EQ(settings.keys[static_cast<std::size_t>(PadControl::Cross)], key::None);
    // Circle comes before the left stick in the list, so it keeps the key and walking loses it.
    EXPECT_EQ(settings.keys[static_cast<std::size_t>(PadControl::Circle)], 'W');
    EXPECT_EQ(settings.keys[static_cast<std::size_t>(PadControl::LeftStickUp)], key::None);
}

TEST(KeyboardMouse, AGameCanHaveALayoutOfItsOwn) {
    KeyboardMouseSettings settings = enabled();
    GameKeyLayout racing;
    racing.titleId = "PPSA00001";
    racing.name = "A racing game";
    racing.keys = settings.keys;
    racing.keys[static_cast<std::size_t>(PadControl::LeftStickUp)] = key::None;
    racing.keys[static_cast<std::size_t>(PadControl::R2)] = 'W'; // accelerate where walking was
    racing.mouseSensitivity = 2.5f;
    racing.invertY = true;
    settings.games.push_back(racing);

    // While that game runs its layout applies, whole: keys, sensitivity, direction.
    const KeyboardMouseSettings inGame = settingsFor(settings, "PPSA00001");
    EXPECT_TRUE(inGame.enabled);
    EXPECT_EQ(inGame.keys, racing.keys);
    EXPECT_FLOAT_EQ(inGame.mouseSensitivity, 2.5f);
    EXPECT_TRUE(inGame.invertY);
    KeyboardMouseMapper mapper;
    mapper.configure(inGame);
    mapper.key('W', true);
    const ControllerState state = mapper.state(KeyboardMouseMapper::Clock::time_point{});
    EXPECT_EQ(state.r2, 255);
    EXPECT_EQ(state.leftY, 0);

    // Any other game, and no game at all, get the general layout.
    EXPECT_EQ(settingsFor(settings, "PPSA99999").keys, settings.keys);
    EXPECT_EQ(settingsFor(settings, "").keys, settings.keys);
    EXPECT_EQ(gameLayout(settings, ""), nullptr);
    ASSERT_NE(gameLayout(settings, "PPSA00001"), nullptr);
}

TEST(KeyboardMouse, TidiesTheLayoutsOfGames) {
    KeyboardMouseSettings settings = enabled();
    GameKeyLayout one;
    one.titleId = "PPSA00001";
    one.mouseSensitivity = 99.0f;
    one.keys[static_cast<std::size_t>(PadControl::Cross)] = key::F1; // the program's own key
    GameKeyLayout again = one;
    again.name = "second entry for the same game";
    GameKeyLayout nameless; // no title id: belongs to no game
    settings.games = {one, again, nameless};
    sanitize(settings);
    ASSERT_EQ(settings.games.size(), 1u);
    EXPECT_TRUE(settings.games[0].name.empty());
    EXPECT_FLOAT_EQ(settings.games[0].mouseSensitivity, 4.0f);
    EXPECT_EQ(settings.games[0].keys[static_cast<std::size_t>(PadControl::Cross)], key::None);
}

#include "input/KeyboardMouse.hpp"

#include <utility>

#include <algorithm>
#include <cmath>

namespace veloplay::input {

namespace {

constexpr std::array<std::string_view, kPadControlCount> kNames = {
    "cross",         "circle",          "square",          "triangle",         "dpad_up",        "dpad_down",
    "dpad_left",     "dpad_right",      "l1",              "r1",               "l2",             "r2",
    "l3",            "r3",              "options",         "create",           "touchpad",       "ps",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right", "right_stick_up", "right_stick_down",
    "right_stick_left", "right_stick_right",
};

// A mouse moved at this many counts per second pushes the stick all the way
// at sensitivity 1. A mouse with 800 counts per inch gets there at about
// three inches a second, a steady turn of the wrist.
constexpr double kFullSpeed = 2400.0;
// Mice report 125 to 1000 times a second, the state is read 500 times a
// second: without smoothing the stick would flicker between "moving" and
// "not moving" from one read to the next.
constexpr double kSmoothing = 0.016; // seconds
// Games ignore a stick near its centre, for thumbs that never rest quite
// still. The smallest movement of the mouse starts just outside of that.
constexpr double kPastTheDeadZone = 0.12;
constexpr double kStandsStill = 0.004;
// How long a notch of the wheel holds its control down.
constexpr std::chrono::milliseconds kTap{70};

std::int16_t axis(double value) {
    return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0, 1.0) * 32767.0));
}

std::size_t index(PadControl control) { return static_cast<std::size_t>(control); }

} // namespace

std::string_view padControlName(PadControl control) {
    const std::size_t at = index(control);
    return at < kNames.size() ? kNames[at] : std::string_view{};
}

std::optional<PadControl> parsePadControl(std::string_view name) {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (kNames[i] == name) return static_cast<PadControl>(i);
    }
    return std::nullopt;
}

bool isReservedKey(int code) {
    return code == key::Escape || code == key::F1 || code == key::F8 || code == key::F9 || code == key::F10 || code == key::F11 ||
           code == key::F12;
}

std::array<int, kPadControlCount> presetKeyBindings(KeyPreset preset) {
    std::array<int, kPadControlCount> keys{};
    const auto set = [&](PadControl control, int code) { keys[index(control)] = code; };
    // Walking and looking.
    set(PadControl::LeftStickUp, 'W');
    set(PadControl::LeftStickLeft, 'A');
    set(PadControl::LeftStickDown, 'S');
    set(PadControl::LeftStickRight, 'D');
    // Shooting and aiming, which is also accelerating and braking in a car.
    set(PadControl::R2, key::MouseLeft);
    set(PadControl::L2, key::MouseRight);
    // What nearly every game of the last years has on the same button.
    set(PadControl::Cross, key::Space); // jump, confirm; the handbrake in a car
    set(PadControl::L3, key::Shift);    // run
    set(PadControl::Circle, key::Ctrl); // duck, back
    set(PadControl::Square, 'R');       // reload
    set(PadControl::Triangle, 'F');     // use, get in and out
    set(PadControl::DpadUp, key::Up);
    set(PadControl::DpadDown, key::Down);
    set(PadControl::DpadLeft, key::Left);
    set(PadControl::DpadRight, key::Right);
    set(PadControl::Create, 'O');
    set(PadControl::Ps, key::Home);
    if (preset == KeyPreset::OpenWorld) {
        set(PadControl::L1, key::Tab);  // weapon wheel
        set(PadControl::R1, 'Q');       // take cover
        set(PadControl::R3, 'C');       // look back
        set(PadControl::Touchpad, 'M'); // map
        set(PadControl::Options, 'P');  // pause
    } else {
        set(PadControl::L1, 'Q');
        set(PadControl::R1, 'E');
        set(PadControl::R3, key::MouseMiddle);
        set(PadControl::Touchpad, key::Tab);
        set(PadControl::Options, key::Enter);
    }
    return keys;
}

std::optional<KeyPreset> presetOf(const std::array<int, kPadControlCount>& keys) {
    for (const KeyPreset preset : {KeyPreset::Standard, KeyPreset::OpenWorld}) {
        if (keys == presetKeyBindings(preset)) return preset;
    }
    return std::nullopt;
}

std::array<int, kPadControlCount> defaultKeyBindings() { return presetKeyBindings(KeyPreset::Standard); }

std::array<int, kPadControlCount> firstKeyBindings() {
    std::array<int, kPadControlCount> keys{};
    const auto set = [&](PadControl control, int code) { keys[index(control)] = code; };
    // Walking and looking.
    set(PadControl::LeftStickUp, 'W');
    set(PadControl::LeftStickLeft, 'A');
    set(PadControl::LeftStickDown, 'S');
    set(PadControl::LeftStickRight, 'D');
    // Shooting and aiming, which is also accelerating and braking in a car.
    set(PadControl::R2, key::MouseLeft);
    set(PadControl::L2, key::MouseRight);
    set(PadControl::R3, key::MouseMiddle);
    // The four buttons where the left hand finds them.
    set(PadControl::Cross, key::Shift);  // run
    set(PadControl::Square, key::Space); // jump
    set(PadControl::Triangle, 'F');      // get in, get out
    set(PadControl::Circle, 'R');        // reload
    set(PadControl::L1, key::Tab);       // weapon wheel
    set(PadControl::R1, 'Q');            // take cover
    set(PadControl::L3, key::Ctrl);      // duck
    set(PadControl::DpadUp, key::Up);
    set(PadControl::DpadDown, key::Down);
    set(PadControl::DpadLeft, key::Left);
    set(PadControl::DpadRight, key::Right);
    set(PadControl::Touchpad, 'V');      // change the view
    set(PadControl::Options, 'P');       // pause
    set(PadControl::Create, 'O');
    set(PadControl::Ps, key::Home);
    return keys;
}

namespace {

void sanitizeLayout(std::array<int, kPadControlCount>& keys, float& mouseSensitivity) {
    mouseSensitivity = std::clamp(mouseSensitivity, 0.2f, 4.0f);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        int& code = keys[i];
        if (code < 0 || code > key::WheelDown || isReservedKey(code)) code = key::None;
        // A key that stands for two things would press both. The first one keeps it.
        for (std::size_t earlier = 0; earlier < i && code != key::None; ++earlier) {
            if (keys[earlier] == code) code = key::None;
        }
    }
}

} // namespace

void sanitize(KeyboardMouseSettings& settings) {
    sanitizeLayout(settings.keys, settings.mouseSensitivity);
    // A layout without a game is no use, and a game has one layout: the first.
    std::vector<GameKeyLayout> kept;
    for (GameKeyLayout& game : settings.games) {
        if (game.titleId.empty() || kept.size() >= kMaxGameLayouts) continue;
        if (std::any_of(kept.begin(), kept.end(), [&](const GameKeyLayout& k) { return k.titleId == game.titleId; })) continue;
        sanitizeLayout(game.keys, game.mouseSensitivity);
        kept.push_back(std::move(game));
    }
    settings.games = std::move(kept);
}

const GameKeyLayout* gameLayout(const KeyboardMouseSettings& settings, std::string_view titleId) {
    if (titleId.empty()) return nullptr;
    for (const GameKeyLayout& game : settings.games) {
        if (game.titleId == titleId) return &game;
    }
    return nullptr;
}

GameKeyLayout* gameLayout(KeyboardMouseSettings& settings, std::string_view titleId) {
    return const_cast<GameKeyLayout*>(gameLayout(std::as_const(settings), titleId));
}

KeyboardMouseSettings settingsFor(const KeyboardMouseSettings& settings, std::string_view titleId) {
    KeyboardMouseSettings result = settings;
    if (const GameKeyLayout* game = gameLayout(settings, titleId)) {
        result.keys = game->keys;
        result.mouseSensitivity = game->mouseSensitivity;
        result.invertY = game->invertY;
    }
    return result;
}

void KeyboardMouseMapper::configure(const KeyboardMouseSettings& settings) {
    std::lock_guard lock(mutex_);
    if (settings == settings_) return;
    settings_ = settings;
    // Whatever was held belonged to the old layout.
    down_.fill(false);
    tappedUntil_.fill(Clock::time_point{});
    pendingX_ = pendingY_ = speedX_ = speedY_ = 0.0;
}

void KeyboardMouseMapper::key(int code, bool down) {
    if (code == key::None) return;
    std::lock_guard lock(mutex_);
    if (!settings_.enabled) return;
    for (std::size_t i = 0; i < settings_.keys.size(); ++i) {
        if (settings_.keys[i] == code) down_[i] = down;
    }
}

void KeyboardMouseMapper::wheel(int notches, Clock::time_point now) {
    if (notches == 0) return;
    const int code = notches > 0 ? key::WheelUp : key::WheelDown;
    std::lock_guard lock(mutex_);
    if (!settings_.enabled) return;
    for (std::size_t i = 0; i < settings_.keys.size(); ++i) {
        if (settings_.keys[i] == code) tappedUntil_[i] = now + kTap;
    }
}

void KeyboardMouseMapper::mouseMoved(int dx, int dy) {
    std::lock_guard lock(mutex_);
    if (!settings_.enabled) return;
    pendingX_ += dx;
    pendingY_ += dy;
}

void KeyboardMouseMapper::releaseAll() {
    std::lock_guard lock(mutex_);
    down_.fill(false);
    tappedUntil_.fill(Clock::time_point{});
    pendingX_ = pendingY_ = speedX_ = speedY_ = 0.0;
}

bool KeyboardMouseMapper::held(PadControl control, Clock::time_point now) const {
    return down_[index(control)] || now < tappedUntil_[index(control)];
}

ControllerState KeyboardMouseMapper::state(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    ControllerState state;
    if (!settings_.enabled) return state;

    const auto button = [&](PadControl control, Button which) { state.set(which, held(control, now)); };
    button(PadControl::Cross, Button::Cross);
    button(PadControl::Circle, Button::Circle);
    button(PadControl::Square, Button::Square);
    button(PadControl::Triangle, Button::Triangle);
    button(PadControl::DpadUp, Button::DpadUp);
    button(PadControl::DpadDown, Button::DpadDown);
    button(PadControl::DpadLeft, Button::DpadLeft);
    button(PadControl::DpadRight, Button::DpadRight);
    button(PadControl::L1, Button::L1);
    button(PadControl::R1, Button::R1);
    button(PadControl::L3, Button::L3);
    button(PadControl::R3, Button::R3);
    button(PadControl::Options, Button::Options);
    button(PadControl::Create, Button::Create);
    button(PadControl::Touchpad, Button::Touchpad);
    button(PadControl::Ps, Button::Ps);
    state.l2 = held(PadControl::L2, now) ? 255 : 0;
    state.r2 = held(PadControl::R2, now) ? 255 : 0;

    // Four keys make a stick. Two at once point between them, no further out than one alone.
    const auto stick = [&](PadControl up, PadControl down, PadControl left, PadControl right, double& x, double& y) {
        x = (held(right, now) ? 1.0 : 0.0) - (held(left, now) ? 1.0 : 0.0);
        y = (held(down, now) ? 1.0 : 0.0) - (held(up, now) ? 1.0 : 0.0);
        const double length = std::hypot(x, y);
        if (length > 1.0) {
            x /= length;
            y /= length;
        }
    };
    double leftX = 0.0, leftY = 0.0, rightX = 0.0, rightY = 0.0;
    stick(PadControl::LeftStickUp, PadControl::LeftStickDown, PadControl::LeftStickLeft, PadControl::LeftStickRight,
          leftX, leftY);
    stick(PadControl::RightStickUp, PadControl::RightStickDown, PadControl::RightStickLeft, PadControl::RightStickRight,
          rightX, rightY);

    // The mouse: how fast it moves is how far the right stick is pushed.
    double seconds = last_ == Clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - last_).count();
    last_ = now;
    if (seconds > 0.0) {
        // After a long pause the movement collected since then says nothing about speed.
        seconds = std::min(seconds, 0.05);
        const double blend = 1.0 - std::exp(-seconds / kSmoothing);
        speedX_ += (pendingX_ / seconds - speedX_) * blend;
        speedY_ += (pendingY_ / seconds - speedY_) * blend;
        pendingX_ = 0.0;
        pendingY_ = 0.0;
    }
    double mouseX = speedX_ * settings_.mouseSensitivity / kFullSpeed;
    double mouseY = speedY_ * settings_.mouseSensitivity / kFullSpeed * (settings_.invertY ? -1.0 : 1.0);
    const double push = std::hypot(mouseX, mouseY);
    if (push < kStandsStill) {
        mouseX = 0.0;
        mouseY = 0.0;
    } else {
        const double out = kPastTheDeadZone + (1.0 - kPastTheDeadZone) * std::min(1.0, push);
        mouseX *= out / push;
        mouseY *= out / push;
    }

    state.leftX = axis(leftX);
    state.leftY = axis(leftY);
    state.rightX = axis(rightX + mouseX);
    state.rightY = axis(rightY + mouseY);
    return state;
}

} // namespace veloplay::input

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "input/ControllerState.hpp"

// Playing with keyboard and mouse. The console only knows controllers, so
// keys and mouse are turned into what a controller would send: keys press
// buttons and push the left stick, the mouse moves the right stick.
//
// A mouse and a stick are different things. A stick says how fast to turn,
// a mouse says how far. So the speed of the mouse becomes how far the stick
// is pushed, and a game then turns as fast as the hand moves. That is close
// to, but never the same as, a game made for a mouse: the game still adds
// its own dead zone and acceleration, which were made for thumbs.

namespace veloplay::input {

/// What a key or a mouse button can stand for on the controller.
enum class PadControl : std::uint8_t {
    Cross,
    Circle,
    Square,
    Triangle,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    L1,
    R1,
    L2,
    R2,
    L3,
    R3,
    Options,
    Create,
    Touchpad,
    Ps,
    LeftStickUp,
    LeftStickDown,
    LeftStickLeft,
    LeftStickRight,
    RightStickUp,
    RightStickDown,
    RightStickLeft,
    RightStickRight,
    Count,
};
inline constexpr std::size_t kPadControlCount = static_cast<std::size_t>(PadControl::Count);

/// Lowercase identifier for the settings file ("cross", "left_stick_up", ...).
std::string_view padControlName(PadControl control);
std::optional<PadControl> parsePadControl(std::string_view name);

/// Keys and mouse buttons are named by Windows virtual-key codes, which
/// cover both ('W' is 0x57, the left mouse button 0x01). Zero is "no key".
/// The two directions of the mouse wheel have no such code and get their own.
namespace key {
inline constexpr int None = 0;
inline constexpr int MouseLeft = 0x01;
inline constexpr int MouseRight = 0x02;
inline constexpr int MouseMiddle = 0x04;
inline constexpr int MouseBack = 0x05;
inline constexpr int MouseForward = 0x06;
inline constexpr int Backspace = 0x08;
inline constexpr int Tab = 0x09;
inline constexpr int Enter = 0x0D;
inline constexpr int Shift = 0x10;
inline constexpr int Ctrl = 0x11;
inline constexpr int Alt = 0x12;
inline constexpr int Escape = 0x1B;
inline constexpr int Space = 0x20;
inline constexpr int Home = 0x24;
inline constexpr int Left = 0x25;
inline constexpr int Up = 0x26;
inline constexpr int Right = 0x27;
inline constexpr int Down = 0x28;
inline constexpr int Delete = 0x2E;
inline constexpr int F1 = 0x70;
inline constexpr int F8 = 0x77;
inline constexpr int F9 = 0x78;
inline constexpr int F11 = 0x7A;
inline constexpr int F12 = 0x7B;
inline constexpr int WheelUp = 0x1001;
inline constexpr int WheelDown = 0x1002;
} // namespace key

/// Keys VeloPlay keeps for itself while a game is shown: the menu, the
/// display and full screen. They cannot be given to the controller.
[[nodiscard]] bool isReservedKey(int code);

/// A layout for games seen from behind the character: W A S D to walk, the
/// mouse to look, left and right button to shoot and aim.
[[nodiscard]] std::array<int, kPadControlCount> defaultKeyBindings();

/// A layout of its own for one game. The game is told by the title id the
/// console reports for what is running (like "PPSA01325").
struct GameKeyLayout {
    std::string titleId;
    std::string name; // what the console called the game when the layout was made
    std::array<int, kPadControlCount> keys = defaultKeyBindings();
    float mouseSensitivity = 1.0f;
    bool invertY = false;

    friend bool operator==(const GameKeyLayout&, const GameKeyLayout&) = default;
};

struct KeyboardMouseSettings {
    bool enabled = false;
    std::array<int, kPadControlCount> keys = defaultKeyBindings(); // one key per control, 0 = none
    float mouseSensitivity = 1.0f;                                 // 0.2 .. 4.0
    bool invertY = false;
    /// Layouts that replace the one above while their game runs.
    std::vector<GameKeyLayout> games;

    friend bool operator==(const KeyboardMouseSettings&, const KeyboardMouseSettings&) = default;
};

/// The most layouts of their own that are kept.
inline constexpr std::size_t kMaxGameLayouts = 64;

/// Brings every value into its valid range and lets a key stand for one control only.
void sanitize(KeyboardMouseSettings& settings);

/// The game's own layout, or null if it has none.
[[nodiscard]] const GameKeyLayout* gameLayout(const KeyboardMouseSettings& settings, std::string_view titleId);
[[nodiscard]] GameKeyLayout* gameLayout(KeyboardMouseSettings& settings, std::string_view titleId);

/// The settings as they apply while the game with this title id runs: its
/// own keys, sensitivity and direction if it has a layout, the general ones
/// otherwise. An empty id means no game is known.
[[nodiscard]] KeyboardMouseSettings settingsFor(const KeyboardMouseSettings& settings, std::string_view titleId);

/// Turns keys and mouse into the state of a controller. Events come from
/// the window, the state is read from the thread that sends input to the
/// console; a lock inside keeps the two apart.
class KeyboardMouseMapper {
public:
    using Clock = std::chrono::steady_clock;

    void configure(const KeyboardMouseSettings& settings);

    void key(int code, bool down);
    /// One notch of the wheel presses its control for a moment.
    void wheel(int notches, Clock::time_point now);
    /// Movement as the mouse reports it, in its own counts.
    void mouseMoved(int dx, int dy);
    /// Lets go of everything: the window lost the keyboard, or a menu opened.
    void releaseAll();

    /// The controller as keys and mouse have it right now. Neutral while switched off.
    [[nodiscard]] ControllerState state(Clock::time_point now);

private:
    [[nodiscard]] bool held(PadControl control, Clock::time_point now) const;

    mutable std::mutex mutex_;
    KeyboardMouseSettings settings_{};
    std::array<bool, kPadControlCount> down_{};
    std::array<Clock::time_point, kPadControlCount> tappedUntil_{}; // wheel notches
    double pendingX_ = 0.0; // mouse movement not yet turned into stick
    double pendingY_ = 0.0;
    double speedX_ = 0.0;   // smoothed, in counts per second
    double speedY_ = 0.0;
    Clock::time_point last_{};
};

} // namespace veloplay::input

#pragma once

namespace veloplay::linuxos {

/// The code a key has in the layouts for keys and mouse (input::key, named
/// after Windows' virtual keys) for an SDL scancode, or 0 for a key that has
/// no place there. Scancodes name where a key sits, not what is printed on
/// it: W A S D stay where the hand is on a German keyboard as on any other.
/// Left and right Shift, Ctrl and Alt are one key each, as on Windows.
[[nodiscard]] int keyCodeFor(int sdlScancode);

/// The code of a mouse button (SDL_BUTTON_LEFT and so on), or 0.
[[nodiscard]] int mouseButtonCode(int sdlButton);

} // namespace veloplay::linuxos

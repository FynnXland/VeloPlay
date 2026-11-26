#include "linux/KeyMap.hpp"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include "input/KeyboardMouse.hpp"

namespace veloplay::linuxos {

int keyCodeFor(int sdlScancode) {
    const auto code = static_cast<SDL_Scancode>(sdlScancode);
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) return 'A' + (code - SDL_SCANCODE_A);
    // SDL runs 1 to 9, then 0; the codes run 0 to 9.
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9) return '1' + (code - SDL_SCANCODE_1);
    if (code == SDL_SCANCODE_0) return '0';
    if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) return input::key::F1 + (code - SDL_SCANCODE_F1);
    if (code >= SDL_SCANCODE_F13 && code <= SDL_SCANCODE_F24) return input::key::F1 + 12 + (code - SDL_SCANCODE_F13);
    if (code >= SDL_SCANCODE_KP_1 && code <= SDL_SCANCODE_KP_9) return 0x61 + (code - SDL_SCANCODE_KP_1);
    switch (code) {
        case SDL_SCANCODE_KP_0: return 0x60;
        case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
        case SDL_SCANCODE_KP_PLUS: return 0x6B;
        case SDL_SCANCODE_KP_MINUS: return 0x6D;
        case SDL_SCANCODE_KP_PERIOD: return 0x6E;
        case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
        case SDL_SCANCODE_KP_ENTER: return input::key::Enter;
        case SDL_SCANCODE_RETURN: return input::key::Enter;
        case SDL_SCANCODE_ESCAPE: return input::key::Escape;
        case SDL_SCANCODE_BACKSPACE: return input::key::Backspace;
        case SDL_SCANCODE_TAB: return input::key::Tab;
        case SDL_SCANCODE_SPACE: return input::key::Space;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT: return input::key::Shift;
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL: return input::key::Ctrl;
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT: return input::key::Alt;
        case SDL_SCANCODE_CAPSLOCK: return 0x14;
        case SDL_SCANCODE_PAGEUP: return 0x21;
        case SDL_SCANCODE_PAGEDOWN: return 0x22;
        case SDL_SCANCODE_END: return 0x23;
        case SDL_SCANCODE_HOME: return input::key::Home;
        case SDL_SCANCODE_LEFT: return input::key::Left;
        case SDL_SCANCODE_UP: return input::key::Up;
        case SDL_SCANCODE_RIGHT: return input::key::Right;
        case SDL_SCANCODE_DOWN: return input::key::Down;
        case SDL_SCANCODE_INSERT: return 0x2D;
        case SDL_SCANCODE_DELETE: return input::key::Delete;
        // The keys around the letters, by place, as Windows numbers them on a US layout.
        case SDL_SCANCODE_SEMICOLON: return 0xBA;
        case SDL_SCANCODE_EQUALS: return 0xBB;
        case SDL_SCANCODE_COMMA: return 0xBC;
        case SDL_SCANCODE_MINUS: return 0xBD;
        case SDL_SCANCODE_PERIOD: return 0xBE;
        case SDL_SCANCODE_SLASH: return 0xBF;
        case SDL_SCANCODE_GRAVE: return 0xC0;
        case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
        case SDL_SCANCODE_BACKSLASH: return 0xDC;
        case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
        case SDL_SCANCODE_APOSTROPHE: return 0xDE;
        case SDL_SCANCODE_NONUSBACKSLASH: return 0xE2;
        default: return 0;
    }
}

int mouseButtonCode(int sdlButton) {
    switch (sdlButton) {
        case SDL_BUTTON_LEFT: return input::key::MouseLeft;
        case SDL_BUTTON_RIGHT: return input::key::MouseRight;
        case SDL_BUTTON_MIDDLE: return input::key::MouseMiddle;
        case SDL_BUTTON_X1: return input::key::MouseBack;
        case SDL_BUTTON_X2: return input::key::MouseForward;
        default: return 0;
    }
}

} // namespace veloplay::linuxos

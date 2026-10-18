/* SDL scancode to engine key index. The engine's key array (guest
 * 0x171be9c, see docs/FRONTEND.md) is indexed by the PC set-1 scan code
 * (IBM PC/XT make codes; extended E0 keys as 0x80 | code). Written from the
 * public set-1 table. */
#include <SDL.h>

#include "diag.h"
#include "frontend_sdl.h"

int fe_nukey_from_scancode(int scancode)
{
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        static const unsigned char letters[26] = {
            0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
            0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
        };
        return letters[scancode - SDL_SCANCODE_A];
    }
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_0)
        return 0x02 + (scancode - SDL_SCANCODE_1); /* 1..9 = 0x02..0x0a, 0 = 0x0b */
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F10)
        return 0x3b + (scancode - SDL_SCANCODE_F1);
    switch (scancode) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_MINUS: return 0x0c;
    case SDL_SCANCODE_EQUALS: return 0x0d;
    case SDL_SCANCODE_BACKSPACE: return 0x0e;
    case SDL_SCANCODE_TAB: return 0x0f;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1a;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
    case SDL_SCANCODE_RETURN: return 0x1c;
    case SDL_SCANCODE_LCTRL: return 0x1d;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2a;
    case SDL_SCANCODE_BACKSLASH: return 0x2b;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3a;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4a;
    case SDL_SCANCODE_KP_4: return 0x4b;
    case SDL_SCANCODE_KP_5: return 0x4c;
    case SDL_SCANCODE_KP_6: return 0x4d;
    case SDL_SCANCODE_KP_PLUS: return 0x4e;
    case SDL_SCANCODE_KP_1: return 0x4f;
    case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_ENTER: return 0x9c;
    case SDL_SCANCODE_RCTRL: return 0x9d;
    case SDL_SCANCODE_KP_DIVIDE: return 0xb5;
    case SDL_SCANCODE_RALT: return 0xb8;
    case SDL_SCANCODE_HOME: return 0xc7;
    case SDL_SCANCODE_UP: return 0xc8;
    case SDL_SCANCODE_PAGEUP: return 0xc9;
    case SDL_SCANCODE_LEFT: return 0xcb;
    case SDL_SCANCODE_RIGHT: return 0xcd;
    case SDL_SCANCODE_END: return 0xcf;
    case SDL_SCANCODE_DOWN: return 0xd0;
    case SDL_SCANCODE_PAGEDOWN: return 0xd1;
    case SDL_SCANCODE_INSERT: return 0xd2;
    case SDL_SCANCODE_DELETE: return 0xd3;
    case SDL_SCANCODE_LGUI: return 0xdb;
    case SDL_SCANCODE_RGUI: return 0xdc;
    default:
        diag_unsupported(__func__, 0, "SDL scancode %d", (int)scancode);
        return -1;
    }
}

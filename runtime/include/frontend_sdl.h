/* SDL2 front end: native replacements for the game's Objective-C front end
 * (guest block 0x3db000-0x3e2000). See docs/FRONTEND.md. */
#ifndef FRONTEND_SDL_H
#define FRONTEND_SDL_H

/* Engine key index (PC set-1 scan code, 0x00-0xff) for an SDL_Scancode,
 * or -1 when the key has no engine code. frontend_keymap.c. */
int fe_nukey_from_scancode(int scancode);

#endif

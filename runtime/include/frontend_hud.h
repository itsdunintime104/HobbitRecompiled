/* Frame-time overlay (runtime/src/frontend_hud.c). */
#ifndef FRONTEND_HUD_H
#define FRONTEND_HUD_H

#include <SDL.h>

/* Record the frame time and, when the overlay is on, draw it into the back
 * buffer of window's current GL context. w and h are the size of the GL
 * surface in pixels. Call just before each swap. */
void fe_hud_frame(SDL_Window *window, int w, int h);

/* Call right after each swap returns: records the time spent in the swap. */
void fe_hud_swap_done(void);

/* Show or hide the overlay. */
void fe_hud_toggle(void);

#endif

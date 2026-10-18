/* GL call counters for the frame-time overlay (runtime/src/frontend_hud.c).
 * The draw and buffer-map thunks bump them; the overlay reads and clears them
 * once per frame. A relaxed atomic add is one instruction on arm64. */
#ifndef HUD_COUNTERS_H
#define HUD_COUNTERS_H

extern unsigned g_hud_draws, g_hud_maps;

static inline void hud_count_draw(void)
{
    __atomic_fetch_add(&g_hud_draws, 1u, __ATOMIC_RELAXED);
}

static inline void hud_count_map(void)
{
    __atomic_fetch_add(&g_hud_maps, 1u, __ATOMIC_RELAXED);
}

#endif

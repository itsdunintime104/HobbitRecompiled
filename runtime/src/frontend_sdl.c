/* SDL2 front end. Native replacements for the 19 functions of the guest
 * Objective-C front end (block 0x3db000-0x3e2000) that the rest of the game
 * calls and that reach objc_msgSend, plus thunks for the CGDisplay, CGL,
 * NS and objc imports. One SDL window and one legacy (2.1) GL context.
 * Guest addresses and the reasoning are in docs/FRONTEND.md.
 *
 * Every replacement is called like any lifted function: cdecl arguments at
 * g_esp+4, return address at g_esp; it returns in g_eax and pops the return
 * address. The lifted originals are renamed by frontend_rename.h. */
#define RECOMP_GENERATED_CODE
#define GL_SILENCE_DEPRECATION
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "data_imports.h"
#include "frontend_hud.h"
#include "frontend_sdl.h"
#include "guest_call.h"
#include "guest_mem.h"
#include "recomp_types.h"
#include "thunk_cf.h"

#define ARG(n) MEM32(g_esp + 4u + 4u * (n))
#define RET(v) do { g_eax = (uint32_t)(v); g_esp += 4; } while (0)

/* ---- guest data the front end owns or touches (FRONTEND.md section b) ---- */
#define G_KEYS_INDEX 0x0171BE98u    /* current key buffer, 0 or 1 */
#define G_KEYS 0x0171BE9Cu          /* 2 x 0x100 bytes, set-1 scan code index */
#define G_WINDOWS 0x0171C29Cu       /* GameWindow objects by index */
#define G_INIT_ONCE 0x0171C314u     /* byte, set by 3dd420 */
#define G_INIT_TIME 0x0171C31Cu     /* struct timeval, 2 x int32 */
#define G_HOME_BUF 0x0171C338u      /* 0x402-byte home path */
#define G_WINDOW_COUNT 0x0182A37Cu  /* number of game windows */
#define G_DISPLAY_ID 0x0182A388u
#define G_DISPLAY_MODE 0x0182A394u
#define G_RETINA 0x0182A398u        /* byte: backing scale > 1 */
#define G_SCREEN_W 0x0161B2F0u      /* main screen size, read by the game */
#define G_SCREEN_H 0x0161B2F4u
#define G_HAS_FOCUS 0x0161B49Cu     /* byte: last focus state */
#define G_PP_MOUSE_BUTTONS 0x0161B69Cu  /* slots holding pointers */
#define G_PP_MOUSE_X 0x0161B6A0u
#define G_PP_MOUSE_Y 0x0161B6A4u
#define G_PP_MOUSE_WHEEL 0x0161B6A8u
#define G_PP_PUMP_RESET 0x011DB584u
#define G_PP_QUIT 0x011DB948u
#define GUEST_FN_CLEAR_MODIFIERS 0x003DF0E0u  /* lifted, kept */

#define FE_GL_TOKEN 0x00C61000u     /* fake CGLContextObj / pixel format / renderer */
#define FE_DISPLAY_ID 1u
#define FE_MODE_TOKEN 0x00C62000u   /* fake CGDisplayModeRef */

static SDL_Window *s_window;
static SDL_GLContext s_gl;
static int s_shown;

static void fe_die(const char *what)
{
    fprintf(stderr, "frontend: %s: %s\n", what, SDL_GetError());
    abort();
}

static void fe_sdl_init(void)
{
    if (SDL_WasInit(SDL_INIT_VIDEO))
        return;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
        fe_die("SDL_Init");
    SDL_DisableScreenSaver(); /* replaces the two IOPMAssertions of 3dd420 */
}

static void fe_desktop_size(int *w, int *h)
{
    SDL_DisplayMode m;
    fe_sdl_init();
    if (SDL_GetDesktopDisplayMode(0, &m) != 0)
        fe_die("SDL_GetDesktopDisplayMode");
    *w = m.w;
    *h = m.h;
}

/* The render size renders into a GL surface of that size (CGL backing size);
 * macOS scales it to the window. SDL keeps reporting the window size, so once
 * the size is overridden every size given to the game comes from here. The
 * size is RECOMP_RENDER_SIZE=WxH, else the size the game chose in its own
 * resolution option (sub_003DCDF0), else the desktop size. */
static int s_render_override;
static int s_env_size;
static int s_game_w, s_game_h;

static void fe_render_size(int *w, int *h)
{
    static int done, rw, rh;
    if (!done) {
        done = 1;
        const char *e = getenv("RECOMP_RENDER_SIZE");
        int a, b;
        char extra;
        if (e && sscanf(e, "%dx%d%c", &a, &b, &extra) == 2 && a >= 64 && a <= 8192 &&
            b >= 64 && b <= 8192) {
            rw = a;
            rh = b;
            s_env_size = 1;
            s_render_override = 1;
        } else if (e) {
            fprintf(stderr, "frontend: RECOMP_RENDER_SIZE ignored: %s\n", e);
        }
    }
    if (s_env_size) {
        *w = rw;
        *h = rh;
    } else if (s_game_w) {
        *w = s_game_w;
        *h = s_game_h;
    } else {
        fe_desktop_size(w, h);
    }
}

/* The sizes the game's resolution option offers: the panel's pixel size, 75%
 * of it, the desktop size and 50% of it. Filled once. */
#define FE_MAX_MODES 4
static int s_mode_w[FE_MAX_MODES], s_mode_h[FE_MAX_MODES], s_mode_n;

static void fe_add_mode(int w, int h)
{
    if (w < 800 || h < 600)
        return;
    for (int i = 0; i < s_mode_n; i++)
        if (s_mode_w[i] == w && s_mode_h[i] == h)
            return;
    s_mode_w[s_mode_n] = w;
    s_mode_h[s_mode_n] = h;
    s_mode_n++;
}

static void fe_mode_sizes(void)
{
    if (s_mode_n)
        return;
    int dw, dh;
    fe_desktop_size(&dw, &dh);
    int pw = 0, ph = 0, ww = 0, wh = 0; /* native panel mode, widest mode */
    const void *key = kCGDisplayShowDuplicateLowResolutionModes;
    const void *val = kCFBooleanTrue;
    CFDictionaryRef opts = CFDictionaryCreate(NULL, &key, &val, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(CGMainDisplayID(), opts);
    CFRelease(opts);
    if (modes) {
        for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            int w = (int)CGDisplayModeGetPixelWidth(m), h = (int)CGDisplayModeGetPixelHeight(m);
            if (pw == 0 && (CGDisplayModeGetIOFlags(m) & kDisplayModeNativeFlag)) {
                pw = w;
                ph = h;
            }
            if (w > ww) {
                ww = w;
                wh = h;
            }
        }
        CFRelease(modes);
    }
    if (pw == 0) {
        pw = ww;
        ph = wh;
    }
    if (pw == 0) {
        pw = dw;
        ph = dh;
    }
    fe_add_mode(pw, ph);
    fe_add_mode((pw * 3 / 4) & ~1, (ph * 3 / 4) & ~1);
    fe_add_mode(dw, dh);
    fe_add_mode((pw / 2) & ~1, (ph / 2) & ~1);
    fprintf(stderr, "frontend: display modes offered:");
    for (int i = 0; i < s_mode_n; i++)
        fprintf(stderr, "%s %dx%d", i ? "," : "", s_mode_w[i], s_mode_h[i]);
    fputc('\n', stderr);
}

/* Size of the GL surface: the render size when overridden, else the drawable. */
static void fe_surface_size(int *w, int *h)
{
    if (s_render_override)
        fe_render_size(w, h);
    else
        SDL_GL_GetDrawableSize(s_window, w, h);
}

/* The single window and GL context, created hidden on first need so that CGL
 * calls made before the game opens its window (renderer probing) also work. */
static void fe_ensure_gl(void)
{
    if (s_window)
        return;
    fe_sdl_init();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0); /* legacy, not core */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    s_window = SDL_CreateWindow("The Hobbit", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!s_window)
        fe_die("SDL_CreateWindow");
    s_gl = SDL_GL_CreateContext(s_window);
    if (!s_gl)
        fe_die("SDL_GL_CreateContext");
    SDL_GL_MakeCurrent(s_window, s_gl);
    fprintf(stderr, "frontend: GL %s, %s\n", (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER));
    /* Run the GL engine on its own thread: the driver's per-draw work then
     * leaves the game's main thread, which is the frame-rate limit. */
    CGLError mt = CGLEnable(CGLGetCurrentContext(), kCGLCEMPEngine);
    if (mt != kCGLNoError)
        fprintf(stderr, "frontend: multithreaded GL engine not enabled: error %d\n", (int)mt);
    int rw, rh;
    fe_render_size(&rw, &rh);
    if (s_render_override) {
        CGLContextObj ctx = CGLGetCurrentContext();
        GLint dim[2] = {rw, rh};
        CGLError e1 = CGLSetParameter(ctx, kCGLCPSurfaceBackingSize, dim);
        CGLError e2 = CGLEnable(ctx, kCGLCESurfaceBackingSize);
        if (e1 != kCGLNoError || e2 != kCGLNoError)
            fprintf(stderr, "frontend: render size not applied: errors %d %d\n", (int)e1, (int)e2);
        else
            fprintf(stderr, "frontend: render size %dx%d (scaled to the window)\n", rw, rh);
    }
}

/* Show the window as game window 0. Returns the index the game expects. */
static uint32_t fe_open_window(const char *title, int w, int h, int fullscreen)
{
    fe_ensure_gl();
    if (title)
        SDL_SetWindowTitle(s_window, title);
    SDL_SetWindowSize(s_window, w, h);
    SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (fullscreen && SDL_SetWindowFullscreen(s_window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
        fe_die("SDL_SetWindowFullscreen");
    SDL_ShowWindow(s_window);
    SDL_RaiseWindow(s_window);
    SDL_GL_MakeCurrent(s_window, s_gl);
    s_shown = 1;
    /* One SDL window serves every game window; the game sees index 0 and a
     * count of 1, as after its own fullscreen path (3dd5e0 resets to 0). */
    MEM32(G_WINDOWS) = FE_GL_TOKEN;
    MEM32(G_WINDOW_COUNT) = 1;
    return 0;
}

/* ---- replaced functions (FRONTEND.md section b) -------------------------- */

/* 3dcc60 / 3dccb0: create and drain the main NSAutoreleasePool. */
void sub_003DCC60(void) { RET(0); }
void sub_003DCCB0(void) { RET(0); }

/* 3dd420: one-time application setup. */
void sub_003DD420(void)
{
    if (!(MEM8(G_INIT_ONCE) & 1)) {
        struct timeval tv;
        fe_sdl_init();
        memset(guest_ptr(G_KEYS), 0, 0x200);
        gettimeofday(&tv, NULL);
        MEM32(G_INIT_TIME) = (uint32_t)tv.tv_sec;
        MEM32(G_INIT_TIME + 4) = (uint32_t)tv.tv_usec;
        MEM8(G_INIT_ONCE) = 1;
    }
    RET(0);
}

/* 3df9c0: main display setup. Sizes are in points; SDL runs without
 * high-DPI, so points are pixels and the retina flag stays 0. */
void sub_003DF9C0(void)
{
    int w, h;
    fe_render_size(&w, &h);
    MEM32(G_DISPLAY_ID) = FE_DISPLAY_ID;
    MEM32(G_DISPLAY_MODE) = FE_MODE_TOKEN;
    MEM8(G_RETINA) = 0;
    MEM32(G_SCREEN_W) = (uint32_t)w;
    MEM32(G_SCREEN_H) = (uint32_t)h;
    RET(0);
}

/* 3de630(int *w, int *h): main screen size. */
void sub_003DE630(void)
{
    int w, h;
    fe_render_size(&w, &h);
    MEM32(ARG(0)) = (uint32_t)w;
    MEM32(ARG(1)) = (uint32_t)h;
    RET(0);
}

/* 3dcd20(int *w, int *h): frame of the active GL view (points). */
void sub_003DCD20(void)
{
    int w = 0, h = 0;
    if (s_shown && s_render_override)
        fe_render_size(&w, &h);
    else if (s_shown)
        SDL_GetWindowSize(s_window, &w, &h);
    MEM32(ARG(0)) = (uint32_t)w;
    MEM32(ARG(1)) = (uint32_t)h;
    RET(0);
}

/* 3dff70(int *w, int *h): view frame converted to backing pixels. */
void sub_003DFF70(void)
{
    int w = 0, h = 0;
    if (s_shown)
        fe_surface_size(&w, &h);
    MEM32(ARG(0)) = (uint32_t)w;
    MEM32(ARG(1)) = (uint32_t)h;
    RET(0);
}

/* 3de020(const char *title, int x, int y, int w, int h, a6, a7, a8):
 * open a window. a6-a8 go to OpenGLView initWithFrame:shareContext:...;
 * their meaning is not confirmed, so they are logged. Returns the index. */
void sub_003DE020(void)
{
    const char *title = ARG(0) ? (const char *)guest_ptr(ARG(0)) : NULL;
    int w = (int)ARG(3), h = (int)ARG(4);
    fprintf(stderr, "frontend: window %dx%d at %d,%d, view args 0x%x 0x%x 0x%x\n", w, h,
            (int)ARG(1), (int)ARG(2), ARG(5), ARG(6), ARG(7));
    RET(fe_open_window(title, w, h, 0));
}

/* 3dd5e0: open the fullscreen window on the main display. Returns 0. */
void sub_003DD5E0(void)
{
    int w, h;
    fe_desktop_size(&w, &h);
    fprintf(stderr, "frontend: fullscreen %dx%d, args 0x%x 0x%x 0x%x 0x%x 0x%x\n", w, h,
            ARG(0), ARG(1), ARG(2), ARG(3), ARG(4));
    fe_open_window(NULL, w, h, 1);
    SDL_ShowCursor(SDL_DISABLE);
    RET(0);
}

/* 3dfb70: switch the active window to fullscreen. */
void sub_003DFB70(void)
{
    if (s_shown && SDL_SetWindowFullscreen(s_window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
        fe_die("SDL_SetWindowFullscreen");
    SDL_ShowCursor(SDL_DISABLE);
    if (s_shown)
        SDL_GL_MakeCurrent(s_window, s_gl);
    RET(0);
}

/* 3dcdf0(int idx, int w, int h): set the view frame, clear both buffers to
 * black, set the viewport and raise the window. Returns 0. */
void sub_003DCDF0(void)
{
    int w = (int)ARG(1), h = (int)ARG(2);
    if (s_shown) {
        int fullscreen = (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN) != 0;
        if (!fullscreen)
            SDL_SetWindowSize(s_window, w, h);
        SDL_GL_MakeCurrent(s_window, s_gl);
        if (fullscreen && w >= 64 && w <= 8192 && h >= 64 && h <= 8192) {
            /* The game's own resolution option: render at the size it asks for. */
            int cw, ch;
            fe_render_size(&cw, &ch);
            if (s_env_size) {
                if (w != cw || h != ch)
                    fprintf(stderr, "frontend: game asked for %dx%d, RECOMP_RENDER_SIZE keeps %dx%d\n",
                            w, h, cw, ch);
            } else if (w != cw || h != ch) {
                s_game_w = w;
                s_game_h = h;
                s_render_override = 1;
                CGLContextObj ctx = CGLGetCurrentContext();
                GLint dim[2] = {w, h};
                CGLError e1 = CGLSetParameter(ctx, kCGLCPSurfaceBackingSize, dim);
                CGLError e2 = CGLEnable(ctx, kCGLCESurfaceBackingSize);
                if (e1 != kCGLNoError || e2 != kCGLNoError)
                    fprintf(stderr, "frontend: render size not applied: errors %d %d\n", (int)e1,
                            (int)e2);
                else
                    fprintf(stderr, "frontend: game set render size %dx%d\n", w, h);
            }
        }
        for (int i = 0; i < 2; i++) {
            glViewport(0, 0, w, h);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            SDL_GL_SwapWindow(s_window);
        }
        glViewport(0, 0, w, h);
        SDL_RaiseWindow(s_window);
    }
    RET(0);
}

/* 3df250(int idx, int x, int y, int w, int h): set the window frame and show it. */
void sub_003DF250(void)
{
    if (s_shown) {
        SDL_SetWindowSize(s_window, (int)ARG(3), (int)ARG(4));
        SDL_ShowWindow(s_window);
        SDL_RaiseWindow(s_window);
    }
    RET(0);
}

/* 3df4e0(int idx): bring the window to the front and activate the app. */
void sub_003DF4E0(void)
{
    if (s_shown)
        SDL_RaiseWindow(s_window);
    RET(0);
}

/* 3de7f0(int idx): make the window's GL context current. */
void sub_003DE7F0(void)
{
    if (s_window)
        SDL_GL_MakeCurrent(s_window, s_gl);
    RET(0);
}

/* RECOMP_FRAME_DUMP=N: on swaps N, 2N, 4N... (up to 8 files) read the back
 * buffer and write it as a binary PPM to $RECOMP_FRAME_DIR/frame_<swap>.ppm
 * (default: the current folder).
 * Also logs swaps 1, 10, 100 and 1000. */
static void fe_frame_dump(void)
{
    static unsigned long swaps, next_dump;
    static int every = -1, dumped;
    swaps++;
    if (swaps == 1 || swaps == 10 || swaps == 100 || swaps == 1000)
        fprintf(stderr, "frontend: swap %lu\n", swaps);
    if (every < 0) {
        const char *e = getenv("RECOMP_FRAME_DUMP");
        every = e ? atoi(e) : 0;
        if (every < 0)
            every = 0;
        next_dump = (unsigned long)every;
    }
    if (every == 0 || dumped >= 8 || swaps != next_dump)
        return;
    next_dump *= 2;
    dumped++;
    int w = 0, h = 0;
    fe_surface_size(&w, &h);
    if (w <= 0 || h <= 0)
        return;
    unsigned char *px = malloc((size_t)w * (size_t)h * 4u);
    if (!px)
        return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    const char *dir = getenv("RECOMP_FRAME_DIR");
    if (!dir)
        dir = ".";
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%lu.ppm", dir, swaps);
    FILE *f = fopen(path, "wb");
    size_t lit = 0;
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) {
            for (int x = 0; x < w; x++) {
                const unsigned char *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4u;
                if (p[0] | p[1] | p[2])
                    lit++;
                fwrite(p, 1, 3, f);
            }
        }
        fclose(f);
        fprintf(stderr, "frontend: frame %lu dumped, %.2f%% non-black\n", swaps,
                100.0 * (double)lit / ((double)w * (double)h));
    } else {
        fprintf(stderr, "frontend: frame %lu: cannot open %s\n", swaps, path);
    }
    free(px);
}

/* 3de850(int idx): swap buffers. */
void sub_003DE850(void)
{
    if (s_window) {
        int sw = 0, sh = 0;
        fe_surface_size(&sw, &sh);
        fe_hud_frame(s_window, sw, sh);
        fe_frame_dump();
        SDL_GL_SwapWindow(s_window);
        fe_hud_swap_done();
    }
    RET(0);
}

/* 1 when the window has keyboard focus. The game waits in a loop until it has
 * focus (0x3172B0). macOS 14+ gives focus only to an app the user launched, so
 * a test run started by a background process never gets it;
 * RECOMP_ASSUME_FOCUS=1 reports focus for such runs. */
static int fe_has_focus(void)
{
    static int assume = -1;
    if (assume < 0) {
        const char *v = getenv("RECOMP_ASSUME_FOCUS");
        assume = v != NULL && v[0] == '1';
    }
    if (!s_shown)
        return 0;
    return assume || (SDL_GetWindowFlags(s_window) & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0;
}

/* 3df0a0(int idx): 1 when the window has keyboard focus. */
void sub_003DF0A0(void)
{
    RET(fe_has_focus());
}

/* 3df570: home directory with a trailing '/', in the game's own buffer. */
void sub_003DF570(void)
{
    char *buf = guest_ptr(G_HOME_BUF);
    if (buf[0] == '\0') {
        const char *home = getenv("HOME");
        snprintf(buf, 0x401, "%s", home ? home : "");
        size_t n = strlen(buf);
        if (n > 0 && n < 0x401 && buf[n - 1] != '/') {
            buf[n] = '/';
            buf[n + 1] = '\0';
        }
    }
    RET(G_HOME_BUF);
}

/* 3df8f0(const char *title, const char *message): alert panel. */
void sub_003DF8F0(void)
{
    const char *title = ARG(0) ? (const char *)guest_ptr(ARG(0)) : "";
    const char *msg = ARG(1) ? (const char *)guest_ptr(ARG(1)) : "";
    fprintf(stderr, "frontend: alert: %s: %s\n", title, msg);
    /* Test runs set RECOMP_NO_ALERT_UI=1: the log line above is enough, and a
     * modal box would block an unattended run. */
    if (getenv("RECOMP_NO_ALERT_UI") == NULL) {
        fe_sdl_init();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, s_shown ? s_window : NULL);
    }
    RET(0);
}

/* ---- input (FRONTEND.md section c) --------------------------------------- */

static uint8_t *fe_keys_now(void)
{
    return guest_ptr(G_KEYS + 0x100u * (MEM32(G_KEYS_INDEX) & 1u));
}

/* RECOMP_TRACE_KEYS=1: log every key down/up and text event from SDL, to
 * tell a key-up SDL never sent from one the front end lost. */
static int fe_trace_keys(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_TRACE_KEYS");
        on = v != NULL && v[0] == '1';
    }
    return on;
}

static void fe_key(int scancode, int down)
{
    int k = fe_nukey_from_scancode(scancode);
    if (k >= 0)
        fe_keys_now()[k] = (uint8_t)down;
    if (fe_trace_keys())
        fprintf(stderr, "keys: %s %s (key %d) at %.3f s\n", down ? "down" : "up",
                SDL_GetScancodeName((SDL_Scancode)scancode), k, (double)SDL_GetTicks() / 1000.0);
}

static void fe_mouse_pos(int x, int y)
{
    int w, h;
    SDL_GetWindowSize(s_window, &w, &h);
    if (s_render_override && w > 0 && h > 0) {
        int rw, rh;
        fe_render_size(&rw, &rh);
        x = (int)((int64_t)x * rw / w);
        y = (int)((int64_t)y * rh / h);
        h = rh;
    }
    /* NSView coordinates: origin at the bottom left. */
    MEM32(MEM32(G_PP_MOUSE_X)) = (uint32_t)x;
    MEM32(MEM32(G_PP_MOUSE_Y)) = (uint32_t)(h - y);
}

static void fe_mouse_button(uint8_t button, int down)
{
    uint32_t bit = button == SDL_BUTTON_LEFT ? 1u : button == SDL_BUTTON_RIGHT ? 2u : 4u;
    uint32_t p = MEM32(G_PP_MOUSE_BUTTONS);
    MEM32(p) = down ? (MEM32(p) | bit) : (MEM32(p) & ~bit);
}

static void fe_quit(void)
{
    MEM8(MEM32(G_PP_QUIT)) = 1;
}

/* 3de8b0: per-frame event pump. Returns the window count. */
void sub_003DE8B0(void)
{
    SDL_Event ev;
    uint32_t cur = MEM32(G_KEYS_INDEX) & 1u, next = cur ^ 1u;

    MEM32(MEM32(G_PP_PUMP_RESET)) = 0;
    memcpy(guest_ptr(G_KEYS + 0x100u * next), guest_ptr(G_KEYS + 0x100u * cur), 0x100);
    MEM32(G_KEYS_INDEX) = next;

    while ((int32_t)MEM32(G_WINDOW_COUNT) >= 1 && SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            fe_quit();
            break;
        case SDL_KEYDOWN:
            if (ev.key.repeat)
                break;
            /* Cmd+F toggles the frame-time overlay and does not reach the game. */
            if (ev.key.keysym.scancode == SDL_SCANCODE_F && (ev.key.keysym.mod & KMOD_GUI) &&
                !(ev.key.keysym.mod & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT))) {
                fe_hud_toggle();
                break;
            }
            fe_key(ev.key.keysym.scancode, 1);
            if (ev.key.keysym.scancode == SDL_SCANCODE_Q && (ev.key.keysym.mod & KMOD_GUI) &&
                !(ev.key.keysym.mod & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT)))
                fe_quit();
            break;
        case SDL_KEYUP:
            fe_key(ev.key.keysym.scancode, 0);
            break;
        case SDL_TEXTINPUT:
        case SDL_TEXTEDITING:
            if (fe_trace_keys())
                fprintf(stderr, "keys: text %s \"%s\" at %.3f s\n",
                        ev.type == SDL_TEXTINPUT ? "input" : "editing",
                        ev.type == SDL_TEXTINPUT ? ev.text.text : ev.edit.text,
                        (double)SDL_GetTicks() / 1000.0);
            break;
        case SDL_MOUSEMOTION:
            fe_mouse_pos(ev.motion.x, ev.motion.y);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            fe_mouse_pos(ev.button.x, ev.button.y);
            fe_mouse_button(ev.button.button, ev.type == SDL_MOUSEBUTTONDOWN);
            break;
        case SDL_MOUSEWHEEL: {
            float dy = ev.wheel.preciseY;
            memcpy(guest_ptr(MEM32(G_PP_MOUSE_WHEEL)), &dy, 4);
            break;
        }
        default:
            break;
        }
    }

    uint8_t focus = (uint8_t)fe_has_focus();
    if (MEM8(G_HAS_FOCUS) != focus) {
        /* SDL ticks are monotonic milliseconds since SDL_Init. */
        fprintf(stderr, "frontend: focus %s at %.3f s\n", focus ? "gained" : "lost",
                (double)SDL_GetTicks() / 1000.0);
        uint32_t zero = 0;
        guest_call_cdecl(GUEST_FN_CLEAR_MODIFIERS, 1, &zero);
        MEM8(G_HAS_FOCUS) = focus;
    }
    RET(MEM32(G_WINDOW_COUNT));
}

/* ---- dispatch for indirect calls (FRONTEND.md section b) ----------------- */

static const struct { uint32_t va; recomp_func_t fn; } FE_REPLACED[] = {
    {0x003DCC60u, sub_003DCC60}, {0x003DCCB0u, sub_003DCCB0}, {0x003DCD20u, sub_003DCD20},
    {0x003DCDF0u, sub_003DCDF0}, {0x003DD420u, sub_003DD420}, {0x003DD5E0u, sub_003DD5E0},
    {0x003DE020u, sub_003DE020}, {0x003DE630u, sub_003DE630}, {0x003DE7F0u, sub_003DE7F0},
    {0x003DE850u, sub_003DE850}, {0x003DE8B0u, sub_003DE8B0}, {0x003DF0A0u, sub_003DF0A0},
    {0x003DF250u, sub_003DF250}, {0x003DF4E0u, sub_003DF4E0}, {0x003DF570u, sub_003DF570},
    {0x003DF8F0u, sub_003DF8F0}, {0x003DF9C0u, sub_003DF9C0}, {0x003DFB70u, sub_003DFB70},
    {0x003DFF70u, sub_003DFF70},
};

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va < 0x003DB000u || xbox_va >= 0x003E2000u)
        return NULL;
    for (size_t i = 0; i < sizeof FE_REPLACED / sizeof FE_REPLACED[0]; i++)
        if (FE_REPLACED[i].va == xbox_va)
            return FE_REPLACED[i].fn;
    return NULL;
}

/* ---- data imports (FRONTEND.md section e) -------------------------------- */

void frontend_data_init(void)
{
    /* _NSApp is an id; the NS* names are NSString constants. Nothing outside
     * the replaced block reads them, so each is a zeroed guest word. */
    static const char *const names[] = {
        "_NSApp", "_NSDefaultRunLoopMode", "_NSWindowWillCloseNotification",
        "_NSViewGlobalFrameDidChangeNotification",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        data_import_register(names[i], data_import_alloc(4));
}

/* ---- objc thunks (FRONTEND.md section d) --------------------------------- */

static const char *fe_guest_cstr(uint32_t a)
{
    uint8_t b;
    if (a == 0 || guest_mem_probe(a, &b) != 0)
        return "?";
    return (const char *)guest_ptr(a);
}

/* Unreplaced code that still sends messages. The kept keyboard-layout
 * helpers 3dc1c0, 3dc730 and 3dc8b0 only wrap their work in an
 * NSAutoreleasePool (alloc, init, drain); with no Objective-C objects in the
 * process that pool is a no-op. Any other message logs its selector and
 * aborts. Before the runtime fixes them up, class refs and selector refs
 * hold pointers to the name strings. */
void imp__objc_msgSend(void)
{
    uint32_t self = ARG(0);
    const char *sel = fe_guest_cstr(ARG(1));
    if (!strcmp(sel, "alloc") && !strcmp(fe_guest_cstr(self), "NSAutoreleasePool")) {
        RET(FE_GL_TOKEN + 0x100u);
        return;
    }
    if (self == FE_GL_TOKEN + 0x100u && (!strcmp(sel, "init") || !strcmp(sel, "drain"))) {
        RET(self);
        return;
    }
    fprintf(stderr, "objc_msgSend: unreplaced message -%s to 0x%08x (\"%s\"), return 0x%08x\n",
            sel, self, fe_guest_cstr(self), MEM32(g_esp));
    abort();
}

/* struct return: ARG(0) is the result pointer, then self, then the selector. */
void imp__objc_msgSend_stret(void)
{
    fprintf(stderr, "objc_msgSend_stret: unreplaced message -%s, return 0x%08x\n",
            fe_guest_cstr(ARG(2)), MEM32(g_esp));
    abort();
}

void imp__objc_msgSend_fpret(void)
{
    fprintf(stderr, "objc_msgSend_fpret: unreplaced message -%s, return 0x%08x\n",
            fe_guest_cstr(ARG(1)), MEM32(g_esp));
    abort();
}

/* objc_msgSendSuper(struct objc_super *, SEL, ...) */
void imp__objc_msgSendSuper(void)
{
    fprintf(stderr, "objc_msgSendSuper: unreplaced message -%s, return 0x%08x\n",
            fe_guest_cstr(ARG(1)), MEM32(g_esp));
    abort();
}

/* ---- NS thunks ----------------------------------------------------------- */

/* Constant NSString/CFString in the image: {isa, flags, const char *, length}. */
static const char *fe_guest_nsstring(uint32_t s)
{
    uint8_t b;
    if (s == 0 || guest_mem_probe(s + 8u, &b) != 0)
        return "?";
    return fe_guest_cstr(MEM32(s + 8u));
}

/* NSLog(NSString *format, ...): print the format string only. */
void imp__NSLog(void)
{
    fprintf(stderr, "NSLog: %s\n", fe_guest_nsstring(ARG(0)));
    RET(0);
}

/* Only the replaced 3df8f0 called it; a call here is unreplaced code. */
void imp__NSRunAlertPanel(void)
{
    fprintf(stderr, "NSRunAlertPanel: %s: %s\n", fe_guest_nsstring(ARG(0)),
            fe_guest_nsstring(ARG(1)));
    RET(1); /* NSAlertDefaultReturn */
}

/* Only the replaced 3df570 called it; it returns an NSString, which this
 * process cannot make, so a call here is unreplaced code. */
void imp__NSHomeDirectory(void)
{
    fprintf(stderr, "NSHomeDirectory: unreplaced caller, return 0x%08x\n", MEM32(g_esp));
    abort();
}

/* ---- CGDisplay thunks ---------------------------------------------------- */

static void fp_push_f64(double v)
{
    g_fp_top = (g_fp_top + 7) & 7;
    g_fp_stack[g_fp_top] = v;
}

void imp__CGMainDisplayID(void) { RET(FE_DISPLAY_ID); }
void imp__CGDisplayCapture(void) { RET(0); }
void imp__CGDisplayRelease(void) { RET(0); }
void imp__CGDisplayIsCaptured(void) { RET(0); }
void imp__CGDisplayIDToOpenGLDisplayMask(void) { RET(1); }
void imp__CGDisplayIOServicePort(void) { RET(0); }
void imp__CGDisplaySetDisplayMode(void) { RET(0); }
void imp__CGDisplayModeRelease(void) { RET(0); }
void imp__CGDisplayCopyDisplayMode(void) { RET(FE_MODE_TOKEN); }

/* A host CFArray of CFNumber indexes 0..n-1 into the size list of
 * fe_mode_sizes: there is no mode switching, and the CGDisplayMode thunks
 * report the size behind the index. 2a4eb0 counts and walks this array with no
 * NULL check. */
void imp__CGDisplayCopyAllDisplayModes(void)
{
    fe_mode_sizes();
    CFNumberRef idx[FE_MAX_MODES];
    for (int i = 0; i < s_mode_n; i++)
        idx[i] = CFNumberCreate(NULL, kCFNumberIntType, &i);
    CFArrayRef modes = CFArrayCreate(NULL, (const void **)idx, s_mode_n, &kCFTypeArrayCallBacks);
    for (int i = 0; i < s_mode_n; i++)
        CFRelease(idx[i]);
    RET(cf_to_guest(modes));
}

/* Every mode is the desktop mode, 32-bit direct pixels (IO32BitDirectPixels).
 * The guest owns the string; 2a4eb0 compares it and releases it. */
void imp__CGDisplayModeCopyPixelEncoding(void)
{
    RET(cf_to_guest(CFStringCreateWithCString(NULL, "--------RRRRRRRRGGGGGGGGBBBBBBBB",
                                              kCFStringEncodingASCII)));
}
void imp__CGShieldingWindowLevel(void) { RET(2147483628); } /* kCGShieldingWindowLevel */
void imp__CGWindowLevelForKey(void) { RET(0); }

/* Size behind a mode reference: an index from CopyAllDisplayModes, else (the
 * FE_MODE_TOKEN of CopyDisplayMode, or anything else) the current render size. */
static void fe_mode_size(uint32_t ref, int *w, int *h)
{
    if (ref != FE_MODE_TOKEN) {
        CFTypeRef m = cf_from_guest(ref);
        int idx = -1;
        if (m && CFGetTypeID(m) == CFNumberGetTypeID())
            CFNumberGetValue(m, kCFNumberIntType, &idx);
        fe_mode_sizes();
        if (idx >= 0 && idx < s_mode_n) {
            *w = s_mode_w[idx];
            *h = s_mode_h[idx];
            return;
        }
    }
    fe_render_size(w, h);
}

void imp__CGDisplayModeGetWidth(void)
{
    int w, h;
    fe_mode_size(ARG(0), &w, &h);
    RET(w);
}

void imp__CGDisplayModeGetHeight(void)
{
    int w, h;
    fe_mode_size(ARG(0), &w, &h);
    RET(h);
}

void imp__CGDisplayModeGetRefreshRate(void)
{
    SDL_DisplayMode m;
    fe_sdl_init();
    fp_push_f64(SDL_GetDesktopDisplayMode(0, &m) == 0 ? (double)m.refresh_rate : 60.0);
    RET(0);
}

void imp__CGDisplayHideCursor(void)
{
    fe_sdl_init();
    SDL_ShowCursor(SDL_DISABLE);
    RET(0);
}

void imp__CGDisplayShowCursor(void)
{
    fe_sdl_init();
    SDL_ShowCursor(SDL_ENABLE);
    RET(0);
}

/* CGAcquireDisplayFadeReservation(float seconds, token *out) */
void imp__CGAcquireDisplayFadeReservation(void)
{
    MEM32(ARG(1)) = 1;
    RET(0);
}

void imp__CGReleaseDisplayFadeReservation(void) { RET(0); }
void imp__CGDisplayFade(void) { RET(0); } /* no fade: returns at once */

/* ---- CGL thunks: one SDL context behind every CGL object ----------------- */

/* CGLChoosePixelFormat(attribs, CGLPixelFormatObj *pix, GLint *npix) */
void imp__CGLChoosePixelFormat(void)
{
    MEM32(ARG(1)) = FE_GL_TOKEN;
    MEM32(ARG(2)) = 1;
    RET(0);
}

void imp__CGLDestroyPixelFormat(void) { RET(0); }

/* CGLCreateContext(pix, share, CGLContextObj *ctx) */
void imp__CGLCreateContext(void)
{
    fe_ensure_gl();
    MEM32(ARG(2)) = FE_GL_TOKEN;
    RET(0);
}

void imp__CGLDestroyContext(void) { RET(0); }

void imp__CGLSetCurrentContext(void)
{
    if (ARG(0)) {
        fe_ensure_gl();
        SDL_GL_MakeCurrent(s_window, s_gl);
    } else if (s_window) {
        SDL_GL_MakeCurrent(s_window, NULL);
    }
    RET(0);
}

void imp__CGLGetCurrentContext(void)
{
    RET(s_gl && SDL_GL_GetCurrentContext() == s_gl ? FE_GL_TOKEN : 0);
}

void imp__CGLFlushDrawable(void)
{
    if (s_window) {
        int sw = 0, sh = 0;
        fe_surface_size(&sw, &sh);
        fe_hud_frame(s_window, sw, sh);
        SDL_GL_SwapWindow(s_window);
        fe_hud_swap_done();
    }
    RET(0);
}

void imp__CGLLockContext(void) { RET(0); }
void imp__CGLUnlockContext(void) { RET(0); }
void imp__CGLSetFullScreen(void) { RET(0); }

/* CGLSetParameter(ctx, pname, const GLint *params): swap interval only. */
void imp__CGLSetParameter(void)
{
    if (ARG(1) == 222u /* kCGLCPSwapInterval */ && s_window)
        SDL_GL_SetSwapInterval((int)MEM32(ARG(2)));
    RET(0);
}

/* CGLQueryRendererInfo(display_mask, CGLRendererInfoObj *rend, GLint *nrend) */
void imp__CGLQueryRendererInfo(void)
{
    MEM32(ARG(1)) = FE_GL_TOKEN;
    MEM32(ARG(2)) = 1;
    RET(0);
}

/* CGLDescribeRenderer(rend, index, property, GLint *value): one accelerated
 * renderer. Memory sizes are a fixed 1024 MB (the host GPU shares memory). */
void imp__CGLDescribeRenderer(void)
{
    uint32_t v;
    switch (ARG(2)) {
    case 70: v = 0x00024600u; break;      /* kCGLRPRendererID: generic */
    case 73: case 75: case 81: v = 1; break; /* Accelerated, Online, Window */
    case 84: v = 1; break;                /* kCGLRPDisplayMask */
    case 120: case 121: v = 1024u << 20; break; /* Video/TextureMemory bytes */
    case 131: case 132: v = 1024u; break; /* Video/TextureMemoryMegabytes */
    default: v = 0; break;
    }
    MEM32(ARG(3)) = v;
    RET(0);
}

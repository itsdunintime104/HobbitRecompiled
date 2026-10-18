/* Frame-time overlay drawn over the game's back buffer just before each swap.
 * The Metal HUD does not attach to this OpenGL app, so this replaces it.
 * RECOMP_HUD=1 shows it from the start; Cmd+F toggles it.
 *
 * It draws with legacy GL 2.1 immediate mode and saves and restores every
 * state it touches (attributes, matrices, GLSL program, framebuffer), so the
 * game's next frame sees the state it left. The text changes twice a second;
 * it is compiled into a display list then, so a normal frame replays it. */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "frontend_hud.h"
#include "guest_heap.h"
#include "hud_counters.h"
#include "slowframe.h"

#define HUD_FRAMES 240          /* frame times kept for the graph and the spread stats */
#define HUD_GRAPH_MS 50.0       /* graph height in milliseconds */
#define HUD_TEXT_PERIOD 0.5     /* seconds between text refreshes */
#define HUD_SLOW_MS (1000.0 / 30.0)
#define HUD_LINES 7
#define HUD_LINE_LEN 48

static int s_on = -1;
static Uint64 s_last, s_swap_start;
static double s_ms[HUD_FRAMES];
static unsigned s_head, s_count;
static double s_text_age;
static char s_lines[HUD_LINES][HUD_LINE_LEN];
static int s_text_ready, s_text_dirty;
static GLuint s_text_list;

/* Sums since the last text refresh. */
static double s_acc_swap_ms, s_acc_frame_ms;
static unsigned s_acc_frames, s_acc_draws, s_acc_maps;

/* RECOMP_FRAMELOG=<file>: every frame time as CSV, independent of the HUD. */
static int s_log_state = -1;  /* -1 not read yet, 0 off, 1 writing */
static FILE *s_log;
static char s_log_buf[64 * 1024];
static Uint64 s_log_t0;
static double s_log_t0_uptime_s;  /* CLOCK_UPTIME_RAW at s_log_t0, for matching other logs */
static double s_log_flushed;

/* 5x7 glyphs, one byte per row, bit 4 is the leftmost column. */
static const char GLYPH_CHARS[] = "0123456789.ABCDEFGHIJKLMNOPQRSTUVWXYZ%:-x";
static const unsigned char GLYPHS[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},                                             /* . */
    {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* A B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, /* C D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* E F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* G H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, /* I J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* K L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, /* M N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* O P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* Q R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* S T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* U V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* W X */
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Y Z */
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}, {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, /* % : */
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}, /* - x */
};

static void quad(float x, float y, float w, float h)
{
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
}

/* Text at (x, y), top-left origin, each font dot `px` pixels. Characters
 * outside the font draw as a space. */
static void text(const char *s, float x, float y, float px)
{
    glBegin(GL_QUADS);
    for (; *s; s++, x += 6.0f * px) {
        const char *at = strchr(GLYPH_CHARS, *s);
        if (!at)
            continue;
        const unsigned char *g = GLYPHS[at - GLYPH_CHARS];
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col))
                    quad(x + (float)col * px, y + (float)row * px, px, px);
    }
    glEnd();
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void record_frame(double dt_ms)
{
    s_ms[s_head] = dt_ms;
    s_head = (s_head + 1) % HUD_FRAMES;
    if (s_count < HUD_FRAMES)
        s_count++;
    s_acc_frame_ms += dt_ms;
    s_acc_frames++;
    s_acc_draws += __atomic_exchange_n(&g_hud_draws, 0u, __ATOMIC_RELAXED);
    s_acc_maps += __atomic_exchange_n(&g_hud_maps, 0u, __ATOMIC_RELAXED);
    s_text_age += dt_ms / 1000.0;
}

/* Rewrite the text lines. FPS and AVG cover the newest frames that fit in one
 * second; P99, 1% LOW, MAX and the slow count cover all HUD_FRAMES frames;
 * GAME, SWAP, DRAWS and MAPS are per-frame means since the last refresh. */
static void refresh_text(SDL_Window *window, int w, int h)
{
    double sum = 0.0;
    unsigned n = 0;
    for (; n < s_count && sum < 1000.0; n++)
        sum += s_ms[(s_head + HUD_FRAMES - 1 - n) % HUD_FRAMES];
    double avg = n ? sum / n : 0.0;

    double sorted[HUD_FRAMES];
    memcpy(sorted, s_ms, s_count * sizeof sorted[0]);
    qsort(sorted, s_count, sizeof sorted[0], cmp_double);
    double p99 = 0.0, worst = 0.0;
    unsigned slow = 0;
    if (s_count) {
        p99 = sorted[(unsigned)ceil(0.99 * s_count) - 1];
        worst = sorted[s_count - 1];
        for (unsigned i = 0; i < s_count; i++)
            slow += sorted[i] > HUD_SLOW_MS;
    }

    double frames = s_acc_frames ? (double)s_acc_frames : 1.0;
    double swap = s_acc_swap_ms / frames;
    double game = s_acc_frame_ms / frames - swap;

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(window, &win_w, &win_h);
    int vsync = SDL_GL_GetSwapInterval();
    int guard = guest_heap_guard_mode();

    snprintf(s_lines[0], HUD_LINE_LEN, "FPS %.1f   1%% LOW %.1f",
             avg > 0.0 ? 1000.0 / avg : 0.0, p99 > 0.0 ? 1000.0 / p99 : 0.0);
    snprintf(s_lines[1], HUD_LINE_LEN, "AVG %.1f MS  P99 %.1f  MAX %.1f", avg, p99, worst);
    snprintf(s_lines[2], HUD_LINE_LEN, "OVER 33 MS: %u OF %u", slow, s_count);
    snprintf(s_lines[3], HUD_LINE_LEN, "GAME %.1f MS  SWAP %.1f MS", game > 0.0 ? game : 0.0, swap);
    snprintf(s_lines[4], HUD_LINE_LEN, "DRAWS %.0f  MAPS %.0f PER FRAME",
             (double)s_acc_draws / frames, (double)s_acc_maps / frames);
    snprintf(s_lines[5], HUD_LINE_LEN, "RENDER %dx%d  WINDOW %dx%d  %.2fx", w, h, win_w, win_h,
             win_w > 0 ? (double)w / win_w : 0.0);
    /* RECOMP_TSO and RECOMP_OPT come from build/Makefile. */
    snprintf(s_lines[6], HUD_LINE_LEN, "VSYNC %s  TSO %d  -O%d  HEAP GUARD %s",
             vsync ? "ON" : "OFF", RECOMP_TSO, RECOMP_OPT,
             guard == 0 ? "OFF" : guard == 1 ? "ON" : "LOG");

    s_acc_swap_ms = s_acc_frame_ms = 0.0;
    s_acc_frames = s_acc_draws = s_acc_maps = 0;
    s_text_age = 0.0;
    s_text_ready = 1;
    s_text_dirty = 1;
}

static void log_frame(Uint64 now, double dt_ms)
{
    if (s_log_state < 0) {
        const char *path = getenv("RECOMP_FRAMELOG");
        s_log_state = 0;
        if (path && path[0]) {
            s_log = fopen(path, "w");
            if (!s_log) {
                fprintf(stderr, "frontend: cannot open frame log %s\n", path);
            } else {
                setvbuf(s_log, s_log_buf, _IOFBF, sizeof s_log_buf);
                fputs("t_s,frame_ms\n", s_log);
                s_log_state = 1;
                fprintf(stderr, "frontend: frame log %s (t_s 0 = %.3f s uptime)\n", path, s_log_t0_uptime_s);
            }
        }
    }
    if (s_log_state != 1)
        return;
    double t = (double)(now - s_log_t0) / (double)SDL_GetPerformanceFrequency();
    fprintf(s_log, "%.4f,%.3f\n", t, dt_ms);
    if (t - s_log_flushed >= 1.0) {
        fflush(s_log);
        s_log_flushed = t;
    }
}

void fe_hud_toggle(void)
{
    s_on = s_on > 0 ? 0 : 1;
    fprintf(stderr, "frontend: HUD %s\n", s_on ? "on" : "off");
    /* Start the per-frame means fresh, not from when the overlay was hidden. */
    s_acc_swap_ms = s_acc_frame_ms = 0.0;
    s_acc_frames = s_acc_draws = s_acc_maps = 0;
    s_text_age = 0.0;
}

void fe_hud_swap_done(void)
{
    if (s_swap_start)
        s_acc_swap_ms += (double)(SDL_GetPerformanceCounter() - s_swap_start) * 1000.0 /
                         (double)SDL_GetPerformanceFrequency();
}

void fe_hud_frame(SDL_Window *window, int w, int h)
{
    slowframe_mark();
    if (s_on < 0) {
        const char *e = getenv("RECOMP_HUD");
        s_on = e && e[0] == '1';
        if (s_on)
            fprintf(stderr, "frontend: HUD on (Cmd+F toggles)\n");
    }
    Uint64 now = SDL_GetPerformanceCounter();
    if (!s_log_t0) {
        s_log_t0 = now;
        s_log_t0_uptime_s = (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
    }
    if (s_last) {
        double dt_ms = (double)(now - s_last) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        record_frame(dt_ms);
        log_frame(now, dt_ms);
    }
    s_last = now;
    if (!s_on) {
        s_swap_start = SDL_GetPerformanceCounter();
        return;
    }

    if (w <= 0 || h <= 0)
        return;
    if (!s_text_ready || s_text_age >= HUD_TEXT_PERIOD)
        refresh_text(window, w, h);
    int win_w = 0;
    SDL_GetWindowSize(window, &win_w, NULL);
    float px = win_w > 0 && w > win_w ? 4.0f : 2.0f;  /* Retina: twice the dots */

    GLint program = 0, fbo = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fbo);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, w, h, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glUseProgram(0);
    glDisable(GL_VERTEX_PROGRAM_ARB);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    for (GLenum unit = 0; unit < 8; unit++) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glDisable(GL_TEXTURE_1D);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_3D);
        glDisable(GL_TEXTURE_CUBE_MAP);
        glDisable(GL_TEXTURE_RECTANGLE_ARB);
    }
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float pad = 4.0f * px, x0 = pad, y0 = pad, line_h = 10.0f * px;
    float gw = (float)HUD_FRAMES * px * 0.5f, gh = 24.0f * px;
    float text_w = 0.0f;
    for (int i = 0; i < HUD_LINES; i++) {
        float lw = (float)strlen(s_lines[i]) * 6.0f * px;
        if (lw > text_w)
            text_w = lw;
    }
    float box_w = (text_w > gw ? text_w : gw) + 2.0f * pad;
    float gy = y0 + (float)HUD_LINES * line_h + pad;
    float box_h = gy - y0 + gh + 2.0f * pad;
    glColor4f(0.0f, 0.0f, 0.0f, 0.6f);
    glBegin(GL_QUADS);
    quad(x0 - pad * 0.5f, y0 - pad * 0.5f, box_w, box_h);
    glEnd();

    /* The text only changes on a refresh: compile it then, replay it otherwise. */
    if (!s_text_list)
        s_text_list = glGenLists(1);
    if (s_text_list && s_text_dirty) {
        glNewList(s_text_list, GL_COMPILE);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        for (int i = 0; i < HUD_LINES; i++)
            text(s_lines[i], x0, y0 + (float)i * line_h, px);
        glEndList();
        s_text_dirty = 0;
    }
    if (s_text_list)
        glCallList(s_text_list);

    /* Graph: one bar per frame, oldest on the left; green under 16.7 ms,
     * yellow under 33.3 ms, red above. */
    float bar = gw / HUD_FRAMES;
    glBegin(GL_QUADS);
    for (unsigned i = 0; i < s_count; i++) {
        double ms = s_ms[(s_head + HUD_FRAMES - s_count + i) % HUD_FRAMES];
        float bh = (float)(ms > HUD_GRAPH_MS ? 1.0 : ms / HUD_GRAPH_MS) * gh;
        if (ms <= 1000.0 / 60.0 + 0.5)
            glColor4f(0.3f, 0.9f, 0.3f, 0.9f);
        else if (ms <= 1000.0 / 30.0 + 0.5)
            glColor4f(0.95f, 0.85f, 0.2f, 0.9f);
        else
            glColor4f(0.95f, 0.3f, 0.25f, 0.9f);
        quad(x0 + (float)(HUD_FRAMES - s_count + i) * bar, gy + gh - bh, bar, bh);
    }
    glColor4f(1.0f, 1.0f, 1.0f, 0.35f);
    for (int k = 1; k <= 2; k++) {
        float ly = gy + gh - (float)((1000.0 / (60.0 / k)) / HUD_GRAPH_MS) * gh;
        quad(x0, ly, gw, px * 0.5f);
    }
    glEnd();

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glPopAttrib();
    glUseProgram((GLuint)program);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)fbo);
    s_swap_start = SDL_GetPerformanceCounter();
}

/* GL call counter for run diagnostics. Load it into a run with
 *   DYLD_INSERT_LIBRARIES=build.nosync/glcount.dylib build.nosync/hobbit ...
 * It wraps (dyld __interpose) the host GL calls that show whether a frame
 * draws anything, and prints the counts since the previous report on swaps
 * GLCOUNT_EVERY (default 60), 2x, 3x, ... up to GLCOUNT_MAX reports
 * (default 20). Each report also drains glGetError, so the game's own
 * glGetError calls may see fewer errors; it is a diagnostic build only. */
#define GL_SILENCE_DEPRECATION
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define DYLD_INTERPOSE(repl, orig)                                                   \
    __attribute__((used)) static struct {                                            \
        const void *replacement;                                                     \
        const void *replacee;                                                        \
    } interpose_##orig __attribute__((section("__DATA,__interpose"))) = {             \
        (const void *)(unsigned long)&repl, (const void *)(unsigned long)&orig}

/* Children must not inherit the insert: the x86_64 Cg helper cannot load an
 * arm64 dylib, so dyld kills it and every Cg call then returns 0. */
__attribute__((constructor)) static void glcount_no_inherit(void)
{
    unsetenv("DYLD_INSERT_LIBRARIES");
}

enum {
    C_CLEAR, C_DRAW_ARRAYS, C_DRAW_ELEMENTS, C_DRAW_RANGE, C_BEGIN, C_TEX_IMAGE,
    C_TEX_COMPRESSED, C_BIND_TEXTURE, C_USE_PROGRAM, C_USE_PROGRAM_ARB, C_BIND_PROGRAM_ARB,
    C_PROGRAM_STRING_ARB, C_VIEWPORT, C_BIND_FBO, C_BUFFER_DATA, C_VERTEX_ATTRIB_PTR,
    C_VERTEX_PTR, C_GET_ERROR, C_COUNT
};
static const char *const NAMES[C_COUNT] = {
    "glClear", "glDrawArrays", "glDrawElements", "glDrawRangeElements", "glBegin",
    "glTexImage2D", "glCompressedTexImage2D", "glBindTexture", "glUseProgram",
    "glUseProgramObjectARB", "glBindProgramARB", "glProgramStringARB", "glViewport",
    "glBindFramebuffer(EXT)", "glBufferData(ARB)", "glVertexAttribPointer(ARB)",
    "glVertexPointer", "glGetError(game)",
};
static atomic_ulong g_count[C_COUNT];
static atomic_ulong g_total[C_COUNT];
static GLfloat g_clear[4];
static GLint g_viewport[4];
static unsigned g_game_errors; /* non-zero results the game itself saw */

#define BUMP(c) atomic_fetch_add_explicit(&g_count[c], 1, memory_order_relaxed)

/* Detail frames: the frame after each of the first GLCOUNT_DETAIL (default 2)
 * reports logs every clear, FBO bind and draw, with state and one pixel. */
static int g_detail;

static GLint geti(GLenum e)
{
    GLint v = 0;
    glGetIntegerv(e, &v);
    return v;
}

static void detail_state(const char *what)
{
    GLint vp = 0, fp = 0;
    glGetProgramivARB(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB, &vp);
    glGetProgramivARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_BINDING_ARB, &fp);
    GLboolean cm[4] = {0, 0, 0, 0};
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    fprintf(stderr,
            "glcount:     %s fbo=%d vp_on=%d vp=%d fp_on=%d fp=%d tex2d=%d depth=%d blend=%d cull=%d"
            " scissor=%d mask=%d%d%d%d varr=%d vbo=%d\n",
            what, geti(GL_FRAMEBUFFER_BINDING_EXT), glIsEnabled(GL_VERTEX_PROGRAM_ARB), vp,
            glIsEnabled(GL_FRAGMENT_PROGRAM_ARB), fp, geti(GL_TEXTURE_BINDING_2D),
            glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_BLEND), glIsEnabled(GL_CULL_FACE),
            glIsEnabled(GL_SCISSOR_TEST), cm[0], cm[1], cm[2], cm[3], glIsEnabled(GL_VERTEX_ARRAY),
            geti(GL_ARRAY_BUFFER_BINDING));
}

static void detail_pixel(const char *what)
{
    GLint v[4];
    glGetIntegerv(GL_VIEWPORT, v);
    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(v[0] + v[2] / 2, v[1] + v[3] / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    fprintf(stderr, "glcount:     %s centre pixel %u %u %u %u (viewport %d %d %d %d)\n", what, px[0],
            px[1], px[2], px[3], v[0], v[1], v[2], v[3]);
}

static void my_glClear(GLbitfield m)
{
    BUMP(C_CLEAR);
    if (g_detail)
        fprintf(stderr, "glcount:   glClear 0x%X fbo=%d color %.2f %.2f %.2f %.2f\n", m,
                geti(GL_FRAMEBUFFER_BINDING_EXT), g_clear[0], g_clear[1], g_clear[2], g_clear[3]);
    glClear(m);
}
DYLD_INTERPOSE(my_glClear, glClear);
static void my_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    g_clear[0] = r; g_clear[1] = g; g_clear[2] = b; g_clear[3] = a;
    glClearColor(r, g, b, a);
}
DYLD_INTERPOSE(my_glClearColor, glClearColor);
static const GLvoid *g_vptr;
static GLint g_vsize, g_vstride;
static GLenum g_vtype;

static void my_glDrawArrays(GLenum m, GLint f, GLsizei n)
{
    BUMP(C_DRAW_ARRAYS);
    if (g_detail) {
        fprintf(stderr, "glcount:   glDrawArrays mode=%u first=%d count=%d vptr=%p size=%d type=0x%X stride=%d\n",
                m, f, n, g_vptr, g_vsize, g_vtype, g_vstride);
        if (g_vptr != NULL && geti(GL_ARRAY_BUFFER_BINDING) == 0 && g_vtype == GL_FLOAT && n > 0) {
            const char *base = (const char *)g_vptr;
            int st = g_vstride ? g_vstride : g_vsize * 4;
            for (int i = 0; i < n && i < 4; i++) {
                const float *v = (const float *)(base + (size_t)(f + i) * (size_t)st);
                fprintf(stderr, "glcount:     v%d = %.3f %.3f %.3f\n", i, v[0], v[1], g_vsize > 2 ? v[2] : 0.0f);
            }
        }
        detail_state("before");
    }
    glDrawArrays(m, f, n);
    if (g_detail)
        detail_pixel("after");
}
DYLD_INTERPOSE(my_glDrawArrays, glDrawArrays);
static void my_glDrawElements(GLenum m, GLsizei n, GLenum t, const GLvoid *i)
{
    BUMP(C_DRAW_ELEMENTS);
    glDrawElements(m, n, t, i);
}
DYLD_INTERPOSE(my_glDrawElements, glDrawElements);
static void my_glDrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei n, GLenum t, const GLvoid *i)
{
    BUMP(C_DRAW_RANGE);
    glDrawRangeElements(m, s, e, n, t, i);
}
DYLD_INTERPOSE(my_glDrawRangeElements, glDrawRangeElements);
static void my_glBegin(GLenum m) { BUMP(C_BEGIN); glBegin(m); }
DYLD_INTERPOSE(my_glBegin, glBegin);
static void my_glTexImage2D(GLenum t, GLint l, GLint fi, GLsizei w, GLsizei h, GLint b, GLenum f,
                            GLenum ty, const GLvoid *p)
{
    BUMP(C_TEX_IMAGE);
    glTexImage2D(t, l, fi, w, h, b, f, ty, p);
}
DYLD_INTERPOSE(my_glTexImage2D, glTexImage2D);
static void my_glCompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b,
                                      GLsizei n, const GLvoid *p)
{
    BUMP(C_TEX_COMPRESSED);
    glCompressedTexImage2D(t, l, f, w, h, b, n, p);
}
DYLD_INTERPOSE(my_glCompressedTexImage2D, glCompressedTexImage2D);
static void my_glBindTexture(GLenum t, GLuint x) { BUMP(C_BIND_TEXTURE); glBindTexture(t, x); }
DYLD_INTERPOSE(my_glBindTexture, glBindTexture);
static void my_glUseProgram(GLuint p) { BUMP(C_USE_PROGRAM); glUseProgram(p); }
DYLD_INTERPOSE(my_glUseProgram, glUseProgram);
static void my_glUseProgramObjectARB(GLhandleARB p) { BUMP(C_USE_PROGRAM_ARB); glUseProgramObjectARB(p); }
DYLD_INTERPOSE(my_glUseProgramObjectARB, glUseProgramObjectARB);
static void my_glBindProgramARB(GLenum t, GLuint p) { BUMP(C_BIND_PROGRAM_ARB); glBindProgramARB(t, p); }
DYLD_INTERPOSE(my_glBindProgramARB, glBindProgramARB);
static void my_glProgramStringARB(GLenum t, GLenum f, GLsizei n, const GLvoid *s)
{
    BUMP(C_PROGRAM_STRING_ARB);
    glProgramStringARB(t, f, n, s);
}
DYLD_INTERPOSE(my_glProgramStringARB, glProgramStringARB);
static void my_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    BUMP(C_VIEWPORT);
    g_viewport[0] = x; g_viewport[1] = y; g_viewport[2] = w; g_viewport[3] = h;
    glViewport(x, y, w, h);
}
DYLD_INTERPOSE(my_glViewport, glViewport);
static void my_glBindFramebufferEXT(GLenum t, GLuint f)
{
    BUMP(C_BIND_FBO);
    if (g_detail)
        fprintf(stderr, "glcount:   glBindFramebufferEXT 0x%X -> %u\n", t, f);
    glBindFramebufferEXT(t, f);
}
DYLD_INTERPOSE(my_glBindFramebufferEXT, glBindFramebufferEXT);
static void my_glBindFramebuffer(GLenum t, GLuint f) { BUMP(C_BIND_FBO); glBindFramebuffer(t, f); }
DYLD_INTERPOSE(my_glBindFramebuffer, glBindFramebuffer);
static void my_glBufferData(GLenum t, GLsizeiptr n, const GLvoid *d, GLenum u)
{
    BUMP(C_BUFFER_DATA);
    glBufferData(t, n, d, u);
}
DYLD_INTERPOSE(my_glBufferData, glBufferData);
static void my_glBufferDataARB(GLenum t, GLsizeiptrARB n, const GLvoid *d, GLenum u)
{
    BUMP(C_BUFFER_DATA);
    glBufferDataARB(t, n, d, u);
}
DYLD_INTERPOSE(my_glBufferDataARB, glBufferDataARB);
static void my_glVertexAttribPointer(GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const GLvoid *p)
{
    BUMP(C_VERTEX_ATTRIB_PTR);
    glVertexAttribPointer(i, s, t, n, st, p);
}
DYLD_INTERPOSE(my_glVertexAttribPointer, glVertexAttribPointer);
static void my_glVertexAttribPointerARB(GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st,
                                        const GLvoid *p)
{
    BUMP(C_VERTEX_ATTRIB_PTR);
    glVertexAttribPointerARB(i, s, t, n, st, p);
}
DYLD_INTERPOSE(my_glVertexAttribPointerARB, glVertexAttribPointerARB);
static void my_glVertexPointer(GLint s, GLenum t, GLsizei st, const GLvoid *p)
{
    BUMP(C_VERTEX_PTR);
    g_vptr = p;
    g_vsize = s;
    g_vtype = t;
    g_vstride = st;
    glVertexPointer(s, t, st, p);
}
DYLD_INTERPOSE(my_glVertexPointer, glVertexPointer);
static GLenum my_glGetError(void)
{
    BUMP(C_GET_ERROR);
    GLenum e = glGetError();
    if (e != GL_NO_ERROR)
        g_game_errors++;
    return e;
}
DYLD_INTERPOSE(my_glGetError, glGetError);

static unsigned long g_swaps;
static unsigned g_reports;

static void report(void)
{
    char errs[128];
    int n = 0;
    errs[0] = '\0';
    for (int i = 0; i < 8; i++) {
        GLenum e = glGetError();
        if (e == GL_NO_ERROR)
            break;
        n += snprintf(errs + n, sizeof errs - (size_t)n, " 0x%04X", e);
        if (n >= (int)sizeof errs)
            break;
    }
    fprintf(stderr, "glcount: swap %lu: clear color %.2f %.2f %.2f %.2f, viewport %d %d %d %d,"
                    " pending errors:%s, game-seen errors %u\n",
            g_swaps, g_clear[0], g_clear[1], g_clear[2], g_clear[3], g_viewport[0], g_viewport[1],
            g_viewport[2], g_viewport[3], errs[0] ? errs : " none", g_game_errors);
    for (int c = 0; c < C_COUNT; c++) {
        unsigned long v = atomic_exchange_explicit(&g_count[c], 0, memory_order_relaxed);
        unsigned long t = atomic_fetch_add_explicit(&g_total[c], v, memory_order_relaxed) + v;
        if (t != 0)
            fprintf(stderr, "glcount:   %-28s %8lu since last, %10lu total\n", NAMES[c], v, t);
    }
}

static void my_SDL_GL_SwapWindow(SDL_Window *w)
{
    static unsigned long every;
    static unsigned max_reports;
    if (every == 0) {
        const char *e = getenv("GLCOUNT_EVERY"), *m = getenv("GLCOUNT_MAX");
        every = e ? strtoul(e, NULL, 10) : 60;
        max_reports = m ? (unsigned)strtoul(m, NULL, 10) : 20;
        if (every == 0)
            every = 60;
    }
    static unsigned max_detail = 0xFFFFFFFFu, details;
    if (max_detail == 0xFFFFFFFFu) {
        const char *d = getenv("GLCOUNT_DETAIL");
        max_detail = d ? (unsigned)strtoul(d, NULL, 10) : 2;
    }
    g_swaps++;
    if (g_detail) {
        detail_pixel("before swap (window)");
        g_detail = 0;
    }
    if (g_swaps % every == 0 && g_reports < max_reports) {
        g_reports++;
        report();
        if (details < max_detail) {
            details++;
            g_detail = 1;
            fprintf(stderr, "glcount: detail frame %lu\n", g_swaps + 1);
        }
    }
    SDL_GL_SwapWindow(w);
}
DYLD_INTERPOSE(my_SDL_GL_SwapWindow, SDL_GL_SwapWindow);

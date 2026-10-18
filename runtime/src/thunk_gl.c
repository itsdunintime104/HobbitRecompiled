/* GL thunks whose pointer arguments or results are not plain guest pointers,
 * and the GL extension entry points that the game loads with dlsym.
 *
 * The game's GLEW-style loader calls dlopen("OpenGL") and then dlsym for each
 * extension function (call sites 0x0031A7E6, 0x0031A996 and the others in
 * hobbit_0022.c). imp__dlsym in thunk_rt.c asks gl_ext_lookup() first.
 *
 * Pointer rules:
 *  - While a buffer is bound to GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER,
 *    GL_PIXEL_UNPACK_BUFFER or GL_PIXEL_PACK_BUFFER, the matching pointer
 *    argument is a byte offset, not an address. It goes to the host as
 *    (const void *)(uintptr_t)value. Otherwise it is a guest address and goes
 *    through guest_ptr. glBindBuffer caches the four bindings per thread.
 *  - glMapBuffer returns a host pointer outside the guest window. The guest
 *    gets a guest_malloc shadow instead; glUnmapBuffer copies it back.
 *
 * Protocol: docs/PLAN.md, "M3 thunk contract". */
#define GL_SILENCE_DEPRECATION
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "diag.h"
#include "gl_bind.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "hud_counters.h"
#include "recomp_types.h"
#include "thunk_gl.h"

#define ARG(n) MEM32(g_esp + 4u + 4u * (n))
#define U(n) ((uint32_t)ARG(n))
#define S(n) ((int32_t)ARG(n))
#define Z(n) ((GLsizeiptr)(int32_t)ARG(n))
#define P(n) gp(ARG(n))
#define FL(n) arg_f32(n)

static inline void *gp(uint32_t a) { return a ? guest_ptr(a) : NULL; }

static inline float arg_f32(unsigned n)
{
    uint32_t u = ARG(n);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* ---- buffer binding cache ------------------------------------------------ */

static _Thread_local uint32_t g_bound[GL_BIND_COUNT];

static int bind_slot(uint32_t target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return GL_BIND_ARRAY;
    case GL_ELEMENT_ARRAY_BUFFER: return GL_BIND_ELEMENT;
    case GL_PIXEL_PACK_BUFFER: return GL_BIND_PACK;
    case GL_PIXEL_UNPACK_BUFFER: return GL_BIND_UNPACK;
    default:
        THUNK_UNSUPPORTED("buffer target 0x%x", target);
        return -1;
    }
}

/* Pointer argument that is a buffer offset while a buffer is bound. */
const void *gl_buf_ptr(int slot, uint32_t value)
{
    if (g_bound[slot] != 0)
        return (const void *)(uintptr_t)value;
    return gp(value);
}

static void t_BindBuffer(void)
{
    uint32_t target = U(0), buffer = U(1);
    int slot = bind_slot(target);
    glBindBuffer(target, buffer);
    if (slot >= 0)
        g_bound[slot] = buffer;
    g_esp += 4;
}

static void release_maps_of(uint32_t buffer);

static void t_DeleteBuffers(void)
{
    int32_t n = S(0);
    const uint32_t *ids = P(1);
    for (int32_t i = 0; i < n && ids != NULL; i++) {
        if (ids[i] == 0)
            continue; /* GL ignores name 0; it must not match unbound slots */
        release_maps_of(ids[i]); /* before GL forgets the buffer */
        for (int s = 0; s < GL_BIND_COUNT; s++)
            if (g_bound[s] == ids[i])
                g_bound[s] = 0; /* GL unbinds a deleted buffer */
    }
    glDeleteBuffers(n, ids);
    g_esp += 4;
}

/* ---- vertex arrays ------------------------------------------------------- */

#define MAX_ATTRIBS 16

static _Thread_local uint32_t g_attrib_ptr[MAX_ATTRIBS]; /* guest value last set */

static void t_VertexAttribPointer(void)
{
    uint32_t idx = U(0);
    if (idx < MAX_ATTRIBS)
        g_attrib_ptr[idx] = U(5);
    glVertexAttribPointer(idx, S(1), U(2), (GLboolean)U(3), S(4), gl_buf_ptr(GL_BIND_ARRAY, U(5)));
    g_esp += 4;
}

/* GL would return the host value; the guest wants the value it passed. */
static void t_GetVertexAttribPointerv(void)
{
    uint32_t idx = U(0);
    if (U(1) != GL_VERTEX_ATTRIB_ARRAY_POINTER)
        THUNK_UNSUPPORTED("pname 0x%x", U(1));
    else if (idx < MAX_ATTRIBS && ARG(2) != 0)
        MEM32(ARG(2)) = g_attrib_ptr[idx];
    g_esp += 4;
}

static void t_DrawRangeElements(void)
{
    hud_count_draw();
    glDrawRangeElements(U(0), U(1), U(2), S(3), U(4), gl_buf_ptr(GL_BIND_ELEMENT, U(5)));
    g_esp += 4;
}

/* indices is an array of guest pointers (or of offsets with a buffer bound). */
static void t_MultiDrawElements(void)
{
    int32_t n = S(4);
    const uint32_t *guest_idx = P(3);
    hud_count_draw();
    const void **idx = n > 0 ? malloc((size_t)n * sizeof *idx) : NULL;
    for (int32_t i = 0; i < n; i++)
        idx[i] = gl_buf_ptr(GL_BIND_ELEMENT, guest_idx[i]);
    if (n > 0)
        glMultiDrawElements(U(0), P(1), U(2), (const void *const *)idx, n);
    free(idx);
    g_esp += 4;
}

/* Overlay counters (hud_counters.h). Defined here, next to the draws they
 * count, so the thunk tests link without the front end. */
unsigned g_hud_draws, g_hud_maps;

/* Imported by the game; replaces the generated thunk so the overlay can
 * count the draw. */
void imp__glDrawArrays(void)
{
    hud_count_draw();
    glDrawArrays(U(0), S(1), S(2));
    g_esp += 4;
}

/* ---- textures ------------------------------------------------------------ */

/* Bytes per pixel of a client-memory image, or 0 for a format/type pair not
 * listed here. */
static uint32_t pixel_bytes(uint32_t format, uint32_t type)
{
    uint32_t comps;
    switch (format) {
    case GL_LUMINANCE:
    case GL_ALPHA:
    case GL_RED:
    case GL_DEPTH_COMPONENT:
    case GL_INTENSITY: comps = 1; break;
    case GL_LUMINANCE_ALPHA: comps = 2; break;
    case GL_RGB:
    case GL_BGR: comps = 3; break;
    case GL_RGBA:
    case GL_BGRA: comps = 4; break;
    default:
        THUNK_UNSUPPORTED("pixel format 0x%x", format);
        return 0;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE:
    case GL_BYTE: return comps;
    case GL_UNSIGNED_SHORT:
    case GL_SHORT:
    case GL_HALF_FLOAT_ARB: return 2 * comps;
    case GL_UNSIGNED_INT:
    case GL_INT:
    case GL_FLOAT: return 4 * comps;
    case GL_UNSIGNED_INT_8_8_8_8:
    case GL_UNSIGNED_INT_8_8_8_8_REV:
    case GL_UNSIGNED_INT_2_10_10_10_REV: return 4;
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_5_6_5_REV:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_4_4_4_4_REV:
    case GL_UNSIGNED_SHORT_1_5_5_5_REV:
    case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
    default:
        THUNK_UNSUPPORTED("pixel type 0x%x", type);
        return 0;
    }
}

/* Size of a w x h client image with the default unpack state (alignment 4, no
 * row length, no skips). 0 when unknown, over 256 MB, or rows are padded. */
static uint32_t image_bytes(int32_t w, int32_t h, uint32_t format, uint32_t type)
{
    uint64_t bpp = pixel_bytes(format, type);
    if (bpp == 0 || w <= 0 || h <= 0)
        return 0;
    uint64_t row = (uint64_t)w * bpp;
    if (row % 4 != 0)
        return 0;
    uint64_t bytes = (uint64_t)h * row;
    return bytes > ((uint64_t)256 << 20) ? 0 : (uint32_t)bytes;
}

static GLuint g_tex_pbo;
static void *g_tex_pbo_ctx;

/* The game never calls glPixelStorei, so its unpack state is the default. With
 * the multithreaded GL engine a glTexImage2D from client memory makes the main
 * thread wait for the whole GL queue (11% of it in gameplay: a 128x128 texture
 * every frame); glBufferData into a pixel buffer does not wait, and the texture
 * then reads from that buffer. The driver wants h rows of full stride in the
 * buffer, so images with padded rows keep the direct call. */
/* Imported by the game; replaces the generated thunk. */
void imp__glTexImage2D(void)
{
    uint32_t bytes = 0;
    if (U(8) != 0 && g_bound[GL_BIND_UNPACK] == 0)
        bytes = image_bytes(S(3), S(4), U(6), U(7));
    if (bytes > 0) {
        void *ctx = CGLGetCurrentContext();
        if (g_tex_pbo == 0 || g_tex_pbo_ctx != ctx) {
            glGenBuffers(1, &g_tex_pbo);
            g_tex_pbo_ctx = ctx;
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_tex_pbo);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, bytes, guest_ptr(U(8)), GL_STREAM_DRAW);
        glTexImage2D(U(0), S(1), S(2), S(3), S(4), S(5), U(6), U(7), (const void *)0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    } else {
        glTexImage2D(U(0), S(1), S(2), S(3), S(4), S(5), U(6), U(7),
                     gl_buf_ptr(GL_BIND_UNPACK, U(8)));
    }
    g_esp += 4;
}

static void t_CompressedTexImage1D(void)
{
    glCompressedTexImage1D(U(0), S(1), U(2), S(3), S(4), S(5), gl_buf_ptr(GL_BIND_UNPACK, U(6)));
    g_esp += 4;
}

static void t_CompressedTexImage2D(void)
{
    glCompressedTexImage2D(U(0), S(1), U(2), S(3), S(4), S(5), S(6), gl_buf_ptr(GL_BIND_UNPACK, U(7)));
    g_esp += 4;
}

static void t_CompressedTexImage3D(void)
{
    glCompressedTexImage3D(U(0), S(1), U(2), S(3), S(4), S(5), S(6), S(7),
                           gl_buf_ptr(GL_BIND_UNPACK, U(8)));
    g_esp += 4;
}

static void t_CompressedTexSubImage1D(void)
{
    glCompressedTexSubImage1D(U(0), S(1), S(2), S(3), U(4), S(5), gl_buf_ptr(GL_BIND_UNPACK, U(6)));
    g_esp += 4;
}

static void t_CompressedTexSubImage2D(void)
{
    glCompressedTexSubImage2D(U(0), S(1), S(2), S(3), S(4), S(5), U(6), S(7),
                              gl_buf_ptr(GL_BIND_UNPACK, U(8)));
    g_esp += 4;
}

static void t_CompressedTexSubImage3D(void)
{
    glCompressedTexSubImage3D(U(0), S(1), S(2), S(3), S(4), S(5), S(6), S(7), U(8), S(9),
                              gl_buf_ptr(GL_BIND_UNPACK, U(10)));
    g_esp += 4;
}

static void t_GetCompressedTexImage(void)
{
    glGetCompressedTexImage(U(0), S(1), (void *)gl_buf_ptr(GL_BIND_PACK, U(2)));
    g_esp += 4;
}

/* ---- glMapBuffer shadows ------------------------------------------------- */

#define MAX_MAPS 8

static struct {
    uint32_t target; /* 0 marks a free slot */
    uint32_t buffer; /* buffer name bound to target when it was mapped */
    void *ctx;       /* CGL context that mapped it */
    void *real;      /* host mapping; NULL marks a mirror-backed mapping (shadow is the mirror's data) */
    uint32_t shadow; /* guest copy handed to the guest */
    uint32_t size;
    uint32_t access;
} g_maps[MAX_MAPS];
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;

/* Name of the buffer bound to target in the current context; 0 for targets
 * glBindBuffer does not cache. */
static uint32_t bound_buffer(uint32_t target)
{
    GLenum pname;
    switch (target) {
    case GL_ARRAY_BUFFER: pname = GL_ARRAY_BUFFER_BINDING; break;
    case GL_ELEMENT_ARRAY_BUFFER: pname = GL_ELEMENT_ARRAY_BUFFER_BINDING; break;
    case GL_PIXEL_PACK_BUFFER: pname = GL_PIXEL_PACK_BUFFER_BINDING; break;
    case GL_PIXEL_UNPACK_BUFFER: pname = GL_PIXEL_UNPACK_BUFFER_BINDING; break;
    default: return 0;
    }
    /* The array binding is not vertex array object state, so only glBindBuffer
     * changes it. With the multithreaded GL engine, glGetIntegerv waits for the
     * whole GL queue (27% of the main thread when every map asked). */
    if (target == GL_ARRAY_BUFFER)
        return g_bound[GL_BIND_ARRAY];
    GLint b = 0;
    glGetIntegerv(pname, &b);
    return (uint32_t)b;
}

/* Index of the live mapping of (target, buffer, ctx), or -1. Caller holds g_map_lock. */
static int find_map(uint32_t target, uint32_t buffer, void *ctx)
{
    for (int i = 0; i < MAX_MAPS; i++)
        if (g_maps[i].target == target && g_maps[i].buffer == buffer && g_maps[i].ctx == ctx)
            return i;
    return -1;
}

/* With the multithreaded GL engine, glMapBuffer makes the main thread wait for the whole GL queue
 * (19% of it in gameplay). Write-only array maps therefore reuse a guest mirror after the first map
 * and upload only the changed bytes on unmap. data is the copy the guest writes; clean always equals
 * the GL buffer contents. */
#define MAX_MIRRORS 16

static struct {
    void *ctx;
    uint32_t buffer; /* 0 marks a free slot */
    uint32_t data;
    uint32_t clean;
    uint32_t size;
    uint32_t usage;
    uint64_t last;
} g_mirrors[MAX_MIRRORS];
static uint64_t g_mirror_clock;

/* The GL engine copies a glBufferSubData of up to about 16 KB into its queue; a larger one makes the
 * main thread wait for the whole queue. glBufferData with data does not wait (4 MB: about 0.1 ms), so
 * larger changes re-create the buffer from the mirror. */
#define MIRROR_SUBDATA_MAX 16384u

/* Index of the mirror of (ctx, buffer), or -1. Caller holds g_map_lock. */
static int find_mirror(void *ctx, uint32_t buffer)
{
    if (buffer == 0)
        return -1;
    for (int i = 0; i < MAX_MIRRORS; i++)
        if (g_mirrors[i].buffer == buffer && g_mirrors[i].ctx == ctx)
            return i;
    return -1;
}

static void drop_mirror(void *ctx, uint32_t buffer)
{
    uint32_t data = 0, clean = 0;
    pthread_mutex_lock(&g_map_lock);
    int m = find_mirror(ctx, buffer);
    if (m >= 0) {
        for (int i = 0; i < MAX_MAPS; i++)
            if (g_maps[i].target != 0 && g_maps[i].real == NULL && g_maps[i].buffer == buffer &&
                g_maps[i].ctx == ctx)
                g_maps[i].target = 0;
        data = g_mirrors[m].data;
        clean = g_mirrors[m].clean;
        memset(&g_mirrors[m], 0, sizeof g_mirrors[m]);
    }
    pthread_mutex_unlock(&g_map_lock);
    if (data != 0)
        guest_free(data);
    if (clean != 0)
        guest_free(clean);
}

/* Byte range [lo, hi) in which a and b differ; lo == hi when they are equal. */
static void changed_range(const uint8_t *a, const uint8_t *b, uint32_t n, uint32_t *lo, uint32_t *hi)
{
    uint32_t i = 0;
    while (i + 4096u <= n && memcmp(a + i, b + i, 4096u) == 0)
        i += 4096u;
    while (i < n && a[i] == b[i])
        i++;
    *lo = i;
    if (i == n) {
        *hi = i;
        return;
    }
    uint32_t j = n;
    while (j >= i + 4096u && memcmp(a + j - 4096u, b + j - 4096u, 4096u) == 0)
        j -= 4096u;
    while (j > i && a[j - 1] == b[j - 1])
        j--;
    *hi = j;
}

/* glDeleteBuffers unmaps the buffer in GL, so the shadow is dropped without a copy back.
 * Mirror-backed mappings share the mirror's data, which drop_mirror frees. */
static void release_maps_of(uint32_t buffer)
{
    void *ctx = CGLGetCurrentContext();
    for (;;) {
        uint32_t shadow = 0;
        pthread_mutex_lock(&g_map_lock);
        for (int i = 0; i < MAX_MAPS && shadow == 0; i++) {
            if (g_maps[i].target != 0 && g_maps[i].real != NULL && g_maps[i].buffer == buffer &&
                g_maps[i].ctx == ctx) {
                shadow = g_maps[i].shadow;
                g_maps[i].target = 0;
            }
        }
        pthread_mutex_unlock(&g_map_lock);
        if (shadow == 0)
            break;
        guest_free(shadow);
    }
    drop_mirror(ctx, buffer);
}

/* The shadow starts as a copy of the buffer for every access mode, so a
 * guest that writes only part of a write-only mapping does not overwrite the
 * rest with garbage on unmap. */
static void t_MapBuffer(void)
{
    uint32_t target = U(0), access = U(1);
    uint32_t result = 0;
    hud_count_map();
    if (target == GL_ARRAY_BUFFER) {
        uint32_t buffer = bound_buffer(target);
        void *ctx = CGLGetCurrentContext();
        if (access == GL_WRITE_ONLY) {
            pthread_mutex_lock(&g_map_lock);
            int m = find_mirror(ctx, buffer);
            if (m >= 0 && find_map(target, buffer, ctx) < 0) {
                for (int i = 0; i < MAX_MAPS; i++) {
                    if (g_maps[i].target != 0)
                        continue;
                    g_maps[i].target = target;
                    g_maps[i].buffer = buffer;
                    g_maps[i].ctx = ctx;
                    g_maps[i].real = NULL;
                    g_maps[i].shadow = g_mirrors[m].data;
                    g_maps[i].size = g_mirrors[m].size;
                    g_maps[i].access = access;
                    g_mirrors[m].last = ++g_mirror_clock;
                    result = g_mirrors[m].data;
                    break;
                }
            }
            pthread_mutex_unlock(&g_map_lock);
            if (result != 0) {
                g_eax = result;
                g_esp += 4;
                return;
            }
        } else {
            drop_mirror(ctx, buffer);
        }
    }
    void *real = glMapBuffer(target, access);
    if (real != NULL) {
        GLint size = 0;
        glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
        uint32_t shadow = guest_malloc(size > 0 ? (uint32_t)size : 1u);
        uint32_t buffer = bound_buffer(target);
        void *ctx = CGLGetCurrentContext();
        pthread_mutex_lock(&g_map_lock);
        int free_slot = -1;
        for (int i = 0; i < MAX_MAPS && free_slot < 0; i++)
            if (g_maps[i].target == 0)
                free_slot = i;
        if (shadow != 0 && free_slot >= 0) {
            memcpy(guest_ptr(shadow), real, (size_t)size);
            g_maps[free_slot].target = target;
            g_maps[free_slot].buffer = buffer;
            g_maps[free_slot].ctx = ctx;
            g_maps[free_slot].real = real;
            g_maps[free_slot].shadow = shadow;
            g_maps[free_slot].size = (uint32_t)size;
            g_maps[free_slot].access = access;
            result = shadow;
        }
        pthread_mutex_unlock(&g_map_lock);
        if (result == 0) {
            glUnmapBuffer(target);
            if (shadow != 0)
                guest_free(shadow);
        }
    }
    g_eax = result;
    g_esp += 4;
}

/* A write-only array map that went through GL keeps its shadow as a mirror of the buffer. */
static void make_mirror(void *ctx, uint32_t buffer, uint32_t shadow, uint32_t size)
{
    uint32_t clean = size > 0 ? guest_malloc(size) : 0;
    if (clean == 0) {
        guest_free(shadow);
        return;
    }
    memcpy(guest_ptr(clean), guest_ptr(shadow), size);
    GLint usage = GL_STREAM_DRAW;
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_USAGE, &usage);
    drop_mirror(ctx, buffer);
    uint32_t old_data = 0, old_clean = 0;
    pthread_mutex_lock(&g_map_lock);
    int slot = -1;
    for (int i = 0; i < MAX_MIRRORS && slot < 0; i++)
        if (g_mirrors[i].buffer == 0)
            slot = i;
    int evict = -1; /* the least recently mapped mirror that no live mapping uses */
    for (int i = 0; i < MAX_MIRRORS && slot < 0; i++) {
        int busy = 0;
        for (int k = 0; k < MAX_MAPS; k++)
            if (g_maps[k].target != 0 && g_maps[k].shadow == g_mirrors[i].data)
                busy = 1;
        if (!busy && (evict < 0 || g_mirrors[i].last < g_mirrors[evict].last))
            evict = i;
    }
    if (slot < 0)
        slot = evict;
    if (slot >= 0) {
        old_data = g_mirrors[slot].data;
        old_clean = g_mirrors[slot].clean;
        g_mirrors[slot].ctx = ctx;
        g_mirrors[slot].buffer = buffer;
        g_mirrors[slot].data = shadow;
        g_mirrors[slot].clean = clean;
        g_mirrors[slot].size = size;
        g_mirrors[slot].usage = (uint32_t)usage;
        g_mirrors[slot].last = ++g_mirror_clock;
    }
    pthread_mutex_unlock(&g_map_lock);
    if (slot < 0) {
        guest_free(shadow);
        guest_free(clean);
        return;
    }
    if (old_data != 0)
        guest_free(old_data);
    if (old_clean != 0)
        guest_free(old_clean);
}

static void t_UnmapBuffer(void)
{
    uint32_t target = U(0), buffer = bound_buffer(target);
    void *ctx = CGLGetCurrentContext();
    void *real = NULL;
    uint32_t shadow = 0, size = 0, access = 0, clean = 0, usage = GL_STREAM_DRAW;
    pthread_mutex_lock(&g_map_lock);
    int i = find_map(target, buffer, ctx);
    if (i >= 0) {
        real = g_maps[i].real;
        shadow = g_maps[i].shadow;
        size = g_maps[i].size;
        access = g_maps[i].access;
        g_maps[i].target = 0;
        if (real == NULL) {
            int m = find_mirror(ctx, buffer);
            if (m >= 0) {
                clean = g_mirrors[m].clean;
                usage = g_mirrors[m].usage;
            }
        }
    }
    pthread_mutex_unlock(&g_map_lock);
    if (shadow != 0 && real == NULL) {
        if (clean != 0) {
            uint8_t *d = guest_ptr(shadow), *c = guest_ptr(clean);
            uint32_t lo, hi;
            changed_range(d, c, size, &lo, &hi);
            if (hi > lo) {
                if (hi - lo <= MIRROR_SUBDATA_MAX)
                    glBufferSubData(target, lo, hi - lo, d + lo);
                else
                    glBufferData(target, size, d, usage);
                memcpy(c + lo, d + lo, hi - lo);
            }
        }
        g_eax = GL_TRUE;
        g_esp += 4;
        return;
    }
    if (shadow != 0 && access != GL_READ_ONLY)
        memcpy(real, guest_ptr(shadow), size);
    g_eax = glUnmapBuffer(target);
    if (shadow != 0) {
        if (target == GL_ARRAY_BUFFER && access == GL_WRITE_ONLY && g_eax == GL_TRUE)
            make_mirror(ctx, buffer, shadow, size);
        else
            guest_free(shadow);
    }
    g_esp += 4;
}

/* void glGetBufferPointerv(target, pname, void **params): the guest sees the
 * shadow, and 0 when the buffer is not mapped. */
static void t_GetBufferPointerv(void)
{
    uint32_t target = U(0), buffer = bound_buffer(target), shadow = 0;
    pthread_mutex_lock(&g_map_lock);
    int i = find_map(target, buffer, CGLGetCurrentContext());
    if (i >= 0)
        shadow = g_maps[i].shadow;
    pthread_mutex_unlock(&g_map_lock);
    if (U(1) == GL_BUFFER_MAP_POINTER && ARG(2) != 0)
        MEM32(ARG(2)) = shadow;
    g_esp += 4;
}

/* ---- thunks with plain scalar and guest-pointer arguments ---------------- */

#define DEF_V(id, call) static void t_##id(void) { call; g_esp += 4; }
#define DEF_R(id, call) static void t_##id(void) { g_eax = (uint32_t)(call); g_esp += 4; }

DEF_V(GenBuffers, glGenBuffers(S(0), P(1)))
static void t_BufferData(void)
{
    if (U(0) == GL_ARRAY_BUFFER)
        drop_mirror(CGLGetCurrentContext(), bound_buffer(GL_ARRAY_BUFFER));
    glBufferData(U(0), Z(1), P(2), U(3));
    g_esp += 4;
}

/* Keeps the mirror of the bound array buffer equal to the GL contents. */
static void t_BufferSubData(void)
{
    GLintptr offset = Z(1);
    GLsizeiptr size = Z(2);
    const void *src = P(3);
    glBufferSubData(U(0), offset, size, src);
    if (U(0) == GL_ARRAY_BUFFER && src != NULL && offset >= 0 && size >= 0) {
        void *ctx = CGLGetCurrentContext();
        uint32_t buffer = bound_buffer(GL_ARRAY_BUFFER);
        pthread_mutex_lock(&g_map_lock);
        int m = find_mirror(ctx, buffer);
        if (m >= 0 && (uint64_t)offset + (uint64_t)size <= g_mirrors[m].size) {
            memcpy((uint8_t *)guest_ptr(g_mirrors[m].data) + offset, src, (size_t)size);
            memcpy((uint8_t *)guest_ptr(g_mirrors[m].clean) + offset, src, (size_t)size);
        }
        pthread_mutex_unlock(&g_map_lock);
    }
    g_esp += 4;
}
DEF_V(GetBufferSubData, glGetBufferSubData(U(0), Z(1), Z(2), P(3)))
DEF_V(GetBufferParameteriv, glGetBufferParameteriv(U(0), U(1), P(2)))
DEF_R(IsBuffer, glIsBuffer(U(0)))
DEF_V(EnableVertexAttribArray, glEnableVertexAttribArray(U(0)))
DEF_V(DisableVertexAttribArray, glDisableVertexAttribArray(U(0)))
DEF_V(MultiDrawArrays, (hud_count_draw(), glMultiDrawArrays(U(0), P(1), P(2), S(3))))
DEF_V(DrawBuffers, glDrawBuffers(S(0), P(1)))
DEF_V(ActiveTexture, glActiveTexture(U(0)))
DEF_V(ClientActiveTexture, glClientActiveTexture(U(0)))
DEF_V(MultiTexCoord2f, glMultiTexCoord2f(U(0), FL(1), FL(2)))
DEF_V(MultiTexCoord2fv, glMultiTexCoord2fv(U(0), P(1)))
DEF_V(MultiTexCoord3f, glMultiTexCoord3f(U(0), FL(1), FL(2), FL(3)))
DEF_V(MultiTexCoord3fv, glMultiTexCoord3fv(U(0), P(1)))
DEF_V(MultiTexCoord4f, glMultiTexCoord4f(U(0), FL(1), FL(2), FL(3), FL(4)))
DEF_V(MultiTexCoord4fv, glMultiTexCoord4fv(U(0), P(1)))

/* ARB_vertex_program and ARB_fragment_program (the Cg arbvp1/arbfp1 profiles) */
DEF_V(GenProgramsARB, glGenProgramsARB(S(0), P(1)))
DEF_V(DeleteProgramsARB, glDeleteProgramsARB(S(0), P(1)))
DEF_V(BindProgramARB, glBindProgramARB(U(0), U(1)))
DEF_R(IsProgramARB, glIsProgramARB(U(0)))
/* Loads the ARB assembly the game gets from Cg; prints loads over 2 ms. */
static void t_ProgramStringARB(void)
{
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    glProgramStringARB(U(0), U(1), S(2), P(3));
    uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (t1 - t0 > 2000000u)
        fprintf(stderr, "gl: ProgramStringARB took %.1f ms at %.3f s (uptime)\n", (double)(t1 - t0) / 1e6,
                (double)t1 / 1e9);
    g_esp += 4;
}
DEF_V(ProgramEnvParameter4fARB, glProgramEnvParameter4fARB(U(0), U(1), FL(2), FL(3), FL(4), FL(5)))
DEF_V(ProgramEnvParameter4fvARB, glProgramEnvParameter4fvARB(U(0), U(1), P(2)))
DEF_V(ProgramLocalParameter4fARB,
      glProgramLocalParameter4fARB(U(0), U(1), FL(2), FL(3), FL(4), FL(5)))
DEF_V(ProgramLocalParameter4fvARB, glProgramLocalParameter4fvARB(U(0), U(1), P(2)))
DEF_V(ProgramEnvParameters4fvEXT, glProgramEnvParameters4fvEXT(U(0), U(1), S(2), P(3)))
DEF_V(ProgramLocalParameters4fvEXT, glProgramLocalParameters4fvEXT(U(0), U(1), S(2), P(3)))
DEF_V(GetProgramivARB, glGetProgramivARB(U(0), U(1), P(2)))

/* EXT_framebuffer_object */
DEF_V(GenFramebuffersEXT, glGenFramebuffersEXT(S(0), P(1)))
DEF_V(BindFramebufferEXT, glBindFramebufferEXT(U(0), U(1)))
DEF_V(DeleteFramebuffersEXT, glDeleteFramebuffersEXT(S(0), P(1)))
/* The game never compares the result (no framebuffer status constant appears
 * in its code), and with the multithreaded GL engine every real call makes the
 * main thread wait for the whole GL queue: 37% of its time. */
static void t_CheckFramebufferStatusEXT(void)
{
    g_eax = GL_FRAMEBUFFER_COMPLETE_EXT;
    g_esp += 4;
}
DEF_V(FramebufferTexture2DEXT, glFramebufferTexture2DEXT(U(0), U(1), U(2), U(3), S(4)))
DEF_V(FramebufferRenderbufferEXT, glFramebufferRenderbufferEXT(U(0), U(1), U(2), U(3)))
DEF_V(GenRenderbuffersEXT, glGenRenderbuffersEXT(S(0), P(1)))
DEF_V(BindRenderbufferEXT, glBindRenderbufferEXT(U(0), U(1)))
DEF_V(DeleteRenderbuffersEXT, glDeleteRenderbuffersEXT(S(0), P(1)))
DEF_V(RenderbufferStorageEXT, glRenderbufferStorageEXT(U(0), U(1), S(2), S(3)))

/* ---- shader sources (GL 2.0 glShaderSource) ------------------------------ */

/* Builds the host string array from `count` guest string pointers. The length
 * array holds 32-bit ints in both address spaces, so it only needs guest_ptr. */
static void shader_source(uint32_t shader, int32_t count, uint32_t guest_strings, uint32_t guest_len,
                          void (*set)(uint32_t, GLsizei, const GLchar *const *, const GLint *))
{
    const uint32_t *gs = gp(guest_strings);
    const GLchar **strs = count > 0 ? calloc((size_t)count, sizeof *strs) : NULL;
    for (int32_t i = 0; i < count && strs != NULL && gs != NULL; i++)
        strs[i] = gp(gs[i]);
    if (count <= 0 || (strs != NULL && gs != NULL))
        set(shader, count, strs, gp(guest_len));
    free(strs);
}

static void src_core(uint32_t s, GLsizei n, const GLchar *const *str, const GLint *len)
{
    glShaderSource(s, n, str, len);
}

static void t_ShaderSource(void)
{
    shader_source(U(0), S(1), U(2), U(3), src_core);
    g_esp += 4;
}

/* ---- GLhandleARB and GLsync: host pointers behind small guest names ------- */

/* A host GLhandleARB or GLsync is a pointer (8 bytes); the guest has 4. Each
 * table hands out a dense counter from 1, like GL names, and 0 is NULL. A host
 * value keeps its guest name for the life of the process, so a name the host
 * reuses after a delete maps to the same guest value. */
struct handle_map {
    void **host; /* host[guest - 1] */
    uint32_t count, cap;
    pthread_mutex_t lock;
};

static struct handle_map g_obj_map = {.lock = PTHREAD_MUTEX_INITIALIZER};
static struct handle_map g_sync_map = {.lock = PTHREAD_MUTEX_INITIALIZER};

static uint32_t map_to_guest(struct handle_map *m, void *host)
{
    if (host == NULL)
        return 0;
    uint32_t guest = 0;
    pthread_mutex_lock(&m->lock);
    for (uint32_t i = 0; i < m->count && guest == 0; i++)
        if (m->host[i] == host)
            guest = i + 1;
    if (guest == 0) {
        if (m->count == m->cap) {
            uint32_t cap = m->cap ? m->cap * 2 : 64;
            void **grown = realloc(m->host, cap * sizeof *grown);
            if (grown != NULL) {
                m->host = grown;
                m->cap = cap;
            }
        }
        if (m->count < m->cap) {
            m->host[m->count++] = host;
            guest = m->count;
        }
    }
    pthread_mutex_unlock(&m->lock);
    return guest;
}

static void *map_to_host(struct handle_map *m, uint32_t guest)
{
    void *host = NULL;
    pthread_mutex_lock(&m->lock);
    if (guest != 0 && guest <= m->count)
        host = m->host[guest - 1];
    pthread_mutex_unlock(&m->lock);
    return host;
}

#define OBJ_H(n) ((GLhandleARB)map_to_host(&g_obj_map, U(n)))
#define OBJ_G(host) map_to_guest(&g_obj_map, (void *)(host))
#define SYNC_H(n) ((GLsync)map_to_host(&g_sync_map, U(n)))
#define U64(n) ((uint64_t)U(n) | ((uint64_t)U((n) + 1) << 32))

/* ARB_shader_objects */
DEF_V(DeleteObjectARB, glDeleteObjectARB(OBJ_H(0)))
DEF_R(GetHandleARB, OBJ_G(glGetHandleARB(U(0))))
DEF_V(DetachObjectARB, glDetachObjectARB(OBJ_H(0), OBJ_H(1)))
DEF_R(CreateShaderObjectARB, OBJ_G(glCreateShaderObjectARB(U(0))))
DEF_V(CompileShaderARB, glCompileShaderARB(OBJ_H(0)))
DEF_R(CreateProgramObjectARB, OBJ_G(glCreateProgramObjectARB()))
DEF_V(AttachObjectARB, glAttachObjectARB(OBJ_H(0), OBJ_H(1)))
DEF_V(LinkProgramARB, glLinkProgramARB(OBJ_H(0)))
DEF_V(UseProgramObjectARB, glUseProgramObjectARB(OBJ_H(0)))
DEF_V(ValidateProgramARB, glValidateProgramARB(OBJ_H(0)))
DEF_V(GetObjectParameterfvARB, glGetObjectParameterfvARB(OBJ_H(0), U(1), P(2)))
DEF_V(GetObjectParameterivARB, glGetObjectParameterivARB(OBJ_H(0), U(1), P(2)))
DEF_V(GetInfoLogARB, glGetInfoLogARB(OBJ_H(0), S(1), P(2), P(3)))
DEF_R(GetUniformLocationARB, glGetUniformLocationARB(OBJ_H(0), P(1)))
DEF_V(GetActiveUniformARB, glGetActiveUniformARB(OBJ_H(0), U(1), S(2), P(3), P(4), P(5), P(6)))
DEF_V(GetUniformfvARB, glGetUniformfvARB(OBJ_H(0), S(1), P(2)))
DEF_V(GetUniformivARB, glGetUniformivARB(OBJ_H(0), S(1), P(2)))
DEF_V(GetShaderSourceARB, glGetShaderSourceARB(OBJ_H(0), S(1), P(2), P(3)))
DEF_V(BindAttribLocationARB, glBindAttribLocationARB(OBJ_H(0), U(1), P(2)))
DEF_V(GetActiveAttribARB, glGetActiveAttribARB(OBJ_H(0), U(1), S(2), P(3), P(4), P(5), P(6)))
DEF_R(GetAttribLocationARB, glGetAttribLocationARB(OBJ_H(0), P(1)))

static void src_arb(uint32_t s, GLsizei n, const GLchar *const *str, const GLint *len)
{
    glShaderSourceARB(map_to_host(&g_obj_map, s), n, str, len);
}

static void t_ShaderSourceARB(void)
{
    shader_source(U(0), S(1), U(2), U(3), src_arb);
    g_esp += 4;
}

/* void glGetAttachedObjectsARB(container, maxCount, GLsizei *count, GLhandleARB *obj):
 * the host writes 8-byte handles; the guest gets 4-byte names. */
static void t_GetAttachedObjectsARB(void)
{
    int32_t max = S(1);
    GLsizei got = 0;
    GLhandleARB *host = max > 0 ? calloc((size_t)max, sizeof *host) : NULL;
    uint32_t *out = P(3);
    if (max > 0 && host != NULL) {
        glGetAttachedObjectsARB(OBJ_H(0), max, &got, host);
        for (GLsizei i = 0; i < got && out != NULL; i++)
            out[i] = OBJ_G(host[i]);
        if (P(2) != NULL)
            *(GLsizei *)P(2) = got;
    }
    free(host);
    g_esp += 4;
}

/* ARB_sync / GL 3.2 sync objects: GLsync is a host pointer. */
DEF_R(FenceSync, map_to_guest(&g_sync_map, glFenceSync(U(0), U(1))))
DEF_R(IsSync, glIsSync(SYNC_H(0)))
DEF_V(DeleteSync, glDeleteSync(SYNC_H(0)))
DEF_R(ClientWaitSync, glClientWaitSync(SYNC_H(0), U(1), U64(2)))
DEF_V(WaitSync, glWaitSync(SYNC_H(0), U(1), U64(2)))
DEF_V(GetSynciv, glGetSynciv(SYNC_H(0), U(1), S(2), P(3), P(4)))

/* ---- name table ---------------------------------------------------------- */

struct gl_entry {
    const char *name;
    recomp_func_t fn;
};

#define E(id) {"gl" #id, t_##id}
#define E_ARB(id) {"gl" #id, t_##id}, {"gl" #id "ARB", t_##id}
#define E_EXT(id) {"gl" #id, t_##id}, {"gl" #id "EXT", t_##id}

static const struct gl_entry g_table[] = {
    E_ARB(BindBuffer), E_ARB(DeleteBuffers), E_ARB(GenBuffers), E_ARB(BufferData),
    E_ARB(BufferSubData), E_ARB(GetBufferSubData), E_ARB(GetBufferParameteriv),
    E_ARB(IsBuffer), E_ARB(MapBuffer), E_ARB(UnmapBuffer), E_ARB(GetBufferPointerv),
    E_ARB(VertexAttribPointer), E_ARB(GetVertexAttribPointerv),
    E_ARB(EnableVertexAttribArray), E_ARB(DisableVertexAttribArray),
    E_EXT(DrawRangeElements), E_EXT(MultiDrawElements), E_EXT(MultiDrawArrays),
    E_ARB(DrawBuffers),
    E_ARB(ActiveTexture), E_ARB(ClientActiveTexture),
    E_ARB(MultiTexCoord2f), E_ARB(MultiTexCoord2fv), E_ARB(MultiTexCoord3f),
    E_ARB(MultiTexCoord3fv), E_ARB(MultiTexCoord4f), E_ARB(MultiTexCoord4fv),
    E_ARB(CompressedTexImage1D), E_ARB(CompressedTexImage2D), E_ARB(CompressedTexImage3D),
    E_ARB(CompressedTexSubImage1D), E_ARB(CompressedTexSubImage2D),
    E_ARB(CompressedTexSubImage3D), E_ARB(GetCompressedTexImage),
    E(GenProgramsARB), E(DeleteProgramsARB), E(BindProgramARB), E(IsProgramARB),
    E(ProgramStringARB), E(ProgramEnvParameter4fARB), E(ProgramEnvParameter4fvARB),
    E(ProgramLocalParameter4fARB), E(ProgramLocalParameter4fvARB),
    E(ProgramEnvParameters4fvEXT), E(ProgramLocalParameters4fvEXT), E(GetProgramivARB),
    E(GenFramebuffersEXT), E(BindFramebufferEXT), E(DeleteFramebuffersEXT),
    E(CheckFramebufferStatusEXT), E(FramebufferTexture2DEXT),
    E(FramebufferRenderbufferEXT), E(GenRenderbuffersEXT), E(BindRenderbufferEXT),
    E(DeleteRenderbuffersEXT), E(RenderbufferStorageEXT),
    E(ShaderSource),
    E(DeleteObjectARB), E(GetHandleARB), E(DetachObjectARB), E(CreateShaderObjectARB),
    E(ShaderSourceARB), E(CompileShaderARB), E(CreateProgramObjectARB), E(AttachObjectARB),
    E(LinkProgramARB), E(UseProgramObjectARB), E(ValidateProgramARB),
    E(GetObjectParameterfvARB), E(GetObjectParameterivARB), E(GetInfoLogARB),
    E(GetAttachedObjectsARB), E(GetUniformLocationARB), E(GetActiveUniformARB),
    E(GetUniformfvARB), E(GetUniformivARB), E(GetShaderSourceARB), E(BindAttribLocationARB),
    E(GetActiveAttribARB), E(GetAttribLocationARB),
    E(FenceSync), E(IsSync), E(DeleteSync), E(ClientWaitSync), E(WaitSync), E(GetSynciv),
};

recomp_func_t gl_ext_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof g_table / sizeof g_table[0]; i++)
        if (strcmp(g_table[i].name, name) == 0)
            return g_table[i].fn;
    return gl_ext_auto_lookup(name);
}

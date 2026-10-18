/* Unit test for runtime/src/thunk_gl.c and imp__dlsym. Uses a hidden CGL
 * context (no drawable). Prints "thunk_gl ok" and exits 0 when every check
 * passes. */
#define GL_SILENCE_DEPRECATION
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "guest_heap.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "recomp_types.h"
#include "thunk_gl.h"

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define STACK_TOP 0x00FF0000u
#define SCRATCH 0x00F10000u
#define CLIENT_DATA 0x00F20000u
#define RET_MAGIC 0xDEADBEEFu

void imp__dlopen(void);
void imp__dlsym(void);
void imp__glTexImage2D(void);

static int g_failures;

/* host_funcs.c needs the generated dispatch; there is none in this test. */
recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL line %d: ", __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
            g_failures++; \
        } \
    } while (0)

/* Call a thunk with n 32-bit cdecl arguments; returns eax. */
static uint32_t call(recomp_func_t fn, int n, const uint32_t *args)
{
    uint32_t top = STACK_TOP - 4u * (uint32_t)(n + 1);
    g_esp = top;
    MEM32(g_esp) = RET_MAGIC;
    for (int i = 0; i < n; i++)
        MEM32(g_esp + 4u + 4u * (uint32_t)i) = args[i];
    g_eax = 0xCCCCCCCCu;
    fn();
    CHECK(g_esp == top + 4, "thunk left esp 0x%x, want 0x%x", g_esp, top + 4);
    return g_eax;
}

static recomp_func_t gl(const char *name)
{
    recomp_func_t fn = gl_ext_lookup(name);
    CHECK(fn != NULL, "%s missing from the GL table", name);
    return fn;
}

static void test_dlsym(void)
{
    uint32_t h = call(imp__dlopen, 0, NULL);
    CHECK(h != 0, "dlopen must return a non-zero handle");

    const char *names[] = {"glBindBufferARB", "glMapBufferARB", "glProgramStringARB"};
    for (int i = 0; i < 3; i++) {
        strcpy(guest_ptr(SCRATCH), names[i]);
        uint32_t args[2] = {h, SCRATCH};
        uint32_t va = call(imp__dlsym, 2, args);
        CHECK(va >= HOST_FUNC_VA_BASE, "dlsym(%s) = 0x%08x, want a host-func VA", names[i], va);
        CHECK(recomp_lookup_kernel(va) == gl_ext_lookup(names[i]), "dlsym(%s) maps to the wrong thunk",
              names[i]);
    }
    strcpy(guest_ptr(SCRATCH), "glNoSuchFunctionXYZ");
    uint32_t args[2] = {h, SCRATCH};
    CHECK(call(imp__dlsym, 2, args) == 0, "unknown name must give 0");
    /* A name that is an import (the generated table is absent here, so only
     * the NULL-name guard is checked). */
    uint32_t nullname[2] = {h, 0};
    CHECK(call(imp__dlsym, 2, nullname) == 0, "NULL name must give 0");
}

static void *host_attrib_pointer(GLuint index)
{
    void *p = (void *)0x1234;
    glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &p);
    return p;
}

static uint32_t gen_buffer(void)
{
    uint32_t args[2] = {1, SCRATCH + 0x100};
    call(gl("glGenBuffersARB"), 2, args);
    return MEM32(SCRATCH + 0x100);
}

static void test_vbo_offsets(void)
{
    uint32_t vbo = gen_buffer();
    CHECK(vbo != 0, "glGenBuffersARB gave 0");
    uint32_t bind_vbo[2] = {GL_ARRAY_BUFFER, vbo};
    call(gl("glBindBufferARB"), 2, bind_vbo);
    uint32_t data[4] = {GL_ARRAY_BUFFER, 256, 0, GL_STATIC_DRAW};
    call(gl("glBufferDataARB"), 4, data);

    /* Buffer bound: the pointer is an offset and passes through unchanged. */
    uint32_t ptr_args[6] = {0, 3, GL_FLOAT, 0, 12, 0x40};
    call(gl("glVertexAttribPointerARB"), 6, ptr_args);
    CHECK(host_attrib_pointer(0) == (void *)(uintptr_t)0x40, "VBO offset: host saw %p",
          host_attrib_pointer(0));
    uint32_t get_args[3] = {0, GL_VERTEX_ATTRIB_ARRAY_POINTER, SCRATCH + 0x200};
    call(gl("glGetVertexAttribPointerv"), 3, get_args);
    CHECK(MEM32(SCRATCH + 0x200) == 0x40, "guest glGetVertexAttribPointerv gave 0x%x",
          MEM32(SCRATCH + 0x200));

    /* No buffer: the pointer is a guest address. */
    uint32_t unbind[2] = {GL_ARRAY_BUFFER, 0};
    call(gl("glBindBufferARB"), 2, unbind);
    ptr_args[5] = CLIENT_DATA;
    call(gl("glVertexAttribPointerARB"), 6, ptr_args);
    CHECK(host_attrib_pointer(0) == guest_ptr(CLIENT_DATA), "client array: host saw %p, want %p",
          host_attrib_pointer(0), guest_ptr(CLIENT_DATA));
    call(gl("glGetVertexAttribPointerv"), 3, get_args);
    CHECK(MEM32(SCRATCH + 0x200) == CLIENT_DATA, "guest glGetVertexAttribPointerv gave 0x%x",
          MEM32(SCRATCH + 0x200));

    /* Deleting the bound buffer unbinds it, so the cache must follow. */
    call(gl("glBindBufferARB"), 2, bind_vbo);
    MEM32(SCRATCH + 0x300) = vbo;
    uint32_t del[2] = {1, SCRATCH + 0x300};
    call(gl("glDeleteBuffersARB"), 2, del);
    call(gl("glVertexAttribPointerARB"), 6, ptr_args);
    CHECK(host_attrib_pointer(0) == guest_ptr(CLIENT_DATA), "after delete: host saw %p",
          host_attrib_pointer(0));
}

static void test_pbo_upload(void)
{
    uint32_t pbo = gen_buffer();
    uint32_t bind_pbo[2] = {GL_PIXEL_UNPACK_BUFFER, pbo};
    call(gl("glBindBufferARB"), 2, bind_pbo);
    uint32_t data[4] = {GL_PIXEL_UNPACK_BUFFER, 32, 0, GL_STREAM_DRAW};
    call(gl("glBufferDataARB"), 4, data);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    while (glGetError() != GL_NO_ERROR) {
    }
    /* 2x2 RGBA8 is 16 bytes; offset 4 is valid for the 32-byte buffer. A host
     * pointer from guest_ptr(4) would be out of range and set an error. */
    uint32_t args[9] = {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, 4};
    call(imp__glTexImage2D, 9, args);
    GLenum err = glGetError();
    CHECK(err == GL_NO_ERROR, "glTexImage2D with a PBO offset: GL error 0x%x", err);

    uint32_t unbind[2] = {GL_PIXEL_UNPACK_BUFFER, 0};
    call(gl("glBindBufferARB"), 2, unbind);
    memset(guest_ptr(CLIENT_DATA), 0x7F, 16);
    args[8] = CLIENT_DATA;
    call(imp__glTexImage2D, 9, args);
    err = glGetError();
    CHECK(err == GL_NO_ERROR, "glTexImage2D from client memory: GL error 0x%x", err);
}

/* Client-memory uploads go through the runtime's pixel buffer. Each upload
 * must read back identical and leave the unpack binding at 0. */
static void check_staged_state(const char *name)
{
    GLint bound = -1;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &bound);
    CHECK(bound == 0, "%s: unpack binding %d, want 0", name, bound);
    GLenum err = glGetError();
    CHECK(err == GL_NO_ERROR, "%s: GL error 0x%x", name, err);
}

static GLuint staged_upload(uint32_t ifmt, uint32_t w, uint32_t h, uint32_t fmt, uint32_t type,
                            const void *src, size_t src_bytes)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    while (glGetError() != GL_NO_ERROR) {
    }
    memcpy(guest_ptr(CLIENT_DATA), src, src_bytes);
    uint32_t args[9] = {GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, CLIENT_DATA};
    call(imp__glTexImage2D, 9, args);
    return tex;
}

static void test_tex_staged(void)
{
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    uint8_t lum[128 * 128], lum_got[128 * 128];
    for (uint32_t i = 0; i < sizeof lum; i++)
        lum[i] = (uint8_t)(i * 7 + (i >> 8));
    GLuint tex = staged_upload(GL_LUMINANCE8, 128, 128, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum,
                               sizeof lum);
    check_staged_state("LUMINANCE8 128x128");
    glGetTexImage(GL_TEXTURE_2D, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum_got);
    CHECK(memcmp(lum, lum_got, sizeof lum) == 0, "LUMINANCE8 128x128: read-back differs");
    glDeleteTextures(1, &tex);

    /* 3x3 RGB: 9 data bytes + 3 pad per row; the last row is unpadded. */
    uint8_t rgb[33], rgb_got[36];
    memset(rgb, 0xEE, sizeof rgb);
    memset(rgb_got, 0, sizeof rgb_got);
    for (uint32_t y = 0; y < 3; y++)
        for (uint32_t x = 0; x < 9; x++)
            rgb[y * 12 + x] = (uint8_t)(y * 40 + x + 1);
    tex = staged_upload(GL_RGB, 3, 3, GL_RGB, GL_UNSIGNED_BYTE, rgb, sizeof rgb);
    check_staged_state("RGB 3x3");
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb_got);
    for (uint32_t y = 0; y < 3; y++)
        CHECK(memcmp(rgb + y * 12, rgb_got + y * 12, 9) == 0, "RGB 3x3: row %u differs", y);
    glDeleteTextures(1, &tex);

    uint32_t px[16], px_got[16];
    for (uint32_t i = 0; i < 16; i++)
        px[i] = 0x80402010u + i * 0x01010101u;
    tex = staged_upload(GL_RGBA, 4, 4, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px, sizeof px);
    check_staged_state("BGRA 8888_REV 4x4");
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px_got);
    CHECK(memcmp(px, px_got, sizeof px) == 0, "BGRA 8888_REV 4x4: read-back differs");
    glDeleteTextures(1, &tex);
}

static void test_map_roundtrip(void)
{
    uint32_t vbo = gen_buffer();
    uint32_t bind[2] = {GL_ARRAY_BUFFER, vbo};
    call(gl("glBindBufferARB"), 2, bind);
    uint8_t init[16];
    for (int i = 0; i < 16; i++)
        init[i] = (uint8_t)(0x10 + i);
    memcpy(guest_ptr(CLIENT_DATA), init, sizeof init);
    uint32_t data[4] = {GL_ARRAY_BUFFER, 16, CLIENT_DATA, GL_DYNAMIC_DRAW};
    call(gl("glBufferDataARB"), 4, data);

    uint32_t map[2] = {GL_ARRAY_BUFFER, GL_READ_WRITE};
    uint32_t shadow = call(gl("glMapBufferARB"), 2, map);
    CHECK(shadow >= GUEST_HEAP_BASE && shadow < GUEST_HEAP_END, "shadow 0x%08x outside the guest heap",
          shadow);
    if (shadow == 0)
        return;
    CHECK(memcmp(guest_ptr(shadow), init, 16) == 0, "shadow lacks the buffer contents");

    uint32_t ptrv[3] = {GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, SCRATCH + 0x400};
    call(gl("glGetBufferPointervARB"), 3, ptrv);
    CHECK(MEM32(SCRATCH + 0x400) == shadow, "glGetBufferPointervARB gave 0x%x", MEM32(SCRATCH + 0x400));

    for (int i = 0; i < 8; i++)
        ((uint8_t *)guest_ptr(shadow))[i] = (uint8_t)(0xA0 + i); /* partial write */
    uint32_t unmap[1] = {GL_ARRAY_BUFFER};
    CHECK(call(gl("glUnmapBufferARB"), 1, unmap) == GL_TRUE, "glUnmapBufferARB failed");

    uint8_t back[16];
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, 16, back);
    for (int i = 0; i < 16; i++) {
        uint8_t want = i < 8 ? (uint8_t)(0xA0 + i) : init[i];
        CHECK(back[i] == want, "byte %d: buffer has 0x%02x, want 0x%02x", i, back[i], want);
    }
    ptrv[1] = GL_BUFFER_MAP_POINTER;
    call(gl("glGetBufferPointervARB"), 3, ptrv);
    CHECK(MEM32(SCRATCH + 0x400) == 0, "pointer must be 0 after unmap");

    /* Write-only mapping. */
    map[1] = GL_WRITE_ONLY;
    shadow = call(gl("glMapBufferARB"), 2, map);
    CHECK(shadow != 0, "write-only map failed");
    if (shadow != 0) {
        ((uint8_t *)guest_ptr(shadow))[15] = 0xEE;
        CHECK(call(gl("glUnmapBufferARB"), 1, unmap) == GL_TRUE, "second unmap failed");
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, 16, back);
        CHECK(back[15] == 0xEE && back[0] == 0xA0, "write-only round trip: [0]=0x%02x [15]=0x%02x",
              back[0], back[15]);
    }
}

/* Two buffers mapped on the same target at once: each unmap and pointer query
 * must use the mapping of the buffer bound at that moment. */
static void test_map_two_buffers(void)
{
    uint32_t a = gen_buffer(), b = gen_buffer();
    uint8_t init_a[16], init_b[16], back[16];
    for (int i = 0; i < 16; i++) {
        init_a[i] = (uint8_t)(0x10 + i);
        init_b[i] = (uint8_t)(0x50 + i);
    }
    uint32_t bind_a[2] = {GL_ARRAY_BUFFER, a}, bind_b[2] = {GL_ARRAY_BUFFER, b};
    uint32_t map[2] = {GL_ARRAY_BUFFER, GL_READ_WRITE};
    uint32_t unmap[1] = {GL_ARRAY_BUFFER};
    uint32_t data[4] = {GL_ARRAY_BUFFER, 16, CLIENT_DATA, GL_DYNAMIC_DRAW};

    call(gl("glBindBufferARB"), 2, bind_a);
    memcpy(guest_ptr(CLIENT_DATA), init_a, 16);
    call(gl("glBufferDataARB"), 4, data);
    uint32_t shadow_a = call(gl("glMapBufferARB"), 2, map);

    call(gl("glBindBufferARB"), 2, bind_b);
    memcpy(guest_ptr(CLIENT_DATA), init_b, 16);
    call(gl("glBufferDataARB"), 4, data);
    uint32_t shadow_b = call(gl("glMapBufferARB"), 2, map);
    CHECK(shadow_a != 0 && shadow_b != 0 && shadow_a != shadow_b, "shadows 0x%x 0x%x", shadow_a, shadow_b);
    if (shadow_a == 0 || shadow_b == 0)
        return;
    ((uint8_t *)guest_ptr(shadow_a))[0] = 0xAA;
    ((uint8_t *)guest_ptr(shadow_b))[0] = 0xBB;

    uint32_t ptrv[3] = {GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, SCRATCH + 0x400};
    call(gl("glGetBufferPointervARB"), 3, ptrv);
    CHECK(MEM32(SCRATCH + 0x400) == shadow_b, "pointer with B bound: 0x%x, want 0x%x", MEM32(SCRATCH + 0x400),
          shadow_b);

    CHECK(call(gl("glUnmapBufferARB"), 1, unmap) == GL_TRUE, "unmap B failed");
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, 16, back);
    CHECK(back[0] == 0xBB && memcmp(back + 1, init_b + 1, 15) == 0, "buffer B holds [0]=0x%02x", back[0]);

    call(gl("glBindBufferARB"), 2, bind_a);
    call(gl("glGetBufferPointervARB"), 3, ptrv);
    CHECK(MEM32(SCRATCH + 0x400) == shadow_a, "pointer with A bound: 0x%x, want 0x%x", MEM32(SCRATCH + 0x400),
          shadow_a);
    CHECK(call(gl("glUnmapBufferARB"), 1, unmap) == GL_TRUE, "unmap A failed");
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, 16, back);
    CHECK(back[0] == 0xAA && memcmp(back + 1, init_a + 1, 15) == 0, "buffer A holds [0]=0x%02x", back[0]);

    /* Deleting a mapped buffer frees its slot: more deletes than MAX_MAPS (8) must keep mapping. */
    for (int i = 0; i < 10; i++) {
        uint32_t tmp = gen_buffer();
        uint32_t bind_tmp[2] = {GL_ARRAY_BUFFER, tmp};
        call(gl("glBindBufferARB"), 2, bind_tmp);
        call(gl("glBufferDataARB"), 4, data);
        CHECK(call(gl("glMapBufferARB"), 2, map) != 0, "map %d failed after deleting mapped buffers", i);
        MEM32(SCRATCH + 0x300) = tmp;
        uint32_t del[2] = {1, SCRATCH + 0x300};
        call(gl("glDeleteBuffersARB"), 2, del);
    }
    uint32_t unbind[2] = {GL_ARRAY_BUFFER, 0};
    call(gl("glBindBufferARB"), 2, unbind);
}

/* Write-only array maps reuse a guest mirror after the first map; every way the
 * buffer can change (writes, BufferSubData, BufferData, delete) must keep GL and
 * the mirror equal. */
static uint32_t wo_map(void)
{
    uint32_t map[2] = {GL_ARRAY_BUFFER, GL_WRITE_ONLY};
    return call(gl("glMapBufferARB"), 2, map);
}

static uint32_t wo_unmap(void)
{
    uint32_t unmap[1] = {GL_ARRAY_BUFFER};
    return call(gl("glUnmapBufferARB"), 1, unmap);
}

static uint32_t map_pointer(void)
{
    uint32_t ptrv[3] = {GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, SCRATCH + 0x400};
    call(gl("glGetBufferPointervARB"), 3, ptrv);
    return MEM32(SCRATCH + 0x400);
}

static void buffer_data(uint32_t size, const uint8_t *bytes)
{
    memcpy(guest_ptr(CLIENT_DATA), bytes, size);
    uint32_t data[4] = {GL_ARRAY_BUFFER, size, CLIENT_DATA, GL_STREAM_DRAW};
    call(gl("glBufferDataARB"), 4, data);
}

static void check_gl(const uint8_t *want, uint32_t n, const char *what)
{
    uint8_t back[128];
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, n, back);
    for (uint32_t i = 0; i < n; i++)
        if (back[i] != want[i]) {
            CHECK(0, "%s: byte %u is 0x%02x, want 0x%02x", what, i, back[i], want[i]);
            return;
        }
}

static void bind_array(uint32_t buffer)
{
    uint32_t bind[2] = {GL_ARRAY_BUFFER, buffer};
    call(gl("glBindBufferARB"), 2, bind);
}

static void test_map_mirror(void)
{
    uint8_t want[128];
    for (int i = 0; i < 64; i++)
        want[i] = (uint8_t)(0x30 + i);
    uint32_t vbo = gen_buffer();
    bind_array(vbo);
    buffer_data(64, want);

    /* 1: first map goes through GL */
    uint32_t p1 = wo_map();
    CHECK(p1 != 0, "map 1 failed");
    if (p1 == 0)
        return;
    CHECK(memcmp(guest_ptr(p1), want, 64) == 0, "map 1 lacks the buffer contents");
    for (int i = 0; i < 4; i++)
        ((uint8_t *)guest_ptr(p1))[i] = want[i] = (uint8_t)(0xA0 + i);
    CHECK(wo_unmap() == GL_TRUE, "unmap 1 failed");
    check_gl(want, 64, "after unmap 1");

    /* 2: mirror reuse */
    uint32_t p2 = wo_map();
    CHECK(p2 == p1, "map 2 gave 0x%x, want the mirror 0x%x", p2, p1);
    if (p2 == 0)
        return;
    CHECK(memcmp(guest_ptr(p2), want, 64) == 0, "map 2 lacks the current contents");
    CHECK(map_pointer() == p2, "pointer during map 2 is 0x%x", map_pointer());
    for (int i = 20; i < 30; i++)
        ((uint8_t *)guest_ptr(p2))[i] = want[i] = (uint8_t)(0xC0 + i);
    CHECK(wo_unmap() == GL_TRUE, "unmap 2 failed");
    check_gl(want, 64, "after unmap 2");
    CHECK(map_pointer() == 0, "pointer after unmap 2 is 0x%x", map_pointer());

    /* 3: map without writes */
    CHECK(wo_map() == p1, "map 3 lost the mirror");
    CHECK(wo_unmap() == GL_TRUE, "unmap 3 failed");
    check_gl(want, 64, "after unmap 3");

    /* 4: BufferSubData updates the mirror */
    uint8_t patch[8];
    for (int i = 0; i < 8; i++)
        patch[i] = (uint8_t)(0x70 + i);
    memcpy(guest_ptr(CLIENT_DATA), patch, 8);
    uint32_t sub[4] = {GL_ARRAY_BUFFER, 40, 8, CLIENT_DATA};
    call(gl("glBufferSubDataARB"), 4, sub);
    memcpy(want + 40, patch, 8);
    uint32_t p4 = wo_map();
    CHECK(p4 == p1, "map 4 gave 0x%x, want 0x%x", p4, p1);
    if (p4 != 0) {
        CHECK(memcmp((uint8_t *)guest_ptr(p4) + 40, patch, 8) == 0, "map 4 lacks the BufferSubData bytes");
        ((uint8_t *)guest_ptr(p4))[63] = want[63] = 0xEE;
        CHECK(wo_unmap() == GL_TRUE, "unmap 4 failed");
        check_gl(want, 64, "after unmap 4");
    }

    /* 5: BufferData drops the mirror */
    for (int i = 0; i < 128; i++)
        want[i] = (uint8_t)(0x11 + 2 * i);
    buffer_data(128, want);
    uint32_t p5 = wo_map();
    CHECK(p5 != 0, "map 5 failed");
    if (p5 != 0) {
        CHECK(memcmp(guest_ptr(p5), want, 128) == 0, "map 5 lacks the new contents");
        ((uint8_t *)guest_ptr(p5))[100] = want[100] = 0x5A;
        CHECK(wo_unmap() == GL_TRUE, "unmap 5 failed");
        check_gl(want, 128, "after unmap 5");
    }

    /* 6: delete, then a new buffer */
    MEM32(SCRATCH + 0x300) = vbo;
    uint32_t del[2] = {1, SCRATCH + 0x300};
    call(gl("glDeleteBuffersARB"), 2, del);
    vbo = gen_buffer();
    bind_array(vbo);
    for (int i = 0; i < 64; i++)
        want[i] = (uint8_t)(0x90 + i);
    buffer_data(64, want);
    for (int round = 0; round < 2; round++) {
        uint32_t p = wo_map();
        CHECK(p != 0, "map 6.%d failed", round);
        if (p == 0)
            break;
        CHECK(memcmp(guest_ptr(p), want, 64) == 0, "map 6.%d lacks the contents", round);
        ((uint8_t *)guest_ptr(p))[round * 7] = want[round * 7] = (uint8_t)(0x01 + round);
        CHECK(wo_unmap() == GL_TRUE, "unmap 6.%d failed", round);
        check_gl(want, 64, "after unmap 6");
    }
    MEM32(SCRATCH + 0x300) = vbo;
    call(gl("glDeleteBuffersARB"), 2, del);

    /* 7: more buffers than mirror slots */
    uint32_t ids[20];
    uint8_t init[32];
    for (int k = 0; k < 20; k++) {
        ids[k] = gen_buffer();
        bind_array(ids[k]);
        memset(init, 0x40 + k, sizeof init);
        buffer_data(32, init);
        for (int round = 0; round < 2; round++) {
            uint32_t p = wo_map();
            CHECK(p != 0, "map 7.%d.%d failed", k, round);
            if (p == 0)
                break;
            ((uint8_t *)guest_ptr(p))[0] = (uint8_t)(k + round);
            CHECK(wo_unmap() == GL_TRUE, "unmap 7.%d.%d failed", k, round);
            init[0] = (uint8_t)(k + round);
            check_gl(init, 32, "after unmap 7");
        }
    }
    for (int k = 0; k < 20; k++) {
        MEM32(SCRATCH + 0x300) = ids[k];
        call(gl("glDeleteBuffersARB"), 2, del);
    }

    /* 8: a change over 16 KB re-creates the buffer from the mirror; size and usage survive */
    enum { BIG = 65536 };
    static uint8_t big[BIG], back[BIG];
    for (int i = 0; i < BIG; i++)
        big[i] = (uint8_t)(i * 7 + 3);
    uint32_t big_vbo = gen_buffer();
    bind_array(big_vbo);
    buffer_data(BIG, big);
    uint32_t pb = wo_map();
    CHECK(pb != 0, "map 8.0 failed");
    if (pb != 0) {
        CHECK(wo_unmap() == GL_TRUE, "unmap 8.0 failed");
        pb = wo_map();
        CHECK(pb != 0, "map 8.1 failed");
    }
    if (pb != 0) {
        for (int i = 0; i < 32768; i++)
            ((uint8_t *)guest_ptr(pb))[i] = big[i] = (uint8_t)(0xFF - i);
        ((uint8_t *)guest_ptr(pb))[BIG - 1] = big[BIG - 1] = 0x5C;
        CHECK(wo_unmap() == GL_TRUE, "unmap 8.1 failed");
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, BIG, back);
        CHECK(memcmp(back, big, BIG) == 0, "after unmap 8.1 the buffer differs");
        GLint size = 0, usage = 0;
        glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
        glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_USAGE, &usage);
        CHECK(size == BIG, "buffer size %d, want %d", size, BIG);
        CHECK(usage == GL_STREAM_DRAW, "buffer usage 0x%x, want GL_STREAM_DRAW", usage);

        pb = wo_map();
        CHECK(pb != 0, "map 8.2 failed");
        if (pb != 0) {
            ((uint8_t *)guest_ptr(pb))[100] = big[100] = 0x5A;
            CHECK(wo_unmap() == GL_TRUE, "unmap 8.2 failed");
            glGetBufferSubData(GL_ARRAY_BUFFER, 0, BIG, back);
            CHECK(memcmp(back, big, BIG) == 0, "after unmap 8.2 the buffer differs");
        }
    }
    MEM32(SCRATCH + 0x300) = big_vbo;
    call(gl("glDeleteBuffersARB"), 2, del);
    bind_array(0);
}

/* GLhandleARB / GLsync handles reach the guest as small u32 names, and a
 * shader source in guest memory compiles. */
static void test_shader_objects(void)
{
    const char *src = "#version 120\nvoid main() { gl_Position = ftransform(); }\n";
    uint32_t str = SCRATCH + 0x1000, list = SCRATCH + 0x1400, len = SCRATCH + 0x1410;
    strcpy(guest_ptr(str), src);
    MEM32(list) = str;
    MEM32(len) = (uint32_t)strlen(src);

    uint32_t type[1] = {GL_VERTEX_SHADER_ARB};
    uint32_t shader = call(gl("glCreateShaderObjectARB"), 1, type);
    CHECK(shader != 0 && shader < 0x1000, "shader handle 0x%x is not a small non-zero name", shader);

    uint32_t source[4] = {shader, 1, list, len};
    call(gl("glShaderSourceARB"), 4, source);
    uint32_t comp[1] = {shader};
    call(gl("glCompileShaderARB"), 1, comp);

    uint32_t status = SCRATCH + 0x1500;
    MEM32(status) = 0xFFFFFFFFu;
    uint32_t get[3] = {shader, GL_OBJECT_COMPILE_STATUS_ARB, status};
    call(gl("glGetObjectParameterivARB"), 3, get);
    CHECK(MEM32(status) == 1, "GL_OBJECT_COMPILE_STATUS_ARB = %d", (int)MEM32(status));

    uint32_t program = call(gl("glCreateProgramObjectARB"), 0, NULL);
    CHECK(program != 0 && program != shader && program < 0x1000, "program handle 0x%x", program);
    uint32_t attach[2] = {program, shader};
    call(gl("glAttachObjectARB"), 2, attach);
    uint32_t out = SCRATCH + 0x1600, cnt = SCRATCH + 0x1700;
    MEM32(out) = 0;
    uint32_t att[4] = {program, 4, cnt, out};
    call(gl("glGetAttachedObjectsARB"), 4, att);
    CHECK(MEM32(cnt) == 1 && MEM32(out) == shader, "attached count %u first 0x%x, want 1 and 0x%x",
          MEM32(cnt), MEM32(out), shader);

    uint32_t srcbuf = SCRATCH + 0x1800, srclen = SCRATCH + 0x1710;
    uint32_t gs[4] = {shader, 256, srclen, srcbuf};
    call(gl("glGetShaderSourceARB"), 4, gs);
    CHECK(MEM32(srclen) == strlen(src) && strcmp(guest_ptr(srcbuf), src) == 0,
          "glGetShaderSourceARB length %u", MEM32(srclen));

    /* GL 2.0 core name, the one the game's capability check needs. */
    uint32_t core_type[1] = {GL_VERTEX_SHADER};
    uint32_t core = call(gl("glCreateShader"), 1, core_type);
    uint32_t csrc[4] = {core, 1, list, 0};
    call(gl("glShaderSource"), 4, csrc);
    uint32_t ccomp[1] = {core};
    call(gl("glCompileShader"), 1, ccomp);
    MEM32(status) = 0xFFFFFFFFu;
    uint32_t cget[3] = {core, GL_COMPILE_STATUS, status};
    call(gl("glGetShaderiv"), 3, cget);
    CHECK(MEM32(status) == 1, "GL_COMPILE_STATUS = %d", (int)MEM32(status));

    uint32_t fargs[2] = {GL_SYNC_GPU_COMMANDS_COMPLETE, 0};
    uint32_t sync = call(gl("glFenceSync"), 2, fargs);
    CHECK(sync != 0 && sync < 0x1000, "sync name 0x%x", sync);
    uint32_t is[1] = {sync};
    CHECK((call(gl("glIsSync"), 1, is) & 0xFF) == GL_TRUE, "glIsSync false");
    uint32_t wait[4] = {sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000u, 0};
    uint32_t r = call(gl("glClientWaitSync"), 4, wait);
    CHECK(r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED, "glClientWaitSync = 0x%x", r);
    call(gl("glDeleteSync"), 1, is);
}

static int make_context(void)
{
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24, 0};
    CGLPixelFormatObj pf = NULL;
    GLint npf = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || pf == NULL) {
        attrs[0] = (CGLPixelFormatAttribute)kCGLPFAColorSize;
        attrs[1] = (CGLPixelFormatAttribute)24;
        attrs[2] = 0;
        if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || pf == NULL)
            return -1;
    }
    CGLContextObj ctx = NULL;
    if (CGLCreateContext(pf, NULL, &ctx) != kCGLNoError || ctx == NULL)
        return -1;
    CGLDestroyPixelFormat(pf);
    return CGLSetCurrentContext(ctx) == kCGLNoError ? 0 : -1;
}

int main(void)
{
    if (guest_mem_init() != 0)
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    if (mprotect(guest_ptr(STACK_LO), STACK_HI - STACK_LO, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 2;
    }
    if (guest_heap_init() != 0)
        return 2;
    if (make_context() != 0) {
        fprintf(stderr, "no CGL context\n");
        return 2;
    }
    test_dlsym();
    test_vbo_offsets();
    test_pbo_upload();
    test_tex_staged();
    test_map_roundtrip();
    test_map_two_buffers();
    test_map_mirror();
    test_shader_objects();
    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("thunk_gl ok\n");
    return 0;
}

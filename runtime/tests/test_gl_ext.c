/* Unit test for runtime/src/thunk_gl_ext.c. Uses a hidden CGL context (no
 * drawable). Prints "gl_ext ok" and exits 0 when every check passes. */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <stdio.h>
#include <sys/mman.h>

#include "guest_heap.h"
#include "guest_mem.h"
#include "recomp_types.h"
#include "thunk_gl.h"

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define STACK_TOP 0x00FF0000u
#define SCRATCH 0x00F10000u
#define RET_MAGIC 0xDEADBEEFu

static int g_failures;

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

static void test_lookup(void)
{
    const char *names[] = {"glGenQueriesARB", "glUniform4fvARB", "glGetUniformLocation"};
    for (int i = 0; i < 3; i++) {
        CHECK(gl_ext_auto_lookup(names[i]) != NULL, "%s missing from the auto table", names[i]);
        CHECK(gl_ext_lookup(names[i]) == gl_ext_auto_lookup(names[i]),
              "gl_ext_lookup(%s) does not fall back to the auto table", names[i]);
    }
    /* Curated names stay with thunk_gl.c; unknown names stay NULL. */
    CHECK(gl_ext_auto_lookup("glBindBufferARB") == NULL, "curated name is in the auto table");
    CHECK(gl_ext_lookup("glBindBufferARB") != NULL, "curated name lost");
    CHECK(gl_ext_auto_lookup("glNoSuchFunctionXYZ") == NULL, "unknown name must give NULL");
    CHECK(gl_ext_lookup("glNoSuchFunctionXYZ") == NULL, "unknown name must give NULL");
    /* Handle-returning prototypes are skipped. */
    CHECK(gl_ext_auto_lookup("glCreateShaderObjectARB") == NULL, "handle-returning name emitted");
}

static void test_gen_queries(void)
{
    recomp_func_t gen = gl_ext_auto_lookup("glGenQueriesARB");
    if (gen == NULL)
        return;
    MEM32(SCRATCH) = 0;
    uint32_t args[2] = {1, SCRATCH};
    call(gen, 2, args);
    uint32_t id = MEM32(SCRATCH);
    CHECK(id != 0, "glGenQueriesARB wrote id %u into guest memory", id);
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
    test_lookup();
    test_gen_queries();
    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("gl_ext ok\n");
    return 0;
}

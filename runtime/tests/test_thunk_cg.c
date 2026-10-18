/* Unit test for the Cg thunks: compile one vertex shader with the game's
 * profile (arbvp1, CG_SOURCE) through the thunks and the x86_64 helper, then
 * create it again from the compile cache (a temp folder via HOBBIT_CG_CACHE)
 * and compare the two programs. Needs HOBBIT_CGHELPER (and HOBBIT_APP unless
 * the bundle is at the default path). Prints "ok" and exits 0 when every
 * check passes. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "guest_heap.h"
#include "guest_mem.h"
#include "recomp_types.h"

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define STACK_TOP 0x00FF0000u
#define SCRATCH 0x00F10000u
#define RET_MAGIC 0xDEADBEEFu

#define CG_SOURCE 0x1010u
#define CG_COMPILED_PROGRAM 0x100au
#define CG_PROFILE_ARBVP1 0x1806u

void imp__cgCreateContext(void);
void imp__cgCreateProgram(void);
void imp__cgGetProgramString(void);
void imp__cgGetNamedParameter(void);
void imp__cgIsParameterReferenced(void);
void imp__cgGetParameterName(void);
void imp__cgGetParameterResourceIndex(void);
void imp__cgDestroyProgram(void);

extern unsigned g_cg_cache_hits;

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

recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

static uint32_t call(void (*fn)(void), const uint32_t *args, int nargs)
{
    uint32_t sp = STACK_TOP - 4u * (uint32_t)(nargs + 1);
    MEM32(sp) = RET_MAGIC;
    for (int i = 0; i < nargs; i++)
        MEM32(sp + 4 + 4 * (uint32_t)i) = args[i];
    g_esp = sp;
    g_ebx = 0x11111111;
    g_esi = 0x22222222;
    g_edi = 0x33333333;
    g_ebp = 0x44444444;
    fn();
    CHECK(g_esp == sp + 4, "esp %x, want %x", g_esp, sp + 4);
    CHECK(g_ebx == 0x11111111 && g_esi == 0x22222222 && g_edi == 0x33333333 &&
              g_ebp == 0x44444444,
          "callee-saved register changed");
    return g_eax;
}

static uint32_t g_scratch = SCRATCH;

static uint32_t put_str(const char *s)
{
    uint32_t a = g_scratch;
    size_t n = strlen(s) + 1;
    memcpy(guest_ptr(a), s, n);
    g_scratch += ((uint32_t)n + 15u) & ~15u;
    return a;
}

/* Count the .cg files in dir; name receives the last one found. */
static int count_cg(const char *dir, char *name, size_t n)
{
    int count = 0;
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        size_t l = strlen(e->d_name);
        if (l > 3 && strcmp(e->d_name + l - 3, ".cg") == 0) {
            snprintf(name, n, "%s/%s", dir, e->d_name);
            count++;
        }
    }
    closedir(d);
    return count;
}

static double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
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

    uint32_t src = put_str("float4 main(float4 pos : POSITION, uniform float4x4 mvp) : POSITION"
                           " { return mul(mvp, pos); }");
    uint32_t entry = put_str("main");
    uint32_t name = put_str("mvp");
    uint32_t argv = g_scratch;
    g_scratch += 16;
    MEM32(argv) = put_str("-DOPENGL");
    MEM32(argv + 4) = 0;

    char dir[] = "/tmp/hobbit_cg_test.XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    setenv("HOBBIT_CG_CACHE", dir, 1);
    char file[600] = "";

    uint32_t ctx = call(imp__cgCreateContext, NULL, 0);
    CHECK(ctx != 0, "cgCreateContext returned 0");

    uint32_t a_prog[] = {ctx, CG_SOURCE, src, CG_PROFILE_ARBVP1, entry, argv};
    double t0 = now_ms();
    uint32_t prog = call(imp__cgCreateProgram, a_prog, 6);
    double t_source = now_ms() - t0;
    CHECK(prog != 0, "cgCreateProgram returned 0");
    CHECK(count_cg(dir, file, sizeof file) == 1, "first create did not write one cache file");
    CHECK(g_cg_cache_hits == 0, "first create counted as a cache hit");

    uint32_t a_str[] = {prog, CG_COMPILED_PROGRAM};
    uint32_t text = call(imp__cgGetProgramString, a_str, 2);
    CHECK(text >= GUEST_HEAP_BASE && text < GUEST_HEAP_END, "program string %x not in guest heap", text);
    if (text) {
        const char *s = (const char *)guest_ptr(text);
        CHECK(strlen(s) > 0 && strncmp(s, "!!ARBvp1.0", 10) == 0, "program string: %.40s", s);
        CHECK(strstr(s, "program.local") != NULL, "no program.local in program string");
    }

    uint32_t a_named[] = {prog, name};
    uint32_t param = call(imp__cgGetNamedParameter, a_named, 2);
    CHECK(param != 0, "cgGetNamedParameter(mvp) returned 0");
    CHECK(call(imp__cgIsParameterReferenced, &param, 1) == 1, "mvp not referenced");
    uint32_t pname = call(imp__cgGetParameterName, &param, 1);
    CHECK(pname && strcmp((const char *)guest_ptr(pname), "mvp") == 0, "parameter name wrong");
    CHECK(call(imp__cgGetParameterResourceIndex, &param, 1) == 1, "mvp not at c[1]");

    uint32_t a_missing[] = {prog, put_str("nosuch")};
    CHECK(call(imp__cgGetNamedParameter, a_missing, 2) == 0, "unknown name resolved");

    /* Same inputs again: created from the cache. */
    t0 = now_ms();
    uint32_t prog2 = call(imp__cgCreateProgram, a_prog, 6);
    double t_hit = now_ms() - t0;
    fprintf(stderr, "cg compile from source: %.2f ms; create from cache: %.2f ms\n", t_source, t_hit);
    CHECK(prog2 != 0 && prog2 != prog, "cached create returned %u (source %u)", prog2, prog);
    CHECK(g_cg_cache_hits == 1, "second create: %u cache hits, want 1", g_cg_cache_hits);
    CHECK(count_cg(dir, file, sizeof file) == 1, "second create changed the cache file count");

    uint32_t a_str2[] = {prog2, CG_COMPILED_PROGRAM};
    uint32_t text2 = call(imp__cgGetProgramString, a_str2, 2);
    CHECK(text && text2 && strcmp((const char *)guest_ptr(text), (const char *)guest_ptr(text2)) == 0,
          "program strings differ between source and cached program");

    uint32_t a_named2[] = {prog2, name};
    uint32_t param2 = call(imp__cgGetNamedParameter, a_named2, 2);
    CHECK(param2 != 0, "cgGetNamedParameter(mvp) on cached program returned 0");
    CHECK(call(imp__cgIsParameterReferenced, &param2, 1) == call(imp__cgIsParameterReferenced, &param, 1),
          "cgIsParameterReferenced differs");
    CHECK(call(imp__cgGetParameterResourceIndex, &param2, 1) == call(imp__cgGetParameterResourceIndex, &param, 1),
          "cgGetParameterResourceIndex differs");
    uint32_t a_missing2[] = {prog2, a_missing[1]};
    CHECK(call(imp__cgGetNamedParameter, a_missing2, 2) == 0, "unknown name resolved on cached program");
    call(imp__cgDestroyProgram, &prog2, 1);

    /* A cache file with another key is a miss and is replaced. */
    long good_size = file_size(file);
    FILE *f = fopen(file, "wb");
    if (f) {
        uint32_t one = 1;
        fwrite(&one, 4, 1, f);
        fputc('x', f);
        fwrite(&one, 4, 1, f);
        fputc('y', f);
        fclose(f);
    }
    CHECK(file_size(file) == 10, "could not write the corrupt cache file");
    uint32_t prog3 = call(imp__cgCreateProgram, a_prog, 6);
    CHECK(prog3 != 0, "create with a corrupt cache file returned 0");
    CHECK(g_cg_cache_hits == 1, "corrupt cache file counted as a hit");
    CHECK(file_size(file) == good_size, "corrupt cache file not replaced (%ld bytes, want %ld)", file_size(file),
          good_size);
    uint32_t prog4 = call(imp__cgCreateProgram, a_prog, 6);
    CHECK(prog4 != 0 && g_cg_cache_hits == 2, "replaced cache file not used (%u hits)", g_cg_cache_hits);
    call(imp__cgDestroyProgram, &prog4, 1);
    call(imp__cgDestroyProgram, &prog3, 1);

    call(imp__cgDestroyProgram, &prog, 1);
    if (file[0])
        unlink(file);
    rmdir(dir);

    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}

/* Unit test for the printf-family, sscanf and fcntl thunks.
 * Prints "ok" and exits 0 when every check passes. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "guest_mem.h"
#include "handles.h"
#include "recomp_types.h"

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define STACK_TOP 0x00FF0000u
#define SCRATCH 0x00F10000u /* guest strings and output buffers */
#define RET_MAGIC 0xDEADBEEFu
#define FILE_HANDLE 0xFF000001u

void imp__printf(void);
void imp__fprintf(void);
void imp__sprintf(void);
void imp__snprintf(void);
void imp__sscanf(void);
void imp__fcntl_UNIX2003(void);

static FILE *g_test_file;

/* Stand-in for handles.c, which another slice provides. */
void *handle_get(uint32_t handle, enum handle_kind kind)
{
    return handle == FILE_HANDLE && kind == HANDLE_FILE ? (void *)g_test_file : NULL;
}

/* Stand-in for guest_set_errno in thunk_files.c: records the value. */
static int g_guest_errno;

void guest_set_errno(int host_errno)
{
    g_guest_errno = host_errno;
}

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

static uint32_t g_next_scratch = SCRATCH;

/* Copy a host string into guest scratch space; return its guest address. */
static uint32_t put_str(const char *s)
{
    uint32_t a = g_next_scratch;
    size_t n = strlen(s) + 1;
    memcpy(guest_ptr(a), s, n);
    g_next_scratch += (uint32_t)((n + 15) & ~15u);
    return a;
}

static uint32_t alloc_buf(uint32_t size)
{
    uint32_t a = g_next_scratch;
    memset(guest_ptr(a), 0xAA, size);
    g_next_scratch += (size + 15) & ~15u;
    return a;
}

static const char *gstr(uint32_t a)
{
    return (const char *)guest_ptr(a);
}

/* Call a thunk with the given 32-bit argument words and check the protocol:
 * esp up by 4, callee-saved registers unchanged. */
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

static void dbl_words(double d, uint32_t *lo, uint32_t *hi)
{
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    *lo = (uint32_t)b;
    *hi = (uint32_t)(b >> 32);
}

static void check_str(uint32_t buf, const char *want, uint32_t ret)
{
    CHECK(strcmp(gstr(buf), want) == 0, "got \"%s\", want \"%s\"", gstr(buf), want);
    CHECK((int)ret == (int)strlen(want), "returned %d, want %zu", (int)ret, strlen(want));
}

static void test_sprintf(void)
{
    uint32_t buf = alloc_buf(256);
    uint32_t dlo, dhi, d2lo, d2hi;
    dbl_words(3.14159, &dlo, &dhi);
    dbl_words(2.5, &d2lo, &d2hi);
    uint32_t fmt = put_str("%d|%u|%x|%s|%c|%.2f|%lld|%5d|%-4d|%05.1f|%%");
    uint32_t s = put_str("guest");
    uint32_t args[] = {buf, fmt, (uint32_t)-42, 4000000000u, 0xbeefu, s, 'Z', dlo, dhi,
                          0xFFFFFFFFu, 0xFFFFFFFFu, 77, 9, d2lo, d2hi};
    uint32_t ret = call(imp__sprintf, args, 15);
    check_str(buf, "-42|4000000000|beef|guest|Z|3.14|-1|   77|9   |002.5|%", ret);

    /* long long with a value above 32 bits, plus width and precision from '*' */
    uint32_t fmt2 = put_str("%lld/%llx/%*d/%.*s/%-*d|");
    uint32_t args2[] = {buf, fmt2, 0x00000001u, 0x00000002u, /* 0x200000001 */
                        0xdeadbeefu, 0x00000001u,            /* 0x1deadbeef */
                        6, 42, 3, put_str("abcdef"), 4, 7};
    ret = call(imp__sprintf, args2, 12);
    check_str(buf, "8589934593/1deadbeef/    42/abc/7   |", ret);

    /* %n stores the count so far; %p prints the guest address */
    uint32_t cnt = alloc_buf(4);
    uint32_t fmt3 = put_str("ab%ncd%p");
    uint32_t args3[] = {buf, fmt3, cnt, 0x1234};
    ret = call(imp__sprintf, args3, 4);
    check_str(buf, "abcd0x1234", ret);
    CHECK(MEM32(cnt) == 2, "%%n stored %u", MEM32(cnt));
}

static void test_snprintf(void)
{
    uint32_t buf = alloc_buf(64);
    uint32_t fmt = put_str("%s-%d");
    uint32_t args[] = {buf, 8, fmt, put_str("truncated"), 12345};
    uint32_t ret = call(imp__snprintf, args, 5);
    CHECK(strcmp(gstr(buf), "truncat") == 0, "got \"%s\"", gstr(buf));
    CHECK(ret == 15, "full length %u, want 15", ret);

    /* size 0 writes nothing */
    uint32_t b2 = alloc_buf(8);
    uint32_t args0[] = {b2, 0, fmt, put_str("x"), 1};
    ret = call(imp__snprintf, args0, 5);
    CHECK(*(uint8_t *)guest_ptr(b2) == 0xAA, "size 0 wrote to the buffer");
    CHECK(ret == 3, "size 0 returned %u", ret);

    /* fits exactly */
    uint32_t args1[] = {buf, 64, fmt, put_str("ok"), 7};
    ret = call(imp__snprintf, args1, 5);
    check_str(buf, "ok-7", ret);
}

static void test_sscanf(void)
{
    uint32_t i = alloc_buf(4), str = alloc_buf(32), f = alloc_buf(8), dd = alloc_buf(8);
    uint32_t args[] = {put_str("  -123 hello 2.5 6.25 rest"), put_str("%d %s %f %lf"), i, str, f, dd};
    uint32_t ret = call(imp__sscanf, args, 6);
    CHECK(ret == 4, "assigned %u, want 4", ret);
    CHECK((int32_t)MEM32(i) == -123, "int %d", (int)MEM32(i));
    CHECK(strcmp(gstr(str), "hello") == 0, "string \"%s\"", gstr(str));
    float fv;
    memcpy(&fv, guest_ptr(f), sizeof fv);
    CHECK(fv == 2.5f, "float %f", (double)fv);
    double dv;
    memcpy(&dv, guest_ptr(dd), sizeof dv);
    CHECK(dv == 6.25, "double %f", dv);

    /* literal, suppressed conversion, scanset, hex and %n */
    uint32_t j = alloc_buf(4), word = alloc_buf(16), pos = alloc_buf(4);
    uint32_t args2[] = {put_str("x=0x1F skip abc,def"), put_str("x=%x %*s %[a-c],%n"), j, word, pos};
    ret = call(imp__sscanf, args2, 5);
    CHECK(ret == 2, "assigned %u, want 2", ret);
    CHECK(MEM32(j) == 0x1F, "hex %x", MEM32(j));
    CHECK(strcmp(gstr(word), "abc") == 0, "scanset \"%s\"", gstr(word));
    CHECK(MEM32(pos) == 16, "%%n %u", MEM32(pos));

    /* matching failure after one assignment */
    uint32_t k = alloc_buf(4), l = alloc_buf(4);
    MEM32(l) = 0x55555555;
    uint32_t args3[] = {put_str("7 abc"), put_str("%d %d"), k, l};
    ret = call(imp__sscanf, args3, 4);
    CHECK(ret == 1 && MEM32(k) == 7 && MEM32(l) == 0x55555555, "partial scan %u", ret);

    /* empty input is EOF */
    uint32_t args4[] = {put_str(""), put_str("%d"), k};
    ret = call(imp__sscanf, args4, 3);
    CHECK((int32_t)ret == -1, "empty input returned %d", (int)ret);

    /* 64-bit and 16-bit targets */
    uint32_t q = alloc_buf(8), h = alloc_buf(4);
    MEM32(h) = 0xAAAAAAAA;
    uint32_t args5[] = {put_str("5000000000 70000"), put_str("%lld %hd"), q, h};
    ret = call(imp__sscanf, args5, 4);
    long long qv;
    memcpy(&qv, guest_ptr(q), sizeof qv);
    CHECK(ret == 2 && qv == 5000000000LL, "lld %lld", qv);
    CHECK((MEM32(h) & 0xFFFF) == (70000 & 0xFFFF) && (MEM32(h) >> 16) == 0xAAAA,
          "%%hd wrote %x", MEM32(h));
}

static void test_printf_pipe(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        CHECK(0, "pipe failed");
        return;
    }
    fflush(stdout);
    int saved = dup(1);
    dup2(fds[1], 1);
    uint32_t dlo, dhi;
    dbl_words(1.5, &dlo, &dhi);
    uint32_t args[] = {put_str("n=%d s=%s f=%.1f\n"), 99, put_str("pipe"), dlo, dhi};
    uint32_t ret = call(imp__printf, args, 5);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    close(fds[1]);
    char out[128] = {0};
    ssize_t got = read(fds[0], out, sizeof out - 1);
    close(fds[0]);
    CHECK(got > 0 && strcmp(out, "n=99 s=pipe f=1.5\n") == 0, "pipe got \"%s\"", out);
    CHECK(ret == 18, "printf returned %u", ret);
}

static void test_fprintf(void)
{
    g_test_file = tmpfile();
    uint32_t args[] = {FILE_HANDLE, put_str("%d-%s"), 5, put_str("file")};
    uint32_t ret = call(imp__fprintf, args, 4);
    CHECK(ret == 6, "fprintf returned %u", ret);
    rewind(g_test_file);
    char out[32] = {0};
    size_t n = fread(out, 1, sizeof out - 1, g_test_file);
    CHECK(n == 6 && strcmp(out, "5-file") == 0, "file got \"%s\"", out);

    uint32_t bad[] = {0xFF000099u, put_str("x")};
    ret = call(imp__fprintf, bad, 2);
    CHECK((int32_t)ret == -1, "bad handle returned %d", (int)ret);
    fclose(g_test_file);
}

static void test_fcntl(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        CHECK(0, "pipe failed");
        return;
    }
    uint32_t get_fl[] = {(uint32_t)fds[0], F_GETFL};
    int before = (int)call(imp__fcntl_UNIX2003, get_fl, 2);
    CHECK(before >= 0 && !(before & O_NONBLOCK), "F_GETFL %d", before);

    uint32_t set_fl[] = {(uint32_t)fds[0], F_SETFL, (uint32_t)(before | O_NONBLOCK)};
    CHECK((int)call(imp__fcntl_UNIX2003, set_fl, 3) == 0, "F_SETFL failed");
    int after = (int)call(imp__fcntl_UNIX2003, get_fl, 2);
    CHECK((after & O_NONBLOCK) != 0, "O_NONBLOCK not set: %d", after);

    uint32_t set_fd[] = {(uint32_t)fds[0], F_SETFD, FD_CLOEXEC};
    CHECK((int)call(imp__fcntl_UNIX2003, set_fd, 3) == 0, "F_SETFD failed");
    uint32_t get_fd[] = {(uint32_t)fds[0], F_GETFD};
    CHECK(((int)call(imp__fcntl_UNIX2003, get_fd, 2) & FD_CLOEXEC) != 0, "FD_CLOEXEC not set");

    g_guest_errno = 0;
    uint32_t dup_fd[] = {(uint32_t)fds[0], F_DUPFD, 10};
    CHECK((int)call(imp__fcntl_UNIX2003, dup_fd, 3) == -1 && g_guest_errno == EINVAL,
          "F_DUPFD: guest errno %d", g_guest_errno);

    g_guest_errno = 0;
    uint32_t unknown[] = {(uint32_t)fds[0], 99, 0};
    CHECK((int)call(imp__fcntl_UNIX2003, unknown, 3) == -1 && g_guest_errno == EINVAL,
          "fcntl 99: guest errno %d", g_guest_errno);
    close(fds[0]);
    close(fds[1]);
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
    test_sprintf();
    test_snprintf();
    test_sscanf();
    test_printf_pipe();
    test_fprintf();
    test_fcntl();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}

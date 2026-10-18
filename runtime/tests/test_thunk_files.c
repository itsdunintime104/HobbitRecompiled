/* Unit test for the T3 thunks: stdio, directories, errno, stat, time.
 * Usage: test_thunk_files <scratch dir>. Prints "ok" and exits 0 on success. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "handles.h"

void imp____error(void);
void imp__fopen(void);
void imp__fclose(void);
void imp__fread(void);
void imp__fwrite_UNIX2003(void);
void imp__fseek(void);
void imp__ftell(void);
void imp__stat(void);
void imp__mkdir(void);
void imp__rmdir(void);
void imp__opendir_UNIX2003(void);
void imp__readdir(void);
void imp__closedir_UNIX2003(void);
void imp__gmtime_r(void);
void imp__localtime_r(void);
void imp__timegm(void);
void imp__getpwuid(void);

#define STACK_TOP 0x00F40000u
#define SCRATCH 0x00F60000u

static int g_failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            g_failures++; \
        } \
    } while (0)

static uint32_t g_scratch_next = SCRATCH;

/* Copy bytes into the guest scratch area and return the guest address. */
static uint32_t gput(const void *data, size_t n)
{
    uint32_t a = g_scratch_next;
    memcpy(guest_ptr(a), data, n);
    g_scratch_next += (uint32_t)((n + 15) & ~(size_t)15);
    return a;
}

static uint32_t gstr(const char *s) { return gput(s, strlen(s) + 1); }

static uint32_t galloc(size_t n)
{
    uint32_t a = g_scratch_next;
    memset(guest_ptr(a), 0, n);
    g_scratch_next += (uint32_t)((n + 15) & ~(size_t)15);
    return a;
}

static uint32_t rd32(uint32_t a)
{
    uint32_t v;
    memcpy(&v, guest_ptr(a), 4);
    return v;
}

static uint32_t rd16(uint32_t a)
{
    uint16_t v;
    memcpy(&v, guest_ptr(a), 2);
    return v;
}

static uint64_t rd64(uint32_t a)
{
    uint64_t v;
    memcpy(&v, guest_ptr(a), 8);
    return v;
}

/* Call a thunk the way generated code does: arguments at esp+4, a return
 * address at esp. Checks that esp rose by 4 and callee-saved registers held. */
static void call(void (*fn)(void), int nargs, ...)
{
    va_list ap;

    g_ebx = 0x11111111u;
    g_esi = 0x22222222u;
    g_edi = 0x33333333u;
    g_ebp = 0x44444444u;
    g_esp = STACK_TOP;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) {
        uint32_t v = va_arg(ap, uint32_t);
        memcpy(guest_ptr(STACK_TOP + 4u + 4u * (uint32_t)i), &v, 4);
    }
    va_end(ap);
    memset(guest_ptr(STACK_TOP), 0xEE, 4);
    fn();
    CHECK(g_esp == STACK_TOP + 4);
    CHECK(g_ebx == 0x11111111u && g_esi == 0x22222222u);
    CHECK(g_edi == 0x33333333u && g_ebp == 0x44444444u);
}

static uint32_t guest_errno(void)
{
    call(imp____error, 0);
    return rd32(g_eax);
}

static void test_files(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/f.bin", dir);
    uint32_t gpath = gstr(path);

    call(imp__fopen, 2, gpath, gstr("w+"));
    uint32_t f = g_eax;
    CHECK(f >= HANDLE_BASE && f < HANDLE_BASE + HANDLE_MAX);

    uint32_t data = gstr("0123456789");
    call(imp__fwrite_UNIX2003, 4, data, 1u, 10u, f);
    CHECK(g_eax == 10);
    call(imp__ftell, 1, f);
    CHECK(g_eax == 10);
    call(imp__fseek, 3, f, 2u, 0u);
    CHECK(g_eax == 0);
    call(imp__ftell, 1, f);
    CHECK(g_eax == 2);

    uint32_t buf = galloc(16);
    call(imp__fread, 4, buf, 2u, 2u, f);
    CHECK(g_eax == 2);
    CHECK(memcmp(guest_ptr(buf), "2345", 4) == 0);

    call(imp__fseek, 3, f, 0u, 2u); /* SEEK_END */
    CHECK(g_eax == 0);
    call(imp__ftell, 1, f);
    CHECK(g_eax == 10);

    call(imp__fclose, 1, f);
    CHECK(g_eax == 0);
    call(imp__fclose, 1, f); /* stale handle */
    CHECK(g_eax == (uint32_t)-1);
    CHECK(guest_errno() == EBADF);
}

static void test_stat(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/f.bin", dir);
    struct stat host;
    CHECK(stat(path, &host) == 0);

    uint32_t st = galloc(96);
    call(imp__stat, 2, gstr(path), st);
    CHECK(g_eax == 0);
    CHECK(rd16(st + 8) == (host.st_mode & 0xFFFF)); /* st_mode */
    CHECK((rd16(st + 8) & S_IFMT) == S_IFREG);
    CHECK(rd16(st + 10) == 1);                      /* st_nlink */
    CHECK(rd64(st + 48) == 10);                     /* st_size */
    CHECK(rd32(st + 32) == (uint32_t)host.st_mtimespec.tv_sec);
    CHECK(rd32(st + 12) == host.st_uid);
    CHECK(rd32(st + 16) == host.st_gid);
    CHECK(rd64(st + 56) == (uint64_t)host.st_blocks);
    CHECK(rd32(st + 64) == (uint32_t)host.st_blksize);

    snprintf(path, sizeof path, "%s/missing", dir);
    call(imp__stat, 2, gstr(path), st);
    CHECK(g_eax == (uint32_t)-1);
    CHECK(guest_errno() == ENOENT);
}

static void test_dirs(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/d", dir);
    uint32_t gpath = gstr(path);

    call(imp__mkdir, 2, gpath, 0755u);
    CHECK(g_eax == 0);
    call(imp__mkdir, 2, gpath, 0755u);
    CHECK(g_eax == (uint32_t)-1);
    CHECK(guest_errno() == EEXIST);

    const char *names[] = {"alpha", "b"};
    for (int i = 0; i < 2; i++) {
        char file[1100];
        snprintf(file, sizeof file, "%s/%s", path, names[i]);
        FILE *fp = fopen(file, "w");
        CHECK(fp != NULL);
        if (fp != NULL)
            fclose(fp);
    }

    call(imp__opendir_UNIX2003, 1, gpath);
    uint32_t d = g_eax;
    CHECK(d >= HANDLE_BASE);
    int seen_dot = 0, seen_dotdot = 0, seen_alpha = 0, seen_b = 0, total = 0;
    for (;;) {
        call(imp__readdir, 1, d);
        uint32_t de = g_eax;
        if (de == 0)
            break;
        total++;
        const char *name = guest_ptr(de + 8);
        unsigned namlen = *(uint8_t *)guest_ptr(de + 7);
        CHECK(namlen == strlen(name));
        CHECK(rd16(de + 4) == 8 + ((namlen + 1 + 3) & ~3u)); /* d_reclen */
        unsigned type = *(uint8_t *)guest_ptr(de + 6);
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            CHECK(type == 4); /* DT_DIR */
            if (name[1] == 0)
                seen_dot = 1;
            else
                seen_dotdot = 1;
        } else {
            CHECK(type == 8); /* DT_REG */
            if (strcmp(name, "alpha") == 0)
                seen_alpha = 1;
            else if (strcmp(name, "b") == 0)
                seen_b = 1;
        }
        if (total > 10)
            break;
    }
    CHECK(total == 4 && seen_dot && seen_dotdot && seen_alpha && seen_b);
    call(imp__closedir_UNIX2003, 1, d);
    CHECK(g_eax == 0);

    call(imp__rmdir, 1, gpath); /* not empty */
    CHECK(g_eax == (uint32_t)-1);
    CHECK(guest_errno() == ENOTEMPTY);
}

static void test_errno(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/nope/x", dir);

    call(imp__fopen, 2, gstr(path), gstr("r"));
    CHECK(g_eax == 0);
    uint32_t slot = guest_errno();
    CHECK(slot == ENOENT);
    call(imp____error, 0);
    CHECK(g_eax >= GUEST_STATIC_BASE && g_eax < GUEST_STATIC_BASE + GUEST_STATIC_SIZE);
    CHECK(rd32(g_eax) == ENOENT);

    call(imp__getpwuid, 1, 501u);
    CHECK(g_eax == 0);
}

static void check_tm(uint32_t out, const struct tm *want)
{
    CHECK(rd32(out + 0) == (uint32_t)want->tm_sec);
    CHECK(rd32(out + 4) == (uint32_t)want->tm_min);
    CHECK(rd32(out + 8) == (uint32_t)want->tm_hour);
    CHECK(rd32(out + 12) == (uint32_t)want->tm_mday);
    CHECK(rd32(out + 16) == (uint32_t)want->tm_mon);
    CHECK(rd32(out + 20) == (uint32_t)want->tm_year);
    CHECK(rd32(out + 24) == (uint32_t)want->tm_wday);
    CHECK(rd32(out + 28) == (uint32_t)want->tm_yday);
    CHECK(rd32(out + 32) == (uint32_t)want->tm_isdst);
    CHECK(rd32(out + 36) == (uint32_t)want->tm_gmtoff);
    uint32_t zone = rd32(out + 40);
    CHECK(zone >= GUEST_STATIC_BASE && zone < GUEST_STATIC_BASE + GUEST_STATIC_SIZE);
    CHECK(strcmp(guest_ptr(zone), want->tm_zone) == 0);
}

static void test_time(void)
{
    /* One winter and one summer instant, so tm_isdst/tm_gmtoff are exercised. */
    const int32_t instants[] = {1700000000, 1720000000};
    for (int i = 0; i < 2; i++) {
        time_t t = instants[i];
        uint32_t gt = galloc(4);
        memcpy(guest_ptr(gt), &instants[i], 4);
        uint32_t out = galloc(44);

        struct tm want;
        localtime_r(&t, &want);
        call(imp__localtime_r, 2, gt, out);
        CHECK(g_eax == out);
        check_tm(out, &want);

        gmtime_r(&t, &want);
        call(imp__gmtime_r, 2, gt, out);
        CHECK(g_eax == out);
        check_tm(out, &want);
        CHECK(rd32(out + 36) == 0);

        call(imp__timegm, 1, out);
        CHECK(g_eax == (uint32_t)instants[i]);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]);
        return 2;
    }
    if (guest_mem_init() != 0)
        return 1;
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    /* 0xF80000 and up belongs to the thunks' static area; thunk_files_init maps it. */
    if (mprotect(guest_ptr(0x00F00000u), 0x80000u, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 1;
    }
    thunk_files_init();

    CHECK(rd32(GUEST_STDOUTP_ADDR) >= HANDLE_BASE);
    CHECK(rd32(GUEST_STDERRP_ADDR) >= HANDLE_BASE);
    CHECK(rd32(GUEST_STDINP_ADDR) >= HANDLE_BASE);
    CHECK(handle_get(rd32(GUEST_STDERRP_ADDR), HANDLE_FILE) == stderr);

    test_files(argv[1]);
    test_stat(argv[1]);
    test_dirs(argv[1]);
    test_errno(argv[1]);
    test_time();

    if (g_failures != 0) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}

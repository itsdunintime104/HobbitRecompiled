/* Thunks for stdio files, directories, errno, stat and time (M3 slice T3).
 *
 * Calling protocol: docs/PLAN.md, "M3 thunk contract". Each imp__<name> reads
 * its cdecl arguments from the guest stack, runs the host call, stores the
 * result in g_eax (g_edx:g_eax for 64 bits) and pops the return address.
 * Paths are passed to the host unchanged. */
#include <dirent.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "diag.h"
#include "recomp_types.h"
#include "guest_mem.h"
#include "handles.h"

/* Static guest area layout (offsets from GUEST_STATIC_BASE):
 *   0x0000  stdin/stdout/stderr handle slots (handles.h)
 *   0x0100  per-thread errno slots, ERRNO_SLOTS u32 each
 *   0x0500  interned tm_zone strings, ZONE_SLOTS of ZONE_LEN bytes
 *   0x1000  readdir buffers, DIRENT_SLOTS of DIRENT_STRIDE bytes */
#define ERRNO_OFF 0x0100u
#define ERRNO_SLOTS 256u
#define ZONE_OFF 0x0500u
#define ZONE_SLOTS 16u
#define ZONE_LEN 16u
#define DIRENT_OFF 0x1000u
#define DIRENT_SLOTS 32u
#define DIRENT_STRIDE 272u

/* i386 Darwin, legacy (non-INODE64) layouts, checked with
 * clang -target i386-apple-macosx10.9 -D_DARWIN_NO_64_BIT_INODE offsetof probes.
 * The game imports plain _stat and _readdir, so only these layouts occur. */
#define STAT_SIZE 96u
#define STAT_DEV 0u
#define STAT_INO 4u
#define STAT_MODE 8u
#define STAT_NLINK 10u
#define STAT_UID 12u
#define STAT_GID 16u
#define STAT_RDEV 20u
#define STAT_ATIME 24u
#define STAT_MTIME 32u
#define STAT_CTIME 40u
#define STAT_SIZE_OFF 48u
#define STAT_BLOCKS 56u
#define STAT_BLKSIZE 64u
#define STAT_FLAGS 68u
#define STAT_GEN 72u

#define DIRENT_SIZE 264u
#define DIRENT_INO 0u
#define DIRENT_RECLEN 4u
#define DIRENT_TYPE 6u
#define DIRENT_NAMLEN 7u
#define DIRENT_NAME 8u
#define DIRENT_NAME_MAX 255u

#define TM_SIZE 44u /* nine ints, tm_gmtoff (4 bytes), tm_zone (guest pointer) */
#define TM_GMTOFF 36u
#define TM_ZONE 40u

/* ---- guest memory access ------------------------------------------------ */

static uint32_t arg(unsigned i)
{
    uint32_t v;
    memcpy(&v, guest_ptr(g_esp + 4u + 4u * i), sizeof v);
    return v;
}

static void ret32(uint32_t v)
{
    g_eax = v;
    g_esp += 4;
}

static void put8(uint32_t addr, uint8_t v) { memcpy(guest_ptr(addr), &v, sizeof v); }
static void put16(uint32_t addr, uint16_t v) { memcpy(guest_ptr(addr), &v, sizeof v); }
static void put32(uint32_t addr, uint32_t v) { memcpy(guest_ptr(addr), &v, sizeof v); }
static void put64(uint32_t addr, uint64_t v) { memcpy(guest_ptr(addr), &v, sizeof v); }

static uint32_t get32(uint32_t addr)
{
    uint32_t v;
    memcpy(&v, guest_ptr(addr), sizeof v);
    return v;
}

/* ---- errno ------------------------------------------------------------- */

static _Thread_local uint32_t t_errno_slot;
static unsigned g_errno_slots_used;
static unsigned g_errno_free[ERRNO_SLOTS];
static unsigned g_errno_free_count;
static pthread_mutex_t g_errno_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_errno_key;
static pthread_once_t g_errno_key_once = PTHREAD_ONCE_INIT;

/* Key destructor: return the exiting thread's slot. The value is index + 1 so
 * it is never NULL. */
static void release_errno_slot(void *value)
{
    unsigned n = (unsigned)(uintptr_t)value - 1u;
    pthread_mutex_lock(&g_errno_lock);
    if (g_errno_free_count < ERRNO_SLOTS)
        g_errno_free[g_errno_free_count++] = n;
    pthread_mutex_unlock(&g_errno_lock);
}

static void make_errno_key(void)
{
    if (pthread_key_create(&g_errno_key, release_errno_slot) != 0) {
        perror("guest_errno_slot: pthread_key_create");
        abort();
    }
}

uint32_t guest_errno_slot(void)
{
    if (t_errno_slot == 0) {
        pthread_once(&g_errno_key_once, make_errno_key);
        unsigned n;
        pthread_mutex_lock(&g_errno_lock);
        if (g_errno_free_count > 0) {
            n = g_errno_free[--g_errno_free_count];
        } else if (g_errno_slots_used < ERRNO_SLOTS) {
            n = g_errno_slots_used++;
        } else {
            pthread_mutex_unlock(&g_errno_lock);
            fprintf(stderr, "guest errno slots exhausted (%u threads)\n", ERRNO_SLOTS);
            abort();
        }
        pthread_mutex_unlock(&g_errno_lock);
        t_errno_slot = GUEST_STATIC_BASE + ERRNO_OFF + 4u * n;
        put32(t_errno_slot, 0);
        pthread_setspecific(g_errno_key, (void *)(uintptr_t)(n + 1u));
    }
    return t_errno_slot;
}

void guest_set_errno(int host_errno)
{
    put32(guest_errno_slot(), (uint32_t)host_errno);
}

void imp____error(void) { ret32(guest_errno_slot()); }

/* ---- init -------------------------------------------------------------- */

static void map_area(void)
{
    if (mprotect(guest_ptr(GUEST_STATIC_BASE), GUEST_STATIC_SIZE, PROT_READ | PROT_WRITE) != 0) {
        perror("thunk_files_init: mprotect static area");
        abort();
    }
    put32(GUEST_STDINP_ADDR, handle_put(stdin, HANDLE_FILE));
    put32(GUEST_STDOUTP_ADDR, handle_put(stdout, HANDLE_FILE));
    put32(GUEST_STDERRP_ADDR, handle_put(stderr, HANDLE_FILE));
}

void thunk_files_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, map_area);
}

/* ---- stdio ------------------------------------------------------------- */

static FILE *file_arg(uint32_t h)
{
    FILE *f = handle_get(h, HANDLE_FILE);
    if (f == NULL)
        guest_set_errno(EBADF);
    return f;
}

/* RECOMP_TRACE_OPENS=1: log every path-taking call, read from the
 * environment once. errno is preserved for the caller. */
static pthread_once_t g_trace_once = PTHREAD_ONCE_INIT;
static bool g_trace_opens;
static bool g_trace_chain;

static void trace_opens_init(void)
{
    const char *v = getenv("RECOMP_TRACE_OPENS");
    g_trace_opens = v != NULL && (v[0] == '1' || v[0] == '2');
    g_trace_chain = v != NULL && v[0] == '2';
}

/* RECOMP_TRACE_OPENS=2: also the guest return addresses, walked along the
 * ebp chain (thunks preserve ebp, so g_ebp is the caller's frame). */
static void trace_chain(void)
{
    uint32_t e = g_ebp;

    fprintf(stderr, "  guest chain: %08x", MEM32(g_esp));
    for (int i = 0; i < 8 && e > g_esp && e - g_esp < 0x1000000; i++) {
        fprintf(stderr, " %08x", MEM32(e + 4));
        e = MEM32(e);
    }
    fputc('\n', stderr);
}

static void trace_open(const char *path, const char *op, int ok)
{
    int saved = errno;

    pthread_once(&g_trace_once, trace_opens_init);
    if (g_trace_opens) {
        if (ok)
            fprintf(stderr, "OPEN %s %s -> ok\n", path, op);
        else
            fprintf(stderr, "OPEN %s %s -> errno=%d\n", path, op, saved);
        if (g_trace_chain)
            trace_chain();
    }
    errno = saved;
}

void imp__fopen(void)
{
    const char *path = guest_ptr(arg(0));
    const char *mode = guest_ptr(arg(1));
    uint32_t h = 0;

    FILE *f = fopen(path, mode);
    trace_open(path, mode, f != NULL);
    if (f == NULL) {
        guest_set_errno(errno);
    } else {
        h = handle_put(f, HANDLE_FILE);
        if (h == 0) {
            fclose(f);
            guest_set_errno(ENFILE);
            THUNK_UNSUPPORTED("file handle table full");
        }
    }
    ret32(h);
}

void imp__fclose(void)
{
    uint32_t result = (uint32_t)EOF;
    FILE *f = handle_release(arg(0), HANDLE_FILE);

    if (f == NULL) {
        guest_set_errno(EBADF);
    } else {
        result = (uint32_t)fclose(f);
        if (result != 0)
            guest_set_errno(errno);
    }
    ret32(result);
}

void imp__ferror(void)
{
    FILE *f = file_arg(arg(0));
    ret32(f != NULL ? (uint32_t)ferror(f) : 0);
}

void imp__fread(void)
{
    uint32_t result = 0;
    FILE *f = file_arg(arg(3));

    if (f != NULL) {
        size_t n = fread(guest_ptr(arg(0)), arg(1), arg(2), f);
        if (n < arg(2) && ferror(f))
            guest_set_errno(errno);
        result = (uint32_t)n;
    }
    ret32(result);
}

void imp__fwrite_UNIX2003(void)
{
    uint32_t result = 0;
    FILE *f = file_arg(arg(3));

    if (f != NULL) {
        size_t n = fwrite(guest_ptr(arg(0)), arg(1), arg(2), f);
        if (n < arg(2))
            guest_set_errno(errno);
        result = (uint32_t)n;
    }
    ret32(result);
}

void imp__fseek(void)
{
    uint32_t result = (uint32_t)-1;
    FILE *f = file_arg(arg(0));

    if (f != NULL) {
        result = (uint32_t)fseek(f, (int32_t)arg(1), (int)arg(2));
        if (result != 0)
            guest_set_errno(errno);
    }
    ret32(result);
}

void imp__ftell(void)
{
    uint32_t result = (uint32_t)-1;
    FILE *f = file_arg(arg(0));

    if (f != NULL) {
        long pos = ftell(f);
        if (pos < 0 || pos > INT32_MAX) {
            guest_set_errno(pos < 0 ? errno : EOVERFLOW);
        } else {
            result = (uint32_t)pos;
        }
    }
    ret32(result);
}

/* ---- directories ------------------------------------------------------- */

struct host_dir {
    DIR *dir;
    uint32_t buf; /* guest address of this DIR's dirent buffer */
};

static pthread_mutex_t g_dirent_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_dirent_used[DIRENT_SLOTS];

static uint32_t dirent_buf_alloc(void)
{
    uint32_t buf = 0;

    pthread_mutex_lock(&g_dirent_lock);
    for (unsigned i = 0; i < DIRENT_SLOTS; i++) {
        if (!g_dirent_used[i]) {
            g_dirent_used[i] = true;
            buf = GUEST_STATIC_BASE + DIRENT_OFF + DIRENT_STRIDE * i;
            break;
        }
    }
    pthread_mutex_unlock(&g_dirent_lock);
    return buf;
}

static void dirent_buf_free(uint32_t buf)
{
    pthread_mutex_lock(&g_dirent_lock);
    g_dirent_used[(buf - GUEST_STATIC_BASE - DIRENT_OFF) / DIRENT_STRIDE] = false;
    pthread_mutex_unlock(&g_dirent_lock);
}

void imp__opendir_UNIX2003(void)
{
    const char *path = guest_ptr(arg(0));
    uint32_t h = 0;
    struct host_dir *hd = malloc(sizeof *hd);

    if (hd == NULL) {
        guest_set_errno(ENOMEM);
        ret32(0);
        return;
    }
    hd->buf = dirent_buf_alloc();
    hd->dir = hd->buf != 0 ? opendir(path) : NULL;
    if (hd->buf != 0)
        trace_open(path, "opendir", hd->dir != NULL);
    if (hd->buf == 0) {
        guest_set_errno(EMFILE);
        THUNK_UNSUPPORTED("directory table full");
    } else if (hd->dir == NULL) {
        guest_set_errno(errno);
    } else {
        h = handle_put(hd, HANDLE_DIR);
        if (h == 0) {
            closedir(hd->dir);
            guest_set_errno(ENFILE);
        }
    }
    if (h == 0) {
        if (hd->buf != 0)
            dirent_buf_free(hd->buf);
        free(hd);
    }
    ret32(h);
}

void imp__readdir(void)
{
    uint32_t result = 0;
    struct host_dir *hd = handle_get(arg(0), HANDLE_DIR);

    if (hd == NULL) {
        guest_set_errno(EBADF);
        ret32(0);
        return;
    }
    errno = 0;
    struct dirent *de = readdir(hd->dir);
    if (de == NULL) {
        if (errno != 0)
            guest_set_errno(errno);
    } else {
        unsigned namlen = de->d_namlen < DIRENT_NAME_MAX ? de->d_namlen : DIRENT_NAME_MAX;
        memset(guest_ptr(hd->buf), 0, DIRENT_SIZE);
        put32(hd->buf + DIRENT_INO, (uint32_t)de->d_ino);
        put16(hd->buf + DIRENT_RECLEN, (uint16_t)(DIRENT_NAME + ((namlen + 1u + 3u) & ~3u)));
        put8(hd->buf + DIRENT_TYPE, de->d_type);
        put8(hd->buf + DIRENT_NAMLEN, (uint8_t)namlen);
        memcpy(guest_ptr(hd->buf + DIRENT_NAME), de->d_name, namlen);
        result = hd->buf;
    }
    ret32(result);
}

void imp__closedir_UNIX2003(void)
{
    uint32_t result = (uint32_t)-1;
    struct host_dir *hd = handle_release(arg(0), HANDLE_DIR);

    if (hd == NULL) {
        guest_set_errno(EBADF);
    } else {
        result = (uint32_t)closedir(hd->dir);
        if (result != 0)
            guest_set_errno(errno);
        dirent_buf_free(hd->buf);
        free(hd);
    }
    ret32(result);
}

/* ---- path calls -------------------------------------------------------- */

static void ret_status(int rc)
{
    if (rc != 0)
        guest_set_errno(errno);
    ret32((uint32_t)rc);
}

void imp__mkdir(void)
{
    const char *path = guest_ptr(arg(0));
    int rc = mkdir(path, (mode_t)(arg(1) & 0xFFFFu));
    trace_open(path, "mkdir", rc == 0);
    ret_status(rc);
}

void imp__rmdir(void) { ret_status(rmdir(guest_ptr(arg(0)))); }

void imp__chdir(void)
{
    const char *path = guest_ptr(arg(0));
    int rc = chdir(path);
    trace_open(path, "chdir", rc == 0);
    ret_status(rc);
}

void imp__stat(void)
{
    struct stat st;
    uint32_t out = arg(1);
    const char *path = guest_ptr(arg(0));
    int rc = stat(path, &st);
    trace_open(path, "stat", rc == 0);

    if (rc == 0) {
        memset(guest_ptr(out), 0, STAT_SIZE);
        /* Field widths follow the i386 legacy struct; 64-bit host values are
         * truncated the way the legacy 32-bit st_ino and time_t are. */
        put32(out + STAT_DEV, (uint32_t)st.st_dev);
        put32(out + STAT_INO, (uint32_t)st.st_ino);
        put16(out + STAT_MODE, st.st_mode);
        put16(out + STAT_NLINK, st.st_nlink);
        put32(out + STAT_UID, st.st_uid);
        put32(out + STAT_GID, st.st_gid);
        put32(out + STAT_RDEV, (uint32_t)st.st_rdev);
        put32(out + STAT_ATIME, (uint32_t)st.st_atimespec.tv_sec);
        put32(out + STAT_ATIME + 4, (uint32_t)st.st_atimespec.tv_nsec);
        put32(out + STAT_MTIME, (uint32_t)st.st_mtimespec.tv_sec);
        put32(out + STAT_MTIME + 4, (uint32_t)st.st_mtimespec.tv_nsec);
        put32(out + STAT_CTIME, (uint32_t)st.st_ctimespec.tv_sec);
        put32(out + STAT_CTIME + 4, (uint32_t)st.st_ctimespec.tv_nsec);
        put64(out + STAT_SIZE_OFF, (uint64_t)st.st_size);
        put64(out + STAT_BLOCKS, (uint64_t)st.st_blocks);
        put32(out + STAT_BLKSIZE, (uint32_t)st.st_blksize);
        put32(out + STAT_FLAGS, st.st_flags);
        put32(out + STAT_GEN, st.st_gen);
    }
    ret_status(rc);
}

void imp__utimes(void)
{
    struct timeval tv[2];
    uint32_t times = arg(1);
    int rc;

    if (times != 0) {
        for (unsigned i = 0; i < 2; i++) {
            tv[i].tv_sec = (int32_t)get32(times + 8u * i);
            tv[i].tv_usec = (int32_t)get32(times + 8u * i + 4u);
        }
    }
    rc = utimes(guest_ptr(arg(0)), times != 0 ? tv : NULL);
    ret_status(rc);
}

/* ---- time -------------------------------------------------------------- */

void imp__gettimeofday(void)
{
    struct timeval tv;
    uint32_t out = arg(0);
    uint32_t tz = arg(1);

    int rc = gettimeofday(&tv, NULL);
    if (rc == 0) {
        if (out != 0) {
            put32(out, (uint32_t)tv.tv_sec);
            put32(out + 4, (uint32_t)tv.tv_usec);
        }
        if (tz != 0) /* struct timezone, two ints: not used by modern systems */
            memset(guest_ptr(tz), 0, 8);
    }
    ret_status(rc);
}

void imp__mach_absolute_time(void)
{
    uint64_t t = mach_absolute_time();
    g_eax = (uint32_t)t;
    g_edx = (uint32_t)(t >> 32);
    g_esp += 4;
}

void imp__mach_timebase_info(void)
{
    mach_timebase_info_data_t info;
    uint32_t out = arg(0);
    kern_return_t kr = mach_timebase_info(&info);

    if (kr == KERN_SUCCESS) {
        put32(out, info.numer);
        put32(out + 4, info.denom);
    }
    ret32((uint32_t)kr);
}

static pthread_mutex_t g_zone_lock = PTHREAD_MUTEX_INITIALIZER;

/* Guest address of a static copy of the zone name, or 0 if the table of
 * ZONE_SLOTS names is full. Names longer than ZONE_LEN - 1 are truncated. */
static uint32_t zone_intern(const char *zone)
{
    uint32_t result = 0;
    char name[ZONE_LEN] = {0};

    if (zone == NULL)
        return 0;
    strncpy(name, zone, ZONE_LEN - 1);
    pthread_mutex_lock(&g_zone_lock);
    for (unsigned i = 0; i < ZONE_SLOTS; i++) {
        uint32_t addr = GUEST_STATIC_BASE + ZONE_OFF + ZONE_LEN * i;
        if (memcmp(guest_ptr(addr), name, ZONE_LEN) == 0 || *(char *)guest_ptr(addr) == 0) {
            memcpy(guest_ptr(addr), name, ZONE_LEN);
            result = addr;
            break;
        }
    }
    pthread_mutex_unlock(&g_zone_lock);
    if (result == 0)
        THUNK_UNSUPPORTED("time zone table full");
    return result;
}

static void put_tm(uint32_t out, const struct tm *tm)
{
    put32(out + 0, (uint32_t)tm->tm_sec);
    put32(out + 4, (uint32_t)tm->tm_min);
    put32(out + 8, (uint32_t)tm->tm_hour);
    put32(out + 12, (uint32_t)tm->tm_mday);
    put32(out + 16, (uint32_t)tm->tm_mon);
    put32(out + 20, (uint32_t)tm->tm_year);
    put32(out + 24, (uint32_t)tm->tm_wday);
    put32(out + 28, (uint32_t)tm->tm_yday);
    put32(out + 32, (uint32_t)tm->tm_isdst);
    put32(out + TM_GMTOFF, (uint32_t)tm->tm_gmtoff);
    put32(out + TM_ZONE, zone_intern(tm->tm_zone));
}

static void get_tm(uint32_t in, struct tm *tm)
{
    memset(tm, 0, sizeof *tm);
    tm->tm_sec = (int)get32(in + 0);
    tm->tm_min = (int)get32(in + 4);
    tm->tm_hour = (int)get32(in + 8);
    tm->tm_mday = (int)get32(in + 12);
    tm->tm_mon = (int)get32(in + 16);
    tm->tm_year = (int)get32(in + 20);
    tm->tm_wday = (int)get32(in + 24);
    tm->tm_yday = (int)get32(in + 28);
    tm->tm_isdst = (int)get32(in + 32);
}

/* time_t is 4 bytes in the i386 guest. */
static void tm_thunk(struct tm *(*conv)(const time_t *, struct tm *))
{
    time_t t = (int32_t)get32(arg(0));
    uint32_t out = arg(1);
    struct tm tm;
    uint32_t result = 0;

    if (conv(&t, &tm) != NULL) {
        put_tm(out, &tm);
        result = out;
    } else {
        guest_set_errno(errno);
    }
    ret32(result);
}

void imp__gmtime_r(void) { tm_thunk(gmtime_r); }
void imp__localtime_r(void) { tm_thunk(localtime_r); }

void imp__timegm(void)
{
    struct tm tm;
    uint32_t in = arg(0);
    time_t t;

    get_tm(in, &tm);
    errno = 0;
    t = timegm(&tm);
    if (t == (time_t)-1 && errno != 0) {
        guest_set_errno(errno);
    } else {
        put_tm(in, &tm); /* timegm normalises the fields in place */
    }
    ret32((uint32_t)t);
}

/* ---- not needed by the game -------------------------------------------- */

/* The game never uses the result of getpwuid or gethostbyname. They return
 * NULL with errno ENOSYS rather than build guest passwd or hostent structs. */
void imp__getpwuid(void)
{
    guest_set_errno(ENOSYS);
    ret32(0);
}

void imp__gethostbyname(void)
{
    guest_set_errno(ENOSYS);
    ret32(0);
}

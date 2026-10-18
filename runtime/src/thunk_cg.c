/* Cg runtime thunks (M4).
 *
 * The game compiles Cg source with cgCreateProgram (vertex profile arbvp1 =
 * 0x1806 at 0x3ee91e, fragment profile arbfp1 = 0x1b58 at 0x3ef0f3), reads the
 * ARB assembly with cgGetProgramString(CG_COMPILED_PROGRAM), edits it, and
 * loads it into GL itself (Ghidra export of block 0x3ee000, not committed).
 * There are no cgGL imports.
 *
 * The bundled Cg.framework has no arm64 slice, so each call is forwarded to
 * tools/cghelper (x86_64, under Rosetta) over a socketpair; see
 * tools/cghelper/cg_wire.h. The helper starts on the first Cg call, which is
 * cgCreateContext. Its path is $HOBBIT_CGHELPER, or "cghelper" next to the
 * running executable. If it cannot start, every Cg call returns 0/NULL and
 * the cause is printed once.
 *
 * Returned strings are copied into guest memory:
 * - cgGetProgramString: one block per program, freed on the next call for
 *   that program and on cgDestroyProgram;
 * - cgGetLastListing: one block, freed on the next call;
 * - names, semantics, error and version strings: interned by content.
 *
 * Compiled programs are cached on disk: cgCreateProgram of CG_SOURCE keeps the
 * ARB assembly under $HOBBIT_CG_CACHE, else ~/Library/Caches/HobbitRecomp/cg,
 * and a repeat of the same inputs creates the program from that assembly
 * (CG_OBJECT) without the compiler. Delete the folder to clear the cache. */
#include <errno.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "diag.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "recomp_types.h"
#include "../../tools/cghelper/cg_wire.h"

extern char **environ;

static pthread_mutex_t g_cg_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_cg_fd = -1;
static int g_cg_started; /* 1 once a start was tried */

#define ARG(i) MEM32(g_esp + 4u * (uint32_t)((i) + 1))

/* ---- helper process ---- */

static int helper_path(char *out, size_t n)
{
    const char *env = getenv("HOBBIT_CGHELPER");
    if (env && *env) {
        snprintf(out, n, "%s", env);
        return 0;
    }
    char exe[4096];
    uint32_t len = sizeof exe;
    if (_NSGetExecutablePath(exe, &len) != 0)
        return -1;
    char *slash = strrchr(exe, '/');
    if (slash)
        slash[1] = 0;
    snprintf(out, n, "%scghelper", slash ? exe : "./");
    return 0;
}

static int io_full(int write_side, void *buf, size_t n)
{
    for (uint8_t *p = buf; n;) {
        ssize_t r = write_side ? write(g_cg_fd, p, n) : read(g_cg_fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void helper_lost(const char *what)
{
    fprintf(stderr, "thunk_cg: helper %s failed; Cg calls now return 0\n", what);
    close(g_cg_fd);
    g_cg_fd = -1;
}

/* One request/reply. strs[i] are host pointers (NULL allowed). On success
 * returns 0, fills *rep and, when rep->slen != CGW_NULL, sets *str to a
 * malloc'd NUL-terminated copy. Caller holds g_cg_lock. */
static int transact(uint32_t op, uint32_t w0, uint32_t w1, uint32_t w2, const char *const *strs,
                    uint32_t nstr, struct cgw_rep *rep, char **str)
{
    if (str)
        *str = NULL;
    if (g_cg_fd < 0)
        return -1;
    struct cgw_req q = {op, {w0, w1, w2}, nstr};
    if (io_full(1, &q, sizeof q) != 0)
        return helper_lost("write"), -1;
    for (uint32_t i = 0; i < nstr; i++) {
        uint32_t len = strs[i] ? (uint32_t)strlen(strs[i]) : CGW_NULL;
        if (io_full(1, &len, 4) != 0 || (strs[i] && io_full(1, (void *)strs[i], len) != 0))
            return helper_lost("write"), -1;
    }
    if (io_full(0, rep, sizeof *rep) != 0)
        return helper_lost("read"), -1;
    if (rep->slen != CGW_NULL) {
        char *s = malloc((size_t)rep->slen + 1);
        if (!s || io_full(0, s, rep->slen) != 0) {
            free(s);
            return helper_lost("read"), -1;
        }
        s[rep->slen] = 0;
        if (str)
            *str = s;
        else
            free(s);
    }
    return 0;
}

/* Start the helper once. Caller holds g_cg_lock. */
static void helper_start(void)
{
    if (g_cg_started)
        return;
    g_cg_started = 1;
    char path[4096];
    int sv[2];
    if (helper_path(path, sizeof path) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        fprintf(stderr, "thunk_cg: cannot set up the Cg helper\n");
        return;
    }
    int one = 1;
    setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    /* The child keeps only stderr and the socket on CGW_FD. sv[0] may itself
     * be CGW_FD, so the socket is not closed by number in the child. */
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    posix_spawn_file_actions_addinherit_np(&fa, STDERR_FILENO);
    if (sv[1] == CGW_FD)
        posix_spawn_file_actions_addinherit_np(&fa, CGW_FD);
    else
        posix_spawn_file_actions_adddup2(&fa, sv[1], CGW_FD);
    pid_t pid;
    char *argv[] = {path, NULL};
    int rc = posix_spawn(&pid, path, &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(sv[1]);
    if (rc != 0) {
        fprintf(stderr, "thunk_cg: cannot start %s: %s\n", path, strerror(rc));
        close(sv[0]);
        return;
    }
    g_cg_fd = sv[0];
    struct cgw_rep rep;
    char *err;
    if (transact(CGW_HELLO, 0, 0, 0, NULL, 0, &rep, &err) != 0)
        return;
    if (rep.value != 1) {
        fprintf(stderr, "thunk_cg: helper cannot load Cg: %s\n", err ? err : "?");
        close(g_cg_fd);
        g_cg_fd = -1;
    }
    free(err);
}

/* ---- guest strings ---- */

static uint32_t guest_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t a = guest_malloc((uint32_t)n);
    if (a)
        memcpy(guest_ptr(a), s, n);
    return a;
}

struct interned {
    char *host;
    uint32_t guest;
};
static struct interned *g_intern;
static size_t g_intern_n, g_intern_cap;

static uint32_t intern(const char *s)
{
    for (size_t i = 0; i < g_intern_n; i++)
        if (strcmp(g_intern[i].host, s) == 0)
            return g_intern[i].guest;
    if (g_intern_n == g_intern_cap) {
        size_t cap = g_intern_cap ? 2 * g_intern_cap : 64;
        struct interned *t = realloc(g_intern, cap * sizeof *t);
        if (!t)
            return 0;
        g_intern = t;
        g_intern_cap = cap;
    }
    uint32_t a = guest_strdup(s);
    char *h = strdup(s);
    if (!a || !h) {
        free(h);
        return a;
    }
    g_intern[g_intern_n++] = (struct interned){h, a};
    return a;
}

struct prog_str {
    uint32_t prog;
    uint32_t guest;
};
static struct prog_str *g_progstr;
static size_t g_progstr_n, g_progstr_cap;
static uint32_t g_listing;

static void progstr_drop(uint32_t prog)
{
    for (size_t i = 0; i < g_progstr_n; i++)
        if (g_progstr[i].prog == prog) {
            guest_free(g_progstr[i].guest);
            g_progstr[i] = g_progstr[--g_progstr_n];
            return;
        }
}

static uint32_t progstr_set(uint32_t prog, const char *s)
{
    progstr_drop(prog);
    uint32_t a = guest_strdup(s);
    if (!a)
        return 0;
    if (g_progstr_n == g_progstr_cap) {
        size_t cap = g_progstr_cap ? 2 * g_progstr_cap : 64;
        struct prog_str *t = realloc(g_progstr, cap * sizeof *t);
        if (!t)
            return a; /* untracked: freed never, still valid */
        g_progstr = t;
        g_progstr_cap = cap;
    }
    g_progstr[g_progstr_n++] = (struct prog_str){prog, a};
    return a;
}

/* ---- call shapes ---- */

/* Print helper calls over 2 ms; the game makes them on the main thread. */
static void log_slow(uint32_t op, uint64_t t0)
{
    uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (t1 - t0 > 2000000u)
        fprintf(stderr, "cg: op %u took %.1f ms at %.3f s (uptime)\n", op, (double)(t1 - t0) / 1e6,
                (double)t1 / 1e9);
}

/* Forward a call whose result is a handle, int or bool. */
static uint32_t call_value(uint32_t op, uint32_t w0, uint32_t w1, uint32_t w2, const char *const *strs,
                           uint32_t nstr)
{
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_cg_lock);
    helper_start();
    struct cgw_rep rep;
    uint32_t v = transact(op, w0, w1, w2, strs, nstr, &rep, NULL) == 0 ? rep.value : 0;
    pthread_mutex_unlock(&g_cg_lock);
    log_slow(op, t0);
    return v;
}

enum str_owner { STR_INTERN, STR_PROGRAM, STR_LISTING };

/* Forward a call whose result is a const char *; return its guest copy. */
static uint32_t call_string(uint32_t op, uint32_t w0, uint32_t w1, enum str_owner owner)
{
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_cg_lock);
    helper_start();
    struct cgw_rep rep;
    char *s;
    uint32_t a = 0;
    if (transact(op, w0, w1, 0, NULL, 0, &rep, &s) == 0 && s) {
        if (owner == STR_PROGRAM) {
            a = progstr_set(w0, s);
        } else if (owner == STR_LISTING) {
            if (g_listing)
                guest_free(g_listing);
            a = g_listing = guest_strdup(s);
        } else {
            a = intern(s);
        }
    }
    free(s);
    pthread_mutex_unlock(&g_cg_lock);
    log_slow(op, t0);
    return a;
}

static const char *gstr(uint32_t a) { return a ? (const char *)guest_ptr(a) : NULL; }

#define THUNK_VALUE1(name, op) \
    void imp__##name(void) { g_eax = call_value(op, ARG(0), 0, 0, NULL, 0); g_esp += 4; }
#define THUNK_STRING1(name, op, owner) \
    void imp__##name(void) { g_eax = call_string(op, ARG(0), 0, owner); g_esp += 4; }

/* CGcontext cgCreateContext(void) */
void imp__cgCreateContext(void)
{
    g_eax = call_value(CGW_CREATE_CONTEXT, 0, 0, 0, NULL, 0);
    g_esp += 4;
}

/* ---- compile cache ---- */

/* Values from NVIDIA cg.h / cgGL.h; no Cg header ships with the macOS SDK. */
#define CG_SOURCE 0x1010u
#define CG_OBJECT 0x1011u
#define CG_COMPILED_PROGRAM 0x100au

/* Programs created from the cache; read by the unit test. */
unsigned g_cg_cache_hits;

/* Key: profile as 4 bytes, then each string followed by a NUL (NULL = empty). */
static uint8_t *cache_key(uint32_t profile, const char *const *strs, uint32_t nstr, size_t *n)
{
    size_t len = 4;
    for (uint32_t i = 0; i < nstr; i++)
        len += (strs[i] ? strlen(strs[i]) : 0) + 1;
    uint8_t *k = malloc(len);
    if (!k)
        return NULL;
    memcpy(k, &profile, 4);
    size_t o = 4;
    for (uint32_t i = 0; i < nstr; i++) {
        size_t l = strs[i] ? strlen(strs[i]) : 0;
        memcpy(k + o, strs[i] ? strs[i] : "", l);
        o += l;
        k[o++] = 0;
    }
    *n = len;
    return k;
}

static int cache_dir(char *out, size_t n)
{
    const char *env = getenv("HOBBIT_CG_CACHE");
    if (env && *env)
        return snprintf(out, n, "%s", env) < (int)n ? 0 : -1;
    const char *home = getenv("HOME");
    if (!home || !*home)
        return -1;
    return snprintf(out, n, "%s/Library/Caches/HobbitRecomp/cg", home) < (int)n ? 0 : -1;
}

/* File name: 64-bit FNV-1a of the key as 16 hex digits. */
static int cache_path(const uint8_t *key, size_t keyn, char *out, size_t n)
{
    char dir[3072];
    if (cache_dir(dir, sizeof dir) != 0)
        return -1;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < keyn; i++)
        h = (h ^ key[i]) * 0x100000001b3ull;
    return snprintf(out, n, "%s/%016llx.cg", dir, (unsigned long long)h) < (int)n ? 0 : -1;
}

/* mkdir -p for the folder of a file path. */
static void cache_mkdirs(const char *path)
{
    char d[4096];
    snprintf(d, sizeof d, "%s", path);
    char *last = strrchr(d, '/');
    if (!last)
        return;
    *last = 0;
    for (char *p = d + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(d, 0777);
            *p = '/';
        }
    mkdir(d, 0777);
}

/* Cached assembly (malloc'd, NUL-terminated) when the file holds this key. */
static char *cache_read(const char *path, const uint8_t *key, size_t keyn)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *text = NULL;
    uint8_t *k = malloc(keyn ? keyn : 1);
    uint32_t klen, alen;
    if (k && fread(&klen, 4, 1, f) == 1 && klen == keyn && fread(k, 1, keyn, f) == keyn &&
        memcmp(k, key, keyn) == 0 && fread(&alen, 4, 1, f) == 1 && alen > 0 && alen < (64u << 20) &&
        (text = malloc((size_t)alen + 1)) != NULL) {
        if (fread(text, 1, alen, f) == alen) {
            text[alen] = 0;
        } else {
            free(text);
            text = NULL;
        }
    }
    free(k);
    fclose(f);
    return text;
}

/* Best effort: any error leaves no file or a stale one that is never trusted. */
static void cache_write(const char *path, const uint8_t *key, size_t keyn, const char *text)
{
    cache_mkdirs(path);
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    uint32_t klen = (uint32_t)keyn, alen = (uint32_t)strlen(text);
    int ok = fwrite(&klen, 4, 1, f) == 1 && fwrite(key, 1, keyn, f) == keyn && fwrite(&alen, 4, 1, f) == 1 &&
             fwrite(text, 1, alen, f) == alen;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0)
        unlink(tmp);
}

/* ARB assembly of a program, malloc'd, or NULL. Does not touch the guest copy. */
static char *fetch_asm(uint32_t prog)
{
    pthread_mutex_lock(&g_cg_lock);
    struct cgw_rep rep;
    char *s;
    if (transact(CGW_GET_PROGRAM_STRING, prog, CG_COMPILED_PROGRAM, 0, NULL, 0, &rep, &s) != 0)
        s = NULL;
    pthread_mutex_unlock(&g_cg_lock);
    return s;
}

/* CGprogram cgCreateProgram(CGcontext, CGenum type, const char *program,
 *                           CGprofile, const char *entry, const char **args) */
void imp__cgCreateProgram(void)
{
    uint32_t args = ARG(5);
    uint32_t nargs = 0;
    while (args && nargs < CGW_MAX_STR - 2 && MEM32(args + 4u * nargs))
        nargs++;
    if (args && nargs == CGW_MAX_STR - 2 && MEM32(args + 4u * nargs))
        THUNK_UNSUPPORTED("more than %d compiler arguments", (int)(CGW_MAX_STR - 2));
    const char *strs[CGW_MAX_STR];
    strs[0] = gstr(ARG(2));
    strs[1] = gstr(ARG(4));
    for (uint32_t i = 0; i < nargs; i++)
        strs[2 + i] = gstr(MEM32(args + 4u * i));
    uint32_t nstr = 2 + nargs, ctx = ARG(0), type = ARG(1), profile = ARG(3);

    uint8_t *key = NULL;
    size_t keyn = 0;
    char path[4096];
    int cached = 0; /* key and path are usable */
    if (type == CG_SOURCE && !diag_caches_off() &&
        (key = cache_key(profile, strs, nstr, &keyn)) != NULL)
        cached = cache_path(key, keyn, path, sizeof path) == 0;

    uint32_t h = 0;
    char *text = cached ? cache_read(path, key, keyn) : NULL;
    if (text) {
        const char *src = strs[0];
        strs[0] = text;
        h = call_value(CGW_CREATE_PROGRAM, ctx, CG_OBJECT, profile, strs, nstr);
        strs[0] = src;
        free(text);
        if (h)
            g_cg_cache_hits++;
        else
            unlink(path);
    }
    if (!h) {
        h = call_value(CGW_CREATE_PROGRAM, ctx, type, profile, strs, nstr);
        if (h && cached && (text = fetch_asm(h)) != NULL) {
            if (*text)
                cache_write(path, key, keyn, text);
            free(text);
        }
    }
    free(key);
    g_eax = h;
    g_esp += 4;
}

/* void cgDestroyProgram(CGprogram) */
void imp__cgDestroyProgram(void)
{
    uint32_t prog = ARG(0);
    call_value(CGW_DESTROY_PROGRAM, prog, 0, 0, NULL, 0);
    pthread_mutex_lock(&g_cg_lock);
    progstr_drop(prog);
    pthread_mutex_unlock(&g_cg_lock);
    g_esp += 4;
}

/* void cgDestroyParameter(CGparameter) */
void imp__cgDestroyParameter(void)
{
    call_value(CGW_DESTROY_PARAMETER, ARG(0), 0, 0, NULL, 0);
    g_esp += 4;
}

/* CGparameter cgGetArrayParameter(CGparameter, int index) */
void imp__cgGetArrayParameter(void)
{
    g_eax = call_value(CGW_GET_ARRAY_PARAMETER, ARG(0), ARG(1), 0, NULL, 0);
    g_esp += 4;
}

/* CGparameter cgGetNamedParameter(CGprogram, const char *name) */
void imp__cgGetNamedParameter(void)
{
    const char *name = gstr(ARG(1));
    g_eax = call_value(CGW_GET_NAMED_PARAMETER, ARG(0), 0, 0, &name, 1);
    g_esp += 4;
}

/* CGbool cgIsParameterUsed(CGparameter, CGhandle) */
void imp__cgIsParameterUsed(void)
{
    g_eax = call_value(CGW_IS_PARAMETER_USED, ARG(0), ARG(1), 0, NULL, 0);
    g_esp += 4;
}

/* const char *cgGetProgramString(CGprogram, CGenum) */
void imp__cgGetProgramString(void)
{
    g_eax = call_string(CGW_GET_PROGRAM_STRING, ARG(0), ARG(1), STR_PROGRAM);
    g_esp += 4;
}

THUNK_VALUE1(cgGetFirstStructParameter, CGW_GET_FIRST_STRUCT_PARAMETER)
THUNK_VALUE1(cgGetParameterClass, CGW_GET_PARAMETER_CLASS)
THUNK_VALUE1(cgGetParameterDirection, CGW_GET_PARAMETER_DIRECTION)
THUNK_VALUE1(cgGetParameterResource, CGW_GET_PARAMETER_RESOURCE)
THUNK_VALUE1(cgGetParameterResourceIndex, CGW_GET_PARAMETER_RESOURCE_INDEX)
THUNK_VALUE1(cgIsParameter, CGW_IS_PARAMETER)
THUNK_VALUE1(cgIsParameterReferenced, CGW_IS_PARAMETER_REFERENCED)
THUNK_STRING1(cgGetErrorString, CGW_GET_ERROR_STRING, STR_INTERN)
THUNK_STRING1(cgGetLastListing, CGW_GET_LAST_LISTING, STR_LISTING)
THUNK_STRING1(cgGetParameterName, CGW_GET_PARAMETER_NAME, STR_INTERN)
THUNK_STRING1(cgGetParameterSemantic, CGW_GET_PARAMETER_SEMANTIC, STR_INTERN)
THUNK_STRING1(cgGetString, CGW_GET_STRING, STR_INTERN)

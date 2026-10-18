/* Fake Steam interfaces. The game calls Steam through the C++ vtables of the
 * objects that SteamRemoteStorage(), SteamApps(), SteamUser(), SteamUserStats()
 * and SteamUtils() return. Each object here is one guest u32 that points to a
 * guest vtable. Every entry is a host function (host_func_va).
 *
 * Slot layout: public Steamworks SDK 1.26 headers, from the Proton tree
 * (lsteamclient/steamworks_sdk_126/, branch proton_9.0):
 *   https://github.com/ValveSoftware/Proton/tree/proton_9.0/lsteamclient/steamworks_sdk_126
 * The interface versions are the ones that the game's libsteam_api.dylib
 * (i386 slice) requests, from its strings:
 *   STEAMREMOTESTORAGE_INTERFACE_VERSION012, STEAMAPPS_INTERFACE_VERSION006,
 *   SteamUser017, STEAMUSERSTATS_INTERFACE_VERSION011, SteamUtils006.
 * Vapor.framework re-exports the accessors from that dylib. Header 1.26 is
 * the first SDK that has all five versions.
 *
 * ABI: i386 GCC Itanium C++. `this` is the first stack argument, the caller
 * pops, a slot is 4 bytes in declaration order (no virtual destructor, the
 * overloaded GetStat/SetStat keep declaration order). bool comes back in al,
 * int64 in edx:eax.
 *
 * The slots the lifted game calls (found after each imp__Steam* call in
 * generated.nosync, hobbit_0016/0017/0025.c) are implemented with their
 * offline value; ISteamRemoteStorage keeps the game's saves in a local folder
 * (see that section). Every other slot prints "steam: unexpected slot N of <iface>"
 * and aborts, so a call that was missed shows itself. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "guest_heap.h"
#include "host_funcs.h"
#include "data_imports.h"
#include "steam_fake.h"

/* LEGO The Hobbit on Steam. The game passes it to SteamAPI_RestartAppIfNecessary
 * (hobbit_0025.c:2940, 0x459E8) and appmanifest_285160.acf has the same id. */
#define HOBBIT_APP_ID 285160u

#define MAX_SLOTS 52u

static uint32_t arg32(unsigned n)
{
    uint32_t v;
    memcpy(&v, guest_ptr(g_esp + 4 + 4 * n), sizeof v);
    return v;
}

/* Return v in eax; pops the return address only (the caller pops the args). */
static void ret32(uint32_t v)
{
    g_eax = v;
    g_esp += 4;
}

static void gwr32(uint32_t a, uint32_t v)
{
    memcpy(guest_ptr(a), &v, sizeof v);
}

static void ret64(uint64_t v)
{
    g_eax = (uint32_t)v;
    g_edx = (uint32_t)(v >> 32);
    g_esp += 4;
}

/* ---- ISteamRemoteStorage 012: a local folder ----
 * The game saves only through this interface (the "stmcld:" device). The root
 * folder is $HOBBIT_SAVE_DIR, else $HOME/Library/Application Support/
 * HobbitRecomp/remote. On the first call, when the root does not exist, it is
 * seeded with a copy of the Steam Cloud folder
 * $HOME/Library/Application Support/Steam/userdata/<id>/285160/remote (the
 * one whose newest file is newest). Nothing is ever written under the Steam
 * folder. A guest name maps to a path by ASCII lower-casing and turning '\\'
 * into '/'; empty names, absolute names and names with a ".." component fail.
 * One mutex covers all of this. Slots 18 and 19 (cloud enabled) stay false. */
#define RS_NAME_MAX 256
#define RS_PATH_MAX 2048

static pthread_mutex_t g_rs_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_rs_ready;
static char g_rs_root[RS_PATH_MAX];

static struct {
    char *name;
    uint32_t va;
} *g_rs_names;
static size_t g_rs_nnames;

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdirs(const char *path)
{
    char buf[RS_PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf)
        return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = 0;
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return -1;
        buf[i] = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Copy src to dst (read src only) and give dst the mtime of src. */
static int copy_file(const char *src, const char *dst, const struct stat *st)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ok = out >= 0;
    char buf[65536];
    while (ok) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0)
            ok = 0;
        if (r <= 0)
            break;
        for (ssize_t off = 0; ok && off < r;) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0)
                ok = 0;
            else
                off += w;
        }
    }
    close(in);
    if (out >= 0) {
        ok = ok && fsync(out) == 0;
        ok = close(out) == 0 && ok;
        struct timeval tv[2] = {{st->st_mtime, 0}, {st->st_mtime, 0}};
        if (ok)
            utimes(dst, tv);
    }
    return ok ? 0 : -1;
}

static int copy_tree(const char *src, const char *dst)
{
    DIR *d = opendir(src);
    if (!d)
        return -1;
    int rc = mkdirs(dst);
    struct dirent *e;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char s[RS_PATH_MAX], t[RS_PATH_MAX];
        if (snprintf(s, sizeof s, "%s/%s", src, e->d_name) >= (int)sizeof s ||
            snprintf(t, sizeof t, "%s/%s", dst, e->d_name) >= (int)sizeof t) {
            rc = -1;
            break;
        }
        struct stat st;
        if (stat(s, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            rc = copy_tree(s, t);
        else if (S_ISREG(st.st_mode))
            rc = copy_file(s, t, &st);
    }
    closedir(d);
    return rc;
}

/* mtime of the newest regular file under dir; 0 when there is none. */
static time_t newest_mtime(const char *dir)
{
    time_t best = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char p[RS_PATH_MAX];
        struct stat st;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p ||
            stat(p, &st) != 0)
            continue;
        time_t m = S_ISDIR(st.st_mode) ? newest_mtime(p) : S_ISREG(st.st_mode) ? st.st_mtime : 0;
        if (m > best)
            best = m;
    }
    closedir(d);
    return best;
}

/* Steam Cloud folder to seed from; src gets "" when there is none. */
static void find_steam_remote(const char *home, char *src, size_t cap)
{
    char users[RS_PATH_MAX];
    time_t best = -1;
    src[0] = 0;
    snprintf(users, sizeof users, "%s/Library/Application Support/Steam/userdata", home);
    DIR *d = opendir(users);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char cand[RS_PATH_MAX];
        if (e->d_name[0] == '.' ||
            snprintf(cand, sizeof cand, "%s/%s/%u/remote", users, e->d_name, HOBBIT_APP_ID) >=
                (int)sizeof cand ||
            !is_dir(cand))
            continue;
        time_t m = newest_mtime(cand);
        if (m > best && strlen(cand) < cap) {
            best = m;
            strcpy(src, cand);
        }
    }
    closedir(d);
}

/* Called with the lock held. Returns 1 when the root is usable. */
static int rs_init(void)
{
    if (g_rs_ready)
        return g_rs_root[0] != 0;
    g_rs_ready = 1;
    const char *env = getenv("HOBBIT_SAVE_DIR");
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "";
    }
    char root[RS_PATH_MAX];
    int n = (env && env[0])
                ? snprintf(root, sizeof root, "%s", env)
                : snprintf(root, sizeof root, "%s/Library/Application Support/HobbitRecomp/remote", home);
    if (n <= 0 || n >= (int)sizeof root) {
        fprintf(stderr, "steam: save folder path is empty or too long\n");
        return 0;
    }
    if (!is_dir(root)) {
        char src[RS_PATH_MAX];
        find_steam_remote(home, src, sizeof src);
        if (src[0] && copy_tree(src, root) != 0)
            fprintf(stderr, "steam: copying %s to %s failed\n", src, root);
        if (mkdirs(root) != 0) {
            fprintf(stderr, "steam: cannot create %s\n", root);
            return 0;
        }
        if (src[0])
            fprintf(stderr, "steam: saves in %s (seeded from %s)\n", root, src);
        else
            fprintf(stderr, "steam: saves in %s (new)\n", root);
    }
    strcpy(g_rs_root, root);
    return 1;
}

/* Guest name at address a to a relative host path in out. 0 when it is not a
 * valid name. */
static int rs_name(uint32_t a, char *out)
{
    if (a == 0)
        return 0;
    const char *s = guest_ptr(a);
    size_t n = 0;
    for (; n < RS_NAME_MAX && s[n]; n++) {
        char c = s[n];
        out[n] = c == '\\' ? '/' : (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    if (n == 0 || n == RS_NAME_MAX || out[0] == '/')
        return 0;
    out[n] = 0;
    for (const char *p = out; *p;) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.')
            return 0;
        p += len + (e ? 1 : 0);
    }
    return 1;
}

/* Full host path for the guest name at address a. 0 on a bad name or no root. */
static int rs_path(uint32_t a, char *path)
{
    char rel[RS_NAME_MAX + 1];
    if (!rs_init() || !rs_name(a, rel))
        return 0;
    return snprintf(path, RS_PATH_MAX, "%s/%s", g_rs_root, rel) < RS_PATH_MAX;
}

static int rs_stat(uint32_t name, struct stat *st)
{
    char path[RS_PATH_MAX];
    return rs_path(name, path) && stat(path, st) == 0 && S_ISREG(st->st_mode);
}

static void rs_file_write(void)  /* 0 bool FileWrite(file, data, cub) */
{
    uint32_t name = arg32(1), data = arg32(2);
    int32_t cub = (int32_t)arg32(3);
    char path[RS_PATH_MAX], tmp[RS_PATH_MAX + 8];
    uint32_t ok = 0;
    int err = EINVAL;
    pthread_mutex_lock(&g_rs_lock);
    if (rs_path(name, path) && cub >= 0 && (data != 0 || cub == 0) &&
        snprintf(tmp, sizeof tmp, "%s.tmp", path) < (int)sizeof tmp) {
        char dir[RS_PATH_MAX];
        strcpy(dir, path);
        *strrchr(dir, '/') = 0;
        int fd = -1;
        if (mkdirs(dir) == 0) {
            fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        }
        err = errno;
        if (fd >= 0) {
            const char *p = cub ? guest_ptr(data) : "";
            int32_t off = 0;
            while (off < cub) {
                ssize_t w = write(fd, p + off, (size_t)(cub - off));
                if (w < 0) {
                    err = errno;
                    break;
                }
                off += (int32_t)w;
            }
            ok = off == cub;
            if (ok && fsync(fd) != 0) {
                err = errno;
                ok = 0;
            }
            if (close(fd) != 0 && ok) {
                err = errno;
                ok = 0;
            }
            if (ok && rename(tmp, path) != 0) {
                err = errno;
                ok = 0;
            }
            if (!ok)
                unlink(tmp);
        }
    }
    pthread_mutex_unlock(&g_rs_lock);
    if (!ok)
        fprintf(stderr, "steam: FileWrite %.*s (%d bytes) failed: %s\n", RS_NAME_MAX,
                name != 0 ? (const char *)guest_ptr(name) : "?", (int)cub, strerror(err));
    ret32(ok);
}

static void rs_file_read(void)  /* 1 int32 FileRead(file, data, cub) */
{
    uint32_t name = arg32(1), data = arg32(2);
    int32_t cub = (int32_t)arg32(3);
    char path[RS_PATH_MAX];
    int32_t got = 0;
    pthread_mutex_lock(&g_rs_lock);
    if (rs_path(name, path) && cub > 0 && data != 0) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            while (got < cub) {
                ssize_t r = read(fd, (char *)guest_ptr(data) + got, (size_t)(cub - got));
                if (r <= 0)
                    break;
                got += (int32_t)r;
            }
            close(fd);
        }
    }
    pthread_mutex_unlock(&g_rs_lock);
    ret32((uint32_t)got);
}

static void rs_file_delete(void)  /* 3 bool FileDelete(file) */
{
    char path[RS_PATH_MAX];
    struct stat st;
    pthread_mutex_lock(&g_rs_lock);
    int ok = rs_stat(arg32(1), &st) && rs_path(arg32(1), path) && unlink(path) == 0;
    pthread_mutex_unlock(&g_rs_lock);
    ret32(ok);
}

static void rs_file_exists(void)  /* 10 bool FileExists(file) */
{
    struct stat st;
    pthread_mutex_lock(&g_rs_lock);
    int ok = rs_stat(arg32(1), &st);
    pthread_mutex_unlock(&g_rs_lock);
    ret32(ok);
}

static void rs_file_size(void)  /* 12 int32 GetFileSize(file) */
{
    struct stat st;
    pthread_mutex_lock(&g_rs_lock);
    uint32_t n = rs_stat(arg32(1), &st) ? (uint32_t)st.st_size : 0;
    pthread_mutex_unlock(&g_rs_lock);
    ret32(n);
}

static void rs_file_timestamp(void)  /* 13 int64 GetFileTimestamp(file) */
{
    struct stat st;
    pthread_mutex_lock(&g_rs_lock);
    uint64_t t = rs_stat(arg32(1), &st) ? (uint64_t)st.st_mtime : 0;
    pthread_mutex_unlock(&g_rs_lock);
    ret64(t);
}

/* Relative paths ('/' separators, no *.tmp) of the regular files under
 * root/rel, appended to *list. */
static void list_walk(const char *rel, char ***list, size_t *n, size_t *cap)
{
    char dir[RS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s%s%s", g_rs_root, rel[0] ? "/" : "", rel);
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char r[RS_PATH_MAX], full[RS_PATH_MAX];
        struct stat st;
        if (snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) >= (int)sizeof r ||
            snprintf(full, sizeof full, "%s/%s", g_rs_root, r) >= (int)sizeof full ||
            stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            list_walk(r, list, n, cap);
        } else if (S_ISREG(st.st_mode)) {
            size_t len = strlen(r);
            if (len >= 4 && strcmp(r + len - 4, ".tmp") == 0)
                continue;
            if (*n == *cap) {
                *cap = *cap ? *cap * 2 : 16;
                *list = realloc(*list, *cap * sizeof **list);
            }
            (*list)[(*n)++] = strdup(r);
        }
    }
    closedir(d);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Sorted list of the files; returns the count. Caller frees with list_free. */
static size_t rs_list(char ***list)
{
    size_t n = 0, cap = 0;
    *list = NULL;
    if (rs_init()) {
        list_walk("", list, &n, &cap);
        qsort(*list, n, sizeof **list, cmp_str);
    }
    return n;
}

static void list_free(char **list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(list[i]);
    free(list);
}

static void rs_file_count(void)  /* 15 int32 GetFileCount() */
{
    char **list;
    pthread_mutex_lock(&g_rs_lock);
    size_t n = rs_list(&list);
    list_free(list, n);
    pthread_mutex_unlock(&g_rs_lock);
    ret32((uint32_t)n);
}

/* Guest copy of a name, one per distinct name. Called with the lock held. */
static uint32_t rs_name_va(const char *name)
{
    for (size_t i = 0; i < g_rs_nnames; i++)
        if (strcmp(g_rs_names[i].name, name) == 0)
            return g_rs_names[i].va;
    size_t len = strlen(name) + 1;
    uint32_t va = guest_malloc((uint32_t)len);
    if (va == 0) {
        fprintf(stderr, "steam: guest heap exhausted\n");
        abort();
    }
    memcpy(guest_ptr(va), name, len);
    g_rs_names = realloc(g_rs_names, (g_rs_nnames + 1) * sizeof *g_rs_names);
    g_rs_names[g_rs_nnames].name = strdup(name);
    g_rs_names[g_rs_nnames].va = va;
    g_rs_nnames++;
    return va;
}

static void rs_file_name(void)  /* 16 const char *GetFileNameAndSize(i, pSize) */
{
    int32_t i = (int32_t)arg32(1);
    uint32_t psize = arg32(2), va = 0, size = 0;
    char **list;
    pthread_mutex_lock(&g_rs_lock);
    size_t n = rs_list(&list);
    if (i >= 0 && (size_t)i < n) {
        char full[RS_PATH_MAX];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", g_rs_root, list[i]);
        if (stat(full, &st) == 0) {
            size = (uint32_t)st.st_size;
            va = rs_name_va(list[i]);
        }
    }
    list_free(list, n);
    pthread_mutex_unlock(&g_rs_lock);
    if (psize)
        gwr32(psize, size);
    ret32(va);
}

static void rs_cloud_account(void)  { ret32(0); }  /* 18 bool IsCloudEnabledForAccount() */
static void rs_cloud_app(void)      { ret32(0); }  /* 19 bool IsCloudEnabledForApp() */

/* ---- ISteamApps 006: the base game only, no DLC ---- */
static uint32_t g_language; /* guest "english" */

static void apps_language(void)     { ret32(g_language); }  /* 4 const char *GetCurrentGameLanguage() */
static void apps_subscribed_app(void)                       /* 6 bool BIsSubscribedApp(appID) */
{
    ret32(arg32(1) == HOBBIT_APP_ID);
}
static void apps_dlc_installed(void) { ret32(0); }          /* 7 bool BIsDlcInstalled(appID) */

/* ---- ISteamUser 017: not logged on ---- */
static void user_logged_on(void)    { ret32(0); }  /* 1 bool BLoggedOn() */

/* ---- ISteamUserStats 011: the game calls slot 0 only after BLoggedOn ----
 * sub_001EA560 unlocks an achievement: SetAchievement(name), then StoreStats()
 * only if that returned true. Offline, stats are never received, so Steam
 * returns false and the game clears its achievements-on byte (0x0177F48C). */
static void stats_request(void)     { ret32(0); }  /* 0 bool RequestCurrentStats() */
static void stats_set_ach(void)     { ret32(0); }  /* 7 bool SetAchievement(const char *) */

/* ---- ISteamUtils 006 ---- */
static void utils_app_id(void)      { ret32(HOBBIT_APP_ID); }  /* 9 uint32 GetAppID() */

struct iface {
    const char *accessor;
    unsigned nslots;
    uint32_t obj;
};

enum { RS, APPS, USER, STATS, UTILS, NIFACE };

/* nslots: the PC vtable length of each SDK 1.26 header (PS3-only and
 * game-server-only methods left out). */
static struct iface g_ifaces[NIFACE] = {
    {"SteamRemoteStorage", 52, 0},
    {"SteamApps", 22, 0},
    {"SteamUser", 24, 0},
    {"SteamUserStats", 43, 0},
    {"SteamUtils", 24, 0},
};

static void unexpected(unsigned slot)
{
    uint32_t self = arg32(0);
    const char *name = "unknown interface";
    for (unsigned i = 0; i < NIFACE; i++)
        if (g_ifaces[i].obj == self)
            name = g_ifaces[i].accessor;
    fprintf(stderr, "steam: unexpected slot %u of %s\n", slot, name);
    abort();
}

#define ALL_SLOTS(X) \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) \
    X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
    X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) X(32) X(33) X(34) \
    X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) \
    X(46) X(47) X(48) X(49) X(50) X(51)

#define X(n) static void unexp_##n(void) { unexpected(n); }
ALL_SLOTS(X)
#undef X

static recomp_func_t const g_unexp[MAX_SLOTS] = {
#define X(n) unexp_##n,
    ALL_SLOTS(X)
#undef X
};

/* Implemented slots: {interface, slot, function}. */
static const struct {
    int iface;
    unsigned slot;
    recomp_func_t fn;
} g_impl[] = {
    {RS, 0, rs_file_write},   {RS, 1, rs_file_read},     {RS, 3, rs_file_delete},
    {RS, 10, rs_file_exists}, {RS, 12, rs_file_size},    {RS, 13, rs_file_timestamp},
    {RS, 15, rs_file_count},  {RS, 16, rs_file_name},    {RS, 18, rs_cloud_account},
    {RS, 19, rs_cloud_app},
    {APPS, 4, apps_language}, {APPS, 6, apps_subscribed_app}, {APPS, 7, apps_dlc_installed},
    {USER, 1, user_logged_on},
    {STATS, 0, stats_request}, {STATS, 7, stats_set_ach},
    {UTILS, 9, utils_app_id},
};

static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void build(void)
{
    g_language = data_import_alloc(8);
    memcpy(guest_ptr(g_language), "english", 8);

    for (int i = 0; i < NIFACE; i++) {
        struct iface *f = &g_ifaces[i];
        uint32_t vtbl = data_import_alloc(4 * f->nslots);
        for (unsigned s = 0; s < f->nslots; s++)
            gwr32(vtbl + 4 * s, host_func_va(g_unexp[s]));
        for (unsigned k = 0; k < sizeof g_impl / sizeof g_impl[0]; k++)
            if (g_impl[k].iface == i)
                gwr32(vtbl + 4 * g_impl[k].slot, host_func_va(g_impl[k].fn));
        f->obj = data_import_alloc(4);
        gwr32(f->obj, vtbl);
    }
}

uint32_t steam_fake_iface(const char *accessor)
{
    pthread_once(&g_once, build);
    for (int i = 0; i < NIFACE; i++)
        if (strcmp(accessor, g_ifaces[i].accessor) == 0)
            return g_ifaces[i].obj;
    fprintf(stderr, "steam_fake_iface: unknown accessor %s\n", accessor);
    abort();
}

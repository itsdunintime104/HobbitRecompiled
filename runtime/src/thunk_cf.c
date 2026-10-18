/* CoreFoundation thunks over the host CoreFoundation.
 *
 * Guest CF references are HANDLE_CF handles: a host-pointer-to-handle map
 * gives one object one handle for its whole life. A guest address inside the
 * image's __cfstring section is a constant string; the host string is built
 * once per entry and never released. Out-parameter buffers are guest memory.
 * Dictionaries made with NULL callbacks keep their keys or values as raw
 * guest words; the others hold host CF objects behind handles. */
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_imports.h"
#include "guest_call.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "handles.h"
#include "feral_loader.h"
#include "host_funcs.h"
#include "recomp_types.h"
#include "thunk_cf.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define ARG(n) MEM32(g_esp + 4u + 4u * (n))
#define RET(v) do { g_eax = (uint32_t)(v); g_esp += 4; } while (0)
#define RETV() do { g_esp += 4; } while (0)

/* thunk_printf.c */
uint32_t printf_format_from(uint32_t fmt_guest, uint32_t args_guest, char *out, size_t cap);

static void cf_die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("thunk_cf: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}

/* i386 return of a double is st(0). */
static void fp_push_f64(double v)
{
    g_fp_top = (g_fp_top + 7u) & 7u;
    g_fp_stack[g_fp_top] = v;
}

/* ---- handle map ---------------------------------------------------------- */

#define MAP_BUCKETS 16384u

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    const void *host;
    int32_t next;      /* next node in the bucket chain, -1 at the end */
    uint8_t immortal;  /* constant string: retain and release do nothing */
} g_node[HANDLE_MAX];
static int32_t g_bucket[MAP_BUCKETS];
static int g_map_ready;

static uint32_t bucket_of(const void *p)
{
    return (uint32_t)(((uintptr_t)p >> 4) * 2654435761u) % MAP_BUCKETS;
}

static void map_init_locked(void)
{
    if (!g_map_ready) {
        for (uint32_t i = 0; i < MAP_BUCKETS; i++)
            g_bucket[i] = -1;
        g_map_ready = 1;
    }
}

static uint32_t map_find_locked(const void *p)
{
    for (int32_t i = g_bucket[bucket_of(p)]; i >= 0; i = g_node[i].next)
        if (g_node[i].host == p)
            return HANDLE_BASE + (uint32_t)i;
    return 0;
}

static uint32_t map_add_locked(const void *p, int immortal)
{
    uint32_t h = handle_put((void *)p, HANDLE_CF);
    if (h == 0)
        cf_die("handle table full");
    uint32_t i = h - HANDLE_BASE, b = bucket_of(p);
    g_node[i].host = p;
    g_node[i].immortal = (uint8_t)immortal;
    g_node[i].next = g_bucket[b];
    g_bucket[b] = (int32_t)i;
    return h;
}

static void map_drop_locked(uint32_t h)
{
    uint32_t i = h - HANDLE_BASE;
    int32_t *link = &g_bucket[bucket_of(g_node[i].host)];
    while (*link >= 0 && (uint32_t)*link != i)
        link = &g_node[*link].next;
    if (*link >= 0)
        *link = g_node[i].next;
    g_node[i].host = NULL;
    g_node[i].immortal = 0;
    handle_release(h, HANDLE_CF);
}

uint32_t cf_to_guest(CFTypeRef p)
{
    if (p == NULL)
        return 0;
    pthread_mutex_lock(&g_lock);
    map_init_locked();
    uint32_t h = map_find_locked(p);
    if (h == 0)
        h = map_add_locked(p, 0);
    pthread_mutex_unlock(&g_lock);
    return h;
}

/* ---- constant strings ---------------------------------------------------- */

#define LC_SEGMENT_ 0x1u
#define CFSTRING_ENTRY 16u
#define CFSTRING_FLAG_UTF16 0x10u

static uint32_t g_cfs_addr, g_cfs_size;
static int g_cfs_ready;
static CFStringRef *g_cfs_cache;

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Find the __cfstring section in the image's load commands. */
static void cfs_find_section(void)
{
    const uint8_t *h = guest_image_header();
    uint32_t ncmds = rd32(h + 16), off = 28;
    for (uint32_t i = 0; i < ncmds && off + 8 <= GUEST_HEADER_SIZE; i++) {
        uint32_t cmd = rd32(h + off), cmdsize = rd32(h + off + 4);
        if (cmd == LC_SEGMENT_) {
            uint32_t nsects = rd32(h + off + 48);
            const uint8_t *s = h + off + 56;
            for (uint32_t k = 0; k < nsects; k++, s += 68) {
                if (strncmp((const char *)s, "__cfstring", 16) == 0) {
                    g_cfs_addr = rd32(s + 32);
                    g_cfs_size = rd32(s + 36);
                    return;
                }
            }
        }
        off += cmdsize;
    }
}

/* Test hook: use this range instead of the image's __cfstring section. */
void thunk_cf_set_cfstring_section(uint32_t addr, uint32_t size)
{
    pthread_mutex_lock(&g_lock);
    g_cfs_addr = addr;
    g_cfs_size = size;
    free(g_cfs_cache);
    g_cfs_cache = NULL;
    g_cfs_ready = 1;
    pthread_mutex_unlock(&g_lock);
}

/* Host string for the __cfstring entry holding guest address a, or NULL when
 * a is outside the section. Returns its handle through *h. */
static CFStringRef cfs_lookup(uint32_t a, uint32_t *h)
{
    pthread_mutex_lock(&g_lock);
    map_init_locked();
    if (!g_cfs_ready) {
        cfs_find_section();
        g_cfs_ready = 1;
    }
    if (a < g_cfs_addr || a - g_cfs_addr >= g_cfs_size) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    uint32_t idx = (a - g_cfs_addr) / CFSTRING_ENTRY;
    if (g_cfs_cache == NULL)
        g_cfs_cache = calloc(g_cfs_size / CFSTRING_ENTRY + 1, sizeof *g_cfs_cache);
    CFStringRef s = g_cfs_cache[idx];
    if (s == NULL) {
        const uint8_t *e = guest_ptr(g_cfs_addr + idx * CFSTRING_ENTRY);
        uint32_t flags = rd32(e + 4), str = rd32(e + 8), len = rd32(e + 12);
        if (flags & CFSTRING_FLAG_UTF16)
            s = CFStringCreateWithCharacters(NULL, (const UniChar *)guest_ptr(str), len);
        else
            s = CFStringCreateWithBytes(NULL, guest_ptr(str), len, kCFStringEncodingMacRoman, false);
        if (s == NULL)
            cf_die("constant string at 0x%08x did not convert", a);
        g_cfs_cache[idx] = s;
        map_add_locked(s, 1);
    }
    *h = map_find_locked(s);
    pthread_mutex_unlock(&g_lock);
    return s;
}

CFTypeRef cf_from_guest(uint32_t v)
{
    if (v == 0)
        return NULL;
    void *host = handle_get(v, HANDLE_CF);
    if (host != NULL)
        return host;
    uint32_t h;
    CFStringRef s = cfs_lookup(v, &h);
    if (s != NULL)
        return s;
    cf_die("0x%08x is neither a CF handle nor a constant string", v);
    return NULL;
}

static int is_immortal(CFTypeRef p)
{
    pthread_mutex_lock(&g_lock);
    uint32_t h = map_find_locked(p);
    int r = h != 0 && g_node[h - HANDLE_BASE].immortal;
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* ---- per-object side tables ---------------------------------------------- */

/* Dictionaries made with NULL callbacks: keys or values are raw guest words. */
static struct dflag {
    const void *d;
    uint8_t kraw, vraw;
} *g_dflags;
static size_t g_dflags_n, g_dflags_cap;

static void dflag_set(const void *d, int kraw, int vraw)
{
    if (!kraw && !vraw)
        return;
    pthread_mutex_lock(&g_lock);
    if (g_dflags_n == g_dflags_cap) {
        g_dflags_cap = g_dflags_cap ? g_dflags_cap * 2 : 16;
        g_dflags = realloc(g_dflags, g_dflags_cap * sizeof *g_dflags);
        if (g_dflags == NULL)
            cf_die("out of memory");
    }
    g_dflags[g_dflags_n++] = (struct dflag){d, (uint8_t)kraw, (uint8_t)vraw};
    pthread_mutex_unlock(&g_lock);
}

static void dflag_get(const void *d, int *kraw, int *vraw)
{
    *kraw = *vraw = 0;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_dflags_n; i++) {
        if (g_dflags[i].d == d) {
            *kraw = g_dflags[i].kraw;
            *vraw = g_dflags[i].vraw;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* Guest copy of each CFData's bytes, for CFDataGetBytePtr. */
static struct dcopy {
    const void *d;
    uint32_t addr, cap;
} *g_dcopies;
static size_t g_dcopies_n, g_dcopies_cap;

/* Forget side-table entries of an object that is being freed. */
static void forget_object(const void *p)
{
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_dflags_n; i++) {
        if (g_dflags[i].d == p) {
            g_dflags[i] = g_dflags[--g_dflags_n];
            break;
        }
    }
    for (size_t i = 0; i < g_dcopies_n; i++) {
        if (g_dcopies[i].d == p) {
            guest_free(g_dcopies[i].addr);
            g_dcopies[i] = g_dcopies[--g_dcopies_n];
            break;
        }
    }
    uint32_t h = map_find_locked(p);
    if (h != 0)
        map_drop_locked(h);
    pthread_mutex_unlock(&g_lock);
}

/* ---- CFRetain, CFRelease and type queries -------------------------------- */

void imp__CFRetain(void)
{
    CFTypeRef p = cf_from_guest(ARG(0));
    if (p != NULL && !is_immortal(p))
        CFRetain(p);
    RET(cf_to_guest(p));
}

void cf_release(CFTypeRef p)
{
    if (p != NULL && !is_immortal(p)) {
        CFIndex n = CFGetRetainCount(p);
        CFRelease(p);
        if (n == 1)
            forget_object(p);
    }
}

void imp__CFRelease(void)
{
    cf_release(cf_from_guest(ARG(0)));
    RETV();
}

void imp__CFGetTypeID(void) { RET(CFGetTypeID(cf_from_guest(ARG(0)))); }
void imp__CFArrayGetTypeID(void) { RET(CFArrayGetTypeID()); }
void imp__CFDataGetTypeID(void) { RET(CFDataGetTypeID()); }
void imp__CFDictionaryGetTypeID(void) { RET(CFDictionaryGetTypeID()); }

void imp__CFShow(void)
{
    CFShow(cf_from_guest(ARG(0)));
    RETV();
}

void imp__CFAbsoluteTimeGetCurrent(void)
{
    fp_push_f64(CFAbsoluteTimeGetCurrent());
    RETV();
}

void imp__CFBooleanGetValue(void) { RET(CFBooleanGetValue(cf_from_guest(ARG(0)))); }

/* ---- arrays -------------------------------------------------------------- */

void imp__CFArrayGetCount(void) { RET(CFArrayGetCount(cf_from_guest(ARG(0)))); }

void imp__CFArrayGetValueAtIndex(void)
{
    RET(cf_to_guest(CFArrayGetValueAtIndex(cf_from_guest(ARG(0)), (int32_t)ARG(1))));
}

/* The range is a CFRange {location, length}: two i386 words. */
void imp__CFArrayApplyFunction(void)
{
    CFArrayRef a = cf_from_guest(ARG(0));
    int32_t loc = (int32_t)ARG(1), len = (int32_t)ARG(2);
    uint32_t applier = ARG(3), context = ARG(4);
    for (int32_t i = loc; i < loc + len; i++) {
        uint32_t args[2] = {cf_to_guest(CFArrayGetValueAtIndex(a, i)), context};
        guest_call_cdecl(applier, 2, args);
    }
    RETV();
}

/* ---- dictionaries -------------------------------------------------------- */

static uint32_t g_key_copystr, g_key_type, g_val_type;

static const CFDictionaryKeyCallBacks *key_callbacks(uint32_t a, int *raw)
{
    *raw = 0;
    if (a == 0) {
        *raw = 1;
        return NULL;
    }
    if (a == g_key_copystr)
        return &kCFCopyStringDictionaryKeyCallBacks;
    if (a == g_key_type)
        return &kCFTypeDictionaryKeyCallBacks;
    cf_die("unknown dictionary key callbacks at 0x%08x", a);
    return NULL;
}

static const CFDictionaryValueCallBacks *value_callbacks(uint32_t a, int *raw)
{
    *raw = 0;
    if (a == 0) {
        *raw = 1;
        return NULL;
    }
    if (a == g_val_type)
        return &kCFTypeDictionaryValueCallBacks;
    cf_die("unknown dictionary value callbacks at 0x%08x", a);
    return NULL;
}

static const void *dict_key(int raw, uint32_t k) { return raw ? (const void *)(uintptr_t)k : cf_from_guest(k); }
static const void *dict_val(int raw, uint32_t v) { return raw ? (const void *)(uintptr_t)v : cf_from_guest(v); }
static uint32_t dict_val_out(int raw, const void *v)
{
    return raw ? (uint32_t)(uintptr_t)v : cf_to_guest(v);
}

void imp__CFDictionaryCreateMutable(void)
{
    int kraw, vraw;
    const CFDictionaryKeyCallBacks *kc = key_callbacks(ARG(2), &kraw);
    const CFDictionaryValueCallBacks *vc = value_callbacks(ARG(3), &vraw);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, (int32_t)ARG(1), kc, vc);
    dflag_set(d, kraw, vraw);
    RET(cf_to_guest(d));
}

void imp__CFDictionarySetValue(void)
{
    CFMutableDictionaryRef d = (CFMutableDictionaryRef)cf_from_guest(ARG(0));
    int kraw, vraw;
    dflag_get(d, &kraw, &vraw);
    CFDictionarySetValue(d, dict_key(kraw, ARG(1)), dict_val(vraw, ARG(2)));
    RETV();
}

void imp__CFDictionaryGetValue(void)
{
    CFDictionaryRef d = cf_from_guest(ARG(0));
    int kraw, vraw;
    dflag_get(d, &kraw, &vraw);
    RET(dict_val_out(vraw, CFDictionaryGetValue(d, dict_key(kraw, ARG(1)))));
}

void imp__CFDictionaryGetValueIfPresent(void)
{
    CFDictionaryRef d = cf_from_guest(ARG(0));
    uint32_t out = ARG(2);
    int kraw, vraw;
    dflag_get(d, &kraw, &vraw);
    const void *v = NULL;
    Boolean found = CFDictionaryGetValueIfPresent(d, dict_key(kraw, ARG(1)), &v);
    if (found && out != 0) {
        uint32_t g = dict_val_out(vraw, v);
        memcpy(guest_ptr(out), &g, 4);
    }
    RET(found);
}

/* ---- numbers ------------------------------------------------------------- */

/* The i386 sizes match the host except kCFNumberLongType (10),
 * kCFNumberCFIndexType (14) and kCFNumberNSIntegerType (15), 4 bytes in the
 * guest, and kCFNumberCGFloatType (16), a float in the guest. */
static CFNumberType host_number_type(uint32_t t)
{
    switch (t) {
    case 10: case 14: case 15:
        return kCFNumberSInt32Type;
    case 16:
        return kCFNumberFloat32Type;
    default:
        return (CFNumberType)t;
    }
}

void imp__CFNumberCreate(void)
{
    RET(cf_to_guest(CFNumberCreate(NULL, host_number_type(ARG(1)), guest_ptr(ARG(2)))));
}

void imp__CFNumberGetValue(void)
{
    RET(CFNumberGetValue(cf_from_guest(ARG(0)), host_number_type(ARG(1)), guest_ptr(ARG(2))));
}

/* ---- strings ------------------------------------------------------------- */

void imp__CFStringGetLength(void) { RET(CFStringGetLength(cf_from_guest(ARG(0)))); }
void imp__CFStringGetSystemEncoding(void) { RET(CFStringGetSystemEncoding()); }

void imp__CFStringGetCharacterAtIndex(void)
{
    RET(CFStringGetCharacterAtIndex(cf_from_guest(ARG(0)), (int32_t)ARG(1)));
}

void imp__CFStringCompare(void)
{
    RET(CFStringCompare(cf_from_guest(ARG(0)), cf_from_guest(ARG(1)), ARG(2)));
}

/* CFRange comes back in eax (location) and edx (length). */
void imp__CFStringFind(void)
{
    CFRange r = CFStringFind(cf_from_guest(ARG(0)), cf_from_guest(ARG(1)), ARG(2));
    g_edx = (uint32_t)r.length;
    RET(r.location);
}

void imp__CFStringGetCString(void)
{
    uint32_t buf = ARG(1);
    Boolean ok = buf != 0 && CFStringGetCString(cf_from_guest(ARG(0)), guest_ptr(buf), (int32_t)ARG(2), ARG(3));
    RET(ok);
}

/* printf conversion size on the i386 stack, in bytes, for the spec spanning
 * [s, e) where e - 1 is the conversion character. */
static uint32_t spec_stack_size(const char *s, const char *e)
{
    uint32_t n = 0;
    int ll = 0, big = 0;
    for (const char *p = s; p < e - 1; p++) {
        if (*p == '*')
            n += 4;
        else if (*p == 'l' && p[1] == 'l')
            ll = 1;
        else if (*p == 'q' || *p == 'j')
            ll = 1;
        else if (*p == 'L')
            big = 1;
    }
    switch (e[-1]) {
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        return n + (big ? 12 : 8);
    case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
        return n + (ll ? 8 : 4);
    default:
        return n + 4;
    }
}

static void append_c(CFMutableStringRef out, const char *s, size_t n)
{
    if (n == 0)
        return;
    char *tmp = malloc(n + 1);
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    CFStringAppendCString(out, tmp, kCFStringEncodingUTF8);
    free(tmp);
}

/* Format with the CF format string fmt and the i386 argument area at argp.
 * %@ takes a CF object; every other conversion goes through the guest
 * printf walker one conversion at a time. */
static CFStringRef format_guest(CFStringRef fmt, uint32_t argp)
{
    CFIndex cap = CFStringGetMaximumSizeForEncoding(CFStringGetLength(fmt), kCFStringEncodingUTF8) + 1;
    char *f = malloc((size_t)cap);
    if (f == NULL || !CFStringGetCString(fmt, f, cap, kCFStringEncodingUTF8))
        cf_die("CFStringCreateWithFormat: format did not convert");
    CFMutableStringRef out = CFStringCreateMutable(NULL, 0);
    const char *p = f;
    while (*p) {
        const char *lit = p;
        while (*p && *p != '%')
            p++;
        append_c(out, lit, (size_t)(p - lit));
        if (!*p)
            break;
        const char *start = p++;
        if (*p == '%') {
            p++;
            CFStringAppendCString(out, "%", kCFStringEncodingUTF8);
            continue;
        }
        while (*p && strchr("-+ #0'123456789.*hlqjztL", *p))
            p++;
        if (!*p) {
            append_c(out, start, (size_t)(p - start));
            break;
        }
        char conv = *p++;
        if (conv == '@') {
            CFTypeRef o = cf_from_guest(MEM32(argp));
            argp += 4;
            if (o == NULL) {
                CFStringAppendCString(out, "(null)", kCFStringEncodingUTF8);
            } else if (CFGetTypeID(o) == CFStringGetTypeID()) {
                CFStringAppend(out, o);
            } else {
                CFStringRef d = CFCopyDescription(o);
                if (d != NULL) {
                    CFStringAppend(out, d);
                    CFRelease(d);
                }
            }
        } else if (conv == 'C') {
            UniChar c = (UniChar)MEM32(argp);
            argp += 4;
            CFStringAppendCharacters(out, &c, 1);
        } else if (conv == 'S') {
            uint32_t g = MEM32(argp);
            argp += 4;
            const UniChar *u = guest_ptr(g);
            CFIndex n = 0;
            while (u[n])
                n++;
            CFStringAppendCharacters(out, u, n);
        } else {
            size_t sl = (size_t)(p - start);
            uint32_t scratch = guest_malloc((uint32_t)sl + 1);
            if (scratch == 0)
                cf_die("guest heap exhausted");
            memcpy(guest_ptr(scratch), start, sl);
            ((char *)guest_ptr(scratch))[sl] = '\0';
            char small[256];
            uint32_t n = printf_format_from(scratch, argp, small, sizeof small);
            if (n != (uint32_t)-1 && n < sizeof small) {
                append_c(out, small, n);
            } else if (n != (uint32_t)-1) {
                char *big = malloc((size_t)n + 1);
                printf_format_from(scratch, argp, big, (size_t)n + 1);
                append_c(out, big, n);
                free(big);
            }
            guest_free(scratch);
            argp += spec_stack_size(start, p);
        }
    }
    free(f);
    return out;
}

/* (alloc, formatOptions, format, ...): the variadic arguments start at ARG(3). */
void imp__CFStringCreateWithFormat(void)
{
    CFStringRef fmt = cf_from_guest(ARG(2));
    CFStringRef s = format_guest(fmt, g_esp + 4u + 12u);
    RET(cf_to_guest(s));
}

/* ---- data ---------------------------------------------------------------- */

/* The host returns a host pointer; the guest gets a guest copy made on the
 * first call and refreshed on later ones. */
void imp__CFDataGetBytePtr(void)
{
    CFDataRef d = cf_from_guest(ARG(0));
    uint32_t len = (uint32_t)CFDataGetLength(d);
    uint32_t need = len ? len : 1;
    pthread_mutex_lock(&g_lock);
    struct dcopy *c = NULL;
    for (size_t i = 0; i < g_dcopies_n; i++)
        if (g_dcopies[i].d == d)
            c = &g_dcopies[i];
    if (c == NULL) {
        if (g_dcopies_n == g_dcopies_cap) {
            g_dcopies_cap = g_dcopies_cap ? g_dcopies_cap * 2 : 16;
            g_dcopies = realloc(g_dcopies, g_dcopies_cap * sizeof *g_dcopies);
            if (g_dcopies == NULL)
                cf_die("out of memory");
        }
        c = &g_dcopies[g_dcopies_n++];
        *c = (struct dcopy){d, 0, 0};
    }
    if (c->addr == 0 || c->cap < need) {
        if (c->addr != 0)
            guest_free(c->addr);
        c->addr = guest_malloc(need);
        c->cap = need;
        if (c->addr == 0)
            cf_die("guest heap exhausted for CFData copy");
    }
    if (len)
        memcpy(guest_ptr(c->addr), CFDataGetBytePtr(d), len);
    uint32_t addr = c->addr;
    pthread_mutex_unlock(&g_lock);
    RET(addr);
}

void imp__CFPropertyListCreateFromXMLData(void)
{
    uint32_t errslot = ARG(3);
    CFStringRef err = NULL;
    CFPropertyListRef r = CFPropertyListCreateFromXMLData(NULL, cf_from_guest(ARG(1)), ARG(2),
                                                          errslot ? &err : NULL);
    if (errslot) {
        uint32_t g = cf_to_guest(err);
        memcpy(guest_ptr(errslot), &g, 4);
    }
    RET(cf_to_guest(r));
}

/* ---- URLs ---------------------------------------------------------------- */

void imp__CFURLCopyFileSystemPath(void)
{
    RET(cf_to_guest(CFURLCopyFileSystemPath(cf_from_guest(ARG(0)), (CFURLPathStyle)(int32_t)ARG(1))));
}

void imp__CFURLCreateCopyAppendingPathComponent(void)
{
    RET(cf_to_guest(CFURLCreateCopyAppendingPathComponent(NULL, cf_from_guest(ARG(1)), cf_from_guest(ARG(2)),
                                                          (Boolean)(ARG(3) & 0xFFu))));
}

void imp__CFURLGetFileSystemRepresentation(void)
{
    uint32_t buf = ARG(2);
    Boolean ok = buf != 0 && CFURLGetFileSystemRepresentation(cf_from_guest(ARG(0)), (Boolean)(ARG(1) & 0xFFu),
                                                              guest_ptr(buf), (int32_t)ARG(3));
    RET(ok);
}

/* (alloc, url, CFDataRef *data, CFDictionaryRef *props, CFArrayRef want, SInt32 *err) */
void imp__CFURLCreateDataAndPropertiesFromResource(void)
{
    uint32_t data_slot = ARG(2), props_slot = ARG(3), err_slot = ARG(5);
    CFDataRef data = NULL;
    CFDictionaryRef props = NULL;
    SInt32 err = 0;
    Boolean ok = CFURLCreateDataAndPropertiesFromResource(NULL, cf_from_guest(ARG(1)),
                                                          data_slot ? &data : NULL, props_slot ? &props : NULL,
                                                          cf_from_guest(ARG(4)), err_slot ? &err : NULL);
    if (data_slot) {
        uint32_t g = cf_to_guest(data);
        memcpy(guest_ptr(data_slot), &g, 4);
    }
    if (props_slot) {
        uint32_t g = cf_to_guest(props);
        memcpy(guest_ptr(props_slot), &g, 4);
    }
    if (err_slot)
        memcpy(guest_ptr(err_slot), &err, 4);
    RET(ok);
}

/* ---- bundles ------------------------------------------------------------- */

/* Steam's install path, relative to the user's home folder. */
#define DEFAULT_APP_IN_HOME \
    "/Library/Application Support/Steam/steamapps/common/LEGO - The Hobbit/LEGO The Hobbit.app"

static CFBundleRef g_main_bundle;

/* The main bundle is the game's, not the host binary's. */
void imp__CFBundleGetMainBundle(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_main_bundle == NULL) {
        const char *path = getenv("HOBBIT_APP");
        char buf[1024];
        if (path == NULL || *path == '\0') {
            const char *home = getenv("HOME");
            snprintf(buf, sizeof buf, "%s%s", home ? home : "", DEFAULT_APP_IN_HOME);
            path = buf;
        }
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), true);
        g_main_bundle = url ? CFBundleCreate(NULL, url) : NULL;
        if (url != NULL)
            CFRelease(url);
        if (g_main_bundle == NULL)
            fprintf(stderr, "thunk_cf: no bundle at %s\n", path);
    }
    CFBundleRef b = g_main_bundle;
    pthread_mutex_unlock(&g_lock);
    RET(cf_to_guest(b));
}

static CFBundleRef bundle_arg(uint32_t a) { return (CFBundleRef)cf_from_guest(a); }

void imp__CFBundleCreate(void)
{
    RET(cf_to_guest(CFBundleCreate(NULL, cf_from_guest(ARG(1)))));
}

void imp__CFBundleCopyResourcesDirectoryURL(void)
{
    RET(cf_to_guest(CFBundleCopyResourcesDirectoryURL(bundle_arg(ARG(0)))));
}

void imp__CFBundleCopyBuiltInPlugInsURL(void)
{
    RET(cf_to_guest(CFBundleCopyBuiltInPlugInsURL(bundle_arg(ARG(0)))));
}

void imp__CFBundleCopyResourceURL(void)
{
    RET(cf_to_guest(CFBundleCopyResourceURL(bundle_arg(ARG(0)), cf_from_guest(ARG(1)), cf_from_guest(ARG(2)),
                                            cf_from_guest(ARG(3)))));
}

void imp__CFBundleCopyLocalizedString(void)
{
    RET(cf_to_guest(CFBundleCopyLocalizedString(bundle_arg(ARG(0)), cf_from_guest(ARG(1)),
                                                cf_from_guest(ARG(2)), cf_from_guest(ARG(3)))));
}

#define LOGGED_NAMES 64

/* Look the name up among the import thunks; each name is logged once. */
void imp__CFBundleGetFunctionPointerForName(void)
{
    CFStringRef name = cf_from_guest(ARG(1));
    char buf[256];
    uint32_t va = 0;
    buf[0] = '_';
    if (name != NULL && CFStringGetCString(name, buf + 1, sizeof buf - 1, kCFStringEncodingUTF8)) {
        /* Feral's Content.loader entry points are native (feral_loader.c). */
        va = feral_loader_lookup(buf + 1);
        recomp_func_t fn = va == 0 ? import_func_by_name(buf) : NULL;
        if (fn != NULL)
            va = host_func_va(fn);
        static char *logged[LOGGED_NAMES];
        static int nlogged;
        pthread_mutex_lock(&g_lock);
        int seen = 0;
        for (int i = 0; i < nlogged; i++)
            if (strcmp(logged[i], buf) == 0)
                seen = 1;
        if (!seen) {
            if (nlogged < LOGGED_NAMES)
                logged[nlogged++] = strdup(buf);
            fprintf(stderr, "thunk_cf: CFBundleGetFunctionPointerForName %s -> 0x%08x\n", buf + 1, va);
        }
        pthread_mutex_unlock(&g_lock);
    }
    RET(va);
}

/* ---- locales ------------------------------------------------------------- */

void imp__CFLocaleCopyCurrent(void) { RET(cf_to_guest(CFLocaleCopyCurrent())); }
void imp__CFLocaleCopyPreferredLanguages(void) { RET(cf_to_guest(CFLocaleCopyPreferredLanguages())); }
void imp__CFLocaleCreate(void) { RET(cf_to_guest(CFLocaleCreate(NULL, cf_from_guest(ARG(1))))); }
void imp__CFLocaleGetIdentifier(void) { RET(cf_to_guest(CFLocaleGetIdentifier(cf_from_guest(ARG(0))))); }

void imp__CFLocaleGetValue(void)
{
    RET(cf_to_guest(CFLocaleGetValue(cf_from_guest(ARG(0)), cf_from_guest(ARG(1)))));
}

/* ---- run loops ----------------------------------------------------------- */

void imp__CFRunLoopGetCurrent(void) { RET(cf_to_guest(CFRunLoopGetCurrent())); }

/* A 0 source does nothing: the guest registers sources it never made. */
void imp__CFRunLoopAddSource(void)
{
    if (ARG(1) != 0)
        CFRunLoopAddSource((CFRunLoopRef)cf_from_guest(ARG(0)), (CFRunLoopSourceRef)cf_from_guest(ARG(1)), cf_from_guest(ARG(2)));
    RETV();
}

void imp__CFRunLoopRemoveSource(void)
{
    if (ARG(1) != 0)
        CFRunLoopRemoveSource((CFRunLoopRef)cf_from_guest(ARG(0)), (CFRunLoopSourceRef)cf_from_guest(ARG(1)), cf_from_guest(ARG(2)));
    RETV();
}

void imp__CFRunLoopContainsSource(void)
{
    Boolean r = ARG(1) != 0 &&
                CFRunLoopContainsSource((CFRunLoopRef)cf_from_guest(ARG(0)), (CFRunLoopSourceRef)cf_from_guest(ARG(1)), cf_from_guest(ARG(2)));
    RET(r);
}

/* ---- UUIDs --------------------------------------------------------------- */

void imp__CFUUIDGetConstantUUIDWithBytes(void)
{
    UInt8 b[16];
    for (uint32_t i = 0; i < 16; i++)
        b[i] = (UInt8)ARG(1 + i);
    RET(cf_to_guest(CFUUIDGetConstantUUIDWithBytes(NULL, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8],
                                                   b[9], b[10], b[11], b[12], b[13], b[14], b[15])));
}

/* CFUUIDBytes is 16 bytes: the caller passes a hidden result pointer first
 * and the callee pops it, so the thunk pops 8 (return address and pointer). */
void imp__CFUUIDGetUUIDBytes(void)
{
    uint32_t dst = ARG(0);
    CFUUIDBytes u = CFUUIDGetUUIDBytes(cf_from_guest(ARG(1)));
    memcpy(guest_ptr(dst), &u, sizeof u);
    g_eax = dst;
    g_esp += 8;
}

/* ---- data imports -------------------------------------------------------- */

/* Carbon TextInputSources keys (thunk_sys.c TISGetInputSourceProperty). */
extern const CFStringRef kTISPropertyInputSourceLanguages;
extern const CFStringRef kTISPropertyUnicodeKeyLayoutData;

static void register_object(const char *name, CFTypeRef host)
{
    uint32_t a = data_import_alloc(4);
    uint32_t h = cf_to_guest(host);
    memcpy(guest_ptr(a), &h, 4);
    data_import_register(name, a);
}

static uint32_t register_callbacks(const char *name)
{
    uint32_t a = data_import_alloc(32);
    data_import_register(name, a);
    return a;
}

void thunk_cf_data_init(void)
{
    register_object("_kCFAllocatorDefault", kCFAllocatorDefault);
    register_object("_kCFLocaleCountryCode", kCFLocaleCountryCode);
    register_object("_kCFLocaleLanguageCode", kCFLocaleLanguageCode);
    register_object("_kCFRunLoopDefaultMode", kCFRunLoopDefaultMode);
    register_object("_kTISPropertyInputSourceLanguages", kTISPropertyInputSourceLanguages);
    register_object("_kTISPropertyUnicodeKeyLayoutData", kTISPropertyUnicodeKeyLayoutData);
    g_key_copystr = register_callbacks("_kCFCopyStringDictionaryKeyCallBacks");
    g_key_type = register_callbacks("_kCFTypeDictionaryKeyCallBacks");
    g_val_type = register_callbacks("_kCFTypeDictionaryValueCallBacks");
}

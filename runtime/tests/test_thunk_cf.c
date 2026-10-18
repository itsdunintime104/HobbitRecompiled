/* Unit test for the CoreFoundation thunks. Prints "ok" and exits 0 when every
 * check passes. host_func_va, import_func_by_name, guest_resolve and the
 * data-import allocator are stubbed here. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "data_imports.h"
#include "guest_call.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "handles.h"
#include "host_funcs.h"
#include "recomp_types.h"

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define STACK_TOP 0x00FF0000u
#define SCRATCH 0x00F10000u
#define CFS_BASE 0x00F20000u /* fake __cfstring section */
#define CFS_TEXT 0x00F21000u
#define RET_MAGIC 0xDEADBEEFu
#define VA_APPLIER 0x00401000u
#define FAKE_VA 0xFE000010u

#define DECL(n) void imp__##n(void)
DECL(CFRetain); DECL(CFRelease); DECL(CFArrayGetCount); DECL(CFArrayGetValueAtIndex);
DECL(CFArrayApplyFunction); DECL(CFDictionaryCreateMutable); DECL(CFDictionarySetValue);
DECL(CFDictionaryGetValue); DECL(CFDictionaryGetValueIfPresent); DECL(CFNumberCreate); DECL(CFNumberGetValue);
DECL(CFStringGetLength); DECL(CFStringGetCString); DECL(CFStringGetCharacterAtIndex); DECL(CFStringFind);
DECL(CFStringCompare); DECL(CFStringCreateWithFormat); DECL(CFDataGetBytePtr); DECL(CFBundleGetMainBundle);
DECL(CFBundleGetFunctionPointerForName); DECL(CFRunLoopGetCurrent); DECL(CFRunLoopAddSource);
DECL(CFRunLoopContainsSource); DECL(CFRunLoopRemoveSource); DECL(CFUUIDGetConstantUUIDWithBytes);
DECL(CFUUIDGetUUIDBytes); DECL(CFAbsoluteTimeGetCurrent); DECL(CFLocaleCopyCurrent);

uint32_t cf_to_guest(CFTypeRef p);
CFTypeRef cf_from_guest(uint32_t v);
void thunk_cf_set_cfstring_section(uint32_t addr, uint32_t size);

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

/* ---- stubs for the host-function and data-import contracts --------------- */

static struct {
    char name[64];
    uint32_t value;
} g_reg[16];
static int g_nreg;

void data_import_register(const char *name, uint32_t value)
{
    snprintf(g_reg[g_nreg].name, sizeof g_reg[g_nreg].name, "%s", name);
    g_reg[g_nreg++].value = value;
}

uint32_t data_import_alloc(uint32_t n)
{
    uint32_t a = guest_malloc(n);
    memset(guest_ptr(a), 0, n);
    return a;
}

static uint32_t registered(const char *name)
{
    for (int i = 0; i < g_nreg; i++)
        if (strcmp(g_reg[i].name, name) == 0)
            return g_reg[i].value;
    return 0xFFFFFFFFu;
}

static void dummy_thunk(void) {}

uint32_t host_func_va(recomp_func_t fn)
{
    return fn == dummy_thunk ? FAKE_VA : 0;
}

recomp_func_t import_func_by_name(const char *name)
{
    return strcmp(name, "_glClear") == 0 ? dummy_thunk : NULL;
}

/* Guest applier: records its two arguments. */
static uint32_t g_applied[8][2];
static int g_napplied;

static void guest_applier(void)
{
    g_applied[g_napplied][0] = MEM32(g_esp + 4);
    g_applied[g_napplied][1] = MEM32(g_esp + 8);
    g_napplied++;
}

recomp_func_t recomp_lookup(uint32_t va)
{
    return va == VA_APPLIER ? guest_applier : NULL;
}

recomp_func_t guest_resolve(uint32_t va)
{
    return recomp_lookup(va);
}

/* Call a thunk with the given argument words. Checks that esp rises by pop
 * bytes and callee-saved registers survive. */
static uint32_t call_pop(void (*fn)(void), const uint32_t *args, int nargs, uint32_t pop)
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
    CHECK(g_esp == sp + pop, "esp %x, want %x", g_esp, sp + pop);
    CHECK(g_ebx == 0x11111111 && g_esi == 0x22222222 && g_edi == 0x33333333 && g_ebp == 0x44444444,
          "callee-saved register changed");
    return g_eax;
}

static uint32_t call(void (*fn)(void), const uint32_t *args, int nargs)
{
    return call_pop(fn, args, nargs, 4);
}

#define CALL(fn, ...) \
    ({ const uint32_t a_[] = {__VA_ARGS__}; call(fn, a_, (int)(sizeof a_ / sizeof a_[0])); })
#define CALL0(fn) call(fn, NULL, 0)

static void put_cfstring(uint32_t entry, uint32_t flags, uint32_t str, uint32_t len)
{
    MEM32(entry) = 0;
    MEM32(entry + 4) = flags;
    MEM32(entry + 8) = str;
    MEM32(entry + 12) = len;
}

#define K_HELLO (CFS_BASE + 0x00)
#define K_WIDE (CFS_BASE + 0x10)
#define K_FMT (CFS_BASE + 0x20)

static void set_up_constants(void)
{
    memcpy(guest_ptr(CFS_TEXT), "Hello", 6);
    put_cfstring(K_HELLO, 0x7C8, CFS_TEXT, 5);
    uint16_t wide[] = {'W', 0x00F6, 'r', 'l', 'd'};
    memcpy(guest_ptr(CFS_TEXT + 0x20), wide, sizeof wide);
    put_cfstring(K_WIDE, 0x7D0, CFS_TEXT + 0x20, 5);
    const char *fmt = "n=%d s=%s @=%@ f=%.1f %lld|%5d";
    memcpy(guest_ptr(CFS_TEXT + 0x40), fmt, strlen(fmt) + 1);
    put_cfstring(K_FMT, 0x7C8, CFS_TEXT + 0x40, (uint32_t)strlen(fmt));
    thunk_cf_set_cfstring_section(CFS_BASE, 0x30);
}

static void test_constant_strings(void)
{
    CHECK(CALL(imp__CFStringGetLength, K_HELLO) == 5, "length of Hello");
    CHECK(CALL(imp__CFStringGetLength, K_WIDE) == 5, "length of wide string");
    CHECK(CALL(imp__CFStringGetCharacterAtIndex, K_WIDE, 1) == 0x00F6, "wide char 1");

    uint32_t buf = SCRATCH;
    CHECK(CALL(imp__CFStringGetCString, K_HELLO, buf, 16, kCFStringEncodingUTF8) == 1, "GetCString failed");
    CHECK(strcmp(guest_ptr(buf), "Hello") == 0, "GetCString wrote \"%s\"", (char *)guest_ptr(buf));
    CHECK(CALL(imp__CFStringGetCString, K_HELLO, buf, 3, kCFStringEncodingUTF8) == 0, "short buffer accepted");

    uint32_t h1 = CALL(imp__CFRetain, K_HELLO), h2 = CALL(imp__CFRetain, K_HELLO);
    CHECK(h1 >= HANDLE_BASE && h1 == h2, "constant string handles 0x%x 0x%x", h1, h2);
    CHECK(cf_from_guest(h1) == cf_from_guest(K_HELLO), "handle and address name different objects");
    CALL(imp__CFRelease, h1);
    CHECK(CALL(imp__CFStringGetLength, h1) == 5, "constant string died on release");

    CFStringRef host = CFStringCreateWithCString(NULL, "Hello", kCFStringEncodingUTF8);
    CHECK(CALL(imp__CFStringCompare, K_HELLO, cf_to_guest(host), 0) == 0, "compare equal");
    CHECK((int32_t)CALL(imp__CFStringCompare, K_HELLO, K_WIDE, 0) != 0, "compare differs");
    CFRelease(host);

    uint32_t found = CALL(imp__CFStringFind, K_HELLO, K_HELLO, 0);
    CHECK(found == 0 && g_edx == 5, "find self: loc %u len %u", found, g_edx);
    CFStringRef lo = CFStringCreateWithCString(NULL, "lo", kCFStringEncodingUTF8);
    found = CALL(imp__CFStringFind, K_HELLO, cf_to_guest(lo), 0);
    CHECK(found == 3 && g_edx == 2, "find lo: loc %u len %u", found, g_edx);
    CFRelease(lo);
    CFStringRef no = CFStringCreateWithCString(NULL, "zz", kCFStringEncodingUTF8);
    found = CALL(imp__CFStringFind, K_HELLO, cf_to_guest(no), 0);
    CHECK(found == 0xFFFFFFFFu && g_edx == 0, "find missing: loc %u len %u", found, g_edx);
    CFRelease(no);
}

static void test_same_handle(void)
{
    /* A short ASCII CFString is a tagged pointer with no retain count; use data. */
    const uint8_t bytes[] = {9};
    CFDataRef s = CFDataCreate(NULL, bytes, sizeof bytes);
    uint32_t h = cf_to_guest(s);
    CHECK(h >= HANDLE_BASE && cf_to_guest(s) == h, "same object, different handle");
    CHECK(cf_from_guest(h) == s, "handle does not map back");
    CHECK(CALL(imp__CFRetain, h) == h, "CFRetain changed the handle");
    CALL(imp__CFRelease, h);
    CHECK(handle_get(h, HANDLE_CF) == s, "handle dropped while the object is alive");
    CALL(imp__CFRelease, h); /* last reference: frees the host object */
    CHECK(handle_get(h, HANDLE_CF) == NULL, "handle kept after the last release");
    CHECK(cf_to_guest(NULL) == 0 && cf_from_guest(0) == NULL, "NULL is not 0");
}

static void test_dictionary(void)
{
    uint32_t copy_cb = registered("_kCFCopyStringDictionaryKeyCallBacks");
    uint32_t type_k = registered("_kCFTypeDictionaryKeyCallBacks");
    uint32_t type_v = registered("_kCFTypeDictionaryValueCallBacks");
    CHECK(copy_cb != 0xFFFFFFFFu && type_k != 0xFFFFFFFFu && type_v != 0xFFFFFFFFu, "callback structs not registered");

    /* Copy-string keys, raw guest values. */
    uint32_t d = CALL(imp__CFDictionaryCreateMutable, 0, 0, copy_cb, 0);
    CHECK(d >= HANDLE_BASE, "dictionary handle 0x%x", d);
    CALL(imp__CFDictionarySetValue, d, K_HELLO, 0x1234);
    CHECK(CALL(imp__CFDictionaryGetValue, d, K_HELLO) == 0x1234, "round trip by constant key");
    CFStringRef k = CFStringCreateWithCString(NULL, "Hello", kCFStringEncodingUTF8);
    CHECK(CALL(imp__CFDictionaryGetValue, d, cf_to_guest(k)) == 0x1234, "lookup by equal host string");
    CHECK(CALL(imp__CFDictionaryGetValue, d, K_WIDE) == 0, "missing key not 0");
    uint32_t slot = SCRATCH + 0x100;
    MEM32(slot) = 0xAAAAAAAAu;
    CHECK(CALL(imp__CFDictionaryGetValueIfPresent, d, K_HELLO, slot) == 1 && MEM32(slot) == 0x1234,
          "GetValueIfPresent hit");
    MEM32(slot) = 0xAAAAAAAAu;
    CHECK(CALL(imp__CFDictionaryGetValueIfPresent, d, K_WIDE, slot) == 0 && MEM32(slot) == 0xAAAAAAAAu,
          "GetValueIfPresent miss");
    CFRelease(k);

    /* CF type callbacks: values are objects behind handles. */
    uint32_t d2 = CALL(imp__CFDictionaryCreateMutable, 0, 4, type_k, type_v);
    CALL(imp__CFDictionarySetValue, d2, K_HELLO, K_WIDE);
    uint32_t v = CALL(imp__CFDictionaryGetValue, d2, K_HELLO);
    CHECK(cf_from_guest(v) == cf_from_guest(K_WIDE), "type-callback value is a different object");
    CALL(imp__CFRelease, d);
    CALL(imp__CFRelease, d2);
}

static void test_number(void)
{
    uint32_t val = SCRATCH + 0x200, out = SCRATCH + 0x240;
    MEM32(val) = (uint32_t)-42;
    uint32_t n = CALL(imp__CFNumberCreate, 0, kCFNumberSInt32Type, val);
    MEM32(out) = 0;
    CHECK(CALL(imp__CFNumberGetValue, n, kCFNumberSInt32Type, out) == 1 && (int32_t)MEM32(out) == -42,
          "int32 round trip");
    double d = 0;
    CHECK(CALL(imp__CFNumberGetValue, n, kCFNumberFloat64Type, out) == 1, "int to double");
    memcpy(&d, guest_ptr(out), 8);
    CHECK(d == -42.0, "int as double %g", d);

    double in = 2.5;
    memcpy(guest_ptr(val), &in, 8);
    uint32_t n2 = CALL(imp__CFNumberCreate, 0, kCFNumberFloat64Type, val);
    memset(guest_ptr(out), 0, 8);
    CHECK(CALL(imp__CFNumberGetValue, n2, kCFNumberFloat64Type, out) == 1, "double get");
    memcpy(&d, guest_ptr(out), 8);
    CHECK(d == 2.5, "double round trip %g", d);

    /* kCFNumberLongType and kCFNumberCFIndexType are 4 bytes in the guest. */
    MEM32(val) = (uint32_t)-7;
    uint32_t n3 = CALL(imp__CFNumberCreate, 0, kCFNumberLongType, val);
    MEM32(out) = 0;
    MEM32(out + 4) = 0x5A5A5A5Au;
    CHECK(CALL(imp__CFNumberGetValue, n3, kCFNumberCFIndexType, out) == 1, "long get");
    CHECK((int32_t)MEM32(out) == -7 && MEM32(out + 4) == 0x5A5A5A5Au, "long wrote %x %x", MEM32(out), MEM32(out + 4));
    CALL(imp__CFRelease, n);
    CALL(imp__CFRelease, n2);
    CALL(imp__CFRelease, n3);
}

static void test_array_apply(void)
{
    CFStringRef a = CFStringCreateWithCString(NULL, "a", kCFStringEncodingUTF8);
    CFStringRef b = CFStringCreateWithCString(NULL, "b", kCFStringEncodingUTF8);
    const void *items[] = {a, b};
    CFArrayRef arr = CFArrayCreate(NULL, items, 2, &kCFTypeArrayCallBacks);
    uint32_t h = cf_to_guest(arr);
    CHECK(CALL(imp__CFArrayGetCount, h) == 2, "array count");
    CHECK(CALL(imp__CFArrayGetValueAtIndex, h, 1) == cf_to_guest(b), "array value 1");

    g_napplied = 0;
    CALL(imp__CFArrayApplyFunction, h, 0, 2, VA_APPLIER, 0xC0DE);
    CHECK(g_napplied == 2, "applier ran %d times", g_napplied);
    CHECK(g_applied[0][0] == cf_to_guest(a) && g_applied[0][1] == 0xC0DE, "applier call 0");
    CHECK(g_applied[1][0] == cf_to_guest(b) && g_applied[1][1] == 0xC0DE, "applier call 1");

    g_napplied = 0;
    CALL(imp__CFArrayApplyFunction, h, 1, 1, VA_APPLIER, 7);
    CHECK(g_napplied == 1 && g_applied[0][0] == cf_to_guest(b), "ranged apply");
    CFRelease(arr);
    CFRelease(a);
    CFRelease(b);
}

static void test_format(void)
{
    uint32_t str = SCRATCH + 0x300;
    strcpy(guest_ptr(str), "guest");
    double d = 2.5;
    uint64_t bits;
    memcpy(&bits, &d, 8);
    uint32_t args[] = {0, 0, K_FMT, (uint32_t)-42, str, K_HELLO, (uint32_t)bits, (uint32_t)(bits >> 32),
                       0x00000001u, 0x00000002u, 42};
    uint32_t s = call(imp__CFStringCreateWithFormat, args, 11);
    CHECK(s >= HANDLE_BASE, "format handle 0x%x", s);
    uint32_t buf = SCRATCH + 0x400;
    CALL(imp__CFStringGetCString, s, buf, 128, kCFStringEncodingUTF8);
    const char *want = "n=-42 s=guest @=Hello f=2.5 8589934593|   42";
    CHECK(strcmp(guest_ptr(buf), want) == 0, "format gave \"%s\"", (char *)guest_ptr(buf));
    CALL(imp__CFRelease, s);
}

static void test_data(void)
{
    const uint8_t bytes[] = {1, 2, 3, 4, 5};
    CFDataRef data = CFDataCreate(NULL, bytes, sizeof bytes);
    uint32_t h = cf_to_guest(data);
    uint32_t p = CALL(imp__CFDataGetBytePtr, h);
    CHECK(p != 0 && memcmp(guest_ptr(p), bytes, 5) == 0, "data copy");
    CHECK(CALL(imp__CFDataGetBytePtr, h) == p, "data copy moved");
    CFRelease(data);
}

static void test_uuid(void)
{
    uint32_t args[17];
    args[0] = 0;
    for (uint32_t i = 0; i < 16; i++)
        args[1 + i] = 0xA0 + i;
    uint32_t u = call(imp__CFUUIDGetConstantUUIDWithBytes, args, 17);
    CHECK(u >= HANDLE_BASE, "uuid handle");
    uint32_t dst = SCRATCH + 0x500;
    memset(guest_ptr(dst), 0, 16);
    uint32_t gargs[] = {dst, u};
    uint32_t r = call_pop(imp__CFUUIDGetUUIDBytes, gargs, 2, 8);
    CHECK(r == dst, "struct return did not hand back the pointer");
    for (uint32_t i = 0; i < 16; i++)
        CHECK(((uint8_t *)guest_ptr(dst))[i] == 0xA0 + i, "uuid byte %u", i);
}

static void test_runloop_bundle_misc(void)
{
    uint32_t rl = CALL0(imp__CFRunLoopGetCurrent);
    CHECK(rl >= HANDLE_BASE && CALL0(imp__CFRunLoopGetCurrent) == rl, "run loop handle");
    CALL(imp__CFRunLoopAddSource, rl, 0, K_HELLO);
    CALL(imp__CFRunLoopRemoveSource, rl, 0, K_HELLO);
    CHECK(CALL(imp__CFRunLoopContainsSource, rl, 0, K_HELLO) == 0, "contains null source");

    setenv("HOBBIT_APP", "/System/Applications/Calculator.app", 1);
    uint32_t b = CALL0(imp__CFBundleGetMainBundle);
    CHECK(b >= HANDLE_BASE && CALL0(imp__CFBundleGetMainBundle) == b, "main bundle handle");
    CFStringRef nm = CFStringCreateWithCString(NULL, "glClear", kCFStringEncodingUTF8);
    CHECK(CALL(imp__CFBundleGetFunctionPointerForName, b, cf_to_guest(nm)) == FAKE_VA, "function pointer lookup");
    CFRelease(nm);
    nm = CFStringCreateWithCString(NULL, "nothere", kCFStringEncodingUTF8);
    CHECK(CALL(imp__CFBundleGetFunctionPointerForName, b, cf_to_guest(nm)) == 0, "missing function pointer");
    CFRelease(nm);

    uint32_t loc = CALL0(imp__CFLocaleCopyCurrent);
    CHECK(loc >= HANDLE_BASE, "locale handle");
    CALL(imp__CFRelease, loc);

    g_fp_top = 0;
    CALL0(imp__CFAbsoluteTimeGetCurrent);
    CHECK(g_fp_stack[g_fp_top] > 0, "absolute time");
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
    thunk_cf_data_init();
    set_up_constants();
    test_constant_strings();
    test_same_handle();
    test_dictionary();
    test_number();
    test_array_apply();
    test_format();
    test_data();
    test_uuid();
    test_runloop_bundle_misc();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}

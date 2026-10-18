/* Unit test for the IOKit, keyboard-layout and socket thunks.
 * Prints "ok" and exits 0 when every check passes. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <CoreFoundation/CoreFoundation.h>

#include "guest_mem.h"
#include "handles.h"
#include "recomp_types.h"

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define STACK_TOP 0x00FF0000u
#define SCRATCH 0x00F10000u
#define RET_MAGIC 0xDEADBEEFu

void imp__IOIteratorNext(void);
void imp__UCKeyTranslate(void);
void imp__socket(void);

/* Host functions, from Carbon. */
extern uint8_t LMGetKbdType(void);
extern CFTypeRef TISCopyCurrentKeyboardLayoutInputSource(void);
extern void *TISGetInputSourceProperty(CFTypeRef source, CFStringRef key);
extern CFStringRef kTISPropertyUnicodeKeyLayoutData;

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

/* Call a thunk with the given argument words and check the protocol: esp up
 * by 4, callee-saved registers unchanged. */
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

static void test_iokit(void)
{
    uint32_t args[] = {0x1234};
    CHECK(call(imp__IOIteratorNext, args, 1) == 0, "IOIteratorNext != 0");
}

static void test_socket(void)
{
    MEM32(guest_errno_slot()) = 0;
    uint32_t args[] = {2, 1, 0};
    CHECK((int)call(imp__socket, args, 3) == -1, "socket != -1");
    CHECK(MEM32(guest_errno_slot()) == ENETDOWN, "errno %u, want ENETDOWN", MEM32(guest_errno_slot()));
}

static void test_keyboard(void)
{
    /* A NULL layout is paramErr with length 0. */
    MEM32(SCRATCH + 0x40) = 7;
    uint32_t null_args[] = {0, 0, 0, 0, 0, 0, SCRATCH + 0x44, 4, SCRATCH + 0x40, SCRATCH + 0x50};
    CHECK((int)call(imp__UCKeyTranslate, null_args, 10) == -50, "UCKeyTranslate(NULL layout) != -50");
    CHECK(MEM32(SCRATCH + 0x40) == 0, "actualStringLength not zeroed");

    /* A real layout from the host: the space key types ' '. */
    CFTypeRef source = TISCopyCurrentKeyboardLayoutInputSource();
    CFDataRef data = source ? (CFDataRef)TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) : NULL;
    if (data == NULL) {
        fputs("skip: no host unicode keyboard layout\n", stderr);
        if (source)
            CFRelease(source);
        return;
    }
    uint32_t layout = SCRATCH + 0x1000;
    memcpy(guest_ptr(layout), CFDataGetBytePtr(data), (size_t)CFDataGetLength(data));
    MEM32(SCRATCH + 0x40) = 7;
    MEM32(SCRATCH + 0x44) = 0;
    uint32_t args[] = {layout, 49, 0, 0, LMGetKbdType(), 1, SCRATCH + 0x44, 4, SCRATCH + 0x40, SCRATCH + 0x50};
    CHECK((int)call(imp__UCKeyTranslate, args, 10) == 0, "UCKeyTranslate(space) != 0");
    CHECK(MEM32(SCRATCH + 0x40) == 1, "UCKeyTranslate length %u, want 1", MEM32(SCRATCH + 0x40));
    CHECK(*(uint16_t *)guest_ptr(SCRATCH + 0x50) == ' ', "UCKeyTranslate char %x, want 0x20",
          *(uint16_t *)guest_ptr(SCRATCH + 0x50));
    CFRelease(source);
}

/* The generated dispatch table defines this in the game build; no guest
 * functions exist in this test. */
recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
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
    thunk_files_init();
    test_iokit();
    test_socket();
    test_keyboard();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}

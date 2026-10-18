/* First-pass system thunks: IOKit, keyboard layout and sockets. The game runs
 * without gamepads and offline. Every call returns a value the game accepts
 * and does no real work. Audio is in thunk_audio.c. */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "guest_mem.h"
#include "handles.h"
#include "recomp_types.h"
#include "thunk_cf.h"

#define ARG(n) MEM32(g_esp + 4u + 4u * (n))
#define RET(v) do { g_eax = (uint32_t)(v); g_esp += 4; } while (0)

#define K_IO_RETURN_UNSUPPORTED 0xE00002C7u
#define K_PARAM_ERR (-50)

/* Host functions, from Carbon and IOKit. */
extern uint8_t LMGetKbdType(void);
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern CFTypeRef TISCopyCurrentKeyboardInputSource(void);
extern void *TISGetInputSourceProperty(CFTypeRef source, CFStringRef key);
extern int32_t UCKeyTranslate(const void *keyLayoutPtr, uint16_t virtualKeyCode, uint16_t keyAction,
                              uint32_t modifierKeyState, uint32_t keyboardType, uint32_t keyTranslateOptions,
                              uint32_t *deadKeyState, unsigned long maxStringLength,
                              unsigned long *actualStringLength, uint16_t *unicodeString);

/* Backing object for every fake handle; handle_put needs a non-NULL pointer. */
static char g_fake_object;

static uint32_t fake_handle(enum handle_kind kind)
{
    return handle_put(&g_fake_object, kind);
}

/* Store v at guest address addr unless addr is NULL. */
static void put32(uint32_t addr, uint32_t v)
{
    if (addr != 0)
        MEM32(addr) = v;
}

/* ---- IOKit: no devices ---------------------------------------------------- */

void imp__IOMasterPort(void)
{
    put32(ARG(1), 0);
    RET(0);
}

/* The real API returns a CFMutableDictionary that the caller fills in. */
void imp__IOServiceMatching(void)
{
    const char *name = ARG(0) ? (const char *)guest_ptr(ARG(0)) : NULL;
    RET(cf_to_guest(IOServiceMatching(name)));
}

/* The real API consumes one reference to the matching dictionary (ARG(2)). */
void imp__IOServiceAddMatchingNotification(void)
{
    cf_release(cf_from_guest(ARG(2)));
    put32(ARG(5), fake_handle(HANDLE_IOKIT));
    RET(0);
}

void imp__IOServiceAddInterestNotification(void)
{
    put32(ARG(5), fake_handle(HANDLE_IOKIT));
    RET(0);
}

/* No devices: the iterator is always empty. */
void imp__IOIteratorNext(void) { RET(0); }

void imp__IONotificationPortCreate(void) { RET(fake_handle(HANDLE_IOKIT)); }

/* A 0 source is a no-op for the CF lane. */
void imp__IONotificationPortGetRunLoopSource(void) { RET(0); }

void imp__IOObjectRelease(void)
{
    handle_release(ARG(0), HANDLE_IOKIT);
    RET(0);
}

void imp__IOCreatePlugInInterfaceForService(void)
{
    put32(ARG(3), 0);
    put32(ARG(4), 0);
    RET(K_IO_RETURN_UNSUPPORTED);
}

void imp__IODestroyPlugInInterface(void) { RET(0); }

void imp__IORegistryEntryCreateCFProperties(void)
{
    put32(ARG(1), 0);
    RET(0);
}

void imp__IORegistryEntryGetParentEntry(void)
{
    put32(ARG(2), 0);
    RET(0);
}

/* IORegistryEntrySearchCFProperty(entry, plane, key, allocator, options).
 * 2a4eb0 reads the GPU's PCI "vendor-id" and "device-id" (4-byte CFData,
 * little-endian) from the display's IOService parents and calls
 * CFDataGetBytePtr with no NULL or type check, so on the Macs the game targets
 * both are always present. The host GPU has no PCI ids: report Apple's vendor
 * id 0x106B and device 0. Any other key is absent, NULL as in the real API. */
void imp__IORegistryEntrySearchCFProperty(void)
{
    CFTypeRef key = cf_from_guest(ARG(2));
    uint32_t id;
    if (CFEqual(key, CFSTR("vendor-id")))
        id = 0x106Bu;
    else if (CFEqual(key, CFSTR("device-id")))
        id = 0;
    else {
        RET(0);
        return;
    }
    RET(cf_to_guest(CFDataCreate(NULL, (const UInt8 *)&id, sizeof id)));
}

/* TODO: call the host IOPMAssertionCreateWithName once the CF lane provides
 * cf_from_guest() to convert the two CFString arguments (none exists yet), so
 * the display stays awake. Until then no host call is made. */
void imp__IOPMAssertionCreateWithName(void)
{
    put32(ARG(3), 0);
    RET(0);
}

/* ---- Keyboard layout ------------------------------------------------------ */

/* Keyboard text input will come from the SDL front end, not from these. */
void imp__LMGetKbdType(void) { RET(LMGetKbdType()); }

/* A CF object in the real API; the guest releases it with CFRelease. */
void imp__TISCopyCurrentKeyboardInputSource(void) { RET(cf_to_guest(TISCopyCurrentKeyboardInputSource())); }

/* Get rule: the input source owns the result. sub_003DC8B0 passes the
 * InputSourceLanguages array straight to CFArrayGetValueAtIndex (0x003DC92F)
 * with no NULL check, so the thunk returns the host's real property. */
void imp__TISGetInputSourceProperty(void)
{
    RET(cf_to_guest(TISGetInputSourceProperty(cf_from_guest(ARG(0)), (CFStringRef)cf_from_guest(ARG(1)))));
}

/* UCKeyTranslate(layout, keyCode, keyAction, modifiers, keyboardType, options,
 * deadKeyState, maxLength, actualLength, string). The layout is the guest copy
 * of the host CFData bytes; UCKeyboardLayout uses offsets only, so the copy is
 * valid in place. UniCharCount is 4 bytes in the guest and 8 on the host. */
void imp__UCKeyTranslate(void)
{
    if (ARG(0) == 0 || ARG(6) == 0 || ARG(8) == 0) {
        put32(ARG(8), 0); /* *actualStringLength */
        RET(K_PARAM_ERR);
        return;
    }
    unsigned long actual = 0;
    int32_t status = UCKeyTranslate(guest_ptr(ARG(0)), (uint16_t)ARG(1), (uint16_t)ARG(2), ARG(3), ARG(4), ARG(5),
                                    guest_ptr(ARG(6)), ARG(7), &actual,
                                    ARG(9) ? guest_ptr(ARG(9)) : NULL);
    put32(ARG(8), (uint32_t)actual);
    RET(status);
}

/* ---- Sockets: the network is down ----------------------------------------- */

static void net_down(void)
{
    guest_set_errno(ENETDOWN);
    RET(-1);
}

void imp__socket(void) { net_down(); }
void imp__connect_UNIX2003(void) { net_down(); }
void imp__send_UNIX2003(void) { net_down(); }
void imp__recv_UNIX2003(void) { net_down(); }
void imp__setsockopt(void) { net_down(); }
void imp__shutdown(void) { net_down(); }

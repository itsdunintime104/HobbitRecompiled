/* Gamepads through SDL's GameController API. Native replacements for the
 * five functions of the game's pad layer (block 0x3d18a0-0x3d1c00), which sit
 * above the HID Utilities library. They return fake device records in guest
 * memory and fill the game's 0x110-byte pad state from SDL. Layouts and the
 * reasoning are in docs/FRONTEND.md, section "Gamepad".
 *
 * The lifted originals are weak symbols (recon/native_overrides.txt); the
 * strong definitions here win at link time, including for the callers in the
 * same generated chunk. Every function is called like any lifted function:
 * cdecl arguments at g_esp+4, return address at g_esp; it returns in g_eax and
 * pops the return address. */
#define RECOMP_GENERATED_CODE
#include <SDL.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "diag.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "recomp_types.h"

#define ARG(n) MEM32(g_esp + 4u + 4u * (n))
#define RET(v) do { g_eax = (uint32_t)(v); g_esp += 4; } while (0)

/* Device record offsets (HID Utilities pRecDevice, as the game reads them). */
#define DEV_SIZE 0x470u
#define DEV_VENDOR 0x114u
#define DEV_PRODUCT 0x118u
#define DEV_NAME 0x220u       /* char[0x40] */
#define DEV_USAGE 0x424u
#define DEV_USAGE_PAGE 0x428u
#define DEV_N_AXES 0x440u
#define DEV_N_BUTTONS 0x444u
#define DEV_N_HATS 0x448u
#define DEV_GONE 0x458u       /* byte: device unplugged */
#define DEV_CLAIMED 0x459u    /* byte: handed to the game */

/* Pad state (filled by 3d1af0). */
#define ST_SIZE 0x110u
#define ST_AXIS 0x00u         /* 6 x int32: X Y Z Rx Ry Rz */
#define ST_HAT 0x20u          /* 4 x int32, centidegrees, -1 neutral */
#define ST_BUTTON 0x30u       /* up to 0x80 bytes, 0 or 0xFF */

#define PAD_VENDOR 0x045Eu    /* every pad reports as a wired Xbox 360 pad */
#define PAD_PRODUCT 0x028Eu
#define MAX_PADS 8
#define EVQ_LEN 32

/* Button byte n is "PADDIGITAL,n" in the game's ControllerMappings.txt. The
 * entry for 045e:028e ("XBOX 360 For Windows") binds 2-5 D-pad up, down,
 * left, right; 6 start; 7 back; 8/9 stick clicks; 10 r1; 11 l1; 13 A (rdown);
 * 14 B (rright); 15 X (rleft); 16 Y (rup). The PS3 entry in the same file
 * matches the DualShock 3 HID order, which confirms n is the 0-based byte. */
static const SDL_GameControllerButton BUTTON_ORDER[] = {
    SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_INVALID,
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_GUIDE, SDL_CONTROLLER_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
};
#define N_BUTTONS ((int)(sizeof BUTTON_ORDER / sizeof BUTTON_ORDER[0]))

static struct {
    SDL_GameController *gc;
    SDL_JoystickID id;
} s_pad[MAX_PADS];

static struct { int add; int which; } s_evq[EVQ_LEN]; /* which: index or instance id */
static int s_evq_n;
static uint32_t s_recs;   /* guest block of MAX_PADS records */
static int s_inited;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t rec_addr(int i) { return s_recs + (uint32_t)i * DEV_SIZE; }

/* SDL calls this on whichever thread pushes the event; only queue here. */
static int pad_event_watch(void *ud, SDL_Event *e)
{
    (void)ud;
    if (e->type != SDL_CONTROLLERDEVICEADDED && e->type != SDL_CONTROLLERDEVICEREMOVED)
        return 0;
    pthread_mutex_lock(&s_lock);
    if (s_evq_n < EVQ_LEN) {
        s_evq[s_evq_n].add = e->type == SDL_CONTROLLERDEVICEADDED;
        s_evq[s_evq_n].which = e->cdevice.which;
        s_evq_n++;
    } else {
        diag_unsupported(__func__, 0, "hot-plug queue full");
    }
    pthread_mutex_unlock(&s_lock);
    return 0;
}

static void pad_add(int index)
{
    SDL_GameController *gc = SDL_GameControllerOpen(index);
    if (!gc)
        return;
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    int free_slot = -1;
    for (int i = 0; i < MAX_PADS; i++) {
        if (s_pad[i].gc && s_pad[i].id == id) { /* already open: drop the extra reference */
            SDL_GameControllerClose(gc);
            return;
        }
        if (!s_pad[i].gc && !MEM8(rec_addr(i) + DEV_GONE) && free_slot < 0)
            free_slot = i; /* a gone pad keeps its slot until the game releases it */
    }
    if (free_slot < 0) {
        diag_unsupported(__func__, 0, "pad slots full");
        SDL_GameControllerClose(gc);
        return;
    }
    uint32_t r = rec_addr(free_slot);
    memset(guest_ptr(r), 0, DEV_SIZE);
    MEM32(r + DEV_VENDOR) = PAD_VENDOR;
    MEM32(r + DEV_PRODUCT) = PAD_PRODUCT;
    const char *name = SDL_GameControllerName(gc);
    if (name)
        strncpy((char *)guest_ptr(r + DEV_NAME), name, 0x3F);
    MEM32(r + DEV_USAGE) = 5;       /* generic desktop: game pad */
    MEM32(r + DEV_USAGE_PAGE) = 1;
    MEM32(r + DEV_N_AXES) = 6;
    MEM32(r + DEV_N_BUTTONS) = (uint32_t)N_BUTTONS;
    MEM32(r + DEV_N_HATS) = 1;
    s_pad[free_slot].gc = gc;
    s_pad[free_slot].id = id;
}

/* Unplugged: close SDL, mark the record gone. The game drops its pointer when
 * 3d1af0 fails and then calls 3d1ae0, which frees the slot. */
static void pad_remove(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_PADS; i++) {
        if (s_pad[i].gc && s_pad[i].id == id) {
            SDL_GameControllerClose(s_pad[i].gc);
            s_pad[i].gc = NULL;
            MEM8(rec_addr(i) + DEV_GONE) = 1;
        }
    }
}

/* Runs with s_lock held at the start of every entry point. */
static void pad_sync(void)
{
    if (!s_inited) {
        s_inited = 1;
        if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        s_recs = guest_calloc(MAX_PADS * DEV_SIZE, 1);
        SDL_AddEventWatch(pad_event_watch, NULL);
        for (int i = 0; i < SDL_NumJoysticks(); i++) /* pads present before the watch */
            if (SDL_IsGameController(i))
                pad_add(i);
    }
    while (s_evq_n > 0) {
        int add = s_evq[0].add, which = s_evq[0].which;
        memmove(&s_evq[0], &s_evq[1], (size_t)(s_evq_n - 1) * sizeof s_evq[0]);
        s_evq_n--;
        if (add)
            pad_add(which);
        else
            pad_remove((SDL_JoystickID)which);
    }
}

static int slot_of(uint32_t dev)
{
    if (dev < s_recs || dev >= s_recs + MAX_PADS * DEV_SIZE || (dev - s_recs) % DEV_SIZE)
        return -1;
    return (int)((dev - s_recs) / DEV_SIZE);
}

/* 3d18a0(uint32_t *out, int max): claim every present pad, return the count. */
void sub_003D18A0(void)
{
    uint32_t out = ARG(0);
    int max = (int)ARG(1), n = 0;
    pthread_mutex_lock(&s_lock);
    pad_sync();
    for (int i = 0; i < MAX_PADS && n < max; i++) {
        if (s_pad[i].gc) {
            MEM8(rec_addr(i) + DEV_CLAIMED) = 1;
            MEM32(out + 4u * (uint32_t)n++) = rec_addr(i);
        }
    }
    pthread_mutex_unlock(&s_lock);
    RET(n);
}

/* 3d19c0(uint32_t *out, int max): pads not yet claimed (hot-plug). */
void sub_003D19C0(void)
{
    uint32_t out = ARG(0);
    int max = (int)ARG(1), n = 0;
    pthread_mutex_lock(&s_lock);
    pad_sync();
    for (int i = 0; i < MAX_PADS && n < max; i++) {
        if (s_pad[i].gc && !MEM8(rec_addr(i) + DEV_CLAIMED)) {
            MEM8(rec_addr(i) + DEV_CLAIMED) = 1;
            MEM32(out + 4u * (uint32_t)n++) = rec_addr(i);
        }
    }
    pthread_mutex_unlock(&s_lock);
    RET(n);
}

/* 3d1a80(dev): Xbox-type pad. All of ours report as one. */
void sub_003D1A80(void)
{
    RET(1);
}

/* 3d1ae0(dev): release. An unplugged pad's slot becomes free. */
void sub_003D1AE0(void)
{
    pthread_mutex_lock(&s_lock);
    int i = slot_of(ARG(0));
    if (i >= 0 && !s_pad[i].gc)
        memset(guest_ptr(rec_addr(i)), 0, DEV_SIZE);
    pthread_mutex_unlock(&s_lock);
    RET(0);
}

static int32_t hat_centidegrees(SDL_GameController *gc)
{
    int u = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP);
    int d = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    int l = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    int r = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    int x = r - l, y = u - d; /* opposite directions cancel */
    if (x == 0 && y == 0) return -1;
    if (x == 0) return y > 0 ? 0 : 18000;
    if (y == 0) return x > 0 ? 9000 : 27000;
    if (x > 0) return y > 0 ? 4500 : 13500;
    return y > 0 ? 31500 : 22500;
}

/* 3d1af0(state *, dev) -> 1 on success, 0 if the pad is gone. */
void sub_003D1AF0(void)
{
    uint32_t st = ARG(0);
    int ok = 0;
    pthread_mutex_lock(&s_lock);
    pad_sync();
    int i = slot_of(ARG(1));
    if (i >= 0 && s_pad[i].gc && !MEM8(rec_addr(i) + DEV_GONE)) {
        SDL_GameController *gc = s_pad[i].gc;
        memset(guest_ptr(st), 0, ST_SIZE);
        SDL_GameControllerUpdate();
        /* Xbox-type layout: sticks signed 16-bit, triggers 0..32767.
         * Y keeps SDL's sign (negative up); unconfirmed against the game. */
        static const SDL_GameControllerAxis AXIS_ORDER[6] = {
            SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
            SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_RIGHTX,
            SDL_CONTROLLER_AXIS_RIGHTY, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
        };
        for (int a = 0; a < 6; a++)
            MEM32(st + ST_AXIS + 4u * (uint32_t)a) =
                (uint32_t)(int32_t)SDL_GameControllerGetAxis(gc, AXIS_ORDER[a]);
        for (int h = 0; h < 4; h++)
            MEM32(st + ST_HAT + 4u * (uint32_t)h) = 0xFFFFFFFFu;
        MEM32(st + ST_HAT) = (uint32_t)hat_centidegrees(gc);
        for (int b = 0; b < N_BUTTONS; b++)
            MEM8(st + ST_BUTTON + (uint32_t)b) =
                BUTTON_ORDER[b] != SDL_CONTROLLER_BUTTON_INVALID &&
                SDL_GameControllerGetButton(gc, BUTTON_ORDER[b]) ? 0xFF : 0;
        ok = 1;
    }
    pthread_mutex_unlock(&s_lock);
    RET(ok);
}

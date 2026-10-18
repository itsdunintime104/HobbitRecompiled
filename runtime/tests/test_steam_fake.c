/* Unit test for runtime/src/steam_fake.c: calls fake Steam methods through the
 * guest vtables with guest_call_cdecl. Prints "ok" and exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "recomp_types.h"
#include "guest_call.h"
#include "guest_mem.h"
#include "steam_fake.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Hooks the glue and the host function table leave to generated code. */
recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

const struct {
    const char *name;
    recomp_func_t fn;
} g_import_table[1] = {{"_none", NULL}};
const unsigned g_import_count = 0;

int import_func_is_placeholder(recomp_func_t fn)
{
    (void)fn;
    return 0;
}

static uint32_t rd32(uint32_t a)
{
    uint32_t v;
    memcpy(&v, guest_ptr(a), sizeof v);
    return v;
}

/* Call a slot of the interface object obj with this plus nextra arguments. */
static uint32_t call_slotv(uint32_t obj, unsigned slot, int nextra, const uint32_t *extra)
{
    uint32_t args[5] = {obj};
    for (int i = 0; i < nextra; i++)
        args[1 + i] = extra[i];
    uint32_t fn = rd32(rd32(obj) + 4 * slot);
    return guest_call_cdecl(fn, 1 + nextra, args);
}

static uint32_t call_slot(uint32_t obj, unsigned slot, int nextra, uint32_t a1)
{
    return call_slotv(obj, slot, nextra, &a1);
}

/* Guest scratch area for names and data. */
#define SCRATCH 0x00F10000u
#define NAME_A (SCRATCH)
#define NAME_B (SCRATCH + 0x200)
#define BUF_IN (SCRATCH + 0x1000)
#define BUF_OUT (SCRATCH + 0x2000)
#define PSIZE (SCRATCH + 0x3000)

static uint32_t put_name(uint32_t at, const char *name)
{
    memcpy(guest_ptr(at), name, strlen(name) + 1);
    return at;
}

static uint32_t rs_write(uint32_t obj, const char *name, const void *data, uint32_t cub)
{
    memcpy(guest_ptr(BUF_IN), data, cub);
    uint32_t a[3] = {put_name(NAME_A, name), BUF_IN, cub};
    return call_slotv(obj, 0, 3, a);
}

static uint32_t rs_read(uint32_t obj, const char *name, uint32_t cap)
{
    memset(guest_ptr(BUF_OUT), 0xEE, 64);
    uint32_t a[3] = {put_name(NAME_A, name), BUF_OUT, cap};
    return call_slotv(obj, 1, 3, a);
}

static uint32_t rs_by_name(uint32_t obj, unsigned slot, const char *name)
{
    return call_slot(obj, slot, 1, put_name(NAME_A, name));
}

/* The bytes that the Python test puts in the seeded Steam save file. */
static const unsigned char SEED[] = {'s', 'e', 'e', 'd', 0x00, 0x01, 0xfe, 0xff, '-', 'e', 'n', 'd'};

int main(void)
{
    CHECK(guest_mem_init() == 0);
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    CHECK(mprotect(guest_ptr(0x00F00000u), 0x00080000u, PROT_READ | PROT_WRITE) == 0);
    g_esp = 0x00F7FFF0u;

    uint32_t rs = steam_fake_iface("SteamRemoteStorage");
    uint32_t apps = steam_fake_iface("SteamApps");
    uint32_t user = steam_fake_iface("SteamUser");
    uint32_t utils = steam_fake_iface("SteamUtils");
    uint32_t stats = steam_fake_iface("SteamUserStats");
    CHECK(rs != 0 && apps != 0 && user != 0 && utils != 0 && rs != apps);
    CHECK(steam_fake_iface("SteamRemoteStorage") == rs);

    /* ISteamRemoteStorage 012 slot 18: bool IsCloudEnabledForAccount() */
    g_eax = 0xCCCCCCCCu;
    CHECK(call_slot(rs, 18, 0, 0) == 0);
    /* A null name fails on every slot that takes one. */
    CHECK(call_slot(rs, 10, 1, 0) == 0);
    g_edx = 0xCCCCCCCCu;
    CHECK(call_slot(rs, 13, 1, 0) == 0 && g_edx == 0);

    /* (a) The Steam save is seeded: HOME is a temp dir with one Steam file. */
    const char *seeded = "SavedGames\\slot1\\game1.SaveGameData";
    CHECK(rs_by_name(rs, 10, seeded) == 1);
    CHECK(rs_by_name(rs, 12, seeded) == sizeof SEED);
    CHECK(rs_read(rs, seeded, 4096) == sizeof SEED);
    CHECK(memcmp(guest_ptr(BUF_OUT), SEED, sizeof SEED) == 0);
    CHECK(rs_read(rs, seeded, 5) == 5 && memcmp(guest_ptr(BUF_OUT), SEED, 5) == 0);
    CHECK(call_slot(rs, 15, 0, 0) == 1);

    /* (b) Write a new file, read it back, count and list it. */
    const char payload[] = "payload\0with\xff" "bytes";
    const char *fresh = "SavedGames\\Slot2\\Other.Dat";
    CHECK(rs_by_name(rs, 10, fresh) == 0 && rs_by_name(rs, 12, fresh) == 0);
    CHECK(rs_write(rs, fresh, payload, sizeof payload) == 1);
    CHECK(rs_by_name(rs, 10, fresh) == 1);
    CHECK(rs_by_name(rs, 12, fresh) == sizeof payload);
    CHECK(rs_read(rs, fresh, 4096) == sizeof payload);
    CHECK(memcmp(guest_ptr(BUF_OUT), payload, sizeof payload) == 0);
    CHECK(rs_read(rs, "savedgames/slot2/other.dat", 4096) == sizeof payload);
    g_edx = 0xCCCCCCCCu;
    uint32_t ts_lo = rs_by_name(rs, 13, fresh);
    CHECK(g_edx == 0 && ts_lo > 0);
    CHECK(call_slot(rs, 15, 0, 0) == 2);
    /* An overwrite replaces the content. */
    CHECK(rs_write(rs, fresh, "xy", 2) == 1);
    CHECK(rs_by_name(rs, 12, fresh) == 2 && rs_read(rs, fresh, 4096) == 2);
    CHECK(call_slot(rs, 15, 0, 0) == 2);
    /* Sorted by relative path with '/': slot1 before slot2. */
    uint32_t a1[2] = {0, PSIZE};
    uint32_t n0 = call_slotv(rs, 16, 2, a1);
    CHECK(n0 != 0 && strcmp((const char *)guest_ptr(n0), "savedgames/slot1/game1.savegamedata") == 0);
    CHECK(rd32(PSIZE) == sizeof SEED);
    a1[0] = 1;
    uint32_t n1 = call_slotv(rs, 16, 2, a1);
    CHECK(n1 != 0 && strcmp((const char *)guest_ptr(n1), "savedgames/slot2/other.dat") == 0);
    CHECK(rd32(PSIZE) == 2);
    CHECK(call_slotv(rs, 16, 2, a1) == n1); /* one guest string per name */
    a1[0] = 2;
    CHECK(call_slotv(rs, 16, 2, a1) == 0);

    /* (c) Delete removes it; a second delete fails. */
    CHECK(rs_by_name(rs, 3, fresh) == 1);
    CHECK(rs_by_name(rs, 10, fresh) == 0 && rs_by_name(rs, 3, fresh) == 0);
    CHECK(call_slot(rs, 15, 0, 0) == 1);

    /* (d) Names that leave the root fail and write nothing. */
    const char *bad[] = {"../evil", "..\\evil", "/abs/evil", "\\evil", "a/../../evil",
                         "a\\..\\..\\evil", "..", ""};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(rs_write(rs, bad[i], "x", 1) == 0);
        CHECK(rs_read(rs, bad[i], 16) == 0);
        CHECK(rs_by_name(rs, 10, bad[i]) == 0);
        CHECK(rs_by_name(rs, 12, bad[i]) == 0);
        CHECK(rs_by_name(rs, 3, bad[i]) == 0);
    }
    CHECK(call_slot(rs, 15, 0, 0) == 1);

    /* ISteamApps 006 slot 4: const char *GetCurrentGameLanguage() */
    uint32_t lang = call_slot(apps, 4, 0, 0);
    CHECK(lang != 0 && strcmp((const char *)guest_ptr(lang), "english") == 0);
    /* slot 6 BIsSubscribedApp: the base game only; slot 7 BIsDlcInstalled: none */
    CHECK(call_slot(apps, 6, 1, 285160) == 1);
    CHECK(call_slot(apps, 6, 1, 285161) == 0);
    CHECK(call_slot(apps, 7, 1, 285161) == 0);

    /* ISteamUser 017 slot 1: bool BLoggedOn(); ISteamUtils 006 slot 9: GetAppID() */
    CHECK(call_slot(user, 1, 0, 0) == 0);
    CHECK(call_slot(utils, 9, 0, 0) == 285160);

    /* ISteamUserStats 011 slot 0 RequestCurrentStats; slot 7 SetAchievement(name):
     * offline, stats are never received, so both return false. */
    CHECK(call_slot(stats, 0, 0, 0) == 0);
    CHECK(call_slot(stats, 7, 1, put_name(NAME_A, "ACH_TEST")) == 0);

    puts("ok");
    return 0;
}

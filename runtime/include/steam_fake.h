/* Fake Steam interface objects for "Steam running, user offline, cloud
 * disabled, no DLC beyond the base game". See runtime/src/steam_fake.c. */
#ifndef STEAM_FAKE_H
#define STEAM_FAKE_H

#include <stdint.h>

/* Guest address of the interface object for a Steam accessor name:
 * "SteamRemoteStorage", "SteamApps", "SteamUser", "SteamUserStats" or
 * "SteamUtils". The object is one u32 vtable pointer to a guest vtable of
 * host function addresses. Created on first use and never freed. An unknown
 * name aborts. */
uint32_t steam_fake_iface(const char *accessor);

#endif

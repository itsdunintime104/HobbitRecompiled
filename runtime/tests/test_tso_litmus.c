/* Message-passing litmus test for the TSO accessors in recomp_types.h.
 *
 * argv[1] picks the mode (default "ordered"):
 *   ordered  writer GST32(data); GST32(flag).   reader f = GLD32(flag); d = GLD32(data).
 *   publish  writer MEM32(data) = i; GST32(flag).  reader f = GLD32(flag); d = GLD32(data).
 *            A plain frame store handed over by an ordered store.
 *   consume  writer GST32(data); GST32(flag).   reader f = GLD32(flag); d = MEM32(data).
 *            An ordered load that hands over to a plain frame load.
 * On x86 the reader can never see d < f. On arm64 plain stores and loads can
 * be reordered, so with -DRECOMP_TSO=0 violations may appear; with
 * -DRECOMP_TSO=1 (the lifter default) there must be none.
 *
 * Exit code: 1 if RECOMP_TSO is 1 and a violation was seen, else 0. */
#include "recomp_types.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROUNDS 10000000u
/* Guest addresses on different 128-byte cache lines. */
#define DATA_VA 0x1000u
#define FLAG_VA 0x1100u

ptrdiff_t g_xbox_mem_offset;

typedef enum { ORDERED, PUBLISH, CONSUME } litmus_mode;
static litmus_mode g_mode = ORDERED;

static uint64_t g_reads;
static uint64_t g_violations;

static void *writer(void *arg) {
    (void)arg;
    for (uint32_t i = 1; i <= ROUNDS; i++) {
        if (g_mode == PUBLISH) MEM32(DATA_VA) = i;
        else GST32(DATA_VA, i);
        GST32(FLAG_VA, i);
    }
    return NULL;
}

static void *reader(void *arg) {
    (void)arg;
    uint64_t reads = 0, violations = 0;
    uint32_t f;
    do {
        f = GLD32(FLAG_VA);
        uint32_t d = g_mode == CONSUME ? MEM32(DATA_VA) : GLD32(DATA_VA);
        if (d < f) violations++;
        reads++;
    } while (f != ROUNDS);
    g_reads = reads;
    g_violations = violations;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (!strcmp(argv[1], "publish")) g_mode = PUBLISH;
        else if (!strcmp(argv[1], "consume")) g_mode = CONSUME;
        else if (strcmp(argv[1], "ordered")) return 2;
    }
    uint8_t *host = calloc(1, 0x2000);
    if (!host) return 2;
    g_xbox_mem_offset = (ptrdiff_t)host;
    pthread_t w, r;
    pthread_create(&r, NULL, reader, NULL);
    pthread_create(&w, NULL, writer, NULL);
    pthread_join(w, NULL);
    pthread_join(r, NULL);
    printf("mode=%s RECOMP_TSO=%d rounds=%u reads=%llu violations=%llu\n", argc > 1 ? argv[1] : "ordered", RECOMP_TSO, ROUNDS,
           (unsigned long long)g_reads, (unsigned long long)g_violations);
    return (RECOMP_TSO && g_violations) ? 1 : 0;
}

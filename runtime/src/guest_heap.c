#include "guest_heap.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "diag.h"
#include "guest_mem.h"
#include "recomp_types.h"

/* dlmalloc.c is built with ONLY_MSPACES; these are its entry points. */
typedef void *mspace;
mspace create_mspace_with_base(void *base, size_t capacity, int locked);
void *mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void *mem);
void *mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);
void *mspace_realloc(mspace msp, void *mem, size_t newsize);

static mspace g_heap;
static pthread_once_t g_heap_once = PTHREAD_ONCE_INIT;

__thread uint32_t g_heap_site;

/* Heap guard layout: [16-byte header][n user bytes][16-byte 0xFB tail]. */
#define GUARD_LIVE 0x4C495645u
#define GUARD_FREE 0x46524545u
#define GUARD_HDR 16u
#define GUARD_TAIL 16u
#define GUARD_TAIL_BYTE 0xFBu
#define GUARD_FREE_BYTE 0xDDu
#define QUAR_MAX_ENTRIES 65536u
#define QUAR_MAX_BYTES ((uint64_t)64 << 20)
#define CHECK_PERIOD_S 1u
#define REPORT_KEYS 256u
#define REPORT_LIMIT 1000u

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t alloc_site;
    uint32_t free_site;
} GuardHeader;

typedef struct {
    uint32_t user; /* guest address of the user bytes */
    uint32_t size;
} QuarEntry;

/* 0 = off, 1 = abort on the first finding, 2 = log each finding once and go on. */
static int g_guard;
static pthread_mutex_t g_guard_lock = PTHREAD_MUTEX_INITIALIZER;
static QuarEntry *g_quar;
static uint32_t g_quar_head;
static uint32_t g_quar_count;
static uint64_t g_quar_bytes;

typedef struct {
    const char *kind;
    uint32_t alloc_site;
    uint32_t free_site;
} ReportKey;

static ReportKey g_report_keys[REPORT_KEYS];
static uint32_t g_report_key_count;
static uint32_t g_report_total;

static void *guard_checker(void *arg);

static void guard_start(int mode)
{
    g_quar = calloc(QUAR_MAX_ENTRIES, sizeof *g_quar);
    if (g_quar == NULL) {
        fprintf(stderr, "guest_heap_guard: out of memory, guard disabled\n");
        return;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    /* Mode 2 has no periodic scan: it would hold the lock for up to 64 MiB of
     * compares every second. Quarantined blocks are checked on eviction. */
    int rc = mode == 1 ? pthread_create(&tid, &attr, guard_checker, NULL) : 0;
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "guest_heap_guard: cannot start checker thread, guard disabled\n");
        free(g_quar);
        g_quar = NULL;
        return;
    }
    g_guard = mode;
}

static void heap_setup(void)
{
    if (g_guest_base == NULL)
        return;
    size_t size = (size_t)(GUEST_HEAP_END - GUEST_HEAP_BASE);
    void *base = guest_ptr(GUEST_HEAP_BASE);
    /* The kernel commits pages on first touch, so mapping the whole range
     * read/write costs address space only. */
    if (mprotect(base, size, PROT_READ | PROT_WRITE) != 0)
        return;
    g_heap = create_mspace_with_base(base, size, 1);
    const char *env = getenv("RECOMP_HEAP_GUARD");
    if (g_heap != NULL && env != NULL && (strcmp(env, "1") == 0 || strcmp(env, "2") == 0))
        guard_start(env[0] - '0');
}

int guest_heap_init(void)
{
    pthread_once(&g_heap_once, heap_setup);
    return g_heap != NULL ? 0 : -1;
}

int guest_heap_guard_mode(void)
{
    return g_guard;
}

/* dlmalloc calls this when a heap check fails (see dlmalloc.c). It prints
 * the dlmalloc.c line of the check, the pointer as a guest address with the
 * 16 bytes on each side, and the latest guest indirect calls, then aborts so
 * the crash report keeps the stack. */
void guest_heap_error(const char *what, int line, const void *p)
{
    const uint8_t *b = p;
    fprintf(stderr, "guest_heap: %s (dlmalloc.c:%d)", what, line);
    if (b != NULL && b - 16 >= g_guest_base + GUEST_HEAP_BASE &&
        b + 16 <= g_guest_base + GUEST_HEAP_END) {
        fprintf(stderr, " at 0x%08X:", (uint32_t)(b - g_guest_base));
        for (int i = -16; i < 16; i++)
            fprintf(stderr, "%s%02X", i == 0 ? " | " : " ", b[i]);
    } else if (b != NULL) {
        fprintf(stderr, " at host %p", p);
    }
    char tail[ICALL_TRACE_SIZE * 12];
    recomp_icall_tail(tail, sizeof tail, 8);
    fprintf(stderr, "\nguest_heap: last icalls (newest first): %s\n", tail);
    abort();
}

static uint32_t to_guest(void *p)
{
    return p == NULL ? 0 : (uint32_t)((uint8_t *)p - g_guest_base);
}

static void put_site(uint32_t site, int known)
{
    if (!known)
        fputs("-", stderr);
    else if (site == 0)
        fputs("runtime", stderr);
    else
        fprintf(stderr, "0x%08X", site);
}

/* Mode 2: true if this finding was already reported, or the report limit is
 * reached. Records new findings. */
static int guard_report_skip(const char *kind, const GuardHeader *h)
{
    for (uint32_t i = 0; i < g_report_key_count; i++) {
        const ReportKey *k = &g_report_keys[i];
        if (strcmp(k->kind, kind) == 0 && k->alloc_site == h->alloc_site &&
            k->free_site == h->free_site)
            return 1;
    }
    if (g_report_total == REPORT_LIMIT)
        fputs("guest_heap_guard: report limit reached\n", stderr);
    if (g_report_total >= REPORT_LIMIT) {
        g_report_total++;
        return 1;
    }
    g_report_total++;
    if (g_report_key_count < REPORT_KEYS)
        g_report_keys[g_report_key_count++] = (ReportKey){kind, h->alloc_site, h->free_site};
    return 0;
}

/* Print one guard report, then abort (mode 1) or return (mode 2). dump_at is
 * the guest address of the 16 bytes to show (0 = none). The caller holds the
 * guard lock. */
static void guard_report(const char *kind, uint32_t user, GuardHeader h, int has_free,
                         uint32_t offset, uint32_t dump_at)
{
    if (g_guard == 2 && guard_report_skip(kind, &h))
        return;
    fprintf(stderr, "guest_heap_guard: %s block 0x%08X size %u alloc site ", kind, user,
            h.size);
    put_site(h.alloc_site, 1);
    fputs(" free site ", stderr);
    put_site(h.free_site, has_free);
    fprintf(stderr, " offset %u\n", offset);
    fputs("guest_heap_guard: bytes", stderr);
    if (dump_at != 0) {
        const uint8_t *b = guest_ptr(dump_at);
        fprintf(stderr, " at 0x%08X:", dump_at);
        for (int i = 0; i < 16; i++)
            fprintf(stderr, " %02X", b[i]);
    } else {
        fputs(" unavailable", stderr);
    }
    char tail[ICALL_TRACE_SIZE * 12];
    recomp_icall_tail(tail, sizeof tail, 8);
    fprintf(stderr, "\nguest_heap_guard: last icalls (newest first): %s\n", tail);
    if (g_guard == 1)
        abort();
}

/* Check the user bytes and tail of a quarantined block; report a change.
 * The caller holds the guard lock. */
static void guard_check_freed(const QuarEntry *e)
{
    const GuardHeader *hp = guest_ptr(e->user - GUARD_HDR);
    GuardHeader h = *hp;
    if (h.magic != GUARD_FREE || h.size != e->size) {
        h.size = e->size;
        guard_report("write after free", e->user, h, 1, 0, e->user - GUARD_HDR);
        return;
    }
    const uint8_t *u = guest_ptr(e->user);
    for (uint32_t i = 0; i < e->size; i++) {
        if (u[i] != GUARD_FREE_BYTE) {
            guard_report("write after free", e->user, h, 1, i, e->user + i);
            return;
        }
    }
    for (uint32_t i = 0; i < GUARD_TAIL; i++) {
        if (u[e->size + i] != GUARD_TAIL_BYTE) {
            guard_report("write after free", e->user, h, 1, e->size + i,
                         e->user + e->size + i);
            return;
        }
    }
}

static void *guard_checker(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(CHECK_PERIOD_S);
        pthread_mutex_lock(&g_guard_lock);
        for (uint32_t k = 0; k < g_quar_count; k++)
            guard_check_freed(&g_quar[(g_quar_head + k) % QUAR_MAX_ENTRIES]);
        pthread_mutex_unlock(&g_guard_lock);
    }
    return NULL;
}

/* Check the oldest quarantined block and release it to dlmalloc. */
static void guard_evict_oldest(void)
{
    QuarEntry e = g_quar[g_quar_head];
    guard_check_freed(&e);
    g_quar_head = (g_quar_head + 1) % QUAR_MAX_ENTRIES;
    g_quar_count--;
    g_quar_bytes -= e.size;
    mspace_free(g_heap, guest_ptr(e.user - GUARD_HDR));
}

static uint32_t guard_malloc(uint32_t n)
{
    uint8_t *base = mspace_malloc(g_heap, (size_t)n + GUARD_HDR + GUARD_TAIL);
    if (base == NULL)
        return 0;
    GuardHeader h = {GUARD_LIVE, n, g_heap_site, 0};
    memcpy(base, &h, sizeof h);
    memset(base + GUARD_HDR + n, (int)GUARD_TAIL_BYTE, GUARD_TAIL);
    return to_guest(base) + GUARD_HDR;
}

/* Check that a is a live guarded block with an intact tail; report otherwise.
 * Returns the header (host pointer), or NULL when the block must not be freed
 * (mode 2 after a double or bad free). An overrun is reported and the block
 * is still returned. The caller holds the lock. */
static GuardHeader *guard_validate(uint32_t a)
{
    GuardHeader none = {0, 0, 0, 0};
    if (a < GUEST_HEAP_BASE + GUARD_HDR || a >= GUEST_HEAP_END || a % 16 != 0) {
        guard_report("bad free", a, none, 0, 0, 0);
        return NULL;
    }
    GuardHeader *h = guest_ptr(a - GUARD_HDR);
    uint32_t around = a >= GUEST_HEAP_BASE + 8 && a + 8 <= GUEST_HEAP_END ? a - 8 : 0;
    if (h->magic == GUARD_FREE) {
        guard_report("double free", a, *h, 1, 0, around);
        return NULL;
    }
    if (h->magic != GUARD_LIVE || h->size > GUEST_HEAP_END - a - GUARD_TAIL) {
        guard_report("bad free", a, *h, 0, 0, around);
        return NULL;
    }
    const uint8_t *tail = (const uint8_t *)guest_ptr(a) + h->size;
    for (uint32_t i = 0; i < GUARD_TAIL; i++) {
        if (tail[i] != GUARD_TAIL_BYTE) {
            GuardHeader r = *h;
            r.free_site = g_heap_site;
            guard_report("overrun", a, r, 1, i, a + h->size + i);
            break;
        }
    }
    return h;
}

static void guard_free(uint32_t a)
{
    pthread_mutex_lock(&g_guard_lock);
    GuardHeader *h = guard_validate(a);
    if (h == NULL) {
        pthread_mutex_unlock(&g_guard_lock);
        return;
    }
    h->magic = GUARD_FREE;
    h->free_site = g_heap_site;
    memset(guest_ptr(a), (int)GUARD_FREE_BYTE, h->size);
    if (g_quar_count == QUAR_MAX_ENTRIES)
        guard_evict_oldest();
    g_quar[(g_quar_head + g_quar_count) % QUAR_MAX_ENTRIES] = (QuarEntry){a, h->size};
    g_quar_count++;
    g_quar_bytes += h->size;
    while (g_quar_bytes > QUAR_MAX_BYTES)
        guard_evict_oldest();
    pthread_mutex_unlock(&g_guard_lock);
}

uint32_t guest_malloc(uint32_t n)
{
    if (guest_heap_init() != 0)
        return 0;
    if (g_guard)
        return guard_malloc(n);
    return to_guest(mspace_malloc(g_heap, n));
}

void guest_free(uint32_t a)
{
    if (a == 0 || guest_heap_init() != 0)
        return;
    if (g_guard) {
        guard_free(a);
        return;
    }
    mspace_free(g_heap, guest_ptr(a));
}

uint32_t guest_calloc(uint32_t n, uint32_t s)
{
    if (guest_heap_init() != 0)
        return 0;
    if (g_guard) {
        uint64_t total = (uint64_t)n * s;
        if (total > UINT32_MAX)
            return 0;
        uint32_t a = guard_malloc((uint32_t)total);
        if (a != 0)
            memset(guest_ptr(a), 0, (size_t)total);
        return a;
    }
    return to_guest(mspace_calloc(g_heap, n, s));
}

uint32_t guest_realloc(uint32_t a, uint32_t n)
{
    if (guest_heap_init() != 0)
        return 0;
    if (g_guard) {
        if (a == 0)
            return guard_malloc(n);
        pthread_mutex_lock(&g_guard_lock);
        const GuardHeader *oh = guard_validate(a);
        uint32_t old = oh != NULL ? oh->size : 0;
        pthread_mutex_unlock(&g_guard_lock);
        if (oh == NULL)
            return 0;
        uint32_t b = guard_malloc(n);
        if (b == 0)
            return 0;
        memcpy(guest_ptr(b), guest_ptr(a), old < n ? old : n);
        guard_free(a);
        return b;
    }
    return to_guest(mspace_realloc(g_heap, a == 0 ? NULL : guest_ptr(a), n));
}

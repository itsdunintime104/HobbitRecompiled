/* pthread and OSAtomic thunks. Design notes are in guest_threads.h. */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "guest_threads.h"
#include "host_funcs.h"
#include "diag.h"

/* No-op defaults: diag.c has the strong definitions, and a test binary that
 * does not link it still builds. */
__attribute__((weak)) void diag_thread_register(void) {}
__attribute__((weak)) void diag_thread_unregister(void) {}

#define ARG(n) MEM32(g_esp + 4u + 4u * (n))
#define RET(v) do { g_eax = (uint32_t)(v); g_esp += 4; } while (0)

#define GT_STACK_END 0xF0000000u /* guest_call.h callback stacks start here */
#define GT_MAX_KEYS 256u
#define GT_DEFAULT_STACKSIZE 0x80000u /* Darwin default for secondary threads */
#define GT_HOST_STACKSIZE (8u << 20)  /* lifted code recurses on the host stack */
#define GT_DESTRUCTOR_ITERATIONS 4

/* ---- Side table: guest address -> host object -------------------------- */

/* Open addressing with linear probing; key 0 marks an empty slot (guest
 * address 0 is never a valid object). Host objects are malloc'd one by one,
 * so their addresses stay stable when the table grows. */
struct gmap {
    uint32_t *keys;
    void **vals;
    uint32_t cap; /* power of two, or 0 before first insert */
    uint32_t len;
};

/* One lock for both tables, the thread table and the key table writes. */
static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gmap g_mutexes;
static struct gmap g_conds;

static uint32_t gmap_slot(const struct gmap *m, uint32_t key)
{
    return (key * 0x9E3779B1u) & (m->cap - 1);
}

static void *gmap_find(const struct gmap *m, uint32_t key)
{
    if (m->cap == 0)
        return NULL;
    for (uint32_t i = gmap_slot(m, key);; i = (i + 1) & (m->cap - 1)) {
        if (m->keys[i] == key)
            return m->vals[i];
        if (m->keys[i] == 0)
            return NULL;
    }
}

static void gmap_insert_raw(struct gmap *m, uint32_t key, void *val)
{
    uint32_t i = gmap_slot(m, key);
    while (m->keys[i] != 0 && m->keys[i] != key)
        i = (i + 1) & (m->cap - 1);
    if (m->keys[i] == 0)
        m->len++;
    m->keys[i] = key;
    m->vals[i] = val;
}

/* Insert or replace. Returns 0, or ENOMEM. */
static int gmap_put(struct gmap *m, uint32_t key, void *val)
{
    if ((m->len + 1) * 2 > m->cap) {
        struct gmap old = *m;
        uint32_t cap = old.cap ? old.cap * 2 : 64;
        uint32_t *keys = calloc(cap, sizeof *keys);
        void **vals = calloc(cap, sizeof *vals);
        if (keys == NULL || vals == NULL) {
            free(keys);
            free(vals);
            return ENOMEM;
        }
        m->keys = keys;
        m->vals = vals;
        m->cap = cap;
        m->len = 0;
        for (uint32_t i = 0; i < old.cap; i++)
            if (old.keys[i] != 0)
                gmap_insert_raw(m, old.keys[i], old.vals[i]);
        free(old.keys);
        free(old.vals);
    }
    gmap_insert_raw(m, key, val);
    return 0;
}

/* Remove with backward-shift deletion, so lookups need no tombstones. */
static void gmap_del(struct gmap *m, uint32_t key)
{
    if (m->cap == 0)
        return;
    uint32_t mask = m->cap - 1;
    uint32_t i = gmap_slot(m, key);
    while (m->keys[i] != key) {
        if (m->keys[i] == 0)
            return;
        i = (i + 1) & mask;
    }
    m->len--;
    for (uint32_t j = (i + 1) & mask; m->keys[j] != 0; j = (j + 1) & mask) {
        uint32_t home = gmap_slot(m, m->keys[j]);
        /* Move j into the hole at i if its home slot is not in (i, j]. */
        if (((j - home) & mask) >= ((j - i) & mask)) {
            m->keys[i] = m->keys[j];
            m->vals[i] = m->vals[j];
            i = j;
        }
    }
    m->keys[i] = 0;
    m->vals[i] = NULL;
}

/* ---- Mutexes ------------------------------------------------------------ */

/* Word 1 of an initialised guest mutex records its type ('TYP' tag in the
 * high 24 bits, type in the low byte), so a rebuilt host entry keeps the type. */
#define GT_MUTEX_TYPE_TAG 0x54595000u

/* The side table holds a pointer to `m`, the first member, so it is also the
 * pointer free() and pthread_mutex_destroy() take. `owner` is the host thread
 * id of the last locker, kept only when RECOMP_LOCKWATCH is set. */
struct host_mutex {
    pthread_mutex_t m;
    _Atomic uint64_t owner;
};

static pthread_mutex_t *host_mutex_new(int type)
{
    struct host_mutex *hm = calloc(1, sizeof *hm);
    pthread_mutexattr_t ha;
    if (hm == NULL)
        return NULL;
    pthread_mutexattr_init(&ha);
    pthread_mutexattr_settype(&ha, type);
    int rc = pthread_mutex_init(&hm->m, &ha);
    pthread_mutexattr_destroy(&ha);
    if (rc != 0) {
        free(hm);
        return NULL;
    }
    return &hm->m;
}

/* RECOMP_LOCKWATCH=N (seconds): 0 when unset. Read once. */
static pthread_once_t g_lockwatch_once = PTHREAD_ONCE_INIT;
static unsigned g_lockwatch_secs;

static void lockwatch_init(void)
{
    const char *v = getenv("RECOMP_LOCKWATCH");
    long n = v != NULL ? strtol(v, NULL, 10) : 0;
    g_lockwatch_secs = n > 0 ? (unsigned)n : 0;
}

static unsigned lockwatch_secs(void)
{
    pthread_once(&g_lockwatch_once, lockwatch_init);
    return g_lockwatch_secs;
}

static uint64_t host_tid(void)
{
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return tid;
}

static void note_owner(pthread_mutex_t *hm)
{
    atomic_store_explicit(&((struct host_mutex *)hm)->owner, host_tid(),
                          memory_order_relaxed);
}

static double mono_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* trylock plus a 1 ms sleep, so a stuck lock can be reported. After `secs`
 * seconds, print once and keep waiting. */
static int lock_watched(uint32_t a, pthread_mutex_t *hm, unsigned secs)
{
    double start = mono_seconds();
    int warned = 0;
    int rc;
    while ((rc = pthread_mutex_trylock(hm)) == EBUSY) {
        if (!warned && mono_seconds() - start >= (double)secs) {
            warned = 1;
            fprintf(stderr, "STUCK-LOCK guest_mutex=0x%08X owner_thread=%llu waiter_thread=%llu\n",
                    a,
                    (unsigned long long)atomic_load_explicit(
                        &((struct host_mutex *)hm)->owner, memory_order_relaxed),
                    (unsigned long long)host_tid());
        }
        usleep(1000);
    }
    if (rc == 0)
        note_owner(hm);
    return rc;
}

/* Host mutex type that the guest word 0 (and word 1 for an initialised
 * mutex) means, or -1 if word 0 is not a mutex signature. */
static int mutex_sig_type(volatile uint32_t *sig)
{
    switch (*sig) {
    case GT_MUTEX_SIG_INIT: return PTHREAD_MUTEX_NORMAL;
    case GT_MUTEX_SIG: {
        uint32_t w1 = sig[1];
        return (w1 & 0xFFFFFF00u) == GT_MUTEX_TYPE_TAG && (w1 & 0xFFu) <= PTHREAD_MUTEX_RECURSIVE
                   ? (int)(w1 & 0xFFu)
                   : PTHREAD_MUTEX_NORMAL;
    }
    case GT_ERRORCHECK_MUTEX_SIG_INIT: return PTHREAD_MUTEX_ERRORCHECK;
    case GT_RECURSIVE_MUTEX_SIG_INIT: return PTHREAD_MUTEX_RECURSIVE;
    default: return -1;
    }
}

/* Find the host mutex for guest address a. A mutex that is not in the table
 * yet is created on first use when word 0 holds a static initializer or the
 * initialised signature (a struct copy); anything else is EINVAL, as on
 * Darwin. */
static pthread_mutex_t *mutex_get(uint32_t a, int *err)
{
    if (a == 0) {
        *err = EINVAL;
        return NULL;
    }
    volatile uint32_t *sig = guest_ptr(a);
    pthread_mutex_lock(&g_table_lock);
    pthread_mutex_t *hm = gmap_find(&g_mutexes, a);
    pthread_mutex_t *old = NULL;
    /* A static initializer next to an entry: the guest reused the memory, so
     * the entry is stale (a live object holds GT_MUTEX_SIG). */
    int reinit = *sig == GT_MUTEX_SIG_INIT || *sig == GT_ERRORCHECK_MUTEX_SIG_INIT ||
                 *sig == GT_RECURSIVE_MUTEX_SIG_INIT;
    if (hm == NULL || reinit) {
        int type = mutex_sig_type(sig);
        if (type < 0) {
            pthread_mutex_unlock(&g_table_lock);
            *err = EINVAL;
            return NULL;
        }
        old = hm;
        hm = host_mutex_new(type);
        if (hm == NULL || gmap_put(&g_mutexes, a, hm) != 0) {
            pthread_mutex_unlock(&g_table_lock);
            if (hm != NULL) {
                pthread_mutex_destroy(hm);
                free(hm);
            }
            *err = ENOMEM;
            return NULL;
        }
        sig[1] = GT_MUTEX_TYPE_TAG | (uint32_t)type;
        *sig = GT_MUTEX_SIG;
    }
    pthread_mutex_unlock(&g_table_lock);
    /* A busy old mutex is leaked: a host thread may still block on it. */
    if (old != NULL && pthread_mutex_destroy(old) == 0)
        free(old);
    return hm;
}

void imp__pthread_mutex_init(void)
{
    uint32_t a = ARG(0), attr = ARG(1);
    int type = PTHREAD_MUTEX_NORMAL;
    if (a == 0) {
        RET(EINVAL);
        return;
    }
    if (attr != 0) {
        volatile uint32_t *ga = guest_ptr(attr);
        if (ga[0] != GT_MUTEXATTR_SIG) {
            RET(EINVAL);
            return;
        }
        type = (int)ga[1];
    }
    pthread_mutex_t *hm = host_mutex_new(type);
    if (hm == NULL) {
        RET(ENOMEM);
        return;
    }
    pthread_mutex_lock(&g_table_lock);
    /* An entry already here means the guest re-initialised the address,
     * usually freed memory reused without pthread_mutex_destroy. */
    pthread_mutex_t *old = gmap_find(&g_mutexes, a);
    int rc = gmap_put(&g_mutexes, a, hm);
    if (rc == 0) {
        volatile uint32_t *gm = guest_ptr(a);
        gm[1] = GT_MUTEX_TYPE_TAG | (uint32_t)type;
        gm[0] = GT_MUTEX_SIG;
    }
    pthread_mutex_unlock(&g_table_lock);
    if (rc != 0) {
        pthread_mutex_destroy(hm);
        free(hm);
    } else if (old != NULL && pthread_mutex_destroy(old) == 0) {
        /* A busy old mutex is leaked: a host thread may still block on it. */
        free(old);
    }
    RET(rc);
}

void imp__pthread_mutex_destroy(void)
{
    uint32_t a = ARG(0);
    int rc = EINVAL;
    if (a != 0) {
        volatile uint32_t *sig = guest_ptr(a);
        pthread_mutex_lock(&g_table_lock);
        pthread_mutex_t *hm = gmap_find(&g_mutexes, a);
        if (hm != NULL) {
            rc = pthread_mutex_destroy(hm); /* EBUSY keeps the entry */
            if (rc == 0) {
                gmap_del(&g_mutexes, a);
                free(hm);
                *sig = 0;
            }
        } else if (*sig == GT_MUTEX_SIG || *sig == GT_MUTEX_SIG_INIT ||
                   *sig == GT_ERRORCHECK_MUTEX_SIG_INIT ||
                   *sig == GT_RECURSIVE_MUTEX_SIG_INIT) {
            *sig = 0; /* never used: nothing on the host side */
            rc = 0;
        }
        pthread_mutex_unlock(&g_table_lock);
    }
    RET(rc);
}

void imp__pthread_mutex_lock(void)
{
    int err = 0;
    pthread_mutex_t *hm = mutex_get(ARG(0), &err);
    unsigned secs = hm ? lockwatch_secs() : 0;
    int rc = err;
    if (hm)
        rc = secs ? lock_watched(ARG(0), hm, secs) : pthread_mutex_lock(hm);
    RET(rc);
}

void imp__pthread_mutex_trylock(void)
{
    int err;
    pthread_mutex_t *hm = mutex_get(ARG(0), &err);
    int rc = hm ? pthread_mutex_trylock(hm) : err;
    if (hm && rc == 0 && lockwatch_secs())
        note_owner(hm);
    RET(rc);
}

void imp__pthread_mutex_unlock(void)
{
    int err;
    pthread_mutex_t *hm = mutex_get(ARG(0), &err);
    RET(hm ? pthread_mutex_unlock(hm) : err);
}

/* ---- Condition variables ------------------------------------------------ */

static pthread_cond_t *host_cond_new(void)
{
    pthread_cond_t *hc = malloc(sizeof *hc);
    if (hc != NULL && pthread_cond_init(hc, NULL) != 0) {
        free(hc);
        return NULL;
    }
    return hc;
}

static pthread_cond_t *cond_get(uint32_t a, int *err)
{
    if (a == 0) {
        *err = EINVAL;
        return NULL;
    }
    volatile uint32_t *sig = guest_ptr(a);
    pthread_mutex_lock(&g_table_lock);
    pthread_cond_t *hc = gmap_find(&g_conds, a);
    pthread_cond_t *old = NULL;
    /* A static initializer next to an entry: the guest reused the memory, so
     * the entry is stale (a live object holds GT_COND_SIG). */
    if (hc == NULL || *sig == GT_COND_SIG_INIT) {
        if (*sig != GT_COND_SIG_INIT && *sig != GT_COND_SIG) {
            pthread_mutex_unlock(&g_table_lock);
            *err = EINVAL;
            return NULL;
        }
        old = hc;
        hc = host_cond_new();
        if (hc == NULL || gmap_put(&g_conds, a, hc) != 0) {
            pthread_mutex_unlock(&g_table_lock);
            if (hc != NULL) {
                pthread_cond_destroy(hc);
                free(hc);
            }
            *err = ENOMEM;
            return NULL;
        }
        *sig = GT_COND_SIG;
    }
    pthread_mutex_unlock(&g_table_lock);
    if (old != NULL && pthread_cond_destroy(old) == 0)
        free(old);
    return hc;
}

/* The condattr argument is ignored: no pthread_condattr_* is imported. */
void imp__pthread_cond_init_UNIX2003(void)
{
    uint32_t a = ARG(0);
    if (a == 0) {
        RET(EINVAL);
        return;
    }
    pthread_cond_t *hc = host_cond_new();
    if (hc == NULL) {
        RET(ENOMEM);
        return;
    }
    pthread_mutex_lock(&g_table_lock);
    pthread_cond_t *old = gmap_find(&g_conds, a);
    int rc = gmap_put(&g_conds, a, hc);
    if (rc == 0)
        *(volatile uint32_t *)guest_ptr(a) = GT_COND_SIG;
    pthread_mutex_unlock(&g_table_lock);
    if (rc != 0) {
        pthread_cond_destroy(hc);
        free(hc);
    } else if (old != NULL && pthread_cond_destroy(old) == 0) {
        free(old);
    }
    RET(rc);
}

void imp__pthread_cond_destroy(void)
{
    uint32_t a = ARG(0);
    int rc = EINVAL;
    if (a != 0) {
        volatile uint32_t *sig = guest_ptr(a);
        pthread_mutex_lock(&g_table_lock);
        pthread_cond_t *hc = gmap_find(&g_conds, a);
        if (hc != NULL) {
            rc = pthread_cond_destroy(hc);
            if (rc == 0) {
                gmap_del(&g_conds, a);
                free(hc);
                *sig = 0;
            }
        } else if (*sig == GT_COND_SIG || *sig == GT_COND_SIG_INIT) {
            *sig = 0;
            rc = 0;
        }
        pthread_mutex_unlock(&g_table_lock);
    }
    RET(rc);
}

void imp__pthread_cond_signal(void)
{
    int err;
    pthread_cond_t *hc = cond_get(ARG(0), &err);
    RET(hc ? pthread_cond_signal(hc) : err);
}

void imp__pthread_cond_broadcast(void)
{
    int err;
    pthread_cond_t *hc = cond_get(ARG(0), &err);
    RET(hc ? pthread_cond_broadcast(hc) : err);
}

void imp__pthread_cond_wait_UNIX2003(void)
{
    int err;
    pthread_cond_t *hc = cond_get(ARG(0), &err);
    pthread_mutex_t *hm = hc ? mutex_get(ARG(1), &err) : NULL;
    int rc = hm ? pthread_cond_wait(hc, hm) : err;
    if (hm && lockwatch_secs())
        note_owner(hm); /* the wait re-took the mutex */
    RET(rc);
}

/* ---- Attribute objects (state inline in guest memory) ------------------- */

void imp__pthread_mutexattr_init(void)
{
    uint32_t a = ARG(0);
    if (a == 0) {
        RET(EINVAL);
        return;
    }
    volatile uint32_t *ga = guest_ptr(a);
    ga[0] = GT_MUTEXATTR_SIG;
    ga[1] = PTHREAD_MUTEX_DEFAULT;
    RET(0);
}

void imp__pthread_mutexattr_settype(void)
{
    uint32_t a = ARG(0), type = ARG(1);
    volatile uint32_t *ga = guest_ptr(a);
    if (a == 0 || ga[0] != GT_MUTEXATTR_SIG || type > PTHREAD_MUTEX_RECURSIVE) {
        RET(EINVAL);
        return;
    }
    ga[1] = type;
    RET(0);
}

void imp__pthread_mutexattr_destroy_UNIX2003(void)
{
    uint32_t a = ARG(0);
    volatile uint32_t *ga = guest_ptr(a);
    if (a == 0 || ga[0] != GT_MUTEXATTR_SIG) {
        RET(EINVAL);
        return;
    }
    ga[0] = 0;
    RET(0);
}

void imp__pthread_attr_init(void)
{
    uint32_t a = ARG(0);
    if (a == 0) {
        RET(EINVAL);
        return;
    }
    volatile uint32_t *ga = guest_ptr(a);
    ga[0] = GT_ATTR_SIG;
    ga[1] = GT_DEFAULT_STACKSIZE;
    RET(0);
}

void imp__pthread_attr_setstacksize(void)
{
    uint32_t a = ARG(0), size = ARG(1);
    volatile uint32_t *ga = guest_ptr(a);
    /* Darwin i386: page multiple (4 KB), at least PTHREAD_STACK_MIN (8 KB). */
    if (a == 0 || ga[0] != GT_ATTR_SIG || size < 0x2000u || (size & 0xFFFu) != 0) {
        RET(EINVAL);
        return;
    }
    ga[1] = size;
    RET(0);
}

void imp__pthread_attr_destroy(void)
{
    uint32_t a = ARG(0);
    volatile uint32_t *ga = guest_ptr(a);
    if (a == 0 || ga[0] != GT_ATTR_SIG) {
        RET(EINVAL);
        return;
    }
    ga[0] = 0;
    RET(0);
}

/* ---- Thread-specific keys ----------------------------------------------- */

/* Guest key k (1 <= k < g_key_count) is host key g_keys[k]. Slots are filled
 * under g_table_lock and published by the release store of g_key_count, so
 * get/setspecific read them without the lock. */
static pthread_key_t g_keys[GT_MAX_KEYS];
static uint32_t g_key_dtor[GT_MAX_KEYS];
static _Atomic uint32_t g_key_count = 1;

static int key_valid(uint32_t k)
{
    return k != 0 && k < atomic_load_explicit(&g_key_count, memory_order_acquire);
}

/* The guest destructor is recorded and run by guest_thread_main when a guest
 * thread's start routine returns; the host key has no destructor. */
void imp__pthread_key_create(void)
{
    uint32_t out = ARG(0), dtor = ARG(1);
    int rc = 0;
    if (out == 0) {
        RET(EINVAL);
        return;
    }
    pthread_mutex_lock(&g_table_lock);
    uint32_t k = atomic_load_explicit(&g_key_count, memory_order_relaxed);
    if (k >= GT_MAX_KEYS) {
        rc = EAGAIN;
    } else {
        rc = pthread_key_create(&g_keys[k], NULL);
        if (rc == 0) {
            g_key_dtor[k] = dtor;
            atomic_store_explicit(&g_key_count, k + 1, memory_order_release);
        }
    }
    pthread_mutex_unlock(&g_table_lock);
    if (k >= GT_MAX_KEYS)
        THUNK_UNSUPPORTED("key table full");
    if (rc == 0)
        MEM32(out) = k;
    RET(rc);
}

void imp__pthread_getspecific(void)
{
    uint32_t k = ARG(0);
    RET(key_valid(k) ? (uint32_t)(uintptr_t)pthread_getspecific(g_keys[k]) : 0);
}

void imp__pthread_setspecific(void)
{
    uint32_t k = ARG(0), v = ARG(1);
    RET(key_valid(k) ? pthread_setspecific(g_keys[k], (void *)(uintptr_t)v) : EINVAL);
}

/* ---- Threads ------------------------------------------------------------ */

struct gthread {
    uint32_t start, arg, stack_top, ret;
    int done;
};

static struct gthread g_threads[GT_MAX_THREADS];
static uint32_t g_thread_count; /* handle h is g_threads[h - 1] */
static uint32_t g_next_stack = GT_STACK_BASE;
static pthread_cond_t g_thread_done = PTHREAD_COND_INITIALIZER;

static void guest_call(uint32_t va)
{
    recomp_func_t fn = guest_resolve(va);
    if (fn == NULL)
        recomp_icall_fail_log(va);
    fn();
}

/* Call a one-argument cdecl guest function on this thread's guest stack.
 * Entry esp is 12 mod 16, as the i386 Darwin ABI requires. */
static void guest_call1(uint32_t top, uint32_t va, uint32_t arg)
{
    g_esp = top - 12;
    PUSH32(g_esp, arg);
    PUSH32(g_esp, GT_RET_SENTINEL);
    guest_call(va);
}

static void *guest_thread_main(void *p)
{
    struct gthread *t = p;
    /* Register globals are thread-local and start at zero; the FPU and SSE
     * control words start at their reset values, as on a new x86 thread. */
    g_fp_control_word = 0x037F;
    g_mxcsr = 0x1F80;
    diag_thread_register();
    guest_call1(t->stack_top, t->start, t->arg);
    uint32_t ret = g_eax;

    /* POSIX key destructors, run as guest code on this thread. */
    for (int iter = 0; iter < GT_DESTRUCTOR_ITERATIONS; iter++) {
        int ran = 0;
        uint32_t n = atomic_load_explicit(&g_key_count, memory_order_acquire);
        for (uint32_t k = 1; k < n; k++) {
            uint32_t v = (uint32_t)(uintptr_t)pthread_getspecific(g_keys[k]);
            if (g_key_dtor[k] == 0 || v == 0)
                continue;
            pthread_setspecific(g_keys[k], NULL);
            guest_call1(t->stack_top, g_key_dtor[k], v);
            ran = 1;
        }
        if (!ran)
            break;
    }

    diag_thread_unregister();
    pthread_mutex_lock(&g_table_lock);
    t->ret = ret;
    t->done = 1;
    pthread_cond_broadcast(&g_thread_done);
    pthread_mutex_unlock(&g_table_lock);
    return NULL;
}

/* The guest stack is max(1 MB, attr stack size) rounded up to 1 MB, taken
 * from a bump pointer; slots are not reused (the game creates few threads
 * and never joins them). The host thread is detached. */
void imp__pthread_create(void)
{
    uint32_t out = ARG(0), attr = ARG(1), start = ARG(2), arg = ARG(3);
    uint32_t size = GT_STACK_STEP;
    if (attr != 0) {
        volatile uint32_t *ga = guest_ptr(attr);
        if (ga[0] != GT_ATTR_SIG) {
            RET(EINVAL);
            return;
        }
        uint32_t want = (ga[1] + GT_STACK_STEP - 1) & ~(GT_STACK_STEP - 1);
        if (want > size)
            size = want;
    }

    pthread_mutex_lock(&g_table_lock);
    if (g_thread_count >= GT_MAX_THREADS || GT_STACK_END - g_next_stack < size) {
        pthread_mutex_unlock(&g_table_lock);
        THUNK_UNSUPPORTED("thread slots exhausted");
        RET(EAGAIN);
        return;
    }
    uint32_t base = g_next_stack;
    g_next_stack += size;
    uint32_t handle = ++g_thread_count;
    struct gthread *t = &g_threads[handle - 1];
    pthread_mutex_unlock(&g_table_lock);

    if (mprotect(guest_ptr(base + GT_STACK_GUARD), size - GT_STACK_GUARD,
                 PROT_READ | PROT_WRITE) != 0) {
        RET(EAGAIN);
        return;
    }
    t->start = start;
    t->arg = arg;
    t->stack_top = base + size;
    if (out != 0)
        MEM32(out) = handle;

    pthread_attr_t ha;
    pthread_t host;
    pthread_attr_init(&ha);
    pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&ha, GT_HOST_STACKSIZE);
    int rc = pthread_create(&host, &ha, guest_thread_main, t);
    pthread_attr_destroy(&ha);
    if (rc != 0) {
        pthread_mutex_lock(&g_table_lock);
        t->done = 1; /* guest_thread_wait must not block on it */
        pthread_mutex_unlock(&g_table_lock);
    }
    RET(rc);
}

int guest_thread_wait(uint32_t handle, uint32_t *ret)
{
    pthread_mutex_lock(&g_table_lock);
    if (handle == 0 || handle > g_thread_count) {
        pthread_mutex_unlock(&g_table_lock);
        return EINVAL;
    }
    struct gthread *t = &g_threads[handle - 1];
    while (!t->done)
        pthread_cond_wait(&g_thread_done, &g_table_lock);
    *ret = t->ret;
    pthread_mutex_unlock(&g_table_lock);
    return 0;
}

/* Priorities are not modelled. */
void imp__pthread_setschedparam(void)
{
    RET(0);
}

/* ---- OSAtomic (guest addresses, 32-bit values, seq_cst) ----------------- */

static _Atomic uint32_t *guest_atomic(uint32_t a)
{
    return (_Atomic uint32_t *)guest_ptr(a);
}

/* int32_t OSAtomicAdd32Barrier(int32_t amount, volatile int32_t *p): new value */
void imp__OSAtomicAdd32Barrier(void)
{
    uint32_t amount = ARG(0);
    uint32_t old = atomic_fetch_add_explicit(guest_atomic(ARG(1)), amount,
                                             memory_order_seq_cst);
    RET(old + amount);
}

static uint32_t cas32(uint32_t oldv, uint32_t newv, uint32_t a)
{
    return atomic_compare_exchange_strong_explicit(guest_atomic(a), &oldv, newv,
                                                   memory_order_seq_cst,
                                                   memory_order_seq_cst) ? 1u : 0u;
}

/* bool OSAtomicCompareAndSwap32Barrier(int32_t old, int32_t new, int32_t *p) */
void imp__OSAtomicCompareAndSwap32Barrier(void)
{
    RET(cas32(ARG(0), ARG(1), ARG(2)));
}

/* bool OSAtomicCompareAndSwapPtrBarrier(void *old, void *new, void **p):
 * guest pointers are 32-bit. */
void imp__OSAtomicCompareAndSwapPtrBarrier(void)
{
    RET(cas32(ARG(0), ARG(1), ARG(2)));
}

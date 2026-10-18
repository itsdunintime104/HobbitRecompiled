/* Run-time diagnostics: hangwatch thread and thread registry. See diag.h. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "recomp_types.h"
#include "diag.h"

#define DIAG_MAX_THREADS 64
#define DIAG_HANG_TICKS 3
#define DIAG_TAIL 8

struct diag_thread {
    int used;
    uint64_t tid;
    volatile uint32_t *esp; /* address of the thread's own g_esp */
    uint32_t last_esp;
    unsigned same_ticks;
};

static pthread_mutex_t g_diag_lock = PTHREAD_MUTEX_INITIALIZER;
static struct diag_thread g_diag_threads[DIAG_MAX_THREADS];

uint64_t diag_host_tid(void)
{
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return tid;
}

void diag_thread_register(void)
{
    uint64_t tid = diag_host_tid();
    pthread_mutex_lock(&g_diag_lock);
    for (int i = 0; i < DIAG_MAX_THREADS; i++) {
        if (!g_diag_threads[i].used) {
            g_diag_threads[i] = (struct diag_thread){
                .used = 1, .tid = tid, .esp = &g_esp, .last_esp = g_esp,
            };
            break;
        }
    }
    pthread_mutex_unlock(&g_diag_lock);
}

void diag_thread_unregister(void)
{
    uint64_t tid = diag_host_tid();
    pthread_mutex_lock(&g_diag_lock);
    for (int i = 0; i < DIAG_MAX_THREADS; i++)
        if (g_diag_threads[i].used && g_diag_threads[i].tid == tid)
            g_diag_threads[i].used = 0;
    pthread_mutex_unlock(&g_diag_lock);
}

static void hangwatch_tick(void)
{
    char tail[ICALL_TRACE_SIZE * 12];
    recomp_icall_tail(tail, sizeof tail, DIAG_TAIL);
    pthread_mutex_lock(&g_diag_lock);
    for (int i = 0; i < DIAG_MAX_THREADS; i++) {
        struct diag_thread *t = &g_diag_threads[i];
        if (!t->used)
            continue;
        uint32_t esp = *t->esp;
        t->same_ticks = esp == t->last_esp ? t->same_ticks + 1 : 0;
        t->last_esp = esp;
        fprintf(stderr, "HANGWATCH thread=%llu esp=0x%08X%s\n",
                (unsigned long long)t->tid, esp,
                t->same_ticks >= DIAG_HANG_TICKS ? " HANG?" : "");
    }
    pthread_mutex_unlock(&g_diag_lock);
    fprintf(stderr, "HANGWATCH last icalls (newest first): %s\n", tail);
}

static void *hangwatch_main(void *p)
{
    unsigned secs = (unsigned)(uintptr_t)p;
    for (;;) {
        sleep(secs);
        hangwatch_tick();
    }
    return NULL;
}

void diag_init(void)
{
    diag_thread_register();
    const char *v = getenv("RECOMP_HANGWATCH");
    long secs = v != NULL ? strtol(v, NULL, 10) : 0;
    if (secs <= 0)
        return;
    pthread_attr_t attr;
    pthread_t th;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, hangwatch_main, (void *)(uintptr_t)secs) != 0)
        fprintf(stderr, "diag: cannot start hangwatch thread\n");
    pthread_attr_destroy(&attr);
}

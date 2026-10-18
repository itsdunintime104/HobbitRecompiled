/* RECOMP_SLOWFRAME_MS=N: a watchdog thread samples the main thread's stack
 * every 2 ms once a frame has run N ms, and prints the stacks when the frame
 * ends. For rare frame drops that a profiler run does not catch. */
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "slowframe.h"

#define SF_MAX_SAMPLES 128
#define SF_MAX_DEPTH 24
#define SF_SHOW_STACKS 6
#define SF_SHOW_FRAMES 10
#define SF_ADDR_MASK 0x0000007FFFFFFFFFull

typedef struct {
    int depth;
    uint64_t pcs[SF_MAX_DEPTH];
    int running;
} Sample;

static int s_state = -1; /* -1 not read yet, 0 off, 1 on */
static int s_limit_ms;
static uint64_t s_limit_ns;
static mach_port_t s_target;
static uint64_t s_stack_lo, s_stack_hi;
static _Atomic uint64_t g_frame_start_ns;
static _Atomic uint64_t g_frame_seq;
static Sample s_samples[SF_MAX_SAMPLES]; /* watchdog thread only */
static int s_nsamples;

static uint64_t now_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void take_sample(void)
{
    arm_thread_state64_t st;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    thread_basic_info_data_t info;
    mach_msg_type_number_t icnt = THREAD_BASIC_INFO_COUNT;

    /* Read the run state before suspending: a suspended thread never reports running. */
    if (thread_info(s_target, THREAD_BASIC_INFO, (thread_info_t)&info, &icnt) != KERN_SUCCESS)
        return;
    if (thread_suspend(s_target) != KERN_SUCCESS)
        return;
    if (thread_get_state(s_target, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
        Sample *s = &s_samples[s_nsamples];
        s->depth = 0;
        s->pcs[s->depth++] = (uint64_t)arm_thread_state64_get_pc(st) & SF_ADDR_MASK;
        s->pcs[s->depth++] = (uint64_t)arm_thread_state64_get_lr(st) & SF_ADDR_MASK;
        uint64_t fp = (uint64_t)arm_thread_state64_get_fp(st);
        while (s->depth < SF_MAX_DEPTH && (fp & 15) == 0 && fp >= s_stack_lo && fp <= s_stack_hi - 16) {
            uint64_t next = ((uint64_t *)fp)[0];
            s->pcs[s->depth++] = ((uint64_t *)fp)[1] & SF_ADDR_MASK;
            if (next <= fp)
                break;
            fp = next;
        }
        s->running = info.run_state == TH_STATE_RUNNING;
        s_nsamples++;
    }
    thread_resume(s_target);
}

static void print_name(uint64_t pc, int first)
{
    Dl_info di;
    const char *sep = first ? "" : " <- ";
    if (dladdr((void *)pc, &di) && di.dli_sname)
        fprintf(stderr, "%s%s", sep, di.dli_sname);
    else
        fprintf(stderr, "%s0x%llx", sep, (unsigned long long)pc);
}

static int same_stack(const Sample *a, const Sample *b)
{
    return a->depth == b->depth && memcmp(a->pcs, b->pcs, sizeof(uint64_t) * (size_t)a->depth) == 0;
}

static void report(uint64_t start_ns, uint64_t next_start_ns)
{
    int running = 0;
    for (int i = 0; i < s_nsamples; i++)
        running += s_samples[i].running;
    fprintf(stderr, "SLOWFRAME: frame from %.3f s uptime lasted %.1f ms; %d samples, %d running\n",
            (double)start_ns / 1e9, (double)(next_start_ns - start_ns) / 1e6, s_nsamples, running);

    int done[SF_MAX_SAMPLES] = {0};
    for (int shown = 0; shown < SF_SHOW_STACKS; shown++) {
        int best = -1, best_n = 0;
        for (int i = 0; i < s_nsamples; i++) {
            if (done[i])
                continue;
            int n = 0;
            for (int j = i; j < s_nsamples; j++)
                if (!done[j] && same_stack(&s_samples[i], &s_samples[j]))
                    n++;
            if (n > best_n) {
                best_n = n;
                best = i;
            }
        }
        if (best < 0)
            break;
        const Sample *b = &s_samples[best];
        for (int j = best; j < s_nsamples; j++)
            if (!done[j] && same_stack(b, &s_samples[j]))
                done[j] = 1;
        fprintf(stderr, "  %3d x  ", best_n);
        for (int k = 0; k < b->depth && k < SF_SHOW_FRAMES; k++)
            print_name(b->pcs[k], k == 0);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

static void *watchdog(void *arg)
{
    (void)arg;
    uint64_t last_seq = atomic_load(&g_frame_seq);
    uint64_t last_start = atomic_load(&g_frame_start_ns);
    for (;;) {
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
        uint64_t seq = atomic_load(&g_frame_seq);
        uint64_t start = atomic_load(&g_frame_start_ns);
        if (atomic_load(&g_frame_seq) != seq)
            continue; /* a frame boundary moved while reading; read again */
        if (seq != last_seq) {
            if (s_nsamples > 0)
                report(last_start, start);
            s_nsamples = 0;
            last_seq = seq;
            last_start = start;
        } else if (now_ns() - start > s_limit_ns && s_nsamples < SF_MAX_SAMPLES) {
            take_sample();
        }
    }
    return NULL;
}

void slowframe_mark(void)
{
    if (s_state == 0)
        return;
    if (s_state < 0) {
        const char *e = getenv("RECOMP_SLOWFRAME_MS");
        s_limit_ms = e ? atoi(e) : 0;
        if (s_limit_ms <= 0) {
            s_state = 0;
            return;
        }
        s_limit_ns = (uint64_t)s_limit_ms * 1000000ull;
        s_target = mach_thread_self();
        pthread_t self = pthread_self();
        s_stack_hi = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(self);
        s_stack_lo = s_stack_hi - (uint64_t)pthread_get_stacksize_np(self);
        atomic_store(&g_frame_start_ns, now_ns());
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&th, &attr, watchdog, NULL);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            s_state = 0;
            return;
        }
        s_state = 1;
        fprintf(stderr, "slowframe: watching frames over %d ms\n", s_limit_ms);
        return;
    }
    atomic_store(&g_frame_start_ns, now_ns());
    atomic_fetch_add(&g_frame_seq, 1);
}

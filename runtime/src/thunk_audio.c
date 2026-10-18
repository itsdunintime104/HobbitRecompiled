/* Audio thunks over the host AudioToolbox, AudioUnit and CoreAudio.
 *
 * AudioUnit objects are host pointers held as HANDLE_AUDIO handles. A guest
 * graph handle points to a struct vgraph that wraps the host AUGraph. The game
 * makes one graph per sound and opens and closes each one, which is slow, so a
 * disposed graph is kept open in a pool and handed back by the next NewAUGraph.
 * AUNode, AudioDeviceID and property data are 32-bit on both sides.
 * The game's render callbacks are guest code: the host audio thread enters a
 * trampoline that converts the buffers to the i386 layout and calls the guest
 * function through guest_call.h.
 *
 * Property data the game uses and how each crosses the boundary:
 *   AudioUnit properties (ID, scope, element) the game passes:
 *     kAudioUnitProperty_ElementCount (11) scopes 1 and 2, UInt32: same layout.
 *     kAudioUnitProperty_StreamFormat (8) scopes 1 and 2: AudioStreamBasicDescription,
 *       40 bytes, same layout on i386 and arm64.
 *     kAudioOutputUnitProperty_CurrentDevice (2000) get: UInt32: same layout.
 *   Also converted: kAudioUnitProperty_SetRenderCallback (23): AURenderCallbackStruct
 *     {guest proc, guest refcon} becomes a host struct with the trampoline.
 *   Other scalar properties with the same layout: SampleRate (2), CPULoad (6),
 *     Latency (12), MaximumFramesPerSlice (14).
 *   Any other property returns kAudioUnitErr_InvalidProperty and logs.
 *   AudioDevice properties the game asks for: 'srnd' (AudioChannelLayout, same
 *     layout, 4-byte fields and floats). Also forwarded: 'nsrt' and 'bsiz'.
 *     Anything else returns 'who?'.
 * AudioTimeStamp is 64 bytes with identical offsets on both sides, so it is
 * copied as is. AudioBufferList differs (12-byte buffers in the guest, 16 on
 * the host) and is rebuilt on every call. */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "diag.h"
#include "guest_call.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "handles.h"
#include "recomp_types.h"

#define ARG(n) MEM32(g_esp + 4u + 4u * (n))
/* RECOMP_TRACE_AUDIO=1: log graph lifetimes, callbacks, failed calls and the
 * first render of each callback. Read from the environment once. */
static pthread_once_t g_trace_audio_once = PTHREAD_ONCE_INIT;
static int g_trace_audio;

static void trace_audio_init(void)
{
    const char *v = getenv("RECOMP_TRACE_AUDIO");
    g_trace_audio = v != NULL && v[0] == '1';
}

static int trace_audio_on(void)
{
    pthread_once(&g_trace_audio_once, trace_audio_init);
    return g_trace_audio;
}

static void trace_audio(const char *fmt, ...)
{
    if (!trace_audio_on())
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "audio trace %.3f: ", (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Every thunk here returns an OSStatus; trace each failure with its first argument. */
#define RET(v) do { \
        uint32_t ret_ = (uint32_t)(v); \
        if (ret_ != 0) \
            trace_audio("%s(%#x) -> %d", __func__, ARG(0), (int)ret_); \
        g_eax = ret_; \
        g_esp += 4; \
    } while (0)

/* Guest AudioBufferList: u32 count, then { u32 channels, u32 byteSize, u32 data }. */
#define GUEST_BUF_STRIDE 12u
#define MAX_BUFFERS 16u

/* Guest scratch block per audio thread: flags, timestamp, buffer list. */
#define BLK_FLAGS 0u
#define BLK_TS 16u
#define BLK_ABL 96u
#define BLK_SIZE (BLK_ABL + 4u + MAX_BUFFERS * GUEST_BUF_STRIDE)

static void *host_obj(uint32_t h)
{
    return handle_get(h, HANDLE_AUDIO);
}

static void *host_ptr(uint32_t guest_addr)
{
    return guest_addr != 0 ? guest_ptr(guest_addr) : NULL;
}

/* ---- Render trampoline ---------------------------------------------------- */

struct render_ctx {
    uint32_t proc;
    uint32_t refcon;
    atomic_uint calls;
};

/* Render failures traced so far; only the first ones are logged. */
static atomic_uint g_render_fail_logged;

static OSStatus render_fail(const struct render_ctx *ctx, const char *why, OSStatus st)
{
    if (atomic_fetch_add(&g_render_fail_logged, 1) < 50)
        trace_audio("render ctx %p proc %#x refcon %#x: %s -> %d", (const void *)ctx, ctx->proc,
                    ctx->refcon, why, (int)st);
    return st;
}

/* Memory owned by one host audio thread. A copy lives on the heap and in a
 * pthread key, because __thread storage can be gone when the key destructor
 * runs at thread exit. */
struct audio_owned {
    uint32_t block;
    uint32_t data[MAX_BUFFERS];
    void *scratch[MAX_BUFFERS];
};

/* State of one host audio thread. */
static __thread struct {
    uint32_t block;
    uint32_t data[MAX_BUFFERS];
    uint32_t data_cap[MAX_BUFFERS];
    void *scratch[MAX_BUFFERS];
    uint32_t scratch_cap[MAX_BUFFERS];
    struct audio_owned *owned;
} t_audio;

static pthread_key_t g_audio_key;
static pthread_once_t g_audio_key_once = PTHREAD_ONCE_INIT;
static int g_audio_key_ok;

/* Thread exit: free memory only, never call guest code. */
static void audio_thread_exit(void *value)
{
    struct audio_owned *o = value;
    if (o->block != 0)
        guest_free(o->block);
    for (unsigned i = 0; i < MAX_BUFFERS; i++) {
        if (o->data[i] != 0)
            guest_free(o->data[i]);
        free(o->scratch[i]);
    }
    free(o);
}

static void make_audio_key(void)
{
    g_audio_key_ok = pthread_key_create(&g_audio_key, audio_thread_exit) == 0;
}

/* Copy the thread's pointers into the heap copy the key destructor frees. */
static void audio_owned_sync(void)
{
    struct audio_owned *o = t_audio.owned;
    if (o == NULL)
        return;
    o->block = t_audio.block;
    memcpy(o->data, t_audio.data, sizeof o->data);
    memcpy(o->scratch, t_audio.scratch, sizeof o->scratch);
}

static int thread_state_ready(void)
{
    if (t_audio.owned == NULL) {
        pthread_once(&g_audio_key_once, make_audio_key);
        if (!g_audio_key_ok)
            return 0;
        struct audio_owned *o = calloc(1, sizeof *o);
        if (o == NULL)
            return 0;
        if (pthread_setspecific(g_audio_key, o) != 0) {
            free(o);
            return 0;
        }
        t_audio.owned = o;
    }
    if (t_audio.block == 0) {
        t_audio.block = guest_malloc(BLK_SIZE);
        if (t_audio.block == 0)
            return 0;
        audio_owned_sync();
    }
    return 1;
}

/* Make guest sample buffer i hold at least n bytes. */
static int guest_buffer_reserve(unsigned i, uint32_t n)
{
    if (n <= t_audio.data_cap[i])
        return 1;
    uint32_t cap = n > 4096u ? n : 4096u;
    uint32_t a = guest_malloc(cap);
    if (a == 0)
        return 0;
    if (t_audio.data[i] != 0)
        guest_free(t_audio.data[i]);
    t_audio.data[i] = a;
    t_audio.data_cap[i] = cap;
    audio_owned_sync();
    return 1;
}

static void *host_scratch_reserve(unsigned i, uint32_t n)
{
    if (n > t_audio.scratch_cap[i]) {
        void *p = realloc(t_audio.scratch[i], n);
        if (p == NULL)
            return NULL;
        t_audio.scratch[i] = p;
        t_audio.scratch_cap[i] = n;
        audio_owned_sync();
    }
    return t_audio.scratch[i];
}

static OSStatus render_trampoline(void *refcon, AudioUnitRenderActionFlags *flags,
                                  const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                                  AudioBufferList *io)
{
    struct render_ctx *ctx = refcon;
    if (atomic_fetch_add(&ctx->calls, 1) == 0)
        trace_audio("first render ctx %p proc %#x refcon %#x bus %u frames %u", (void *)ctx,
                    ctx->proc, ctx->refcon, (unsigned)bus, (unsigned)frames);
    guest_thread_attach();
    if (!thread_state_ready())
        return render_fail(ctx, "thread state", kAudio_MemFullError);

    uint32_t blk = t_audio.block;
    uint32_t abl = blk + BLK_ABL;
    MEM32(blk + BLK_FLAGS) = flags != NULL ? *flags : 0;
    if (ts != NULL)
        memcpy(guest_ptr(blk + BLK_TS), ts, sizeof(AudioTimeStamp));
    else
        memset(guest_ptr(blk + BLK_TS), 0, sizeof(AudioTimeStamp));

    uint32_t nb = io != NULL ? io->mNumberBuffers : 0;
    if (nb > MAX_BUFFERS)
        nb = MAX_BUFFERS;
    MEM32(abl) = nb;
    for (uint32_t i = 0; i < nb; i++) {
        const AudioBuffer *b = &io->mBuffers[i];
        if (!guest_buffer_reserve(i, b->mDataByteSize))
            return render_fail(ctx, "guest buffer", kAudio_MemFullError);
        if (b->mData != NULL)
            memcpy(guest_ptr(t_audio.data[i]), b->mData, b->mDataByteSize);
        else
            memset(guest_ptr(t_audio.data[i]), 0, b->mDataByteSize);
        uint32_t e = abl + 4u + i * GUEST_BUF_STRIDE;
        MEM32(e) = b->mNumberChannels;
        MEM32(e + 4) = b->mDataByteSize;
        MEM32(e + 8) = t_audio.data[i];
    }

    uint32_t args[6] = {ctx->refcon, blk + BLK_FLAGS, blk + BLK_TS, bus, frames, abl};
    uint32_t result = guest_call_cdecl(ctx->proc, 6, args);

    if (flags != NULL)
        *flags = MEM32(blk + BLK_FLAGS);
    for (uint32_t i = 0; i < nb; i++) {
        AudioBuffer *b = &io->mBuffers[i];
        uint32_t e = abl + 4u + i * GUEST_BUF_STRIDE;
        uint32_t gsize = MEM32(e + 4);
        uint32_t gdata = MEM32(e + 8);
        if (gdata == 0)
            continue;
        if (b->mData == NULL) {
            b->mData = host_scratch_reserve(i, gsize);
            if (b->mData == NULL)
                return render_fail(ctx, "host scratch", kAudio_MemFullError);
        } else if (gsize > b->mDataByteSize) {
            gsize = b->mDataByteSize;
        }
        memcpy(b->mData, guest_ptr(gdata), gsize);
        b->mDataByteSize = gsize;
    }
    if (result != 0)
        render_fail(ctx, "guest callback", (OSStatus)result);
    return (OSStatus)result;
}

/* The host keeps the pointer until the graph or unit goes. Contexts of render
 * notifies are freed when the graph is retired or disposed; the others stay. */
static struct render_ctx *render_ctx_new(uint32_t proc, uint32_t refcon)
{
    struct render_ctx *c = malloc(sizeof *c);
    if (c != NULL) {
        c->proc = proc;
        c->refcon = refcon;
        atomic_init(&c->calls, 0);
        trace_audio("callback ctx %p proc %#x refcon %#x", (void *)c, proc, refcon);
    }
    return c;
}

/* Build the host AURenderCallbackStruct for a guest {proc, refcon} at addr. */
static OSStatus render_callback_from_guest(uint32_t addr, AURenderCallbackStruct *out)
{
    if (addr == 0)
        return kAudio_ParamError;
    uint32_t proc = MEM32(addr);
    uint32_t refcon = MEM32(addr + 4);
    if (proc == 0) {
        out->inputProc = NULL;
        out->inputProcRefCon = NULL;
        return noErr;
    }
    struct render_ctx *c = render_ctx_new(proc, refcon);
    if (c == NULL)
        return kAudio_MemFullError;
    out->inputProc = render_trampoline;
    out->inputProcRefCon = c;
    return noErr;
}

/* ---- AUGraph -------------------------------------------------------------- */

/* AUGraphNodeInfo returns the same host AudioUnit on every call, so keep one
 * handle per unit. Each entry records the graph that owns the unit. Entries
 * are removed when that graph is really disposed; once the map is full, new
 * units get an uncached handle. */
#define UNIT_MAP_CAP 256u

struct vgraph;

static struct {
    void *unit;
    uint32_t handle;
    struct vgraph *owner;
} g_unit_map[UNIT_MAP_CAP];
static unsigned g_unit_map_count;
static pthread_mutex_t g_unit_map_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t unit_handle(AudioUnit unit, struct vgraph *owner)
{
    if (unit == NULL)
        return 0;
    pthread_mutex_lock(&g_unit_map_lock);
    for (unsigned i = 0; i < g_unit_map_count; i++) {
        if (g_unit_map[i].unit == (void *)unit) {
            uint32_t h = g_unit_map[i].handle;
            g_unit_map[i].owner = owner;
            pthread_mutex_unlock(&g_unit_map_lock);
            return h;
        }
    }
    uint32_t h = handle_put((void *)unit, HANDLE_AUDIO);
    if (h != 0 && g_unit_map_count < UNIT_MAP_CAP) {
        g_unit_map[g_unit_map_count].unit = (void *)unit;
        g_unit_map[g_unit_map_count].handle = h;
        g_unit_map[g_unit_map_count].owner = owner;
        g_unit_map_count++;
    }
    pthread_mutex_unlock(&g_unit_map_lock);
    return h;
}

static struct vgraph *unit_owner(AudioUnit unit)
{
    struct vgraph *owner = NULL;
    pthread_mutex_lock(&g_unit_map_lock);
    for (unsigned i = 0; i < g_unit_map_count; i++) {
        if (g_unit_map[i].unit == (void *)unit) {
            owner = g_unit_map[i].owner;
            break;
        }
    }
    pthread_mutex_unlock(&g_unit_map_lock);
    return owner;
}

/* Drop a unit from the map and free its guest handle. */
static void unit_forget(AudioUnit unit)
{
    pthread_mutex_lock(&g_unit_map_lock);
    for (unsigned i = 0; i < g_unit_map_count; i++) {
        if (g_unit_map[i].unit == (void *)unit) {
            handle_release(g_unit_map[i].handle, HANDLE_AUDIO);
            g_unit_map[i] = g_unit_map[--g_unit_map_count];
            break;
        }
    }
    pthread_mutex_unlock(&g_unit_map_lock);
}

/* A host graph with a record of what it holds. When the game disposes it, the
 * graph stays open in a pool; the next NewAUGraph gets it back with "reused"
 * set. The game's AddNode calls then claim the nodes that match, with no host
 * call, and AUGraphOpen removes what stays unclaimed. State the game changed
 * is undone when the graph is retired: connections and input callbacks are
 * cleared (an initialized graph leaves its units linked, and a linked input
 * refuses the stream format the game sets before AUGraphInitialize), render
 * notifies removed, first-seen parameter values restored. Any array overflow
 * makes the graph not poolable. Every access to a vgraph and to the pool holds g_graph_lock, but
 * host calls run outside it, because the host audio thread can wait on them. */
#define VG_NODES 8u
#define VG_NOTIFIES 4u
#define VG_UNITS 8u
#define VG_PARAMS 64u
#define GRAPH_POOL_CAP 32u

struct vg_node {
    AudioComponentDescription desc;
    AUNode node;
    int claimed;
};

struct vg_param {
    AudioUnit unit;
    AudioUnitParameterID id;
    AudioUnitScope scope;
    AudioUnitElement el;
    AudioUnitParameterValue first;
};

struct vgraph {
    AUGraph graph;
    int open;
    int reused;
    int poolable;
    struct vg_node nodes[VG_NODES];
    unsigned n_nodes;
    struct render_ctx *notifies[VG_NOTIFIES];
    unsigned n_notifies;
    AudioUnit units[VG_UNITS];
    unsigned n_units;
    struct vg_param params[VG_PARAMS];
    unsigned n_params;
};

static pthread_mutex_t g_graph_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vgraph *g_pool[GRAPH_POOL_CAP];
static unsigned g_pool_count;

/* Next free slot of an array in vg, or NULL (and not poolable) when full.
 * Call with g_graph_lock held. */
#define VG_NEXT(vg, field, count) \
    ((vg)->count < sizeof (vg)->field / sizeof (vg)->field[0] \
         ? &(vg)->field[(vg)->count++] \
         : ((vg)->poolable = 0, NULL))

static struct vgraph *vgraph_of(uint32_t h)
{
    return host_obj(h);
}

static uint64_t now_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* Log a host call that started at t0 if it took more than 2 ms. */
static void log_slow(const char *name, uint64_t t0)
{
    uint64_t t1 = now_ns();
    if (t1 - t0 > 2000000u)
        fprintf(stderr, "audio: %s took %.1f ms at %.3f s (uptime)\n", name,
                (double)(t1 - t0) / 1e6, (double)t1 / 1e9);
}

static AudioUnit g_keepalive;
static pthread_once_t g_keepalive_once = PTHREAD_ONCE_INIT;

static OSStatus silence_render(void *refcon, AudioUnitRenderActionFlags *flags,
                               const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                               AudioBufferList *io)
{
    (void)refcon;
    (void)ts;
    (void)bus;
    (void)frames;
    for (UInt32 i = 0; io != NULL && i < io->mNumberBuffers; i++)
        memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize);
    *flags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
}

/* AUGraphStart took 39-58 ms when no graph had been running for a while, because
 * the output device had stopped its I/O. One silent output unit that runs for
 * the whole process keeps the device running. */
static void audio_keepalive_start(void)
{
    const char *step = "find component";
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioUnit unit = NULL;
    int initialized = 0;
    OSStatus err = kAudioUnitErr_InvalidProperty;
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (comp != NULL) {
        step = "instance new";
        err = AudioComponentInstanceNew(comp, &unit);
    }
    if (err == noErr) {
        AURenderCallbackStruct cb = {.inputProc = silence_render, .inputProcRefCon = NULL};
        step = "set render callback";
        err = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                                   kAudioUnitScope_Input, 0, &cb, sizeof cb);
    }
    if (err == noErr) {
        step = "initialize";
        err = AudioUnitInitialize(unit);
        initialized = err == noErr;
    }
    if (err == noErr) {
        step = "start";
        err = AudioOutputUnitStart(unit);
    }
    if (err == noErr) {
        g_keepalive = unit;
        return;
    }
    fprintf(stderr, "audio: keep-alive output unit not started: error %d (%s)\n", (int)err,
            step);
    if (unit != NULL) {
        if (initialized)
            AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
    }
}

/* Test hook: 1 when the keep-alive unit exists and its device I/O is running. */
int audio_keepalive_running(void)
{
    UInt32 running = 0, size = sizeof running;
    if (g_keepalive == NULL)
        return 0;
    return AudioUnitGetProperty(g_keepalive, kAudioOutputUnitProperty_IsRunning,
                                kAudioUnitScope_Global, 0, &running, &size) == noErr &&
           running != 0;
}

void imp__NewAUGraph(void)
{
    pthread_once(&g_keepalive_once, audio_keepalive_start);
    pthread_mutex_lock(&g_graph_lock);
    struct vgraph *vg = g_pool_count > 0 ? g_pool[--g_pool_count] : NULL;
    if (vg != NULL) {
        vg->reused = 1;
        for (unsigned i = 0; i < vg->n_nodes; i++)
            vg->nodes[i].claimed = 0;
    }
    pthread_mutex_unlock(&g_graph_lock);

    OSStatus err = noErr;
    if (vg == NULL) {
        vg = calloc(1, sizeof *vg);
        if (vg == NULL) {
            err = kAudio_MemFullError;
        } else {
            err = NewAUGraph(&vg->graph);
            if (err != noErr) {
                free(vg);
                vg = NULL;
            } else {
                vg->poolable = 1;
            }
        }
    }
    if (err == noErr) {
        uint32_t h = handle_put(vg, HANDLE_AUDIO);
        if (h == 0) {
            DisposeAUGraph(vg->graph);
            free(vg);
            err = kAudio_MemFullError;
        } else if (ARG(0) != 0) {
            MEM32(ARG(0)) = h;
        }
        trace_audio("new graph %#x%s", h, vg->reused ? " (reused)" : "");
    }
    RET(err);
}

/* Real dispose: forget the units, close and dispose the host graph. */
static OSStatus vgraph_destroy(struct vgraph *vg)
{
    AudioUnit units[VG_UNITS];
    struct render_ctx *notifies[VG_NOTIFIES];
    pthread_mutex_lock(&g_graph_lock);
    unsigned n_units = vg->n_units, n_notifies = vg->n_notifies;
    int open = vg->open;
    memcpy(units, vg->units, sizeof units);
    memcpy(notifies, vg->notifies, sizeof notifies);
    pthread_mutex_unlock(&g_graph_lock);

    for (unsigned i = 0; i < n_units; i++)
        unit_forget(units[i]);
    if (open) {
        uint64_t t0 = now_ns();
        AUGraphClose(vg->graph);
        log_slow("AUGraphClose", t0);
    }
    OSStatus st = DisposeAUGraph(vg->graph);
    for (unsigned i = 0; i < n_notifies; i++)
        free(notifies[i]);
    free(vg);
    return st;
}

/* Disposed by the game: undo what the game set, then pool the open graph. */
static OSStatus vgraph_retire(struct vgraph *vg)
{
    struct vgraph snap;
    pthread_mutex_lock(&g_graph_lock);
    int keep = !diag_caches_off() && vg->poolable && vg->open && g_pool_count < GRAPH_POOL_CAP;
    if (keep) {
        snap = *vg;
        vg->n_notifies = 0;
        vg->n_params = 0;
    }
    pthread_mutex_unlock(&g_graph_lock);
    if (!keep)
        return vgraph_destroy(vg);

    OSStatus st = noErr;
    AUGraph graph = snap.graph;
    Boolean flag = 0;
    if (AUGraphIsRunning(graph, &flag) == noErr && flag)
        st = AUGraphStop(graph);
    flag = 0;
    if (AUGraphIsInitialized(graph, &flag) == noErr && flag)
        st = AUGraphUninitialize(graph);
    for (unsigned i = 0; i < snap.n_notifies; i++) {
        st = AUGraphRemoveRenderNotify(graph, render_trampoline, snap.notifies[i]);
        free(snap.notifies[i]);
    }
    st = AUGraphClearConnections(graph);
    for (unsigned i = 0; i < snap.n_params; i++) {
        const struct vg_param *p = &snap.params[i];
        st = AudioUnitSetParameter(p->unit, p->id, p->scope, p->el, p->first, 0);
    }

    pthread_mutex_lock(&g_graph_lock);
    int pooled = g_pool_count < GRAPH_POOL_CAP;
    if (pooled)
        g_pool[g_pool_count++] = vg;
    pthread_mutex_unlock(&g_graph_lock);
    return pooled ? st : vgraph_destroy(vg);
}

void imp__DisposeAUGraph(void)
{
    trace_audio("dispose graph %#x", ARG(0));
    struct vgraph *vg = handle_release(ARG(0), HANDLE_AUDIO);
    RET(vg != NULL ? vgraph_retire(vg) : kAudio_ParamError);
}

void imp__AUGraphAddNode(void)
{
    struct vgraph *vg = vgraph_of(ARG(0));
    if (vg == NULL || ARG(1) == 0) {
        RET(kAudio_ParamError);
        return;
    }
    AudioComponentDescription desc;
    memcpy(&desc, host_ptr(ARG(1)), sizeof desc);
    AUNode node = 0;
    int matched = 0;
    pthread_mutex_lock(&g_graph_lock);
    if (vg->reused) {
        for (unsigned i = 0; i < vg->n_nodes; i++) {
            if (!vg->nodes[i].claimed && memcmp(&vg->nodes[i].desc, &desc, sizeof desc) == 0) {
                vg->nodes[i].claimed = 1;
                node = vg->nodes[i].node;
                matched = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_graph_lock);

    OSStatus err = noErr;
    if (!matched) {
        err = AUGraphAddNode(vg->graph, &desc, &node);
        if (err == noErr) {
            pthread_mutex_lock(&g_graph_lock);
            struct vg_node *n = VG_NEXT(vg, nodes, n_nodes);
            if (n != NULL) {
                n->desc = desc;
                n->node = node;
                n->claimed = 1;
            }
            pthread_mutex_unlock(&g_graph_lock);
        }
    }
    if (err == noErr && ARG(2) != 0)
        MEM32(ARG(2)) = (uint32_t)node;
    trace_audio("graph %#x add node %d type %.4s sub %.4s%s", ARG(0), (int)node,
                (const char *)&(uint32_t){__builtin_bswap32(desc.componentType)},
                (const char *)&(uint32_t){__builtin_bswap32(desc.componentSubType)},
                matched ? " (kept)" : "");
    RET(err);
}

void imp__AUGraphNodeInfo(void)
{
    struct vgraph *vg = vgraph_of(ARG(0));
    if (vg == NULL) {
        RET(kAudio_ParamError);
        return;
    }
    AudioUnit unit = NULL;
    OSStatus err = AUGraphNodeInfo(vg->graph, (AUNode)ARG(1), host_ptr(ARG(2)),
                                   ARG(3) != 0 ? &unit : NULL);
    if (err == noErr && ARG(3) != 0) {
        uint32_t h = unit_handle(unit, vg);
        if (h == 0) {
            err = kAudio_MemFullError;
        } else {
            MEM32(ARG(3)) = h;
            pthread_mutex_lock(&g_graph_lock);
            unsigned i = 0;
            while (i < vg->n_units && vg->units[i] != unit)
                i++;
            if (i == vg->n_units) {
                AudioUnit *slot = VG_NEXT(vg, units, n_units);
                if (slot != NULL)
                    *slot = unit;
            }
            pthread_mutex_unlock(&g_graph_lock);
        }
    }
    RET(err);
}

void imp__AUGraphConnectNodeInput(void)
{
    struct vgraph *vg = vgraph_of(ARG(0));
    if (vg == NULL) {
        RET(kAudio_ParamError);
        return;
    }
    OSStatus err = AUGraphConnectNodeInput(vg->graph, (AUNode)ARG(1), ARG(2), (AUNode)ARG(3), ARG(4));
    trace_audio("graph %#x connect %d:%u -> %d:%u", ARG(0), (int)ARG(1), (unsigned)ARG(2), (int)ARG(3),
                (unsigned)ARG(4));
    RET(err);
}

void imp__AUGraphSetNodeInputCallback(void)
{
    struct vgraph *vg = vgraph_of(ARG(0));
    AURenderCallbackStruct cb;
    OSStatus err = vg != NULL ? render_callback_from_guest(ARG(3), &cb) : kAudio_ParamError;
    if (err == noErr)
        err = AUGraphSetNodeInputCallback(vg->graph, (AUNode)ARG(1), ARG(2), &cb);
    trace_audio("graph %#x node %d bus %u input callback ctx %p", ARG(0), (int)ARG(1),
                (unsigned)ARG(2), err == noErr ? cb.inputProcRefCon : NULL);
    RET(err);
}

void imp__AUGraphAddRenderNotify(void)
{
    struct vgraph *vg = vgraph_of(ARG(0));
    if (vg == NULL || ARG(1) == 0) {
        RET(kAudio_ParamError);
        return;
    }
    struct render_ctx *c = render_ctx_new(ARG(1), ARG(2));
    if (c == NULL) {
        RET(kAudio_MemFullError);
        return;
    }
    OSStatus err = AUGraphAddRenderNotify(vg->graph, render_trampoline, c);
    if (err == noErr) {
        pthread_mutex_lock(&g_graph_lock);
        struct render_ctx **slot = VG_NEXT(vg, notifies, n_notifies);
        if (slot != NULL)
            *slot = c;
        pthread_mutex_unlock(&g_graph_lock);
    } else {
        free(c);
    }
    RET(err);
}

/* First open of a reused graph: remove the nodes the game did not claim again. */
static void vgraph_reconcile(struct vgraph *vg)
{
    for (;;) {
        struct vg_node n;
        int found = 0;
        pthread_mutex_lock(&g_graph_lock);
        for (unsigned i = 0; i < vg->n_nodes; i++) {
            if (!vg->nodes[i].claimed) {
                n = vg->nodes[i];
                memmove(&vg->nodes[i], &vg->nodes[i + 1], (vg->n_nodes - i - 1) * sizeof n);
                vg->n_nodes--;
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_graph_lock);
        if (!found)
            break;
        uint64_t t0 = now_ns();
        AUGraphRemoveNode(vg->graph, n.node);
        log_slow("AUGraphRemoveNode", t0);
    }
    pthread_mutex_lock(&g_graph_lock);
    vg->reused = 0;
    pthread_mutex_unlock(&g_graph_lock);
}

void imp__AUGraphOpen(void)
{
    struct vgraph *vg = vgraph_of(ARG(0));
    if (vg == NULL) {
        RET(kAudio_ParamError);
        return;
    }
    pthread_mutex_lock(&g_graph_lock);
    int reused = vg->reused;
    pthread_mutex_unlock(&g_graph_lock);
    trace_audio("open graph %#x%s", ARG(0), reused ? " (reused)" : "");
    if (reused)
        vgraph_reconcile(vg);

    pthread_mutex_lock(&g_graph_lock);
    int open = vg->open;
    pthread_mutex_unlock(&g_graph_lock);
    OSStatus st = noErr;
    if (!open) {
        uint64_t t0 = now_ns();
        st = AUGraphOpen(vg->graph);
        log_slow("AUGraphOpen", t0);
        if (st == noErr) {
            pthread_mutex_lock(&g_graph_lock);
            vg->open = 1;
            pthread_mutex_unlock(&g_graph_lock);
        }
    }
    RET(st);
}

/* A poolable graph stays open; it is closed when it is really disposed. */
void imp__AUGraphClose(void)
{
    struct vgraph *vg = vgraph_of(ARG(0));
    if (vg == NULL) {
        RET(kAudio_ParamError);
        return;
    }
    pthread_mutex_lock(&g_graph_lock);
    int poolable = vg->poolable;
    pthread_mutex_unlock(&g_graph_lock);
    OSStatus st = noErr;
    if (!poolable) {
        uint64_t t0 = now_ns();
        st = AUGraphClose(vg->graph);
        log_slow("AUGraphClose", t0);
        if (st == noErr) {
            pthread_mutex_lock(&g_graph_lock);
            vg->open = 0;
            pthread_mutex_unlock(&g_graph_lock);
        }
    }
    RET(st);
}

#define GRAPH_THUNK(name, call) \
    void imp__##name(void) \
    { \
        struct vgraph *vg = vgraph_of(ARG(0)); \
        if (vg == NULL) { \
            RET(kAudio_ParamError); \
            return; \
        } \
        uint64_t t0 = now_ns(); \
        OSStatus st = call(vg->graph); \
        log_slow(#name, t0); \
        trace_audio(#name " graph %#x", ARG(0)); \
        RET(st); \
    }

GRAPH_THUNK(AUGraphInitialize, AUGraphInitialize)
GRAPH_THUNK(AUGraphUninitialize, AUGraphUninitialize)
GRAPH_THUNK(AUGraphStart, AUGraphStart)
GRAPH_THUNK(AUGraphStop, AUGraphStop)

/* ---- AudioUnit ------------------------------------------------------------ */

/* Property data size for the properties whose layout is the same on both
 * sides, or 0 for the others. */
static uint32_t plain_property_size(uint32_t id)
{
    switch (id) {
    case kAudioUnitProperty_SampleRate:
    case kAudioUnitProperty_Latency:
        return sizeof(Float64);
    case kAudioUnitProperty_CPULoad:
        return sizeof(Float32);
    case kAudioUnitProperty_StreamFormat:
        return sizeof(AudioStreamBasicDescription);
    case kAudioUnitProperty_ElementCount:
    case kAudioUnitProperty_MaximumFramesPerSlice:
    case kAudioOutputUnitProperty_CurrentDevice:
        return sizeof(UInt32);
    default:
        return 0;
    }
}

static OSStatus invalid_property(const char *fn, uint32_t id, uint32_t scope, uint32_t element)
{
    diag_unsupported(fn, MEM32(g_esp), "property %u scope %u element %u", id, scope, element);
    return kAudioUnitErr_InvalidProperty;
}

void imp__AudioUnitSetProperty(void)
{
    AudioUnit unit = host_obj(ARG(0));
    uint32_t id = ARG(1), scope = ARG(2), element = ARG(3), data = ARG(4), size = ARG(5);
    if (unit == NULL) {
        RET(kAudio_ParamError);
        return;
    }
    if (id == kAudioUnitProperty_SetRenderCallback) {
        AURenderCallbackStruct cb;
        OSStatus err = size == 8 ? render_callback_from_guest(data, &cb) : kAudioUnitErr_InvalidPropertyValue;
        if (err == noErr)
            err = AudioUnitSetProperty(unit, id, scope, element, &cb, sizeof cb);
        trace_audio("unit %#x scope %u element %u render callback ctx %p", ARG(0), (unsigned)scope,
                    (unsigned)element, err == noErr ? cb.inputProcRefCon : NULL);
        RET(err);
        return;
    }
    uint32_t want = plain_property_size(id);
    trace_audio("unit %#x set property %u scope %u element %u", ARG(0), (unsigned)id, (unsigned)scope,
                (unsigned)element);
    if (want == 0) {
        RET(invalid_property("imp__AudioUnitSetProperty", id, scope, element));
        return;
    }
    if (size != want || data == 0) {
        RET(kAudioUnitErr_InvalidPropertyValue);
        return;
    }
    if (id == kAudioUnitProperty_StreamFormat) {
        const AudioStreamBasicDescription *f = guest_ptr(data);
        trace_audio("unit %#x format scope %u: %.0f Hz %.4s flags %#x %u ch %u bit", ARG(0),
                    (unsigned)scope, f->mSampleRate,
                    (const char *)&(uint32_t){__builtin_bswap32(f->mFormatID)},
                    (unsigned)f->mFormatFlags, (unsigned)f->mChannelsPerFrame,
                    (unsigned)f->mBitsPerChannel);
    }
    RET(AudioUnitSetProperty(unit, id, scope, element, guest_ptr(data), size));
}

void imp__AudioUnitGetProperty(void)
{
    AudioUnit unit = host_obj(ARG(0));
    uint32_t id = ARG(1), scope = ARG(2), element = ARG(3), data = ARG(4), size_ptr = ARG(5);
    if (unit == NULL || size_ptr == 0) {
        RET(kAudio_ParamError);
        return;
    }
    if (plain_property_size(id) == 0) {
        RET(invalid_property("imp__AudioUnitGetProperty", id, scope, element));
        return;
    }
    UInt32 size = MEM32(size_ptr);
    OSStatus err = AudioUnitGetProperty(unit, id, scope, element, host_ptr(data), &size);
    MEM32(size_ptr) = size;
    RET(err);
}

static int param_known(const struct vgraph *vg, AudioUnit unit, uint32_t id, uint32_t scope,
                       uint32_t el)
{
    for (unsigned i = 0; i < vg->n_params; i++) {
        const struct vg_param *p = &vg->params[i];
        if (p->unit == unit && p->id == id && p->scope == scope && p->el == el)
            return 1;
    }
    return 0;
}

/* Before the game's first set of a parameter on a unit of a pooled graph, keep
 * the current value so that retiring the graph can put it back. If the read
 * fails, the graph is not poolable. Some units return noErr and leave the value
 * unwritten (the mixer, for global scope); nothing is kept for those. */
static void param_note_first(AudioUnit unit, uint32_t id, uint32_t scope, uint32_t el)
{
    struct vgraph *vg = unit_owner(unit);
    if (vg == NULL)
        return;
    pthread_mutex_lock(&g_graph_lock);
    int skip = !vg->poolable || param_known(vg, unit, id, scope, el);
    pthread_mutex_unlock(&g_graph_lock);
    if (skip)
        return;
    AudioUnitParameterValue first = NAN;
    OSStatus err = AudioUnitGetParameter(unit, id, scope, el, &first);
    pthread_mutex_lock(&g_graph_lock);
    if (err != noErr) {
        vg->poolable = 0;
    } else if (!isnan(first) && !param_known(vg, unit, id, scope, el)) {
        struct vg_param *p = VG_NEXT(vg, params, n_params);
        if (p != NULL) {
            p->unit = unit;
            p->id = id;
            p->scope = scope;
            p->el = el;
            p->first = first;
        }
    }
    pthread_mutex_unlock(&g_graph_lock);
}

/* Trace a parameter set only when its value changed since the last set. */
static void trace_param(uint32_t unit, uint32_t id, uint32_t scope, uint32_t el, float value)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static struct {
        uint32_t unit, id, scope, el;
        float value;
    } last[512];
    static unsigned n;
    pthread_mutex_lock(&lock);
    unsigned i = 0;
    while (i < n && !(last[i].unit == unit && last[i].id == id && last[i].scope == scope &&
                      last[i].el == el))
        i++;
    int changed = i == n || last[i].value != value;
    if (i == n && n < 512)
        n++;
    if (i < 512) {
        last[i].unit = unit;
        last[i].id = id;
        last[i].scope = scope;
        last[i].el = el;
        last[i].value = value;
    }
    pthread_mutex_unlock(&lock);
    if (changed)
        trace_audio("unit %#x param %u scope %u element %u = %g", unit, (unsigned)id, (unsigned)scope,
                    (unsigned)el, (double)value);
}

void imp__AudioUnitSetParameter(void)
{
    AudioUnit unit = host_obj(ARG(0));
    if (unit == NULL) {
        RET(kAudio_ParamError);
        return;
    }
    uint32_t bits = ARG(4);
    Float32 value;
    memcpy(&value, &bits, sizeof value);
    param_note_first(unit, ARG(1), ARG(2), ARG(3));
    if (trace_audio_on())
        trace_param(ARG(0), ARG(1), ARG(2), ARG(3), value);
    RET(AudioUnitSetParameter(unit, ARG(1), ARG(2), ARG(3), value, ARG(5)));
}

/* ---- AudioDevice ---------------------------------------------------------- */

static int device_property_ok(uint32_t id)
{
    return id == kAudioDevicePropertyPreferredChannelLayout || id == kAudioDevicePropertyNominalSampleRate ||
           id == kAudioDevicePropertyBufferSize;
}

void imp__AudioDeviceGetProperty(void)
{
    uint32_t id = ARG(3), size_ptr = ARG(4);
    trace_audio("AudioDeviceGetProperty device %u channel %u input %#x property %.4s size %u", ARG(0),
                ARG(1), ARG(2), (const char *)&(uint32_t){__builtin_bswap32(id)},
                size_ptr != 0 ? MEM32(size_ptr) : 0);
    if (!device_property_ok(id))
        THUNK_UNSUPPORTED("device property '%.4s'", (const char *)&(uint32_t){__builtin_bswap32(id)});
    if (!device_property_ok(id) || size_ptr == 0 || ARG(5) == 0) {
        RET(kAudioHardwareUnknownPropertyError);
        return;
    }
    UInt32 size = MEM32(size_ptr);
    OSStatus err = AudioDeviceGetProperty(ARG(0), ARG(1), (Boolean)(ARG(2) & 0xFFu), id, &size,
                                          guest_ptr(ARG(5)));
    MEM32(size_ptr) = size;
    RET(err);
}

void imp__AudioDeviceGetPropertyInfo(void)
{
    uint32_t id = ARG(3);
    trace_audio("AudioDeviceGetPropertyInfo device %u channel %u input %#x property %.4s", ARG(0), ARG(1),
                ARG(2), (const char *)&(uint32_t){__builtin_bswap32(id)});
    if (!device_property_ok(id)) {
        THUNK_UNSUPPORTED("device property '%.4s'", (const char *)&(uint32_t){__builtin_bswap32(id)});
        RET(kAudioHardwareUnknownPropertyError);
        return;
    }
    UInt32 size = 0;
    Boolean writable = 0;
    OSStatus err = AudioDeviceGetPropertyInfo(ARG(0), ARG(1), (Boolean)(ARG(2) & 0xFFu), id, &size,
                                              &writable);
    if (err == noErr) {
        if (ARG(4) != 0)
            MEM32(ARG(4)) = size;
        if (ARG(5) != 0)
            *(uint8_t *)guest_ptr(ARG(5)) = writable;
    }
    RET(err);
}

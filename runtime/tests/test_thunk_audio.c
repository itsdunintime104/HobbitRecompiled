/* Unit test for the audio thunks: a default-output AUGraph with a guest render
 * callback that writes a 440 Hz sine, run on the host audio device for 0.3 s.
 * Prints "ok" and exits 0 when every check passes. */
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "guest_heap.h"
#include "guest_mem.h"
#include "handles.h"
#include "host_funcs.h"
#include "recomp_types.h"

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define STACK_TOP 0x00FF0000u
#define SCRATCH 0x00F10000u
#define RET_MAGIC 0xDEADBEEFu
#define VA_RENDER 0x00401000u
#define VA_NOTIFY 0x00401100u
#define RENDER_REFCON 0x1234u
#define NOTIFY_REFCON 0x5678u
#define AUDIO_POST_RENDER (1u << 3) /* kAudioUnitRenderAction_PostRender */

#define DECL(n) void imp__##n(void)
DECL(NewAUGraph);
DECL(DisposeAUGraph);
DECL(AUGraphAddNode);
DECL(AUGraphConnectNodeInput);
DECL(AUGraphNodeInfo);
DECL(AUGraphOpen);
DECL(AUGraphClose);
DECL(AUGraphInitialize);
DECL(AUGraphUninitialize);
DECL(AUGraphStart);
DECL(AUGraphStop);
DECL(AUGraphSetNodeInputCallback);
DECL(AUGraphAddRenderNotify);
DECL(AudioUnitGetProperty);
DECL(AudioUnitSetProperty);
DECL(AudioUnitSetParameter);
DECL(AudioDeviceGetProperty);
DECL(AudioDeviceGetPropertyInfo);

static int g_failures;

int audio_keepalive_running(void);

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL line %d: ", __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
            g_failures++; \
        } \
    } while (0)

/* ---- Guest functions, run on the host audio thread ------------------------ */

static _Atomic int g_render_calls;
static _Atomic int g_render_bad_args;
static _Atomic uint32_t g_render_frames;
static _Atomic uint32_t g_render_buffers;
static _Atomic uint32_t g_render_bytes;
static _Atomic int g_notify_calls;
static _Atomic int g_notify_nonzero;
static _Atomic int g_notify_bad_args;
static double g_phase; /* render callback runs on one thread */
static double g_last_sample_time = -1;
static _Atomic int g_ts_not_increasing;

/* Guest render callback: (refcon, flags*, timestamp*, bus, frames, abl*). */
static void guest_render(void)
{
    uint32_t refcon = MEM32(g_esp + 4), ts = MEM32(g_esp + 12), bus = MEM32(g_esp + 16);
    uint32_t frames = MEM32(g_esp + 20), abl = MEM32(g_esp + 24);
    if (refcon != RENDER_REFCON || bus != 0)
        g_render_bad_args++;
    double sample_time;
    memcpy(&sample_time, guest_ptr(ts), sizeof sample_time);
    if (sample_time <= g_last_sample_time)
        g_ts_not_increasing++;
    g_last_sample_time = sample_time;

    uint32_t nb = MEM32(abl);
    g_render_frames += frames;
    g_render_buffers = nb;
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t e = abl + 4 + 12 * i;
        uint32_t bytes = MEM32(e + 4);
        float *out = (float *)guest_ptr(MEM32(e + 8));
        g_render_bytes = bytes;
        double ph = g_phase;
        for (uint32_t k = 0; k < bytes / 4; k++) {
            out[k] = (float)(0.05 * sin(ph));
            ph += 2.0 * M_PI * 440.0 / 44100.0;
        }
        if (i == nb - 1)
            g_phase = ph;
    }
    g_render_calls++;
    g_eax = 0;
}

/* Guest render notify: after the render, the host data copied back must show. */
static void guest_notify(void)
{
    uint32_t refcon = MEM32(g_esp + 4), flags = MEM32(MEM32(g_esp + 8)), abl = MEM32(g_esp + 24);
    if (refcon != NOTIFY_REFCON)
        g_notify_bad_args++;
    g_notify_calls++;
    if ((flags & AUDIO_POST_RENDER) && MEM32(abl) > 0) {
        uint32_t e = abl + 4;
        const float *in = (const float *)guest_ptr(MEM32(e + 8));
        for (uint32_t k = 0; k < MEM32(e + 4) / 4; k++) {
            if (in[k] != 0.0f) {
                g_notify_nonzero++;
                break;
            }
        }
    }
    g_eax = 0;
}

recomp_func_t recomp_lookup(uint32_t va)
{
    if (va == VA_RENDER)
        return guest_render;
    if (va == VA_NOTIFY)
        return guest_notify;
    return NULL;
}

recomp_func_t guest_resolve(uint32_t va)
{
    return recomp_lookup(va);
}

/* ---- Harness -------------------------------------------------------------- */

static uint32_t call(void (*fn)(void), const uint32_t *args, int nargs)
{
    uint32_t sp = STACK_TOP - 4u * (uint32_t)(nargs + 1);
    MEM32(sp) = RET_MAGIC;
    for (int i = 0; i < nargs; i++)
        MEM32(sp + 4 + 4 * (uint32_t)i) = args[i];
    g_esp = sp;
    g_ebx = 0x11111111;
    g_esi = 0x22222222;
    g_edi = 0x33333333;
    g_ebp = 0x44444444;
    fn();
    CHECK(g_esp == sp + 4, "esp %x, want %x", g_esp, sp + 4);
    CHECK(g_ebx == 0x11111111 && g_esi == 0x22222222 && g_edi == 0x33333333 &&
              g_ebp == 0x44444444,
          "callee-saved register changed");
    return g_eax;
}

static void test_graph(void)
{
    const uint32_t desc = SCRATCH, graph_slot = SCRATCH + 0x20, node_slot = SCRATCH + 0x24,
                   unit_slot = SCRATCH + 0x28, cbs = SCRATCH + 0x30;
    const uint32_t asbd = SCRATCH + 0x100, asbd2 = SCRATCH + 0x140, size_slot = SCRATCH + 0x180;

    CHECK(call(imp__NewAUGraph, (uint32_t[]){graph_slot}, 1) == 0, "NewAUGraph");
    uint32_t graph = MEM32(graph_slot);
    CHECK(handle_get(graph, HANDLE_AUDIO) != NULL, "graph handle %x", graph);
    if (audio_keepalive_running())
        fprintf(stderr, "keep-alive output unit running\n");
    else
        fprintf(stderr, "skip: keep-alive output unit not running (no output device?)\n");

    /* AudioComponentDescription: output, default output, Apple. */
    MEM32(desc) = 0x61756F75u;
    MEM32(desc + 4) = 0x64656620u;
    MEM32(desc + 8) = 0x6170706Cu;
    MEM32(desc + 12) = 0;
    MEM32(desc + 16) = 0;
    CHECK(call(imp__AUGraphAddNode, (uint32_t[]){graph, desc, node_slot}, 3) == 0, "AUGraphAddNode");
    uint32_t node = MEM32(node_slot);
    CHECK(call(imp__AUGraphOpen, (uint32_t[]){graph}, 1) == 0, "AUGraphOpen");
    CHECK(call(imp__AUGraphNodeInfo, (uint32_t[]){graph, node, 0, unit_slot}, 4) == 0, "AUGraphNodeInfo");
    uint32_t unit = MEM32(unit_slot);
    CHECK(handle_get(unit, HANDLE_AUDIO) != NULL, "unit handle %x", unit);

    /* AudioStreamBasicDescription round trip (scope input, element 0). */
    MEM32(size_slot) = 40;
    uint32_t get[] = {unit, 8, 1, 0, asbd, size_slot};
    CHECK(call(imp__AudioUnitGetProperty, get, 6) == 0, "get StreamFormat");
    CHECK(MEM32(size_slot) == 40, "StreamFormat size %u", MEM32(size_slot));
    double rate;
    memcpy(&rate, guest_ptr(asbd), sizeof rate);
    CHECK(rate >= 8000.0 && rate <= 192000.0, "sample rate %f", rate);
    CHECK(MEM32(asbd + 8) == 0x6C70636Du, "format id %x, want lpcm", MEM32(asbd + 8));
    CHECK((MEM32(asbd + 12) & 1u) != 0, "format flags %x: not float", MEM32(asbd + 12));
    uint32_t set[] = {unit, 8, 1, 0, asbd, 40};
    CHECK(call(imp__AudioUnitSetProperty, set, 6) == 0, "set StreamFormat");
    MEM32(size_slot) = 40;
    uint32_t get2[] = {unit, 8, 1, 0, asbd2, size_slot};
    CHECK(call(imp__AudioUnitGetProperty, get2, 6) == 0, "get StreamFormat again");
    CHECK(memcmp(guest_ptr(asbd), guest_ptr(asbd2), 40) == 0, "StreamFormat round trip differs");

    /* Properties the game does not use are refused. */
    uint32_t bad[] = {unit, 9999, 0, 0, asbd, size_slot};
    CHECK(call(imp__AudioUnitGetProperty, bad, 6) == (uint32_t)-10879, "unknown property");
    CHECK(call(imp__AudioUnitSetParameter, (uint32_t[]){0x1234, 0, 0, 0, 0x3f800000u, 0}, 6) ==
              (uint32_t)-50,
          "bad unit handle");

    /* Guest callbacks: AURenderCallbackStruct { proc, refcon }. */
    MEM32(cbs) = VA_RENDER;
    MEM32(cbs + 4) = RENDER_REFCON;
    CHECK(call(imp__AUGraphSetNodeInputCallback, (uint32_t[]){graph, node, 0, cbs}, 4) == 0,
          "AUGraphSetNodeInputCallback");
    CHECK(call(imp__AUGraphAddRenderNotify, (uint32_t[]){graph, VA_NOTIFY, NOTIFY_REFCON}, 3) == 0,
          "AUGraphAddRenderNotify");
    CHECK(call(imp__AUGraphInitialize, (uint32_t[]){graph}, 1) == 0, "AUGraphInitialize");

    CHECK(call(imp__AUGraphStart, (uint32_t[]){graph}, 1) == 0, "AUGraphStart");
    usleep(300000);
    CHECK(call(imp__AUGraphStop, (uint32_t[]){graph}, 1) == 0, "AUGraphStop");

    CHECK(call(imp__AUGraphUninitialize, (uint32_t[]){graph}, 1) == 0, "AUGraphUninitialize");
    CHECK(call(imp__AUGraphClose, (uint32_t[]){graph}, 1) == 0, "AUGraphClose");
    CHECK(call(imp__DisposeAUGraph, (uint32_t[]){graph}, 1) == 0, "DisposeAUGraph");
    CHECK(handle_get(graph, HANDLE_AUDIO) == NULL, "graph handle not released");
}

/* ---- Graph reuse ---------------------------------------------------------- */

struct tgraph {
    uint32_t graph, out_node, vari_node, conv_node, out_unit, vari_unit;
};

/* Build and initialize a graph the way the game does: default output and, when
 * with_vari is set, a converter feeding a varispeed feeding it. The guest render
 * callback is the input of the first node of the chain. Like the game, the
 * stream format of the varispeed input is set between Open and Initialize.
 * Every thunk must return 0. */
static void build_graph(struct tgraph *g, int with_vari)
{
    const uint32_t desc = SCRATCH + 0x300, desc_vari = SCRATCH + 0x320, graph_slot = SCRATCH + 0x340,
                   out_slot = SCRATCH + 0x344, vari_slot = SCRATCH + 0x348,
                   out_unit_slot = SCRATCH + 0x34C, vari_unit_slot = SCRATCH + 0x350,
                   cbs = SCRATCH + 0x360, desc_conv = SCRATCH + 0x380, conv_slot = SCRATCH + 0x3A0,
                   asbd = SCRATCH + 0x3C0, size_slot = SCRATCH + 0x3F0,
                   conv_unit_slot = SCRATCH + 0x3F4;
    memset(g, 0, sizeof *g);
    CHECK(call(imp__NewAUGraph, (uint32_t[]){graph_slot}, 1) == 0, "NewAUGraph");
    g->graph = MEM32(graph_slot);
    MEM32(desc) = 0x61756F75u; /* 'auou' */
    MEM32(desc + 4) = 0x64656620u; /* 'def ' */
    MEM32(desc + 8) = 0x6170706Cu; /* 'appl' */
    MEM32(desc + 12) = 0;
    MEM32(desc + 16) = 0;
    CHECK(call(imp__AUGraphAddNode, (uint32_t[]){g->graph, desc, out_slot}, 3) == 0, "AddNode output");
    g->out_node = MEM32(out_slot);
    uint32_t first = g->out_node;
    if (with_vari) {
        MEM32(desc_vari) = 0x61756663u; /* 'aufc' */
        MEM32(desc_vari + 4) = 0x76617269u; /* 'vari' */
        MEM32(desc_vari + 8) = 0x6170706Cu;
        MEM32(desc_vari + 12) = 0;
        MEM32(desc_vari + 16) = 0;
        CHECK(call(imp__AUGraphAddNode, (uint32_t[]){g->graph, desc_vari, vari_slot}, 3) == 0,
              "AddNode varispeed");
        g->vari_node = MEM32(vari_slot);
        MEM32(desc_conv) = 0x61756663u; /* 'aufc' */
        MEM32(desc_conv + 4) = 0x636F6E76u; /* 'conv' */
        MEM32(desc_conv + 8) = 0x6170706Cu;
        MEM32(desc_conv + 12) = 0;
        MEM32(desc_conv + 16) = 0;
        CHECK(call(imp__AUGraphAddNode, (uint32_t[]){g->graph, desc_conv, conv_slot}, 3) == 0,
              "AddNode converter");
        g->conv_node = MEM32(conv_slot);
        first = g->conv_node;
        CHECK(call(imp__AUGraphConnectNodeInput, (uint32_t[]){g->graph, g->conv_node, 0, g->vari_node, 0},
                   5) == 0,
              "ConnectNodeInput converter");
        CHECK(call(imp__AUGraphConnectNodeInput, (uint32_t[]){g->graph, g->vari_node, 0, g->out_node, 0},
                   5) == 0,
              "ConnectNodeInput varispeed");
    }
    CHECK(call(imp__AUGraphOpen, (uint32_t[]){g->graph}, 1) == 0, "AUGraphOpen");
    CHECK(call(imp__AUGraphNodeInfo, (uint32_t[]){g->graph, g->out_node, 0, out_unit_slot}, 4) == 0,
          "NodeInfo output");
    g->out_unit = MEM32(out_unit_slot);
    if (with_vari) {
        CHECK(call(imp__AUGraphNodeInfo, (uint32_t[]){g->graph, g->vari_node, 0, vari_unit_slot}, 4) == 0,
              "NodeInfo varispeed");
        g->vari_unit = MEM32(vari_unit_slot);
        CHECK(call(imp__AUGraphNodeInfo, (uint32_t[]){g->graph, g->conv_node, 0, conv_unit_slot}, 4) == 0,
              "NodeInfo converter");
        uint32_t conv_unit = MEM32(conv_unit_slot);
        MEM32(size_slot) = 40;
        CHECK(call(imp__AudioUnitGetProperty, (uint32_t[]){g->vari_unit, 8, 1, 0, asbd, size_slot}, 6) == 0,
              "get varispeed input StreamFormat");
        /* Formats that are not the defaults, and a varispeed input rate that
         * differs from the converter output, so AUGraphInitialize changes the
         * varispeed input and the next graph sets it again on a used unit. */
        double rate;
        const uint32_t fmt_sets[][3] = {{conv_unit, 2, 0}, {g->vari_unit, 1, 0}, {g->vari_unit, 2, 0},
                                        {g->out_unit, 1, 0}};
        for (unsigned i = 0; i < sizeof fmt_sets / sizeof fmt_sets[0]; i++) {
            rate = i == 1 ? 32000.0 : 22050.0;
            memcpy(guest_ptr(asbd), &rate, sizeof rate);
            CHECK(call(imp__AudioUnitSetProperty,
                       (uint32_t[]){fmt_sets[i][0], 8, fmt_sets[i][1], fmt_sets[i][2], asbd, 40}, 6) == 0,
                  "set StreamFormat %u (unit %#x scope %u)", i, fmt_sets[i][0], fmt_sets[i][1]);
        }
    }
    MEM32(cbs) = VA_RENDER;
    MEM32(cbs + 4) = RENDER_REFCON;
    CHECK(call(imp__AUGraphSetNodeInputCallback, (uint32_t[]){g->graph, first, 0, cbs}, 4) == 0,
          "SetNodeInputCallback");
    CHECK(call(imp__AUGraphAddRenderNotify, (uint32_t[]){g->graph, VA_NOTIFY, NOTIFY_REFCON}, 3) == 0,
          "AddRenderNotify");
    CHECK(call(imp__AUGraphInitialize, (uint32_t[]){g->graph}, 1) == 0, "AUGraphInitialize");
}

static void play_graph(const struct tgraph *g, unsigned ms)
{
    g_last_sample_time = -1; /* a new run starts its sample time again */
    CHECK(call(imp__AUGraphStart, (uint32_t[]){g->graph}, 1) == 0, "AUGraphStart");
    usleep(ms * 1000u);
    CHECK(call(imp__AUGraphStop, (uint32_t[]){g->graph}, 1) == 0, "AUGraphStop");
}

static void retire_graph(const struct tgraph *g)
{
    CHECK(call(imp__AUGraphUninitialize, (uint32_t[]){g->graph}, 1) == 0, "AUGraphUninitialize");
    CHECK(call(imp__AUGraphClose, (uint32_t[]){g->graph}, 1) == 0, "AUGraphClose");
    CHECK(call(imp__DisposeAUGraph, (uint32_t[]){g->graph}, 1) == 0, "DisposeAUGraph");
}

/* A graph built after a disposed one with the same recipe gets the same nodes
 * and units, and plays. */
static void test_reuse(void)
{
    struct tgraph a, b;
    build_graph(&a, 1);
    play_graph(&a, 100);
    retire_graph(&a);
    build_graph(&b, 1);
    CHECK(b.out_node == a.out_node && b.vari_node == a.vari_node, "reuse: node ids %u,%u vs %u,%u",
          b.out_node, b.vari_node, a.out_node, a.vari_node);
    CHECK(b.out_unit == a.out_unit && b.vari_unit == a.vari_unit, "reuse: unit handles differ");
    int before = g_render_calls;
    play_graph(&b, 300);
    CHECK(g_render_calls >= before + 3, "reuse: render callback ran %d times", g_render_calls - before);
    retire_graph(&b);
}

/* A parameter the game set on a disposed graph is back to its first value on
 * the next graph. */
static void test_param_restore(void)
{
    const uint32_t kVarispeedPlaybackRate = 0;
    struct tgraph a, b;
    build_graph(&a, 1);
    AudioUnit unit = handle_get(a.vari_unit, HANDLE_AUDIO);
    CHECK(unit != NULL, "param: varispeed unit handle");
    AudioUnitParameterValue first = -1, now = -1;
    CHECK(AudioUnitGetParameter(unit, kVarispeedPlaybackRate, 0, 0, &first) == 0, "param: get first");
    float want = first == 1.5f ? 2.0f : 1.5f;
    uint32_t bits;
    memcpy(&bits, &want, sizeof bits);
    CHECK(call(imp__AudioUnitSetParameter, (uint32_t[]){a.vari_unit, kVarispeedPlaybackRate, 0, 0, bits, 0},
               6) == 0,
          "param: set");
    CHECK(AudioUnitGetParameter(unit, kVarispeedPlaybackRate, 0, 0, &now) == 0 && now == want,
          "param: value after set %f, want %f", now, want);
    retire_graph(&a);

    build_graph(&b, 1);
    unit = handle_get(b.vari_unit, HANDLE_AUDIO);
    CHECK(b.vari_unit == a.vari_unit, "param: graph not reused");
    now = -1;
    CHECK(AudioUnitGetParameter(unit, kVarispeedPlaybackRate, 0, 0, &now) == 0 && now == first,
          "param: value on next graph %f, want %f", now, first);
    retire_graph(&b);
}

/* A graph with fewer nodes than the pooled one still opens, runs and closes. */
static void test_recipe_change(void)
{
    struct tgraph c;
    build_graph(&c, 0);
    int before = g_render_calls;
    play_graph(&c, 200);
    CHECK(g_render_calls > before, "recipe: render callback did not run");
    retire_graph(&c);
}

static void test_device(void)
{
    const uint32_t size_slot = SCRATCH + 0x200, data = SCRATCH + 0x210;
    /* Properties other than the three forwarded ones give 'who?'. */
    MEM32(size_slot) = 4;
    CHECK(call(imp__AudioDeviceGetProperty, (uint32_t[]){1, 0, 0, 0x6F647666, size_slot, data}, 6) ==
              0x77686F3Fu,
          "AudioDeviceGetProperty other");
    CHECK(call(imp__AudioDeviceGetPropertyInfo, (uint32_t[]){1, 0, 0, 0x6F647666, size_slot, 0}, 6) ==
              0x77686F3Fu,
          "AudioDeviceGetPropertyInfo other");

    /* The preferred channel layout of the default output device reaches the host: the game
     * mixes for 8 speakers when this fails. */
    AudioDeviceID dev = 0;
    UInt32 dev_size = sizeof dev;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice,
                                    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &dev_size, &dev) != noErr ||
        dev == 0)
        return;
    const uint32_t srnd = ('s' << 24) | ('r' << 16) | ('n' << 8) | 'd', layout = SCRATCH + 0x1000;
    MEM32(size_slot) = 0;
    CHECK(call(imp__AudioDeviceGetPropertyInfo, (uint32_t[]){dev, 0, 0, srnd, size_slot, 0}, 6) == 0,
          "AudioDeviceGetPropertyInfo srnd");
    uint32_t n = MEM32(size_slot);
    CHECK(n >= 12 && n <= 0x1000, "srnd size %u", n);
    if (n < 12 || n > 0x1000)
        return;
    CHECK(call(imp__AudioDeviceGetProperty, (uint32_t[]){dev, 0, 0, srnd, size_slot, layout}, 6) == 0,
          "AudioDeviceGetProperty srnd");
    CHECK(MEM32(layout) != 0 || MEM32(layout + 8) > 0, "srnd layout is empty");
}

int main(void)
{
    if (guest_mem_init() != 0)
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    if (mprotect(guest_ptr(STACK_LO), STACK_HI - STACK_LO, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 2;
    }
    if (guest_heap_init() != 0)
        return 2;
    test_graph();
    test_reuse();
    test_param_restore();
    test_recipe_change();
    test_device();

    CHECK(g_render_calls >= 5, "render callback ran %d times", g_render_calls);
    CHECK(g_render_bad_args == 0, "render callback got wrong refcon or bus");
    CHECK(g_render_frames > 0, "no frames rendered");
    CHECK(g_render_buffers > 0 && g_render_bytes > 0, "empty buffer list (%u buffers, %u bytes)",
          g_render_buffers, g_render_bytes);
    CHECK(g_ts_not_increasing == 0, "timestamp sample time did not increase");
    CHECK(g_notify_calls >= 5, "render notify ran %d times", g_notify_calls);
    CHECK(g_notify_bad_args == 0, "render notify got the wrong refcon");
    CHECK(g_notify_nonzero > 0, "no non-zero samples reached the host buffer");
    fprintf(stderr, "render calls %d, frames %u, buffers %u x %u bytes, notify %d (%d with samples)\n",
            g_render_calls, g_render_frames, g_render_buffers, g_render_bytes, g_notify_calls,
            g_notify_nonzero);
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}

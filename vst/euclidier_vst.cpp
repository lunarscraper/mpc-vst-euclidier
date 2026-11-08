/* =============================================================================
 * euclidier_vst.cpp - Euclidier as a VST2 MIDI generator for the MPC OS plugin
 * host (https://github.com/open-mpc/mpc-vst-plugins), engine IN-PROCESS.
 *
 * lunarscraper fork of sd88me/mpc-vst-euclidier. The screen is upstream's design (make_skin.py: tabs LANES and
 * ALL with the circle / lane-row step displays, lane stepper, DRUM MODE ALL, RANDOMISE per lane / selected / all,
 * fresh-instance drum defaults) plus a SETUP tab; underneath it keeps this fork's own engine hosting:
 *
 *   upstream (sd88me, 1.0.x)                  this fork
 *   ------------------------------------      -----------------------------------------
 *   posix_spawn's the standalone engine        the engine (../src) is compiled into the .so
 *   binary per instance, GET/SET over a        (rtmidi_stub/RtMidi.h, main() renamed); its
 *   Unix control socket on a worker thread     ctrlGet()/ctrlSet() are called directly
 *   clock: transport_grid.h -> ALSA ->         clock: PpqClock below, the engine's `tick` is
 *   engine (Song Position Pointers)            SET to the host's pulse index before every pulse
 *   engine's own ALSA port "Euclidier N"       the plugin's own ALSA port "Euclidier" /
 *                                              "Euclidier FX" (MPC OS ignores VST MIDI out)
 *   instrument only                            instrument AND insert effect (see below)
 *   -                                          CC control on a "Control In" port (see below)
 *
 * The engine keeps its state in globals (it was written as a one-instance app), so ONE instance per project drives
 * it; a second instance of the same .so loads but stays silent (logged to /tmp/euclidier_vst.log). Euclidier and
 * Euclidier FX are separate .so files with their own globals, so one of each can run side by side.
 * Every engine call happens under the engine's own engineMutex.
 *
 * Step displays: a worker thread (owner instance only) reads every lane's "l<N>_pattern" about 33 times a second,
 * turns it into the values of the display-only cell params (g<lane>_<cap>_<slot>, c<cap>_<slot>, see make_skin.py)
 * and re-reads the real params after anything that changes the engine behind the host's back (preset load,
 * randomise, a lane or mode change, a CC). processReplacing() reports every changed value to the host with
 * audioMasterAutomate, because MPC does not redraw engine-driven values on its own.
 *
 * TWO VARIANTS from this one file (params.h decides, see ../vst-fx/vst.json "effect": true):
 *   Euclidier     instrument (IsSynth, 0 in / 2 out, writes silence) -> uses one of the 8 plugin instrument slots
 *   Euclidier FX  insert effect (PLUG_EFFECT: 2 in / 2 out, audio passed through untouched, category Effect, own
 *                 UID and own ALSA port "Euclidier FX") -> uses NO instrument slot; insert it on the track it drives.
 * CONTROL BY MIDI CC (both variants): the plugin has a second ALSA port "Control In" that subscribes itself to every
 * hardware MIDI input (also ones plugged in later). CCs on the channel chosen with "Control Ch" (OFF, 1..16; default
 * 16; saved with the project; tab SETUP) set parameters:
 *   CC 10*L + k for lane L = 1..8:  k = 0 enable, 1 steps, 2 fill, 3 shift, 4 loop, 5 gate,
 *                                       6 velocity, 7 humanize, 8 note, 9 division
 *   CC 100 preset, 101 load, 102 save, 103 rand lane (which lanes CC 104 hits), 104 randomise (buttons: value >= 64)
 * "Control Ch" lives in this wrapper only (the engine doesn't know the key).
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <dlfcn.h>

#ifndef NO_ALSA
#include <alsa/asoundlib.h>
#endif

/* ---- the engine, compiled into this translation unit --------------------------------
 * build.sh copies ../src/euclidier.cpp etc. to build/eng/ next to rtmidi_stub/RtMidi.h.
 * Its main() is renamed and never called; the globals and the static ctrlGet()/ctrlSet()
 * become directly usable here. Warnings off: it's the engine's own code. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define main euclidier_standalone_main
#include "euclidier.cpp"
#undef main
#pragma GCC diagnostic pop

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };

static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

/* ---------------------------------------------------------------------------
 * PpqClock: host ppqPos -> 24-PPQN pulses by ABSOLUTE pulse index (this fork's clock fix, verified on a Force).
 *
 * Feeding "every pulse inside this block's own window" is only exact-once if every callback's ppqPos advances by
 * precisely frames*tempo/60/sr. When it doesn't (host splits or repeats a block, reports ppqPos late, re-derives it
 * on a tempo change, restarts without dropping the playing flag) windows overlap or leave holes: pulses get fed twice
 * or never, and every such pulse shifts the pattern against the MPC grid for good.
 *
 * Here pulse N simply IS ppq N/24. We remember the last index fed and feed (sent, last-in-this-block]: an overlap
 * feeds nothing twice, a small hole is caught up at once, and a real jump (loop, locate, restart) is detected and
 * re-anchored instead of silently drifting. The pulses of a block are fed at its start (look-ahead, as upstream's
 * transport_grid.h does), and the engine's tick is set to the pulse index, so the lanes sit on the MPC grid.
 * ------------------------------------------------------------------------- */
struct PpqClock {
    long long sent = 0;        /* absolute index of the last pulse fed */
    bool resync = true;        /* next block re-anchors (transport start) */
    long pulses = 0, gaps = 0, dups = 0, jumps = 0;   /* diagnostics */
};
static const long long PPQ_JUMP_TOL = 12;   /* pulses (half a beat): beyond this it's a locate, not jitter */

/* One audio block. Returns true and sets [*from, *to] (inclusive pulse indices) if pulses are due. */
static inline bool ppq_clock_block(PpqClock *c, double ppq, double tempo, double sr, int frames,
                                   long long *from, long long *to, bool *jumped) {
    double end = ppq + frames * (tempo / 60.0) / sr;
    long long first = (long long)std::ceil(ppq * 24.0 - 1e-6);       /* first pulse at/after block start */
    long long last = (long long)std::ceil(end * 24.0 - 1e-6) - 1;    /* last pulse before block end */
    *jumped = false;
    if (c->resync) {
        c->sent = first - 1; c->resync = false;
    } else if (first > c->sent + 1 + PPQ_JUMP_TOL || last < c->sent - PPQ_JUMP_TOL) {
        *jumped = true; c->jumps++;
        c->sent = first - 1;
    }
    if (first > c->sent + 1) c->gaps += first - (c->sent + 1);                       /* hole: caught up now */
    else if (first <= c->sent) c->dups += (last < c->sent ? last : c->sent) - first + 1;   /* overlap: not fed twice */
    if (last <= c->sent) return false;
    *from = c->sent + 1; *to = last;
    c->pulses += last - c->sent;
    c->sent = last;
    return true;
}

#define BUILD_ID "nofeedback-2026-11-08"
#ifdef PLUG_EFFECT
#define PLUG_MODE "effect"
#else
#define PLUG_MODE "instrument"
#endif
static double now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---- per-instance state ----------------------------------------------------- */
struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    bool owner = false;                    /* this instance drives the (global) engine */
    std::atomic<float> cache[NPARAMS];     /* what the host sees */
    std::atomic<float> told[NPARAMS];      /* what the host last heard (-1: nothing yet); a differing cache is pushed */
    volatile int release[NPARAMS] = {0};   /* momentary triggers / popups to report back */
    float open[NPARAMS] = {0};             /* popup "open" flags (popup.h): wrapper-only */
    float sr = 44100.0f;
    bool was_playing = false;
    std::atomic<bool> playing{false};
    PpqClock clk;
    int jump_logs = 0;
    long blocks = 0;                       /* diagnostics, logged every 5 s */
    long no_ti = 0, midi_in = 0;           /* blocks without host time info / MIDI events received */
    long cc_in = 0, cc_used = 0;           /* control CCs seen on the Control In port / applied */
    int32_t block_min = 1 << 30, block_max = 0;
    double t_last_log = 0, last_ppq = 0, last_tempo = 0;
    std::atomic<bool> refresh{false};      /* the worker re-reads every value from the engine */
    uint8_t inq[64][3];
    int in_n = 0;
    char chunk[8192];

    /* step displays (make_skin.py) */
    struct LaneView { int steps = 0, play = -1, enabled = 0, selected = 0; int bits[64] = {0}; };
    std::thread ui_thread;
    std::mutex ui_mu;
    std::condition_variable ui_cv;
    bool stop_ui = false;
#ifndef NO_ALSA
    snd_seq_t *seq = nullptr;
    int port = -1;
    int ctl_port = -1;                     /* "Control In": CCs from hardware controllers */
#endif
};
static Plugin *g_owner = nullptr;          /* guarded by engineMutex */
static int g_cc_ch_idx = -1;               /* index of the wrapper-only "cc_ch" param */

/* ---- MIDI out: everything the engine sends lands here (rtmidi_stub/RtMidi.h) ---- */
#ifndef NO_ALSA
/* Subscribe Control In to one hardware MIDI input (a USB controller, the DIN input). */
static void ctl_subscribe(Plugin *w, int client, int port) {
    if (client == snd_seq_client_id(w->seq) || client == SND_SEQ_CLIENT_SYSTEM) return;
    snd_seq_port_info_t *pi;
    snd_seq_port_info_alloca(&pi);
    if (snd_seq_get_any_port_info(w->seq, client, port, pi) < 0) return;
    unsigned cap = snd_seq_port_info_get_capability(pi), type = snd_seq_port_info_get_type(pi);
    if ((cap & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) return;
    if (cap & SND_SEQ_PORT_CAP_NO_EXPORT) return;
    if (!(type & SND_SEQ_PORT_TYPE_HARDWARE)) return;         /* not other plugins' virtual ports */
    int rc = snd_seq_connect_from(w->seq, w->ctl_port, client, port);
    LOG("[euclidier_vst] control in <- %d:%d '%s'%s\n", client, port, snd_seq_port_info_get_name(pi),
        rc < 0 ? " (already connected or refused)" : "");
}
static void ctl_subscribe_all(Plugin *w) {
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(w->seq, ci) >= 0) {
        int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(w->seq, pi) >= 0) ctl_subscribe(w, c, snd_seq_port_info_get_port(pi));
    }
}
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        w->seq = nullptr; LOG("[euclidier_vst] snd_seq_open failed\n"); return;
    }
    snd_seq_set_client_pool_output(w->seq, 2048);
    snd_seq_set_client_name(w->seq, PLUG_NAME);   /* "Euclidier" / "Euclidier FX" */
    w->port = snd_seq_create_simple_port(w->seq, "MIDI Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    LOG("[euclidier_vst] ALSA client '" PLUG_NAME "' (%d) port %d\n", snd_seq_client_id(w->seq), w->port);
    w->ctl_port = snd_seq_create_simple_port(w->seq, "Control In",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (w->ctl_port < 0) { LOG("[euclidier_vst] no Control In port\n"); return; }
    /* hot-plug: the system announce port tells us about ports that appear later */
    snd_seq_connect_from(w->seq, w->ctl_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    ctl_subscribe_all(w);
}
static void alsa_send(Plugin *w, const std::vector<unsigned char> &m) {
    if (!w || !w->seq || w->port < 0) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, w->port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    int type = m[0] & 0xF0, ch = m[0] & 0x0F;
    int d1 = m.size() > 1 ? m[1] : 0, d2 = m.size() > 2 ? m[2] : 0;
    if (type == 0x90 && d2) snd_seq_ev_set_noteon(&ev, ch, d1, d2);
    else if (type == 0x80 || type == 0x90) snd_seq_ev_set_noteoff(&ev, ch, d1, 0);
    else if (type == 0xB0) snd_seq_ev_set_controller(&ev, ch, d1, d2);
    else return;   /* clock echo etc.: MPC is the clock master, nothing to forward */
    snd_seq_event_output_direct(w->seq, &ev);
}
static void alsa_close(Plugin *w) { if (w->seq) snd_seq_close(w->seq); w->seq = nullptr; }
static void apply_cc(Plugin *w, int ch, int cc, int val);
/* audio thread, engineMutex held: drain Control In (non-blocking) */
static void alsa_poll(Plugin *w) {
    if (!w->seq || w->ctl_port < 0) return;
    snd_seq_event_t *ev = nullptr;
    for (int guard = 0; guard < 256 && snd_seq_event_input_pending(w->seq, 1) > 0; guard++) {
        if (snd_seq_event_input(w->seq, &ev) < 0 || !ev) break;
        if (ev->type == SND_SEQ_EVENT_CONTROLLER) {
            w->cc_in++;
            apply_cc(w, ev->data.control.channel, (int)ev->data.control.param, ev->data.control.value);
        } else if (ev->type == SND_SEQ_EVENT_PORT_START) {
            ctl_subscribe(w, ev->data.addr.client, ev->data.addr.port);
        }
    }
}
#else
static void alsa_poll(Plugin *) {}
/* Offline test build: EUCLIDIER_TRACE=1 prints every outgoing message. */
static void alsa_open(Plugin *) {}
static void alsa_send(Plugin *, const std::vector<unsigned char> &m) {
    static int trace = std::getenv("EUCLIDIER_TRACE") ? 1 : 0;
    if (trace) std::printf("MIDI %02x %02x %02x\n", m[0], m.size() > 1 ? m[1] : 0, m.size() > 2 ? m[2] : 0);
}
static void alsa_close(Plugin *) {}
#endif
static void engine_send(const std::vector<unsigned char> &m) { alsa_send(g_owner, m); }
void (*g_euclidier_send)(const std::vector<unsigned char> &msg) = engine_send;

/* ---- engine init: euclidier.cpp main()'s setup, minus ports/socket/main loop ----- */
static std::string plugin_dir() {
    Dl_info info;
    if (dladdr((void *)&plugin_dir, &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        size_t s = p.rfind('/');
        if (s != std::string::npos) return p.substr(0, s);
    }
    return "/tmp";
}
static void engine_init() {   /* caller holds engineMutex; runs once per process */
    static bool done = false;
    if (done) return;
    done = true;
    bgprocess = true;                              /* no console output inside MPC */
    feedbackOut = false;                           /* no CC feedback on ch 16 into the driven tracks (see euclidier.cpp) */
    srand(time(NULL));
    basePath = plugin_dir() + "/euclidier_" + BANK;   /* the bank next to the .so */
    BOOT_TIME = now();
    midiOut = new RtMidiOut();
    for (int i = 0; i < SEQS; i++) {               /* main()'s lane set-up; set_defaults() below then applies upstream's */
        SQ[i].setPORT(midiOut);
        SQ[i].updateSeq();
    }
    createBANK(basePath);
    LOG("[euclidier_vst] engine up, bank %s\n", basePath.c_str());
}

/* ---- parameter helpers ------------------------------------------------------------ */
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}

/* ---- display-only step-display params (names from make_skin.py) -------------- */
enum VKind : unsigned char { V_NONE = 0, V_GCELL, V_CCELL };
struct VInfo { VKind kind = V_NONE; int lane = 0, cap = 0, slot = 0; };
static VInfo g_v[NPARAMS];
static std::vector<int> g_vlist;                       /* display-only params */
static std::vector<int> g_gcaps, g_ccaps;              /* size-class capacities, ascending */
static std::unordered_map<std::string, int> g_kidx;    /* key -> param index */
static void add_cap(std::vector<int> &v, int c) { if (std::find(v.begin(), v.end(), c) == v.end()) v.push_back(c); }
static void build_vtable() {
    for (int i = 0; i < NPARAMS; i++) {
        const char *k = PARAMS[i].key;
        int a, b, c;
        g_kidx[k] = i;
        VInfo &v = g_v[i];
        if (k[0] == 'g' && std::sscanf(k, "g%d_%d_%d", &a, &b, &c) == 3) { v = {V_GCELL, a - 1, b, c}; add_cap(g_gcaps, b); g_vlist.push_back(i); }
        else if (k[0] == 'c' && k[1] >= '0' && k[1] <= '9' && std::sscanf(k, "c%d_%d", &a, &b) == 2) { v = {V_CCELL, 0, a, b}; add_cap(g_ccaps, a); g_vlist.push_back(i); }
    }
    std::sort(g_gcaps.begin(), g_gcaps.end());
    std::sort(g_ccaps.begin(), g_ccaps.end());
    auto it = g_kidx.find("cc_ch");
    g_cc_ch_idx = it == g_kidx.end() ? -1 : it->second;
}
static int param_index(const char *key) { auto it = g_kidx.find(key); return it == g_kidx.end() ? -1 : it->second; }
static bool is_virtual(int i) { return g_v[i].kind != V_NONE; }
/* Write-only engine actions: triggers, not state -- never saved in a chunk or replayed from one. */
static bool is_trigger_key(const char *key) {
    return !std::strcmp(key, "rand_go") || !std::strcmp(key, "preset_load") || !std::strcmp(key, "preset_save");
}
static bool is_value_param(int i) {
    return !PARAMS[i].momentary && !popup_is(i) && !is_virtual(i) && !is_trigger_key(PARAMS[i].key);
}
static bool is_engine_value(int i) { return is_value_param(i) && i != g_cc_ch_idx; }

static int class_for(const std::vector<int> &caps, int n) {
    for (int c : caps) if (c >= n) return c;
    return caps.empty() ? 0 : caps.back();
}
/* slot[0..cap): 0 hidden / 1 off / 2 on for `n` steps (bits[]) shown in `cap` slots. `contiguous`: a lane row, one
 * cell per step from the left while n <= cap; otherwise the steps are spread round the circle (several steps
 * sharing a slot when n > cap light it if any is on). */
static void fill_slots(const int *bits, int n, int cap, bool contiguous, int *slot) {
    for (int s = 0; s < cap; s++) slot[s] = 0;
    if (n <= 0 || cap <= 0) return;
    if (contiguous && n <= cap) { for (int s = 0; s < n; s++) slot[s] = 1 + (bits[s] ? 1 : 0); return; }
    for (int s = 0; s < cap; s++) slot[s] = 1;
    for (int i = 0; i < n && i < 64; i++) if (bits[i]) slot[(i * cap) / n] = 2;
    if (n < cap) {   /* only the slots a step lands on exist */
        int used[64] = {0};
        for (int i = 0; i < n; i++) used[(i * cap) / n] = 1;
        for (int s = 0; s < cap; s++) if (!used[s]) slot[s] = 0;
    }
}
static int play_slot(int play, int n, int cap, bool contiguous) {
    if (play < 0 || play >= n) return -1;
    return contiguous && n <= cap ? play : (play * cap) / n;
}
/* "l<N>_pattern" -> "steps|bits|play|loop|enabled|selected" (euclidier.cpp ctrlGet) */
static void parse_pattern(const std::string &r, Plugin::LaneView &L) {
    L = Plugin::LaneView();
    size_t p0 = r.find('|');
    if (p0 == std::string::npos) return;
    L.steps = std::atoi(r.c_str());
    if (L.steps > 64) L.steps = 64;
    size_t p1 = r.find('|', p0 + 1);
    std::string bits = r.substr(p0 + 1, p1 == std::string::npos ? std::string::npos : p1 - p0 - 1);
    int k = 0;
    for (size_t i = 0; i < bits.size() && k < 64; i++) if (bits[i] == '0' || bits[i] == '1') L.bits[k++] = bits[i] == '1';
    if (k < L.steps) L.steps = k;
    if (p1 == std::string::npos) return;
    int f[4] = {-1, 0, 0, 0}, nf = 0;   /* play, loop, enabled, selected */
    const char *q = r.c_str() + p1 + 1;
    while (nf < 4 && *q) { f[nf++] = std::atoi(q); const char *b = std::strchr(q, '|'); if (!b) break; q = b + 1; }
    L.play = f[0]; L.enabled = f[2]; L.selected = f[3];
}
static float idx_norm(int idx, int nopts) { return nopts > 1 ? (float)idx / (nopts - 1) : 0.0f; }
/* a cell's value: 0 hidden, 1 off, 2 on, +2 while the play-head is on it (options "-", OFF, ON, OFF >, ON >) */
static void compute_views(Plugin *w, const Plugin::LaneView *lv) {
    bool playing = w->playing.load();
    int sel = 0;
    for (int i = 0; i < 8; i++) if (lv[i].selected) sel = i;
    int gcls[8], gslot[8][64], ccls = class_for(g_ccaps, lv[sel].steps), cslot[64];
    for (int n = 0; n < 8; n++) {
        gcls[n] = class_for(g_gcaps, lv[n].steps);
        fill_slots(lv[n].bits, lv[n].steps, gcls[n], true, gslot[n]);
        int ps = playing && lv[n].enabled ? play_slot(lv[n].play, lv[n].steps, gcls[n], true) : -1;
        if (ps >= 0 && gslot[n][ps] && gcls[n] <= 16) gslot[n][ps] += 2;   /* the 32-step row has no play-head state (make_skin.py GRID_CUR_MAX) */
    }
    fill_slots(lv[sel].bits, lv[sel].steps, ccls, false, cslot);
    int cps = playing && lv[sel].enabled ? play_slot(lv[sel].play, lv[sel].steps, ccls, false) : -1;
    if (cps >= 0 && cslot[cps]) cslot[cps] += 2;
    for (int i : g_vlist) {
        const VInfo &v = g_v[i];
        float val = 0.0f;
        if (v.kind == V_GCELL) val = v.cap == gcls[v.lane] ? idx_norm(gslot[v.lane][v.slot], PARAMS[i].nopts) : 0.0f;
        else if (v.kind == V_CCELL) val = v.cap == ccls ? idx_norm(cslot[v.slot], PARAMS[i].nopts) : 0.0f;
        w->cache[i].store(val);
    }
}

/* Re-read every engine value (the engine clamps, fills the sel_* copies, changes lanes on its own on a preset load,
 * a randomise, a mode switch). One key per lock, so the audio thread never waits long. Not the audio thread. */
static void refresh_cache(Plugin *w) {
    for (int i = 0; i < NPARAMS; i++) {
        if (!is_engine_value(i)) continue;
        std::string r;
        { std::lock_guard<std::mutex> lk(engineMutex); r = ctrlGet(PARAMS[i].key); }
        if (r != "ERR" && !r.empty()) w->cache[i].store(ui_to_norm(&PARAMS[i], std::atof(r.c_str())));
    }
}
/* worker (owner instance only): ~33 Hz pattern poll + re-reads after engine-side changes */
static void ui_worker(Plugin *w) {
    std::unique_lock<std::mutex> lk(w->ui_mu);
    while (!w->stop_ui) {
        w->ui_cv.wait_for(lk, std::chrono::milliseconds(30), [w] { return w->stop_ui; });
        if (w->stop_ui) break;
        lk.unlock();
        if (w->refresh.exchange(false)) refresh_cache(w);
        Plugin::LaneView lv[8];
        for (int n = 0; n < 8; n++) {
            char k[24];
            std::snprintf(k, sizeof k, "l%d_pattern", n + 1);
            std::string r;
            {
                std::lock_guard<std::mutex> elk(engineMutex);
                if (loading != 0 && getUS() > loading) loading = 0;   /* a preset recall / randomise window, also while stopped */
                r = ctrlGet(k);
            }
            parse_pattern(r, lv[n]);
        }
        compute_views(w, lv);
        lk.lock();
    }
}

/* Keep the selected lane's "sel_<x>" and "l<N>_<x>" caches equal (the engine has one value for both). */
static void mirror_sel(Plugin *w, int i, float n) {
    const char *k = PARAMS[i].key;
    int si = param_index("sel");
    if (si < 0) return;
    int sel = (int)std::lround(w->cache[si].load() * 7.0f);
    std::string other;
    if (!std::strncmp(k, "sel_", 4)) other = "l" + std::to_string(sel + 1) + "_" + (k + 4);
    else if (k[0] == 'l' && k[1] >= '1' && k[1] <= '8' && k[2] == '_' && k[1] - '1' == sel) other = std::string("sel_") + (k + 3);
    else return;
    int o = param_index(other.c_str());
    if (o >= 0) w->cache[o].store(n);
}
static int value_of(Plugin *w, int i) { return norm_to_ui(&PARAMS[i], w->cache[i].load()); }
static bool ends_with(const char *s, const char *suf) {
    size_t a = std::strlen(s), b = std::strlen(suf);
    return a >= b && !std::strcmp(s + a - b, suf);
}

/* ---- engine actions (caller holds engineMutex) ------------------------------------- */
/* set one engine key from a value in its own domain, keep the caches in step */
static void set_value(Plugin *w, int i, int v) {
    const param_t *p = &PARAMS[i];
    float n = ui_to_norm(p, v);
    w->cache[i].store(n);
    mirror_sel(w, i, n);
    if (i != g_cc_ch_idx) ctrlSet(p->key, v);
    if (!std::strcmp(p->key, "sel")) {   /* a new lane: its l<N>_* values become the sel_* ones at once */
        std::string lp = "l" + std::to_string(norm_to_ui(p, n) + 1) + "_";
        for (int j = 0; j < NPARAMS; j++) {
            if (std::strncmp(PARAMS[j].key, "sel_", 4) || !is_value_param(j)) continue;
            int o = param_index((lp + (PARAMS[j].key + 4)).c_str());
            if (o >= 0) w->cache[j].store(w->cache[o].load());
        }
    }
    if (!std::strcmp(p->key, "sel") || ends_with(p->key, "_mode")) w->refresh = true;
}
/* randomise: the engine's rand_go acts on the rand_l<N> flags (none set = all): set the flags, fire, clear them */
static void randomise(Plugin *w, const bool *lanes) {
    for (int l = 0; l < 8; l++) ctrlSet("rand_l" + std::to_string(l + 1), lanes && lanes[l] ? 1 : 0);
    ctrlSet("rand_go", 1);
    for (int l = 0; l < 8; l++) ctrlSet("rand_l" + std::to_string(l + 1), 0);
    w->refresh = true;
}
static int selected_lane(Plugin *w) {   /* 0..7 */
    int si = param_index("sel");
    return si < 0 ? 0 : (int)std::lround(w->cache[si].load() * 7.0f);
}
static void all_drum(Plugin *w) {   /* every lane to DRUM mode; the engine then moves each note to its drum note */
    for (int l = 1; l <= 8; l++) {
        int i = param_index(("l" + std::to_string(l) + "_mode").c_str());
        if (i >= 0) set_value(w, i, 1);
    }
}
/* A fresh instance (upstream's defaults): every lane in DRUM mode (each with its own GM drum note), lanes 1-4 on with
 * four standard Euclidean patterns, lanes 5-8 off. A saved project's chunk (effSetChunk) comes after and overrides it. */
static void set_defaults(Plugin *w) {
    static const int pat[4][2] = {{16, 4}, {8, 3}, {12, 5}, {16, 5}};   /* four on the floor, tresillo, E(5,12), bossa-ish */
    auto put = [&](const std::string &key, int val) { int i = param_index(key.c_str()); if (i >= 0) set_value(w, i, val); };
    put("sel", 0);
    for (int l = 1; l <= 8; l++) {
        std::string b = "l" + std::to_string(l) + "_";
        put(b + "mode", 1);
        put(b + "enable", l <= 4 ? 1 : 0);
        if (l <= 4) { put(b + "steps", pat[l - 1][0]); put(b + "fill", pat[l - 1][1]); put(b + "shift", 0); }
    }
    processQ();
    w->refresh = true;
}

/* ---- control by MIDI CC (see header; caller holds engineMutex, ch 0..15) ------------- */
static bool cc_to_key(int cc, char *key, size_t max) {
    static const char *const LANE[10] = {"enable", "steps", "fill", "shift", "loop", "gate", "vel", "velh", "note", "div"};
    static const char *const GLOB[5] = {"preset", "preset_load", "preset_save", "rand_lane", "rand_go"};
    if (cc >= 10 && cc <= 89) { std::snprintf(key, max, "l%d_%s", cc / 10, LANE[cc % 10]); return true; }
    if (cc >= 100 && cc <= 104) { std::snprintf(key, max, "%s", GLOB[cc - 100]); return true; }
    return false;
}
static void apply_cc(Plugin *w, int ch, int cc, int val) {
    if (!w->owner || g_cc_ch_idx < 0) return;
    int sel = value_of(w, g_cc_ch_idx);   /* 0 = OFF */
    if (sel == 0 || ch != sel - 1) return;
    char key[24];
    if (!cc_to_key(cc, key, sizeof key)) return;
    int i = param_index(key);
    if (i < 0) return;
    const param_t *p = &PARAMS[i];
    if (val < 0) val = 0;
    if (val > 127) val = 127;
    w->cc_used++;
    if (!std::strcmp(key, "rand_go")) {   /* CC 104: the lanes CC 103 ("Rand Lane") picked */
        if (val < 64) return;
        int rl = param_index("rand_lane"), pick = rl >= 0 ? value_of(w, rl) : 0;   /* ALL, LANE 1..8, ALL BUT 1, ALL BUT 5 */
        bool lanes[8];
        for (int l = 0; l < 8; l++) lanes[l] = pick == 0 || (pick <= 8 && l == pick - 1) || (pick == 9 && l != 0) || (pick == 10 && l != 4);
        randomise(w, lanes);
        return;
    }
    if (p->momentary || is_trigger_key(key)) {
        if (val >= 64) { ctrlSet(p->key, 1); w->refresh = true; }
        return;
    }
    int ui = norm_to_ui(p, val / 127.0f);
    set_value(w, i, ui);
    w->refresh = true;
}
static void feed(unsigned char b0) { std::vector<unsigned char> m = {b0}; handleMidi(&m); }

/* ---- audio callback: transport -> clock, queue, note-offs, host updates ---------------- */
static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
#ifdef PLUG_EFFECT
    /* insert effect: the track's audio goes through untouched (the host may hand us in == out) */
    for (int ch = 0; ch < 2; ch++) {
        if (in && in[ch]) { if (out[ch] != in[ch]) std::memmove(out[ch], in[ch], (size_t)n * sizeof(float)); }
        else std::memset(out[ch], 0, (size_t)n * sizeof(float));
    }
#else
    (void)in;
    for (int32_t i = 0; i < n; i++) out[0][i] = out[1][i] = 0.0f;   /* MIDI generator: no audio */
#endif
    if (!w->owner) return;
    VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0,
                                                kVstTempoValid | kVstPpqPosValid, 0, 0);
    if (!ti) w->no_ti++;
    bool playing = ti && (ti->flags & kVstTransportPlaying);
    w->playing.store(playing);
    double tempo = (ti && (ti->flags & kVstTempoValid) && ti->tempo > 20.0 && ti->tempo < 400.0) ? ti->tempo : 0.0;
    {
        std::lock_guard<std::mutex> lk(engineMutex);
        for (int i = 0; i < w->in_n; i++) {        /* notes into the track: the engine's own transpose */
            uint8_t t = w->inq[i][0] & 0xF0;
            if (t == 0x90 || t == 0x80) { std::vector<unsigned char> m(w->inq[i], w->inq[i] + 3); handleMidi(&m); }
            else if (t == 0xB0) apply_cc(w, w->inq[i][0] & 0x0F, w->inq[i][1], w->inq[i][2]);   /* CCs sent to the track */
        }
        w->in_n = 0;
        alsa_poll(w);                               /* CCs from hardware controllers (Control In) */

        /* the host tempo drives gate length (the engine's own estimate from pulse spacing is meaningless here) */
        if (tempo > 0) { hostBPM.store((float)tempo); BPM = (float)tempo; updateBPM(BPM); }
        if (playing && !w->was_playing) { feed(0xFA); w->clk.resync = true; }
        else if (!playing && w->was_playing) feed(0xFC);
        w->was_playing = playing;

        if (playing && (ti->flags & kVstPpqPosValid) && ti->tempo > 0) {
            /* Pulses by absolute index (PpqClock above). The engine's step position is a pure function of its tick
             * count, so tick is SET to the host's pulse index before every pulse: the lanes sit on the MPC grid by
             * construction, also after a loop, a locate, a restart or a tempo change. */
            double sr = (ti->sampleRate > 0) ? ti->sampleRate : (double)w->sr;
            long long from = 0, to = -1, before = w->clk.sent;
            bool jumped = false;
            bool due = ppq_clock_block(&w->clk, ti->ppqPos, ti->tempo, sr, n, &from, &to, &jumped);
            if (jumped && w->jump_logs < 20) {
                w->jump_logs++;
                LOG("[euclidier_vst] ppq jump: last pulse %lld -> ppq %.4f (pulse %.2f), bpm %.2f, block %d\n",
                    before, ti->ppqPos, ti->ppqPos * 24.0, ti->tempo, n);
            }
            for (long long idx = from; due && idx <= to; idx++) {
                if (idx < 0) continue;            /* pre-roll before bar 1 */
                tick = idx;
                feed(0xF8);
                if (tempo > 0) { BPM = (float)tempo; updateBPM(BPM); }
            }
            w->last_ppq = ti->ppqPos; w->last_tempo = ti->tempo;
        }

        long long us = getUS();
        if (loading != 0 && us > loading) loading = 0;
        processQ();                                 /* the standalone only runs this while started; here edits apply at once */
        for (int i = 0; i < SEQS; i++) SQ[i].clock(us);   /* note-offs: also after Stop, so none is left hanging (the
                                                               standalone's loop only ran this while started) */
    }
    w->blocks++;
    if (n < w->block_min) w->block_min = n;
    if (n > w->block_max) w->block_max = n;
    double t1 = now_ms();
    if (t1 - w->t_last_log > 5000.0) {
        if (w->t_last_log > 0)
            LOG("[euclidier_vst] %ld blocks, size %d..%d, playing %d, ppq %.3f, bpm %.2f"
                " | clock: %ld pulses, %ld caught up, %ld overlapped, %ld jumps"
                " | no timeinfo %ld, midi in %ld | control cc %ld seen, %ld used\n",
                w->blocks, w->block_min, w->block_max, (int)w->was_playing, w->last_ppq, w->last_tempo,
                w->clk.pulses, w->clk.gaps, w->clk.dups, w->clk.jumps, w->no_ti, w->midi_in, w->cc_in, w->cc_used);
        w->no_ti = w->midi_in = w->cc_in = w->cc_used = 0;
        w->clk.pulses = w->clk.gaps = w->clk.dups = w->clk.jumps = 0;
        w->jump_logs = 0;
        w->t_last_log = t1; w->blocks = 0; w->block_min = 1 << 30; w->block_max = 0;
    }
    /* tell the host what changed behind its back: display cells first, then real params (upstream's scheme) */
    int budget = 96;   /* host calls per block: a lane resize touches ~100 cells, spread over a few blocks */
    for (int i : g_vlist) {
        if (budget <= 0) break;
        float v = w->cache[i].load();
        if (v != w->told[i].load()) { w->told[i].store(v); w->master(&w->fx, audioMasterAutomate, i, 0, 0, v); budget--; }
    }
    for (int i = 0; i < NPARAMS && budget > 0; i++) {
        if (!is_value_param(i)) continue;
        float v = w->cache[i].load();
        if (v != w->told[i].load()) { w->told[i].store(v); w->master(&w->fx, audioMasterAutomate, i, 0, 0, v); budget--; }
    }
    for (int i = 0; i < NPARAMS; i++)
        if (w->release[i]) { w->release[i] = 0; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
}

/* ---- parameters ------------------------------------------------------------------ */
static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (is_virtual(i)) return;   /* display only */
    if (popup_set(w->open, i, n)) return;
    if (!w->owner) { if (is_value_param(i)) w->cache[i].store(clamp01(n)); return; }
    std::lock_guard<std::mutex> lk(engineMutex);
    if (p->momentary && p->step_target >= 0) {   /* a stepper arrow (LANE, DIV): nudge another param by step_delta */
        if (n > 0.5f) {
            const param_t *t = &PARAMS[p->step_target];
            int lo = t->nopts ? 0 : (int)t->min, hi = t->nopts ? t->nopts - 1 : (int)t->max;
            set_value(w, p->step_target, std::min(hi, std::max(lo, value_of(w, p->step_target) + (int)p->step_delta)));
            processQ();
            w->release[i] = 1;
        }
        return;
    }
    if (p->momentary && !std::strncmp(p->key, "rand_", 5)) {   /* rand_all, rand_sel (the lane on LANES), rand_1..rand_8 */
        if (n > 0.5f) {
            bool lanes[8] = {false};
            if (!std::strcmp(p->key, "rand_sel")) lanes[selected_lane(w)] = true;
            else if (p->key[5] >= '1' && p->key[5] <= '8' && !p->key[6]) lanes[p->key[5] - '1'] = true;
            else if (std::strcmp(p->key, "rand_all")) return;
            randomise(w, !std::strcmp(p->key, "rand_all") ? nullptr : lanes);
            w->release[i] = 1;
        }
        return;
    }
    if (p->momentary && !std::strcmp(p->key, "all_drum")) {
        if (n > 0.5f) { all_drum(w); processQ(); w->release[i] = 1; }
        return;
    }
    if (p->momentary || is_trigger_key(p->key)) {   /* preset load/save, rand_go */
        if (n > 0.5f) {
            ctrlSet(p->key, 1);
            w->refresh = true;
            w->release[i] = 1;
        }
        return;
    }
    const float raw = n;   /* what the host holds; if a nudge snaps it to an option, the snapped value is pushed back */
    bool nudge = false;
    if (p->nopts > 1) {   /* a Q-Link nudge lands between options -> step one option */
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            int idx = (int)std::lround(cur) + (pos > cur ? 1 : -1);
            if (idx < 0) idx = 0;
            if (idx > p->nopts - 1) idx = p->nopts - 1;
            n = (float)idx / (p->nopts - 1);
            nudge = true;
        }
    }
    w->told[i].store(raw);
    set_value(w, i, norm_to_ui(p, n));
    processQ();
    if (!nudge) popup_picked(w->open, w->release, i);   /* a list pick closes it; a Q-Link nudge doesn't */
}

static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    return PARAMS[i].momentary ? 0.0f : w->cache[i].load();
}

/* ---- project chunk: "key=value;" for every value param ------------------------- */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string s = "v=3;";
    char buf[96];
    for (int i = 0; i < NPARAMS; i++) {
        if (!is_value_param(i) || !std::strncmp(PARAMS[i].key, "sel_", 4)) continue;   /* sel_* only mirror l<N>_* */
        std::snprintf(buf, sizeof buf, "%s=%d;", PARAMS[i].key, value_of(w, i));
        s += buf;
    }
    copy_str(w->chunk, s.c_str(), sizeof w->chunk);
    *ptr = w->chunk;
    return (intptr_t)std::strlen(w->chunk) + 1;
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    if (len <= 0 || (size_t)len > sizeof w->chunk) return 0;
    std::memcpy(w->chunk, data, (size_t)len);
    w->chunk[len - 1] = 0;
    /* The engine's SETs are not independent: `sel` picks the lane the sel_* keys write to, and a lane's `mode` makes the
     * engine pick that mode's note, which would overwrite a note set before it: so sel, then modes, then the rest. */
    std::vector<std::pair<int, int>> kv;   /* param index, value */
    char *save = nullptr;
    for (char *tok = strtok_r(w->chunk, ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
        char *eq = std::strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        if (is_trigger_key(tok)) continue;            /* older chunks hold rand_go=0;preset_load=0;preset_save=0; */
        if (!std::strncmp(tok, "sel_", 4)) continue;  /* mirrors of the selected lane (older chunks saved them) */
        int i = param_index(tok);
        if (i < 0 || !is_value_param(i)) continue;
        kv.push_back({i, std::atoi(eq + 1)});
    }
    auto prio = [](int i) { const char *k = PARAMS[i].key; return !std::strcmp(k, "sel") ? 0 : ends_with(k, "_mode") ? 1 : 2; };
    std::stable_sort(kv.begin(), kv.end(), [&](const auto &a, const auto &b) { return prio(a.first) < prio(b.first); });
    std::lock_guard<std::mutex> lk(engineMutex);
    for (int pass = 0; pass < 3; pass++) {
        for (auto &e : kv) {
            if (prio(e.first) != pass) continue;
            if (w->owner) set_value(w, e.first, e.second);
            else w->cache[e.first].store(ui_to_norm(&PARAMS[e.first], e.second));
        }
        if (w->owner) processQ();   /* modes apply (and pick their notes) before the notes are set */
    }
    if (w->owner) w->refresh = true;
    return 1;
}

/* ---- dispatcher ---------------------------------------------------------------- */
static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        if (w->ui_thread.joinable()) {
            { std::lock_guard<std::mutex> lk(w->ui_mu); w->stop_ui = true; }
            w->ui_cv.notify_one();
            w->ui_thread.join();
        }
        {
            std::lock_guard<std::mutex> lk(engineMutex);
            if (w->owner) {
                if (started) clockStop();
                for (int ch = 0; ch < 16; ch++) {           /* All Notes Off everywhere */
                    std::vector<unsigned char> m = {(unsigned char)(0xB0 | ch), 123, 0};
                    alsa_send(w, m);
                }
                g_owner = nullptr;
            }
        }
        alsa_close(w);
        LOG("[euclidier_vst] closed (owner=%d)\n", (int)w->owner);
        delete w;
        return 1;
#ifdef PLUG_EFFECT
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
#else
    case effGetPlugCategory: return 2;   /* kPlugCategSynth */
#endif
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && !is_virtual(idx);
    case effGetParamName:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32);
        return 1;
    case effGetParamLabel:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8);
        return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        const param_t *pp = &PARAMS[idx];
        if (pp->momentary) { copy_str(p, "", 24); return 1; }
        float nv = popup_is(idx) ? w->open[idx] : w->cache[idx].load();
        int ui = norm_to_ui(pp, nv);
        if (pp->nopts) copy_str(p, pp->opts[ui], 24);
        else { char buf[24]; std::snprintf(buf, sizeof buf, "%d", ui); copy_str(p, buf, 24); }
        return 1;
    }
    case effSetSampleRate: if (o > 0) w->sr = o; return 1;
    case effSetBlockSize: case effMainsChanged: return 1;
    case effProcessEvents: {
        VstEvents *ev = (VstEvents *)p;
        std::lock_guard<std::mutex> lk(engineMutex);
        for (int i = 0; ev && i < ev->numEvents && w->in_n < 64; i++) {
            if (ev->events[i]->type != 1) continue;
            w->midi_in++;
            std::memcpy(w->inq[w->in_n++], ((VstMidiEvent *)ev->events[i])->midiData, 3);
        }
        return 1;
    }
    case effCanDo: {
        const char *s = (const char *)p;
        return (!std::strcmp(s, "receiveVstTimeInfo") || !std::strcmp(s, "receiveVstEvents") ||
                !std::strcmp(s, "receiveVstMidiEvent")) ? 1 : -1;
    }
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!g_log) g_log = std::fopen("/tmp/euclidier_vst.log", "a");
    static std::once_flag once;
    std::call_once(once, build_vtable);
    Plugin *w = new Plugin();
    w->master = master;
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->told[i].store(-1.0f); }
    if (g_cc_ch_idx >= 0) w->cache[g_cc_ch_idx].store(1.0f);   /* default: channel 16 */
    {
        std::lock_guard<std::mutex> lk(engineMutex);
        engine_init();
        if (!g_owner) { g_owner = w; w->owner = true; }
        if (w->owner) set_defaults(w);
    }
    if (w->owner) {
        refresh_cache(w);
        w->refresh = false;
        alsa_open(w);
        w->ui_thread = std::thread(ui_worker, w);
    } else LOG("[euclidier_vst] another " PLUG_NAME " instance already runs the engine: this one stays silent\n");
    for (int i = 0; i < NPARAMS; i++) if (!is_virtual(i)) w->told[i].store(w->cache[i].load());   /* the host reads these itself at load */

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
#ifdef PLUG_EFFECT
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
#else
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
#endif
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[euclidier_vst] " PLUG_NAME " up (" BUILD_ID ", " PLUG_MODE "), %d params, owner=%d\n", NPARAMS, (int)w->owner);
    return e;
}

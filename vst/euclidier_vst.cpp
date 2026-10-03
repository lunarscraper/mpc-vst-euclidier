/* =============================================================================
 * euclidier_vst.cpp - Euclidier as a VST2 MIDI generator for the MPC OS plugin
 * host (https://github.com/sd88me/mpc-vst-plugins), engine IN-PROCESS.
 *
 * The first version of this port spawned the standalone `euclidier` binary from a
 * hard-coded AddOns path (/media/662522/..., the developer's own SD card) and fed it a
 * clock over ALSA. On any other device that path doesn't exist, so the engine never
 * started and no notes came out. This version works like the Acid VST instead: the
 * vendored engine (../src, see ../src/VENDORED.md) is compiled straight into the .so,
 * unmodified, and this file plays the part of its main():
 *
 *   euclidier.cpp standalone               euclidier_vst.cpp (MPC plugin)
 *   ------------------------------------   -----------------------------------------
 *   main(): port setup, lane defaults,     engine_init(): same lane defaults, same
 *   BANK.bin next to the binary             createBANK(), BANK file next to the .so
 *   RtMidiIn virtual port, Force clock      audioMasterGetTime -> synthesised 24-PPQN
 *   -> onMIDI() -> handleMidi()             0xF8/0xFA/0xFC -> the same handleMidi()
 *   main loop: processQ(), SQ[i].clock()    processReplacing(): the same calls per block
 *   RtMidiOut virtual port "Euclidier"      rtmidi_stub/RtMidi.h -> this file ->
 *                                            own ALSA seq port "Euclidier" (MPC OS
 *                                            ignores VST MIDI out, docs/NOTES.md)
 *   control socket GET/SET (shadow GUI)     ctrlGet()/ctrlSet() called directly from
 *                                            the VST parameter callbacks
 *
 * No child process, no socket, no AddOns path. The engine keeps its state in globals
 * (it was written as a one-instance app), so ONE instance per project drives it; a
 * second instance loads but stays silent (logged to /tmp/euclidier_vst.log).
 * Every engine call happens under the engine's own engineMutex.
 * ========================================================================== */
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <dlfcn.h>

#ifndef NO_ALSA
#include <alsa/asoundlib.h>
#endif

/* ---- the engine, compiled into this translation unit --------------------------------
 * build.sh copies ../src/euclidier.cpp etc. to build/eng/ next to rtmidi_stub/RtMidi.h.
 * Its main() is renamed and never called; the globals and the static ctrlGet()/ctrlSet()
 * become directly usable here. Warnings off: it's vendored, unmodified code. */
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
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };

static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

/* ---------------------------------------------------------------------------
 * PpqClock: host ppqPos -> 24-PPQN pulses by ABSOLUTE pulse index.
 *
 * The first version fed "every pulse inside this block's own window
 * [ppqPos, ppqPos + block length)". That is only exact-once if every callback's
 * ppqPos advances by precisely frames*tempo/60/sr. When it doesn't (host splits
 * or repeats a block, reports ppqPos late, re-derives it on a tempo change,
 * restarts without dropping the playing flag) windows overlap or leave holes:
 * pulses get fed twice or never, and since both engines COUNT pulses, every
 * such pulse shifts the pattern against the MPC grid for good -> audible as an
 * unstable tempo.
 *
 * Here pulse N simply IS ppq N/24. We remember the last index fed and feed
 * (sent, last-in-this-block]: an overlap feeds nothing twice, a small hole is
 * caught up at once, and a real jump (loop, locate, restart) is detected and
 * re-anchored instead of silently drifting.
 * ------------------------------------------------------------------------- */
struct PpqClock {
    long long sent = 0;        /* absolute index of the last pulse fed */
    bool resync = true;        /* next block re-anchors (transport start) */
    bool wait = false;         /* phase_mod only: holding until the old phase comes round again */
    int phase = 0;
    long pulses = 0, gaps = 0, dups = 0, jumps = 0;   /* diagnostics */
};
static const long long PPQ_JUMP_TOL = 12;   /* pulses (half a beat): beyond this it's a locate, not jitter */
static inline long long ppq_mod(long long a, long long m) { long long r = a % m; return r < 0 ? r + m : r; }

/* One audio block. Returns true and sets [*from, *to] (inclusive pulse indices) if pulses are due.
 * phase_mod 0: caller can place the engine at any index (Euclidier sets its tick = index).
 * phase_mod N: the engine only counts pulses (Acid, 6 per step, 12 per swing pair); after a jump
 *              that isn't a multiple of N, hold until the index is back in the old phase, so the
 *              pattern carries on ON the MPC grid instead of beside it. */
static inline bool ppq_clock_block(PpqClock *c, double ppq, double tempo, double sr, int frames,
                                   int phase_mod, long long *from, long long *to, bool *jumped) {
    double end = ppq + frames * (tempo / 60.0) / sr;
    long long first = (long long)std::ceil(ppq * 24.0 - 1e-6);       /* first pulse at/after block start */
    long long last = (long long)std::ceil(end * 24.0 - 1e-6) - 1;    /* last pulse before block end */
    *jumped = false;
    if (c->resync) {
        c->sent = first - 1; c->resync = false; c->wait = false;
    } else if (!c->wait && (first > c->sent + 1 + PPQ_JUMP_TOL || last < c->sent - PPQ_JUMP_TOL)) {
        *jumped = true; c->jumps++;
        if (phase_mod > 0 && ppq_mod(first - (c->sent + 1), phase_mod) != 0) {
            c->wait = true; c->phase = (int)ppq_mod(c->sent + 1, phase_mod);
        } else c->sent = first - 1;
    }
    if (c->wait) {
        long long k = first + ppq_mod(c->phase - first, phase_mod);   /* next index in the old phase */
        if (k > last) return false;
        c->sent = k - 1; c->wait = false;
    }
    if (first > c->sent + 1) c->gaps += first - (c->sent + 1);                       /* hole: caught up now */
    else if (first <= c->sent) c->dups += (last < c->sent ? last : c->sent) - first + 1;   /* overlap: not fed twice */
    if (last <= c->sent) return false;
    *from = c->sent + 1; *to = last;
    c->pulses += last - c->sent;
    c->sent = last;
    return true;
}

#define BUILD_ID "clockfix-2026-10-03"
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
    volatile int release[NPARAMS] = {0};   /* momentary triggers / popups to report back */
    float open[NPARAMS] = {0};             /* popup "open" flags (popup.h): wrapper-only */
    float sr = 44100.0f;
    bool was_playing = false;
    PpqClock clk;
    int jump_logs = 0;
    long blocks = 0;                       /* diagnostics, logged every 5 s */
    int32_t block_min = 1 << 30, block_max = 0;
    double t_last_log = 0, last_ppq = 0, last_tempo = 0;
    bool refresh = false;                  /* re-read every value after the next processQ() */
    uint8_t inq[64][3];
    int in_n = 0;
    char chunk[8192];
#ifndef NO_ALSA
    snd_seq_t *seq = nullptr;
    int port = -1;
#endif
};
static Plugin *g_owner = nullptr;          /* guarded by engineMutex */

/* ---- MIDI out: everything the engine sends lands here (rtmidi_stub/RtMidi.h) ---- */
#ifndef NO_ALSA
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0) {
        w->seq = nullptr; LOG("[euclidier_vst] snd_seq_open failed\n"); return;
    }
    snd_seq_set_client_pool_output(w->seq, 2048);
    snd_seq_set_client_name(w->seq, "Euclidier");
    w->port = snd_seq_create_simple_port(w->seq, "MIDI Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    LOG("[euclidier_vst] ALSA client 'Euclidier' (%d) port %d\n", snd_seq_client_id(w->seq), w->port);
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
#else
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
    srand(time(NULL));
    basePath = plugin_dir() + "/euclidier_" + BANK;   /* e.g. /sdcard/vst/euclidier_BANK.bin */
    BOOT_TIME = now();
    midiOut = new RtMidiOut();
    for (int i = 0; i < SEQS; i++) {               /* verbatim lane defaults from main() */
        SQ[i].setPORT(midiOut);
        if (i > 3) { SQ[i].ch = 10; SQ[i].mode = 2; }
        if (i == 4) SQ[i].note = 36;
        if (i == 5) { SQ[i].note = 41; SQ[i].pulses = 2; SQ[i].shift = 4; }
        if (i == 6) { SQ[i].note = 38; SQ[i].steps = 4; SQ[i].pulses = 4; }
        if (i == 7) { SQ[i].note = 39; SQ[i].pulses = 4; SQ[i].shift = 2; }
        SQ[i].updateSeq();
    }
    SQ[0].ENABLE(true);
    createBANK(basePath);
    LOG("[euclidier_vst] engine up, bank %s\n", basePath.c_str());
}

/* ---- parameter helpers (caller holds engineMutex) --------------------------------- */
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
static bool is_value_param(int i) { return !PARAMS[i].momentary && !popup_is(i); }
static void refresh_cache(Plugin *w) {
    for (int i = 0; i < NPARAMS; i++) {
        if (!is_value_param(i)) continue;
        std::string r = ctrlGet(PARAMS[i].key);
        if (r != "ERR" && !r.empty()) w->cache[i].store(ui_to_norm(&PARAMS[i], std::atof(r.c_str())));
    }
}
static void feed(unsigned char b0) { std::vector<unsigned char> m = {b0}; handleMidi(&m); }

/* ---- audio callback: transport -> clock, queue, note-offs ------------------------- */
static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    (void)in;
    Plugin *w = (Plugin *)e->object;
    for (int32_t i = 0; i < n; i++) out[0][i] = out[1][i] = 0.0f;   /* MIDI generator: no audio */
    if (!w->owner) return;
    VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0,
                                                kVstTempoValid | kVstPpqPosValid, 0, 0);
    bool playing = ti && (ti->flags & kVstTransportPlaying);
    double tempo = (ti && (ti->flags & kVstTempoValid) && ti->tempo > 20.0 && ti->tempo < 400.0) ? ti->tempo : 0.0;
    bool update_display = false;
    {
        std::lock_guard<std::mutex> lk(engineMutex);
        for (int i = 0; i < w->in_n; i++) {        /* notes into the track: the engine's own transpose */
            uint8_t t = w->inq[i][0] & 0xF0;
            if (t == 0x90 || t == 0x80) { std::vector<unsigned char> m(w->inq[i], w->inq[i] + 3); handleMidi(&m); }
        }
        w->in_n = 0;

        if (playing && !w->was_playing) { feed(0xFA); w->clk.resync = true; }
        else if (!playing && w->was_playing) feed(0xFC);
        w->was_playing = playing;

        if (playing && (ti->flags & kVstPpqPosValid) && ti->tempo > 0) {
            /* Pulses by absolute index (PpqClock above). The engine's step position is a pure
             * function of its tick count, so tick is SET to the host's pulse index before every
             * pulse: the lanes sit on the MPC grid by construction, also after a loop, a locate,
             * a restart or a tempo change -- nothing left to drift. */
            double sr = (ti->sampleRate > 0) ? ti->sampleRate : (double)w->sr;
            long long from = 0, to = -1, before = w->clk.sent;
            bool jumped = false;
            bool due = ppq_clock_block(&w->clk, ti->ppqPos, ti->tempo, sr, n, 0, &from, &to, &jumped);
            if (jumped && w->jump_logs < 20) {
                w->jump_logs++;
                LOG("[euclidier_vst] ppq jump: last pulse %lld -> ppq %.4f (pulse %.2f), bpm %.2f, block %d\n",
                    before, ti->ppqPos, ti->ppqPos * 24.0, ti->tempo, n);
            }
            /* the engine estimates BPM from pulse spacing; block-quantised pulses make that
             * meaningless, so the host tempo overrides it (drives gate length) */
            if (tempo > 0) { BPM = (float)tempo; updateBPM(BPM); }
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
        processQ();                                 /* the standalone only runs this while
                                                       started; here edits apply at once */
        if (started)
            for (int i = 0; i < SEQS; i++) SQ[i].clock(us);   /* note-offs */

        if (w->refresh) { refresh_cache(w); w->refresh = false; update_display = true; }
    }
    w->blocks++;
    if (n < w->block_min) w->block_min = n;
    if (n > w->block_max) w->block_max = n;
    double t1 = now_ms();
    if (t1 - w->t_last_log > 5000.0) {
        if (w->t_last_log > 0)
            LOG("[euclidier_vst] %ld blocks, size %d..%d, playing %d, ppq %.3f, bpm %.2f"
                " | clock: %ld pulses, %ld caught up, %ld overlapped, %ld jumps\n",
                w->blocks, w->block_min, w->block_max, (int)w->was_playing, w->last_ppq, w->last_tempo,
                w->clk.pulses, w->clk.gaps, w->clk.dups, w->clk.jumps);
        w->clk.pulses = w->clk.gaps = w->clk.dups = w->clk.jumps = 0;
        w->jump_logs = 0;
        w->t_last_log = t1; w->blocks = 0; w->block_min = 1 << 30; w->block_max = 0;
    }
    for (int i = 0; i < NPARAMS; i++)
        if (w->release[i]) { w->release[i] = 0; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
    if (update_display) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

/* ---- parameters ------------------------------------------------------------------ */
static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    if (!w->owner) { if (!p->momentary) w->cache[i].store(clamp01(n)); return; }
    std::lock_guard<std::mutex> lk(engineMutex);
    if (p->momentary) {
        if (n > 0.5f) { ctrlSet(p->key, 1); w->release[i] = 1; w->refresh = true; }
        return;
    }
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
    w->cache[i].store(clamp01(n));
    ctrlSet(p->key, norm_to_ui(p, n));
    w->refresh = true;
    if (!nudge) popup_picked(w->open, w->release, i);
}

static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    return PARAMS[i].momentary ? 0.0f : w->cache[i].load();
}

/* ---- project chunk: "key=value;" for every value param ------------------------- */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string s = "v=2;";
    char buf[96];
    for (int i = 0; i < NPARAMS; i++) {
        if (!is_value_param(i)) continue;
        std::snprintf(buf, sizeof buf, "%s=%d;", PARAMS[i].key, norm_to_ui(&PARAMS[i], w->cache[i].load()));
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
    std::lock_guard<std::mutex> lk(engineMutex);
    char *save = nullptr;
    for (char *tok = strtok_r(w->chunk, ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
        char *eq = std::strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        for (int i = 0; i < NPARAMS; i++) {
            if (!is_value_param(i) || std::strcmp(PARAMS[i].key, tok)) continue;
            int v = std::atoi(eq + 1);
            w->cache[i].store(ui_to_norm(&PARAMS[i], v));
            if (w->owner) ctrlSet(PARAMS[i].key, v);
            break;
        }
    }
    if (w->owner) { processQ(); refresh_cache(w); }
    return 1;
}

/* ---- dispatcher ---------------------------------------------------------------- */
static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose:
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
    case effGetPlugCategory: return 2;
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS;
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
    Plugin *w = new Plugin();
    w->master = master;
    for (int i = 0; i < NPARAMS; i++) w->cache[i].store(PARAMS[i].def);
    {
        std::lock_guard<std::mutex> lk(engineMutex);
        engine_init();
        if (!g_owner) { g_owner = w; w->owner = true; }
        if (w->owner) refresh_cache(w);
    }
    if (w->owner) alsa_open(w);
    else LOG("[euclidier_vst] another Euclidier instance already runs the engine: this one stays silent\n");

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[euclidier_vst] up (" BUILD_ID "), %d params, owner=%d\n", NPARAMS, (int)w->owner);
    return e;
}

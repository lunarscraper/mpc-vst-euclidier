/* Offline x86 test of euclidier_vst.cpp with the engine linked in-process (built -DNO_ALSA by test.sh).
 * The fork's checks (audio path of both variants, notes from a synthesised transport, no hanging notes after stop,
 * a second instance stays silent, engine preset save/load, MIDI CC control on the Control Ch, chunk restore) plus
 * upstream's design checks (fresh-instance drum defaults, step-display cells and play-head, lane stepper, randomise
 * one lane / the selected lane / all, DRUM MODE ALL, no triggers in or replayed from a chunk). PASSED/FAILED.
 * The effect variant (Euclidier FX, numInputs == 2) gets an input signal and must pass it through bit-exact,
 * separate buffers and in-place. */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <map>
#include <dlfcn.h>
#include <unistd.h>

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
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4];
                 char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; void *events[2]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

static VstTimeInfo ti;
static std::map<int, int> automated;   /* audioMasterAutomate calls per param index */
static intptr_t master(AEffect *, int32_t op, int32_t idx, intptr_t, void *, float) {
    if (op == 0) automated[idx]++;
    return op == 7 ? (intptr_t)&ti : 0;
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } else { std::printf("ok:   " __VA_ARGS__); std::printf("\n"); } } while (0)

static int nparams(AEffect *e) { return e->numParams; }
/* by key: effGetParamName gives names, so find keys through their (unique) names or name+nth */
static int find(AEffect *e, const char *name, int nth = 0) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name) && nth-- == 0) return i; }
    return -1;
}
static std::string pname(AEffect *e, int i) { char b[64] = {0}; e->dispatcher(e, 8, i, 0, b, 0); return b; }
static std::string disp(AEffect *e, int i) { char b[64] = {0}; if (i >= 0) e->dispatcher(e, 7, i, 0, b, 0); return b; }
static void setv(AEffect *e, int i, int v, int lo, int hi) { e->setParameter(e, i, (float)(v - lo) / (hi - lo)); }

static float g_l[128], g_r[128], g_il[128], g_ir[128];
static void blocks(AEffect *e, int n, bool playing, bool pace = true) {
    float *o[2] = {g_l, g_r}, *iv[2] = {g_il, g_ir};
    ti.flags = (playing ? (1 << 1) : 0) | (1 << 9) | (1 << 10); ti.tempo = 120;
    for (int b = 0; b < n; b++) {
        e->processReplacing(e, e->numInputs ? iv : nullptr, o, 128);
        if (playing) ti.ppqPos += 128 * (120.0 / 60.0) / 44100.0;
        if (pace) usleep(2902);
    }
}
static void settle(AEffect *e, int ms = 150) { blocks(e, ms / 3 + 1, false); }   /* worker re-reads + host updates */

/* real-time-ish run with the MIDI trace captured: the engine times note-offs by wall clock */
static std::string play(AEffect *e, int n, bool stop_at_end = true) {
    std::fflush(stdout);
    char *buf = nullptr; size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    FILE *old = stdout; stdout = mem;
    ti.ppqPos = 0;
    blocks(e, n, true);
    if (stop_at_end) blocks(e, 40, false);
    std::fflush(mem); stdout = old; std::fclose(mem);
    std::string s(buf, len); std::free(buf);
    return s;
}
static void balance(const std::string &s, int &ons, int &hanging) {
    std::map<int, int> held; ons = 0; hanging = 0;
    unsigned st, n, v; const char *p = s.c_str();
    while ((p = std::strstr(p, "MIDI "))) {
        if (std::sscanf(p, "MIDI %x %x %x", &st, &n, &v) == 3) {
            int key = (st & 15) * 128 + n;
            if ((st & 0xF0) == 0x90 && v) { held[key]++; ons++; }
            else if ((st & 0xF0) == 0x80 || (st & 0xF0) == 0x90) held[key] = 0;
        }
        p += 5;
    }
    for (auto &kv : held) if (kv.second > 0) hanging++;
}
static void send_cc(AEffect *a, int ch, int num, int val) {
    VstMidiEvent m; std::memset(&m, 0, sizeof m);
    m.type = 1; m.byteSize = sizeof m;
    m.midiData[0] = (unsigned char)(0xB0 | (ch - 1)); m.midiData[1] = (unsigned char)num; m.midiData[2] = (unsigned char)val;
    VstEvents ev; ev.numEvents = 1; ev.reserved = 0; ev.events[0] = &m; ev.events[1] = nullptr;
    a->dispatcher(a, 25, 0, 0, &ev, 0);
    blocks(a, 3, false);
}

int main(int argc, char **argv) {
    setenv("EUCLIDIER_TRACE", "1", 1);
    void *h = dlopen(argc > 1 ? argv[1] : "./build/euclidier-x86.so", RTLD_NOW);
    if (!h) { std::printf("dlopen: %s\nFAILED\n", dlerror()); return 1; }
    auto entry = (AEffect *(*)(audioMasterCallback))dlsym(h, "VSTPluginMain");
    AEffect *a = entry(master), *b = entry(master);
    CHECK(a && b && a->magic == 0x56737450, "two instances load");
    std::printf("params: %d, inputs: %d, flags: 0x%x, category: %d\n", a->numParams, a->numInputs, a->flags,
                (int)a->dispatcher(a, 35, 0, 0, nullptr, 0));
    for (int i = 0; i < 128; i++) { g_il[i] = 0.25f; g_ir[i] = -0.5f; }
    {   /* audio path: instrument writes silence, effect passes the input through */
        float il[128], ir[128], ol[128], orr[128];
        for (int i = 0; i < 128; i++) { il[i] = i / 128.0f; ir[i] = -i / 256.0f; ol[i] = orr[i] = 9.0f; }
        float *iv[2] = {il, ir}, *ov[2] = {ol, orr};
        a->processReplacing(a, a->numInputs ? iv : nullptr, ov, 128);
        if (a->numInputs) {
            CHECK(a->numInputs == 2 && !(a->flags & (1 << 8)) && a->dispatcher(a, 35, 0, 0, nullptr, 0) == 1,
                  "effect: 2 inputs, not a synth, category Effect");
            CHECK(!std::memcmp(il, ol, sizeof il) && !std::memcmp(ir, orr, sizeof ir), "effect passes audio through");
            float keep[128]; std::memcpy(keep, il, sizeof il);
            a->processReplacing(a, iv, iv, 128);                 /* in-place, as hosts do for inserts */
            CHECK(!std::memcmp(keep, il, sizeof il), "effect in-place pass-through");
            for (int i = 0; i < 128; i++) ol[i] = orr[i] = 9.0f;
            a->processReplacing(a, nullptr, ov, 128);            /* no input buffers: silence, no crash */
            CHECK(ol[5] == 0.0f && orr[5] == 0.0f, "effect without input writes silence");
        } else {
            CHECK((a->flags & (1 << 8)) && a->dispatcher(a, 35, 0, 0, nullptr, 0) == 2, "instrument: synth flag, category Synth");
            CHECK(ol[5] == 0.0f && orr[5] == 0.0f, "instrument writes silence");
        }
    }
    settle(a);

    /* keys (names from make_skin.py / module.json) */
    int en[9], st[9], fl[9], md[9], note[9];
    for (int l = 1; l <= 8; l++) {
        char k[16]; std::snprintf(k, sizeof k, "Lane %d", l);
        en[l] = find(a, k);
        st[l] = find(a, "Steps", l - 1); fl[l] = find(a, "Fill", l - 1); md[l] = find(a, "Mode", l - 1); note[l] = find(a, "Note", l - 1);
    }
    int sel = find(a, "Lane"), sel_next = find(a, "sel_next"), sel_steps = find(a, "Steps", 8), cch = find(a, "Control Ch");
    int rnd_all = find(a, "Randomise All"), rnd_sel = find(a, "Randomise Lane"), rnd3 = find(a, "Randomise 3"), alld = find(a, "All Drum");
    int save = find(a, "Save"), load = find(a, "Load"), preset = find(a, "Preset");
    CHECK(en[1] >= 0 && en[8] >= 0 && st[8] >= 0 && fl[8] >= 0 && md[8] >= 0 && sel >= 0 && sel_next >= 0 && sel_steps >= 0 &&
          cch >= 0 && rnd_all >= 0 && rnd_sel >= 0 && rnd3 >= 0 && alld >= 0 && save >= 0 && load >= 0 && preset >= 0, "param lookup");
    CHECK(cch == nparams(a) - 1, "Control Ch is the last param (upstream indices unchanged)");

    /* fresh-instance defaults (upstream's): lanes 1-4 on with their own patterns, 5-8 off, all DRUM */
    {
        int bad = 0;
        for (int l = 1; l <= 8; l++) { if ((disp(a, en[l]) == "ON") != (l <= 4)) bad++; if (disp(a, md[l]) != "DRUM") bad++; }
        CHECK(!bad && disp(a, st[1]) == "16" && disp(a, fl[1]) == "4" && disp(a, st[2]) == "8" && disp(a, fl[2]) == "3" &&
              disp(a, st[3]) == "12", "fresh defaults: lanes 1-4 on (16/4, 8/3, 12/5, 16/5), 5-8 off, all DRUM");
        CHECK(disp(a, note[1]) == "36" && disp(a, note[2]) == "37", "drum lanes on their GM notes: %s %s", disp(a, note[1]).c_str(), disp(a, note[2]).c_str());
    }

    /* notes from a synthesised transport, none left hanging after stop */
    {
        std::string s = play(a, 700);   /* ~2 s = 1 bar at 120 BPM */
        int ons, hanging;
        balance(s, ons, hanging);
        std::printf("instance A: %d note-ons in ~1 bar, %d hanging after stop\n", ons, hanging);
        CHECK(ons >= 10 && ons <= 40, "note count %d (lanes 1-4: 4+6+5+5 hits per bar)", ons);
        CHECK(hanging == 0, "no hanging notes (%d)", hanging);
        std::string sb = play(b, 100);
        CHECK(sb.find("MIDI 9") == std::string::npos, "second instance silent");
    }

    /* step displays: lane 1 at 12 steps -> the 16-slot class shows 12 cells, the others stay hidden, the play-head moves */
    {
        int i16[32], n16 = 0, wrong = 0;
        for (int s2 = 0; s2 < 16; s2++) { char k[24]; std::snprintf(k, sizeof k, "G1.16.%d", s2); i16[n16++] = find(a, k); }
        setv(a, st[1], 12, 2, 32);
        settle(a);
        int moved = 0, last = -1;
        ti.ppqPos = 0;
        for (int k = 0; k < 120; k++) {
            blocks(a, 8, true, false);
            usleep(10000);
            for (int j = 0; j < n16; j++)   /* the cell showing the play-head: option 3 or 4 of 5 */
                if (a->getParameter(a, i16[j]) > 0.7f && j != last) { last = j; moved++; }
        }
        blocks(a, 5, false);
        int shown = 0;
        for (int j = 0; j < n16; j++) if (a->getParameter(a, i16[j]) > 0.0f) shown++;
        for (int i = 0; i < a->numParams; i++) {
            std::string n = pname(a, i); int c, s2;
            if (std::sscanf(n.c_str(), "G1.%d.%d", &c, &s2) == 2 && c != 16 && a->getParameter(a, i) > 0.0f) wrong++;
        }
        CHECK(shown == 12 && !wrong, "lane 1 at 12 steps: %d cells of the 16-slot class shown, %d of other classes", shown, wrong);
        CHECK(moved > 3, "play-head cell moved %d times", moved);
        int pushed = 0; for (int j = 0; j < n16; j++) pushed += automated[i16[j]];
        CHECK(pushed > 0, "cell changes are reported to the host (%d)", pushed);
        int c12 = 0;
        for (int i = 0; i < a->numParams; i++) { int c, s2; if (std::sscanf(pname(a, i).c_str(), "C%d.%d", &c, &s2) == 2 && c == 12 && a->getParameter(a, i) > 0.0f) c12++; }
        CHECK(c12 == 12, "circle (selected lane 1) shows the 12-dot class: %d", c12);
        setv(a, st[1], 16, 2, 32);
    }

    /* lane stepper and the sel_* copies */
    {
        a->setParameter(a, sel_next, 1.0f);
        settle(a);
        CHECK(disp(a, sel) == "LANE 2" && disp(a, sel_steps) == "8", "sel_next -> %s, its steps %s (expect LANE 2, 8)", disp(a, sel).c_str(), disp(a, sel_steps).c_str());
        setv(a, sel_steps, 10, 2, 32);
        settle(a);
        CHECK(disp(a, st[2]) == "10", "sel_steps writes lane 2: %s", disp(a, st[2]).c_str());
    }

    /* randomise: one lane, the selected lane, all (compare patterns before/after) */
    {
        auto snap = [&](int l) { return disp(a, st[l]) + "/" + disp(a, fl[l]) + "/" + disp(a, find(a, "Shift", l - 1)); };
        bool changed3 = false, others = false;
        for (int t = 0; t < 6 && !changed3; t++) {   /* a random draw can repeat a value: try a few times */
            std::string b1 = snap(1), b3 = snap(3);
            a->setParameter(a, rnd3, 1.0f); settle(a, 250);
            changed3 = snap(3) != b3; others |= snap(1) != b1;
        }
        CHECK(changed3 && !others, "RANDOMISE lane 3 changes lane 3 only");
        bool changed2 = false; others = false;
        for (int t = 0; t < 6 && !changed2; t++) {
            std::string b1 = snap(1), b2 = snap(2);
            a->setParameter(a, rnd_sel, 1.0f); settle(a, 250);
            changed2 = snap(2) != b2; others |= snap(1) != b1;
        }
        CHECK(changed2 && !others, "RANDOMISE (selected lane 2) changes lane 2 only");
        int changed = 0;
        std::string bs[9]; for (int l = 1; l <= 8; l++) bs[l] = snap(l);
        a->setParameter(a, rnd_all, 1.0f); settle(a, 250);
        for (int l = 1; l <= 8; l++) changed += snap(l) != bs[l];
        CHECK(changed >= 5, "RANDOMISE ALL changes most lanes (%d of 8)", changed);
        /* the engine's controller feedback (CCs on ch 16) must not reach the MIDI out: the driven instrument took it
         * as sound changes (level dropping with every RANDOMISE, Force 2026-11-08) */
        std::fflush(stdout);
        char *buf = nullptr; size_t len = 0;
        FILE *mem = open_memstream(&buf, &len), *old = stdout; stdout = mem;
        for (int t = 0; t < 3; t++) { a->setParameter(a, rnd_all, 1.0f); settle(a, 250); }
        a->setParameter(a, save, 1.0f); settle(a, 200);
        a->setParameter(a, load, 1.0f); settle(a, 250);
        std::fflush(mem); stdout = old; std::fclose(mem);
        std::string out(buf, len); std::free(buf);
        int ccs = 0; for (size_t p = 0; (p = out.find("MIDI bf", p)) != std::string::npos; p++) ccs++;
        CHECK(ccs == 0, "randomise / preset load send no CC feedback on ch 16 (%d)", ccs);
    }

    /* DRUM MODE ALL */
    {
        for (int l = 1; l <= 8; l++) a->setParameter(a, md[l], 0.0f);
        settle(a);
        int notes = 0; for (int l = 1; l <= 8; l++) notes += disp(a, md[l]) == "NOTE";
        a->setParameter(a, alld, 1.0f); settle(a);
        int drums = 0; for (int l = 1; l <= 8; l++) drums += disp(a, md[l]) == "DRUM";
        CHECK(notes == 8 && drums == 8, "DRUM MODE ALL: %d NOTE before, %d DRUM after", notes, drums);
    }

    /* engine preset bank: save, randomise, load back */
    {
        setv(a, fl[1], 5, 1, 32); setv(a, st[1], 16, 2, 32);
        settle(a);
        a->setParameter(a, save, 1.0f); settle(a, 200);
        a->setParameter(a, rnd_all, 1.0f); settle(a, 250);
        a->setParameter(a, load, 1.0f); settle(a, 250);
        CHECK(disp(a, fl[1]) == "5" && disp(a, st[1]) == "16", "preset load restores lane 1 (fill %s, steps %s)", disp(a, fl[1]).c_str(), disp(a, st[1]).c_str());
    }

    /* control by MIDI CC: lane 1 fill = CC 12, on the Control Ch (default 16) only */
    {
        CHECK(disp(a, cch) == "16", "control channel default 16: %s", disp(a, cch).c_str());
        send_cc(a, 16, 12, 64); settle(a);
        std::string f = disp(a, fl[1]);
        CHECK(f == "17", "CC 12 on ch 16 sets lane 1 fill: %s", f.c_str());
        send_cc(a, 1, 12, 0); settle(a);
        CHECK(disp(a, fl[1]) == f, "CC on another channel is ignored: %s", disp(a, fl[1]).c_str());
        send_cc(a, 16, 11, 127); settle(a);
        CHECK(disp(a, st[1]) == "32", "CC 11 sets lane 1 steps: %s", disp(a, st[1]).c_str());
        send_cc(a, 16, 20, 127); settle(a);
        CHECK(disp(a, en[2]) == "ON", "CC 20 switches lane 2 on: %s", disp(a, en[2]).c_str());
        a->setParameter(a, cch, 3.0f / 16.0f);
        CHECK(disp(a, cch) == "3", "control channel 3: %s", disp(a, cch).c_str());
        send_cc(a, 16, 12, 0); settle(a);
        CHECK(disp(a, fl[1]) == f, "old channel ignored after change: %s", disp(a, fl[1]).c_str());
        send_cc(a, 3, 12, 0); settle(a);
        CHECK(disp(a, fl[1]) == "1", "new channel works: %s", disp(a, fl[1]).c_str());
        a->setParameter(a, cch, 0.0f);
        send_cc(a, 3, 12, 127); send_cc(a, 1, 12, 127); settle(a);
        CHECK(disp(a, cch) == "OFF" && disp(a, fl[1]) == "1", "OFF ignores every CC: %s", disp(a, fl[1]).c_str());
        a->setParameter(a, cch, 3.0f / 16.0f);
        std::string b4 = disp(a, st[4]) + disp(a, fl[4]), b1 = disp(a, st[1]) + disp(a, fl[1]);
        send_cc(a, 3, 103, 51); settle(a);   /* Rand Lane -> option 4 of 0..10 = LANE 4 */
        bool ch4 = false;
        for (int t = 0; t < 6 && !ch4; t++) { send_cc(a, 3, 104, 127); settle(a, 250); ch4 = disp(a, st[4]) + disp(a, fl[4]) != b4; }
        CHECK(ch4 && disp(a, st[1]) + disp(a, fl[1]) == b1, "CC 103 LANE 4 + CC 104 randomises lane 4 only");
        setv(a, fl[1], 5, 1, 32); setv(a, st[1], 16, 2, 32);
        settle(a);
    }

    /* project chunk: values in, triggers and sel_* mirrors out; restore into the same and a fresh state */
    {
        void *ck = nullptr;
        intptr_t n = a->dispatcher(a, 23, 0, 0, &ck, 0);
        std::string chunk((char *)ck, n);
        CHECK(n > 100 && chunk.find("l1_fill=5;") != std::string::npos && chunk.find("cc_ch=3;") != std::string::npos, "chunk holds fill and the control channel");
        CHECK(chunk.find("rand_go") == std::string::npos && chunk.find("preset_load") == std::string::npos &&
              chunk.find("preset_save") == std::string::npos && chunk.find("sel_") == std::string::npos && chunk.find("G1.") == std::string::npos &&
              chunk.find("g1_") == std::string::npos, "chunk holds no triggers, sel_* mirrors or display cells");
        std::string before[700]; int np = a->numParams;
        for (int i = 0; i < np && i < 700; i++) before[i] = disp(a, i);
        a->setParameter(a, fl[1], 1.0f); a->setParameter(a, cch, 1.0f); a->setParameter(a, md[3], 0.0f);
        setv(a, note[3], 77, 0, 127);
        settle(a);
        std::string old = chunk.substr(0, chunk.size() - 1) + "rand_go=0;preset_load=0;preset_save=0;";   /* what older versions saved */
        a->dispatcher(a, 24, 0, (intptr_t)old.size() + 1, (void *)old.c_str(), 0);
        settle(a, 250);
        int bad = 0;
        for (int i = 0; i < np && i < 700; i++) {
            std::string nm = pname(a, i);
            if ((nm[0] == 'G' || nm[0] == 'C') && nm.find('.') != std::string::npos) continue;   /* display cells */
            if (disp(a, i) != before[i]) { std::printf("  restore differs: %s '%s' -> '%s'\n", nm.c_str(), before[i].c_str(), disp(a, i).c_str()); bad++; }
        }
        CHECK(bad == 0, "chunk restore brings every value back (%d differ; triggers in it not replayed)", bad);
    }

    b->dispatcher(b, 1, 0, 0, nullptr, 0);
    a->dispatcher(a, 1, 0, 0, nullptr, 0);
    AEffect *c = entry(master);   /* after the owner closed, a new instance drives the engine again */
    std::string sc = play(c, 400);
    CHECK(sc.find("MIDI 9") != std::string::npos, "a new instance after closing the owner plays");
    c->dispatcher(c, 1, 0, 0, nullptr, 0);
    std::printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}

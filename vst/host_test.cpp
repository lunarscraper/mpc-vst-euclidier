/* Offline x86 test of euclidier_vst.cpp with the engine linked in-process (built
 * -DNO_ALSA by build.sh): parameter round-trip, popup, second instance stays inert,
 * synthesised transport -> notes, stop -> no hanging notes, chunk restore. PASSED/FAILED.
 * The effect variant (Euclidier FX, numInputs == 2) gets an input signal and must pass it
 * through bit-exact, separate buffers and in-place. */
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
static intptr_t master(AEffect *, int32_t op, int32_t, intptr_t, void *, float) { return op == 7 ? (intptr_t)&ti : 0; }
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static int find(AEffect *e, const char *key_name, int nth = 0) {   /* by display name, nth match */
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, key_name) && nth-- == 0) return i; }
    return -1;
}
static std::string disp(AEffect *e, int i) { char b[64] = {0}; e->dispatcher(e, 7, i, 0, b, 0); return b; }

/* real-time-ish run: the engine times note-offs by wall clock, so blocks are paced */
static std::string play(AEffect *e, int blocks, bool stop_at_end = true) {
    float l[128], r[128]; float *o[2] = {l, r};
    float il[128], ir[128]; float *iv[2] = {il, ir};
    for (int i = 0; i < 128; i++) { il[i] = 0.25f; ir[i] = -0.5f; }
    float **in = e->numInputs ? iv : nullptr;
    fflush(stdout);
    char *buf = nullptr; size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    FILE *old = stdout; stdout = mem;
    ti.flags = (1 << 1) | (1 << 9) | (1 << 10); ti.tempo = 120; ti.ppqPos = 0;
    for (int b = 0; b < blocks; b++) {
        e->processReplacing(e, in, o, 128);
        ti.ppqPos += 128 * (120.0 / 60.0) / 44100.0;
        usleep(2902);
    }
    if (stop_at_end) {
        ti.flags = (1 << 9) | (1 << 10);
        for (int b = 0; b < 40; b++) { e->processReplacing(e, in, o, 128); usleep(2902); }
    }
    fflush(mem); stdout = old; fclose(mem);
    std::string s(buf, len); free(buf);
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

int main(int argc, char **argv) {
    setenv("EUCLIDIER_TRACE", "1", 1);
    void *h = dlopen(argc > 1 ? argv[1] : "./build/euclidier-x86.so", RTLD_NOW);
    if (!h) { std::printf("dlopen: %s\nFAILED\n", dlerror()); return 1; }
    auto entry = (AEffect *(*)(audioMasterCallback))dlsym(h, "VSTPluginMain");
    AEffect *a = entry(master), *b = entry(master);
    CHECK(a && b && a->magic == 0x56737450, "instances");
    std::printf("params: %d, inputs: %d, flags: 0x%x, category: %d\n", a->numParams, a->numInputs, a->flags,
                (int)a->dispatcher(a, 35, 0, 0, nullptr, 0));
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

    int en1 = find(a, "Enable", 0), steps1 = find(a, "Steps", 0), fill1 = find(a, "Fill", 0),
        div1 = find(a, "Div", 0), divlist1 = find(a, "Div List", 0), en2 = find(a, "Enable", 1);
    CHECK(en1 >= 0 && steps1 >= 0 && fill1 >= 0 && div1 >= 0 && divlist1 >= 0, "param lookup");
    CHECK(disp(a, en1) == "ON", "lane 1 enabled by default: %s", disp(a, en1).c_str());
    CHECK(disp(a, steps1) == "16", "lane 1 steps default %s", disp(a, steps1).c_str());
    a->setParameter(a, fill1, (5 - 1) / 63.0f);
    CHECK(disp(a, fill1) == "5", "fill set %s", disp(a, fill1).c_str());
    a->setParameter(a, divlist1, 1.0f);
    CHECK(a->getParameter(a, divlist1) > 0.5f, "popup opens");
    a->setParameter(a, div1, 2.0f / 10.0f);    /* pick "1/8" exactly -> closes the popup */
    CHECK(disp(a, div1) == "1/8" && a->getParameter(a, divlist1) < 0.5f, "div pick %s", disp(a, div1).c_str());
    a->setParameter(a, div1, 0.0f);
    a->setParameter(a, en2, 1.0f);

    std::string s = play(a, 700);   /* ~2 s = 1 bar at 120 BPM */
    int ons, hanging;
    balance(s, ons, hanging);
    std::printf("instance A: %d note-ons in ~1 bar, %d hanging after stop\n", ons, hanging);
    CHECK(ons >= 5 && ons <= 40, "note count %d", ons);
    CHECK(hanging == 0, "hanging notes %d", hanging);
    std::string sb = play(b, 100);
    CHECK(sb.find("MIDI 9") == std::string::npos, "second instance silent");

    int rnd = find(a, "Randomize"), save = find(a, "Save"), load = find(a, "Load");
    CHECK(rnd >= 0 && save >= 0 && load >= 0, "preset/randomize params");
    a->setParameter(a, save, 1.0f);            /* store the current state in slot 1 */
    a->setParameter(a, rnd, 1.0f);             /* randomize, then load slot 1 back */
    play(a, 60);
    a->setParameter(a, load, 1.0f);
    play(a, 60);
    CHECK(disp(a, fill1) == "5", "preset load restores fill: %s", disp(a, fill1).c_str());

    {   /* control by MIDI CC: lane 1 fill = CC 12, on the Control Ch (default 16) only */
        int cch = find(a, "Control Ch");
        CHECK(cch >= 0 && disp(a, cch) == "16", "control channel default 16: %s", cch >= 0 ? disp(a, cch).c_str() : "missing");
        auto cc = [&](int ch, int num, int val) {
            VstMidiEvent m; std::memset(&m, 0, sizeof m);
            m.type = 1; m.byteSize = sizeof m;
            m.midiData[0] = (unsigned char)(0xB0 | (ch - 1)); m.midiData[1] = (unsigned char)num; m.midiData[2] = (unsigned char)val;
            VstEvents ev; ev.numEvents = 1; ev.reserved = 0; ev.events[0] = &m; ev.events[1] = nullptr;
            a->dispatcher(a, 25, 0, 0, &ev, 0);
            float l[128], r[128], il[128] = {0}, ir[128] = {0}; float *o[2] = {l, r}, *iv[2] = {il, ir};
            ti.flags = (1 << 9) | (1 << 10);
            for (int b = 0; b < 3; b++) a->processReplacing(a, a->numInputs ? iv : nullptr, o, 128);
        };
        cc(16, 12, 64);
        CHECK(disp(a, fill1) == "33", "CC 12 on ch 16 sets lane 1 fill: %s", disp(a, fill1).c_str());
        cc(1, 12, 0);
        CHECK(disp(a, fill1) == "33", "CC on another channel is ignored: %s", disp(a, fill1).c_str());
        cc(16, 11, 127);
        CHECK(disp(a, steps1) == "64", "CC 11 sets lane 1 steps: %s", disp(a, steps1).c_str());
        cc(16, 20, 0);
        CHECK(disp(a, en2) == "OFF", "CC 20 switches lane 2 off: %s", disp(a, en2).c_str());
        a->setParameter(a, cch, 3.0f / 16.0f);
        CHECK(disp(a, cch) == "3", "control channel 3: %s", disp(a, cch).c_str());
        cc(16, 12, 0);
        CHECK(disp(a, fill1) == "33", "old channel ignored after change: %s", disp(a, fill1).c_str());
        cc(3, 12, 0);
        CHECK(disp(a, fill1) == "1", "new channel works: %s", disp(a, fill1).c_str());
        a->setParameter(a, cch, 0.0f);
        cc(3, 12, 127); cc(1, 12, 127);
        CHECK(disp(a, cch) == "OFF" && disp(a, fill1) == "1", "OFF ignores every CC: %s", disp(a, fill1).c_str());
        a->setParameter(a, cch, 3.0f / 16.0f);
        a->setParameter(a, fill1, (5 - 1) / 63.0f);
        a->setParameter(a, steps1, (16 - 2) / 62.0f);
    }

    void *ck = nullptr;
    intptr_t n = a->dispatcher(a, 23, 0, 0, &ck, 0);
    CHECK(n > 100 && ck && std::strstr((char *)ck, "l1_fill=5;"), "chunk contains fill");
    CHECK(ck && std::strstr((char *)ck, "cc_ch=3;"), "chunk contains the control channel");
    int cch2 = find(a, "Control Ch");
    std::string chunk((char *)ck, n);
    a->setParameter(a, fill1, 1.0f);
    a->setParameter(a, cch2, 1.0f);
    a->dispatcher(a, 24, 0, n, (void *)chunk.data(), 0);
    CHECK(disp(a, fill1) == "5", "chunk restore fill %s", disp(a, fill1).c_str());
    CHECK(disp(a, cch2) == "3", "chunk restore control channel %s", disp(a, cch2).c_str());

    b->dispatcher(b, 1, 0, 0, nullptr, 0);
    a->dispatcher(a, 1, 0, 0, nullptr, 0);
    std::printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}

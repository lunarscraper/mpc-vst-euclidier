/* Offline x86 test of euclidier_vst.cpp with the engine linked in-process (built
 * -DNO_ALSA by build.sh): parameter round-trip, popup, second instance stays inert,
 * synthesised transport -> notes, stop -> no hanging notes, chunk restore. PASSED/FAILED. */
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
    fflush(stdout);
    char *buf = nullptr; size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    FILE *old = stdout; stdout = mem;
    ti.flags = (1 << 1) | (1 << 9) | (1 << 10); ti.tempo = 120; ti.ppqPos = 0;
    for (int b = 0; b < blocks; b++) {
        e->processReplacing(e, nullptr, o, 128);
        ti.ppqPos += 128 * (120.0 / 60.0) / 44100.0;
        usleep(2902);
    }
    if (stop_at_end) {
        ti.flags = (1 << 9) | (1 << 10);
        for (int b = 0; b < 40; b++) { e->processReplacing(e, nullptr, o, 128); usleep(2902); }
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
    std::printf("params: %d\n", a->numParams);

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

    void *ck = nullptr;
    intptr_t n = a->dispatcher(a, 23, 0, 0, &ck, 0);
    CHECK(n > 100 && ck && std::strstr((char *)ck, "l1_fill=5;"), "chunk contains fill");
    std::string chunk((char *)ck, n);
    a->setParameter(a, fill1, 1.0f);
    a->dispatcher(a, 24, 0, n, (void *)chunk.data(), 0);
    CHECK(disp(a, fill1) == "5", "chunk restore fill %s", disp(a, fill1).c_str());

    b->dispatcher(b, 1, 0, 0, nullptr, 0);
    a->dispatcher(a, 1, 0, 0, nullptr, 0);
    std::printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}

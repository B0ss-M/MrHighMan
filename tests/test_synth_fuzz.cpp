// Omni Sampler 2.4.32: SYNTH on, chords held, random knob twists and preset / program steps on the SYNTH, FX and FX BANK
// pages (params.json ranges), with FP traps (invalid, overflow) around render: the first NaN / blow-up stops it with a
// stack (ASan). Found the NOISE macro filter (2.4.31) and the delay lines' read one past the end (Chorus, ASan).
//   test_synth_fuzz <seed> <steps> <action log>   (tests/fuzz_params.inc: regenerate with tests/gen_fuzz_params.py)
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
extern "C" {
#include "engine.h"
}
struct FP { const char *key; float lo, hi; int mom; };
static const FP PARAMS[] = {
#include "fuzz_params.inc"
};
static const int NP = int(sizeof PARAMS / sizeof PARAMS[0]);
static const mpc_engine_t *e;
static FILE *logf_;
static unsigned rng = 1;
static unsigned rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static float frand() { return float(rnd() & 0xFFFF) / 65535.0f; }
static void setp(void *y, const char *k, const std::string &v) {
    std::fprintf(logf_, "%s=%s\n", k, v.c_str());
    std::fflush(logf_);
    e->set_param(y, k, v.c_str());
}
int main(int argc, char **argv) {
    rng = argc > 1 ? unsigned(std::atoi(argv[1])) : 1;
    const int steps = argc > 2 ? std::atoi(argv[2]) : 2000;
    logf_ = std::fopen(argc > 3 ? argv[3] : "/tmp/fuzz-actions.log", "w");
    const bool trap = !std::getenv("NOTRAP");
    e = mpc_engine();
    std::system("rm -rf /tmp/omni-fuzz");
    void *y = e->create("/tmp/omni-fuzz-test");
    std::vector<int16_t> buf(256);
    setp(y, "syn_on", "1");
    for (int i = 0; i < 50; i++) e->render(y, buf.data(), 128);
    int notes[3] = {48, 55, 60};
    auto chord = [&](bool on) {
        for (int n : notes) { uint8_t m[3] = {uint8_t(on ? 0x90 : 0x80), uint8_t(n), uint8_t(on ? 100 : 0)}; e->midi(y, m, 3); }
    };
    chord(true);
    int dead = 0, loud_seen = 0;
    for (int s = 0; s < steps; s++) {
        std::fprintf(logf_, "# step %d\n", s);
        const int nset = 1 + int(rnd() % 3);
        for (int j = 0; j < nset; j++) {
            const FP &p = PARAMS[rnd() % unsigned(NP)];
            if (p.mom) { setp(y, p.key, "1"); setp(y, p.key, "0"); continue; }
            float v = p.lo + (p.hi - p.lo) * frand();
            if (rnd() % 4 == 0) v = rnd() % 2 ? p.lo : p.hi;   // extremes often
            char b[32]; std::snprintf(b, sizeof b, "%g", double(v));
            setp(y, p.key, b);
        }
        if (s % 40 == 39) { chord(false); notes[0] = 36 + int(rnd() % 40); notes[1] = notes[0] + 7; notes[2] = notes[0] + 12; chord(true); }
        int pk = 0;
        if (trap) feenableexcept(FE_INVALID | FE_OVERFLOW);
        for (int b = 0; b < 8; b++) {
            e->render(y, buf.data(), 128);
            for (int16_t v : buf) pk = std::max(pk, std::abs(int(v)));
        }
        if (trap) fedisableexcept(FE_ALL_EXCEPT);
        if (pk > 300) { loud_seen = 1; dead = 0; } else dead++;
        if (s % 200 == 0) std::printf("step %d peak %d\n", s, pk);
    }
    std::printf("DONE %d steps, longest quiet run at end %d steps (loud seen %d)\n", steps, dead, loud_seen);
    e->destroy(y);
}

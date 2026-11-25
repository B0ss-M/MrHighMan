// 2.4.40 regression (was the 2.4.39 audit; the user: "make sure all the knobs inside the FX BANK work and have an impact on the sound"): for every
// program of slot A (factory rack) and slots B / C / D (expansions), each of the page's 13 controls is set low and high
// (the rest at the page defaults) and the outputs compared. A control "works" for a program when the two renders differ
// by more than -30 dB of the signal (an audible change); the TSV keeps every control's change in dB. Output: per slot, per control, how many programs it affects; and the list of
// programs where a control does nothing (TSV).
//   g++ -std=gnu++17 -O2 -Isrc fx_knob_audit.cpp src/engine/fx_expansion.cpp -o fx_knob_audit
#include "engine/fx_expansion.hpp"
#include "engine/omni_fx_bank.hpp"
#include "engine/fx_knobs.hpp"
#ifndef OMNI_PUBLIC
#define OMNI_PUBLIC 0
#endif
namespace {
#include "engine/fx_knob_usage_a.inc"
#if !OMNI_PUBLIC
#include "engine/fx_knob_usage_bcd.inc"
#endif
}
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
using namespace omni;
using C = omnisampler::fxbank::Controls;

static const char *KN[13] = {"MIX", "RATE", "DEPTH", "FEEDBACK", "TIME", "TONE", "DRIVE", "THRESH", "RATIO", "PITCH", "LOW", "MID", "HIGH"};
// low / high values per control (page ranges)
static void setk(C &c, int k, bool hi) {
    switch (k) {
        case 0: c.mix = hi ? 1.f : .25f; break;
        case 1: c.rateHz = hi ? 8.f : .2f; break;
        case 2: c.depth = hi ? 1.f : .1f; break;
        case 3: c.feedback = hi ? .85f : 0.f; break;
        case 4: c.timeMs = hi ? 900.f : 40.f; break;
        case 5: c.tone = hi ? 1.f : .05f; break;
        case 6: c.driveDb = hi ? 30.f : 0.f; break;
        case 7: c.thresholdDb = hi ? -50.f : -2.f; break;
        case 8: c.ratio = hi ? 20.f : 1.f; break;
        case 9: c.pitchSemitones = hi ? 12.f : -12.f; break;
        case 10: c.lowDb = hi ? 12.f : -12.f; break;
        case 11: c.midDb = hi ? 12.f : -12.f; break;
        case 12: c.highDb = hi ? 12.f : -12.f; break;
    }
}

static const double SR = 44100;
static const int B = 256, BLOCKS = int(1.2 * SR / B);

// a test signal: a chord of saws + noise bursts (excites delays, filters, dynamics, modulation)
static void signal(std::vector<float> &l, std::vector<float> &r, int b) {
    static uint32_t s = 1;
    if (b == 0) s = 1;   // the same input for every render
    for (int i = 0; i < B; i++) {
        const double t = double(b * B + i) / SR;
        double v = 0;
        for (double f : {110.0, 138.6, 164.8}) v += 0.25 * (2 * (t * f - std::floor(t * f)) - 1);
        s = s * 1664525u + 1013904223u;
        const double env = std::fmod(t, 0.5) < 0.12 ? 1.0 : 0.15;
        v = v * env + 0.08 * (double(s >> 8) / 8388608.0 - 1) * env;
        l[size_t(i)] = float(v);
        r[size_t(i)] = float(v * 0.9);
    }
}

// render with controls c through `run` (select + set already done by the caller's lambda)
static std::vector<float> render(const std::function<void(float *, float *, int)> &run) {
    std::vector<float> l(B), r(B), out;
    out.reserve(size_t(BLOCKS * B * 2));
    for (int b = 0; b < BLOCKS; b++) {
        signal(l, r, b);
        run(l.data(), r.data(), B);
        for (int i = 0; i < B; i++) { out.push_back(l[size_t(i)]); out.push_back(r[size_t(i)]); }
    }
    return out;
}
static double diff_db(const std::vector<float> &a, const std::vector<float> &b) {
    double e = 0, p = 1e-12;
    for (size_t i = 0; i < a.size() && i < b.size(); i++) { e += double(a[i] - b[i]) * (a[i] - b[i]); p += double(a[i]) * a[i] + double(b[i]) * b[i]; }
    return 10 * std::log10(std::max(e, 1e-30) / p);
}

static uint16_t usage_named(char slot, const std::string &n0) {
    std::string n = n0;
    const size_t only = n.find(" (64-bit MPC only)");
    if (only != std::string::npos) n.erase(only);
    for (const auto &u : FX_USAGE_MV) if (n == u.name) return u.used;
#if !OMNI_PUBLIC
    for (const auto &u : FX_USAGE_BCD) if (u.slot == slot && n == u.name) return u.used;
#endif
    return 1;
}
static uint16_t usage_a(int cat) { for (const auto &u : FX_USAGE_A) if (u.index == cat) return u.used; return 1; }
static omnisampler::fxknobs::Shared SH;
// the page's defaults as the plugin sends them (RATE = fxb_hz(39), not the Controls struct's 0.7 Hz)
static C base() { C c; c.rateHz = omnisampler::fxknobs::Defaults().rate; return c; }
static int fails = 0;
// programs with no effect to act on: pass-throughs and the mute (no knob can be heard on silence / nothing to mix), and
// MIX on the MV effects that are flat at their own defaults (MIX scales an effect: there is none until their settings change)
static bool exempt(const std::string &name, int k) {
    if (name.find("THRU") != std::string::npos || name.find("Thru") != std::string::npos || name.find("THROUGH") != std::string::npos ||
        name.find("NO EFFECT") != std::string::npos || name == "K2K Dry") return k == 0;
    if (name.find("Mute") != std::string::npos) return true;
    if (k == 0 && (name.rfind("MV Graphic EQ", 0) == 0 || name.rfind("MV Spectrum", 0) == 0 || name.rfind("MV LoFi Noise", 0) == 0)) return true;
    return false;
}

int main(int argc, char **argv) {
    SH.prepare(float(SR), B);
    const int only = argc > 1 ? std::atoi(argv[1]) : -1;   // -1 all slots, 0 A, 1..3 B..D
    FILE *tsv = std::fopen(argc > 2 ? argv[2] : "fx_knob_audit.tsv", "w");
    std::fprintf(tsv, "slot\tprogram\tname"); for (int k = 0; k < 13; k++) std::fprintf(tsv, "\t%s", KN[k]); std::fprintf(tsv, "\n");
    // slot A: the factory rack
    if (only < 0 || only == 0) {
        omnisampler::fxbank::Rack rack;
        rack.prepare(float(SR));
        std::size_t n = 0;
        const omnisampler::fxbank::Program *list = omnisampler::fxbank::programs(n);
        int works[13] = {}, progs = 0, unstable = 0;
        for (std::size_t p = 1; p < n; p++) {
            if (list[p].engine == omnisampler::fxbank::Engine::Bypass) continue;
            progs++;
            {   // defaults: the wrapped program = the bare program, byte for byte (projects unchanged)
                std::vector<float> z[2];
                for (int h = 0; h < 2; h++) {
                    C c = base(); rack.select(0); rack.select(p); rack.reset(); rack.set(c); SH.reset();
                    const uint16_t used = usage_a(int(p));
                    z[h] = render([&](float *l, float *r, int f) { if (h) SH.pre(l, r, f, c, used); rack.process(l, r, size_t(f)); if (h) SH.post(l, r, f, c, used); });
                }
                if (z[0].size() != z[1].size() || std::memcmp(z[0].data(), z[1].data(), z[0].size() * sizeof(float)) != 0) { unstable++; fails++; std::printf("FAIL A %zu %s: defaults changed the sound\n", p, list[p].name); }
            }
            std::string dead;
            for (int k = 0; k < 13; k++) {
                std::vector<float> o[2];
                for (int h = 0; h < 2; h++) {
                    C c = base(); setk(c, k, h == 1);
                    // MIX scales a program's effect: an EQ program is flat at its default gains, so give it some first
                    if (k == 0 && (usage_a(int(p)) & 0x1c00)) { c.lowDb = 6; c.midDb = -6; c.highDb = 6; }
                    rack.select(0); rack.select(p); rack.reset(); rack.set(c);   // a clean start every render
                    SH.reset();
                    const uint16_t used = usage_a(int(p));
                    o[h] = render([&](float *l, float *r, int f) { SH.pre(l, r, f, c, used); rack.process(l, r, size_t(f)); SH.post(l, r, f, c, used); });
                }
                const double d = diff_db(o[0], o[1]);
                if (d > -30 || exempt(list[p].name, k)) works[k]++; else { fails++; std::printf("DEAD A %zu %s: %s (%.0f dB)\n", p, list[p].name, KN[k], d); }
                dead += "\t" + std::to_string(int(std::lround(d)));
            }
            std::fprintf(tsv, "A\t%zu\t%s%s\n", p, list[p].name, dead.c_str());
        }
        std::printf("slot A (%d programs, %d changed at the defaults):", progs, unstable);
        for (int k = 0; k < 13; k++) std::printf(" %s %d", KN[k], works[k]);
        std::printf("\n");
    }
    std::unique_ptr<FxExpansion> x[3];
    make_fx_expansions(x);
#if OMNI_PUBLIC
    x[0] = make_mv_bank_a();   // public 2.3.1: bank A's MV-8800 programs (slots B-D are empty); listed as slot "B"
#endif
    for (int s = 0; s < 3; s++) {
        if (!x[s] || (only >= 0 && only != s + 1)) continue;
        x[s]->prepare(SR, B);
        int works[13] = {};
        const int n = x[s]->count();
        int changed_def = 0;
        for (int p = 1; p <= n; p++) {
            {   // defaults: byte-identical to the bare program
                std::vector<float> z[2];
                for (int h = 0; h < 2; h++) {
                    C c = base(); x[s]->select(p); x[s]->reset(); x[s]->set(c); x[s]->tempo(120); SH.reset();
                    const uint16_t used = usage_named(char('B' + s), x[s]->name(p));
                    z[h] = render([&](float *l, float *r, int f) { if (h) SH.pre(l, r, f, c, used); x[s]->process(l, r, f); if (h) SH.post(l, r, f, c, used); });
                }
                if (std::memcmp(z[0].data(), z[1].data(), z[0].size() * sizeof(float)) != 0) { changed_def++; fails++; std::printf("FAIL %c %d %s: defaults changed the sound\n", 'B' + s, p, x[s]->name(p)); }
            }
            std::string dead;
            for (int k = 0; k < 13; k++) {
                std::vector<float> o[2];
                for (int h = 0; h < 2; h++) {
                    C c = base(); setk(c, k, h == 1);
                    if (k == 0 && (usage_named(char('B' + s), x[s]->name(p)) & 0x1c00)) { c.lowDb = 6; c.midDb = -6; c.highDb = 6; }
                    x[s]->select(p); x[s]->reset(); x[s]->set(c); x[s]->tempo(120);   // a clean start every render
                    SH.reset();
                    const uint16_t used = usage_named(char('B' + s), x[s]->name(p));
                    o[h] = render([&](float *l, float *r, int f) { SH.pre(l, r, f, c, used); x[s]->process(l, r, f); SH.post(l, r, f, c, used); });
                }
                const double d = diff_db(o[0], o[1]);
                if (d > -30 || exempt(x[s]->name(p), k)) works[k]++; else { fails++; std::printf("DEAD %c %d %s: %s (%.0f dB)\n", 'B' + s, p, x[s]->name(p), KN[k], d); }
                dead += "\t" + std::to_string(int(std::lround(d)));
            }
            std::fprintf(tsv, "%c\t%d\t%s%s\n", 'B' + s, p, x[s]->name(p), dead.c_str());
        }
        std::printf("slot %c (%d programs, %d changed at the defaults):", 'B' + s, n, changed_def);
        for (int k = 0; k < 13; k++) std::printf(" %s %d", KN[k], works[k]);
        std::printf("\n");
        std::fflush(stdout);
    }
    std::fclose(tsv);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

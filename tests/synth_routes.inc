// 2.4.2: the SYNTH's modulation routing and filters, on the engine itself (omni::Synth, no plugin): every LFO / env /
// macro / MATRIX source reaches every destination, the filters' responses and slopes, resonance, zipper-free cutoff
// modulation. Included by test_playback.cpp (it uses check()).
#include "engine/synth.hpp"

namespace syt {
using namespace omni;

static std::vector<WavetablePtr> keep;   // the tables the synths read (alive for the whole test)
static const Wavetable *classic(int i) { keep.push_back(factory_classic(i)); return keep.back().get(); }
static const Wavetable *table(int i) { keep.push_back(factory_table(i)); return keep.back().get(); }

// a fresh synth (each render starts from the same seeds): OSC A on (classic wave `wave`: 0 sine, 2 saw), the rest off
static std::unique_ptr<Synth> make(int wave = 2) {
    std::unique_ptr<Synth> s(new Synth());
    s->settings.on.store(1);
    s->settings.sustain.store(1);
    s->settings.osc[0].on.store(1);
    s->settings.osc[0].type.store(Synth::OT_CLASSIC);
    s->settings.osc[0].table.store(classic(wave));
    s->settings.osc[0].unison.store(1);
    return s;
}
struct Out { std::vector<float> l, r; };
// a note held for `secs` (block frames per render call)
static Out run(Synth &s, int note, double secs, int block = 128, std::function<void(Synth &, int)> per_block = nullptr) {
    Out o;
    std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    s.note_on(note, 100);
    const int nb = int(secs * 44100 / block);
    for (int b = 0; b < nb; b++) {
        if (per_block) per_block(s, b);
        std::fill(l.begin(), l.end(), 0.f); std::fill(r.begin(), r.end(), 0.f);
        s.render(l.data(), r.data(), block);
        o.l.insert(o.l.end(), l.begin(), l.end()); o.r.insert(o.r.end(), r.begin(), r.end());
    }
    return o;
}
static double rmsr(const std::vector<float> &x, size_t a, size_t z) {
    double q = 0; z = std::min(z, x.size());
    for (size_t i = a; i < z; i++) q += double(x[i]) * x[i];
    return z > a ? std::sqrt(q / double(z - a)) : 0;
}
// brightness: the first difference's level against the level (a saw through a low-pass: rises with the cutoff)
static double bright(const std::vector<float> &x, size_t a, size_t z) {
    double d = 0, q = 0; z = std::min(z, x.size());
    for (size_t i = a + 1; i < z; i++) { double e = x[i] - x[i - 1]; d += e * e; q += double(x[i]) * x[i]; }
    return q > 0 ? std::sqrt(d / q) : 0;
}
// the spread (max / min) of 10 ms block values of f after the first 100 ms
template <class F> static double spread(const std::vector<float> &x, F f) {
    double lo = 1e30, hi = 0;
    for (size_t b0 = 4410; b0 + 441 <= x.size(); b0 += 441) { double v = f(x, b0, b0 + 441); lo = std::min(lo, v); hi = std::max(hi, v); }
    return lo > 0 ? hi / lo : 1e9;
}
static double diff(const Out &a, const Out &b) {   // how far apart two renders are, against a's level
    double d = 0, q = 0;
    for (size_t i = 0; i < std::min(a.l.size(), b.l.size()); i++) {
        d += std::pow(double(a.l[i]) - b.l[i], 2) + std::pow(double(a.r[i]) - b.r[i], 2);
        q += double(a.l[i]) * a.l[i] + double(a.r[i]) * a.r[i];
    }
    return q > 0 ? std::sqrt(d / q) : 0;
}
static double goertzel(const std::vector<float> &x, size_t a, size_t n, double hz) {   // the level at hz
    const double w = 2 * M_PI * hz / 44100;
    double re = 0, im = 0;
    for (size_t i = 0; i < n; i++) {
        const double win = 0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(n));   // Hann
        re += x[a + i] * win * std::cos(w * double(i)); im += x[a + i] * win * std::sin(w * double(i));
    }
    return std::sqrt(re * re + im * im) * 4 / double(n);
}
static bool clean(const Out &o) {   // finite, no denormals
    for (const auto *v : {&o.l, &o.r})
        for (float x : *v) if (!std::isfinite(x) || (x != 0 && std::fabs(x) < 1e-30f)) return false;
    return true;
}
static double db(double x) { return 20 * std::log10(std::max(1e-12, x)); }

// a synth where every destination is audible: wavetable A (frames to scan), warp, unison; B on; SUB, NOISE; filter
static std::unique_ptr<Synth> rich() {
    auto s = make(2);
    SynthSettings &y = s->settings;
    y.osc[0].type.store(Synth::OT_WAVETABLE); y.osc[0].table.store(table(0)); y.osc[0].pos.store(0.3f);
    y.osc[0].unison.store(3); y.osc[0].detune.store(0.2f); y.osc[0].spread.store(0.5f);
    y.osc[0].warp_mode.store(Synth::WM_BEND); y.osc[0].warp.store(0.3f);
    y.osc[1].on.store(1); y.osc[1].type.store(Synth::OT_WAVETABLE); y.osc[1].table.store(table(1)); y.osc[1].pos.store(0.3f);
    y.osc[1].unison.store(3); y.osc[1].detune.store(0.2f); y.osc[1].octave.store(1);
    y.osc[1].warp_mode.store(Synth::WM_PWM); y.osc[1].warp.store(0.3f);
    y.mix.sub_on.store(1); y.mix.sub_table.store(classic(0)); y.mix.sub_level.store(0.5f);
    y.mix.noise_on.store(1); y.mix.noise_level.store(0.5f); y.mix.noise_tone.store(0.2f);   // 2.4.9: dark, so NOISE TONE +60 % (about 500 Hz -> 8 kHz) shows through the 1.5 kHz filter
    y.mix.flt_on.store(1); y.mix.flt_type.store(Synth::FT_LP24); y.mix.cutoff.store(1500); y.mix.res.store(0.3f); y.mix.drive.store(0.2f);
    y.mix.route.store(0xF);
    y.mod.lfo[0].rate_hz.store(3); y.mod.lfo[0].dest.store(SD_PITCH); y.mod.lfo[0].depth.store(0.05f);   // LFO 1 RATE shows
    y.mod.lfo[1].rate_hz.store(2); y.mod.lfo[1].dest.store(SD_RES); y.mod.lfo[1].depth.store(0.3f);
    // 2.4.17: LFO 3 / 4 running too (their RATE destinations show)
    y.mod.lfo[2].rate_hz.store(2.5f); y.mod.lfo[2].dest.store(SD_PAN); y.mod.lfo[2].depth.store(0.4f);
    y.mod.lfo[3].rate_hz.store(1.5f); y.mod.lfo[3].dest.store(SD_POS_B); y.mod.lfo[3].depth.store(0.4f);
    return s;
}

static void run_all() {
    std::printf("---- synth routing / filters (engine) ----\n");
    const size_t S = 44100;
    {   // every MATRIX destination moves the sound (MACRO 1 as the source, +-60 %)
        auto base = rich();
        Out b0 = run(*base, 57, 0.6);
        int bad = 0;
        for (int d = SD_PITCH; d < SD_COUNT; d++) {
            if (d == SD_GLIDE) continue;   // (2.4.9: only between two notes: its own check below)
            auto s = rich();
            s->settings.mod.macro[0].store(1);
            s->settings.mod.mx_src[3].store(SS_MACRO1); s->settings.mod.mx_dst[3].store(d); s->settings.mod.mx_amt[3].store(d == SD_LEVEL ? -0.6f : 0.6f);
            double df = diff(b0, run(*s, 57, 0.6));
            if (df < 0.05) { std::printf("  matrix destination %d: change %.4f\n", d, df); bad++; }
        }
        check(!bad, "MATRIX: every destination (MACRO 1 -> each) changes the sound");
        bad = 0;
        for (int d = SD_PITCH; d < SD_COUNT; d++) {   // the macros' own destinations (2.4.9: every one, as the MATRIX)
            if (d == SD_GLIDE) continue;
            auto s = rich();
            s->settings.mod.macro[1].store(0.6f); s->settings.mod.macro_dest[1].store(d);
            if (d == SD_LEVEL) s->settings.mod.macro[1].store(0.6f);
            double df = diff(b0, run(*s, 57, 0.6));
            if (df < 0.05) { std::printf("  macro destination %d: change %.4f\n", d, df); bad++; }
        }
        check(!bad, "MACRO destinations: each changes the sound");
        bad = 0;
        for (int d = SD_PITCH; d < SD_COUNT; d++) {   // the mod envelope's destinations (2.4.9: every one)
            if (d == SD_GLIDE) continue;
            auto s = rich();
            s->settings.mod.me_on.store(1); s->settings.mod.me_dest.store(d); s->settings.mod.me_amt.store(0.6f);
            s->settings.mod.me_attack.store(0.2f); s->settings.mod.me_sustain.store(0.5f);
            double df = diff(b0, run(*s, 57, 0.6));
            if (df < 0.05) { std::printf("  mod env destination %d: change %.4f\n", d, df); bad++; }
        }
        check(!bad, "MOD ENV destinations: each changes the sound");
        bad = 0;
        for (int k = 0; k < SYN_XENVS; k++)   // 2.4.17: MOD ENV 2 / 3 on their own destination (no route)
            for (int d : {int(SD_CUTOFF), int(SD_PITCH), int(SD_LFO3_RATE), int(SD_LFO4_RATE)}) {
                auto s = rich();
                SynthModEnvSettings &x = s->settings.mod.mex[k];
                x.on.store(1); x.dest.store(d); x.amt.store(0.6f); x.attack.store(0.2f); x.sustain.store(0.5f);
                double df = diff(b0, run(*s, 57, 0.6));
                if (df < 0.05) { std::printf("  mod env %d destination %d: change %.4f\n", k + 2, d, df); bad++; }
            }
        check(!bad, "MOD ENV 2 / 3: their own destinations change the sound");
        {   // an LFO nothing reads costs nothing and changes nothing; routed, LFO 3 moves the sound
            auto a = rich(), b = rich();
            b->settings.mod.lfo[2].dest.store(0);   // LFO 3 off (unused)
            auto c = rich();
            c->settings.mod.lfo[2].dest.store(0);
            c->settings.mod.mx_src[9].store(SS_LFO3); c->settings.mod.mx_dst[9].store(SD_CUTOFF); c->settings.mod.mx_amt[9].store(0.8f);   // route 10
            const Out oa = run(*a, 57, 0.6), ob = run(*b, 57, 0.6), oc = run(*c, 57, 0.6);
            check(diff(oa, ob) > 0.05 && diff(ob, oc) > 0.05 && clean(oc), "LFO 3 as its own route and through MATRIX route 10 both move the sound");
        }
    }
    {   // 2.4.9: GLIDE as a destination: mono, glide on, a second note while the first is held; +1 slows the glide
        auto glide_run = [](float amt) {
            auto s = make(2);
            s->settings.voice.mode.store(1); s->settings.voice.glide.store(0.15f);
            s->settings.mod.macro[0].store(1);
            s->settings.mod.mx_src[0].store(SS_MACRO1); s->settings.mod.mx_dst[0].store(SD_GLIDE); s->settings.mod.mx_amt[0].store(amt);
            Out a = run(*s, 57, 0.2);
            Out b = run(*s, 69, 0.3);
            a.l.insert(a.l.end(), b.l.begin(), b.l.end()); a.r.insert(a.r.end(), b.r.begin(), b.r.end());
            return a;
        };
        const Out g0 = glide_run(0), g1 = glide_run(1);
        check(clean(g1) && diff(g0, g1) > 0.05, "GLIDE destination: a routed MACRO changes the glide");
    }
    {   // every MATRIX source reaches the cutoff (a saw through LP24 at 600 Hz; the source made non-zero)
        int bad = 0;
        for (int src = SS_LFO1; src < SS_COUNT; src++) {
            double bb[2], lv[2];
            for (int on = 0; on < 2; on++) {
                auto s = make(2);
                SynthSettings &y = s->settings;
                y.mix.flt_on.store(1); y.mix.flt_type.store(Synth::FT_LP24); y.mix.cutoff.store(600);
                y.mix.fe_sustain.store(0.8f); y.mod.me_on.store(1); y.mod.me_attack.store(0.01f); y.mod.me_sustain.store(0.8f);
                y.mod.lfo[0].shape.store(3); y.mod.lfo[1].shape.store(3); y.mod.lfo[0].rate_hz.store(0.3f); y.mod.lfo[1].rate_hz.store(0.3f);
                for (int k = 2; k < SYN_LFOS; k++) { y.mod.lfo[k].shape.store(3); y.mod.lfo[k].rate_hz.store(0.3f); }   // 2.4.17
                for (int k = 0; k < SYN_XENVS; k++) { y.mod.mex[k].attack.store(0.01f); y.mod.mex[k].sustain.store(0.8f); }
                for (int k = 0; k < 4; k++) y.mod.macro[k].store(0.8f);
                s->set_wheel(1); s->set_at(1);
                y.mod.mx_src[0].store(src); y.mod.mx_dst[0].store(SD_CUTOFF); y.mod.mx_amt[0].store(on ? 0.8f : 0.0f);
                Out o = run(*s, src == SS_NOTE ? 96 : 57, 0.4);
                bb[on] = bright(o.l, S / 5, S * 2 / 5); lv[on] = rmsr(o.l, S / 5, S * 2 / 5);
            }
            // RANDOM: -1..1 per note; NOTE: (96 - 60) / 60
            if (std::fabs(std::log(bb[1] / bb[0])) < 0.1 && std::fabs(std::log(lv[1] / lv[0])) < 0.1) {   // brighter or louder
                std::printf("  source %d -> cutoff: brightness %.3f vs %.3f, level %.4f vs %.4f\n", src, bb[1], bb[0], lv[1], lv[0]); bad++;
            }
        }
        check(!bad, "MATRIX: every source moves the cutoff");
    }
    {   // LFO on the cutoff (the FILTER route's destination) sweeps the spectrum; LFO 2 on its own (no shared state)
        auto s = make(2);
        s->settings.mix.flt_on.store(1); s->settings.mix.flt_type.store(Synth::FT_LP24); s->settings.mix.cutoff.store(700);
        Out off = run(*s, 45, 1.0);
        s = make(2);
        s->settings.mix.flt_on.store(1); s->settings.mix.flt_type.store(Synth::FT_LP24); s->settings.mix.cutoff.store(700);
        s->settings.mod.lfo[1].dest.store(SD_CUTOFF); s->settings.mod.lfo[1].depth.store(0.8f); s->settings.mod.lfo[1].rate_hz.store(2);
        Out on = run(*s, 45, 1.0);
        double so = spread(off.l, bright), sn = spread(on.l, bright);
        std::printf("synth: LFO 2 on cutoff: brightness spread %.2f (off %.2f)\n", sn, so);
        check(sn > 3 * so, "LFO -> cutoff sweeps the filter");
    }
    {   // the filter envelope's amount moves the cutoff (bright at the start, dark at the end); 0: steady
        double r[2];
        for (int k = 0; k < 2; k++) {
            auto s = make(2);
            s->settings.mix.flt_on.store(1); s->settings.mix.flt_type.store(Synth::FT_LP12); s->settings.mix.cutoff.store(300);
            s->settings.mix.env_amt.store(k ? 0.8f : 0.0f); s->settings.mix.fe_decay.store(0.3f); s->settings.mix.fe_sustain.store(0);
            Out o = run(*s, 45, 0.8);
            r[k] = bright(o.l, 441, 441 * 4) / bright(o.l, S / 2, S * 7 / 10);
        }
        std::printf("synth: filter env 80: early/late brightness %.2f (0: %.2f)\n", r[1], r[0]);
        check(r[1] > 2 && std::fabs(r[0] - 1) < 0.1, "FILTER ENV amount sweeps the cutoff");
    }
    {   // a free-running LFO moves within a block: 1024-frame blocks sound like 64-frame blocks (no block-rate steps)
        for (int trig = 0; trig < 2; trig++) {
            Out o[2];
            for (int k = 0; k < 2; k++) {
                auto s = make(0);
                s->settings.mod.lfo[0].trig.store(trig); s->settings.mod.lfo[0].rate_hz.store(20);
                s->settings.mod.lfo[0].dest.store(SD_LEVEL); s->settings.mod.lfo[0].depth.store(0.8f);
                o[k] = run(*s, 69, 0.5, k ? 1024 : 64);
            }
            double df = diff(o[0], o[1]);
            std::printf("synth: LFO %s, 20 Hz tremolo, 1024 vs 64 frame blocks differ %.4f\n", trig ? "retrig" : "free run", df);
            check(df < 0.01, trig ? "a retrig LFO doesn't depend on the block size" : "a FREE RUN LFO moves within a block (not stepped per block)");
        }
    }
    {   // the MATRIX's LFO 1 RATE speeds a FREE RUN LFO too (it only reached retriggered ones)
        int zc[2];
        for (int k = 0; k < 2; k++) {
            auto s = make(0);
            s->settings.mod.lfo[0].trig.store(0); s->settings.mod.lfo[0].shape.store(3); s->settings.mod.lfo[0].rate_hz.store(4);
            s->settings.mod.lfo[0].dest.store(SD_PAN); s->settings.mod.lfo[0].depth.store(1);
            s->settings.mod.macro[0].store(1);
            s->settings.mod.mx_src[0].store(SS_MACRO1); s->settings.mod.mx_dst[0].store(SD_LFO1_RATE); s->settings.mod.mx_amt[0].store(k ? 1.0f : 0.0f);
            Out o = run(*s, 69, 1.0);
            int c = 0; bool left = false;   // square LFO on pan: count the swings (left louder <-> right louder per 5 ms)
            for (size_t b0 = 2205; b0 + 220 <= o.l.size(); b0 += 220) {
                bool lf = rmsr(o.l, b0, b0 + 220) > rmsr(o.r, b0, b0 + 220);
                if (b0 > 2205 && lf != left) c++;
                left = lf;
            }
            zc[k] = c;
        }
        std::printf("synth: free-run LFO 1 at 4 Hz: %d swings, with MATRIX LFO 1 RATE +100: %d\n", zc[0], zc[1]);
        check(zc[1] > zc[0] * 4, "MATRIX -> LFO 1 RATE speeds up a FREE RUN LFO");
    }
    {   // pitch modulation moves SUB with the oscillators (a wheel -> pitch +12 st: the sub an octave up)
        int c[2];
        for (int k = 0; k < 2; k++) {
            auto s = make(0);
            s->settings.osc[0].on.store(0);
            s->settings.mix.sub_on.store(1); s->settings.mix.sub_table.store(classic(0)); s->settings.mix.sub_level.store(1);
            s->settings.mod.mx_src[0].store(SS_WHEEL); s->settings.mod.mx_dst[0].store(SD_PITCH); s->settings.mod.mx_amt[0].store(1);
            s->set_wheel(float(k));
            Out o = run(*s, 69, 0.5);
            c[k] = 0;
            for (size_t i = S / 10 + 1; i < S / 2; i++) c[k] += (o.l[i - 1] < 0) != (o.l[i] < 0);
        }
        std::printf("synth: SUB crossings %d, wheel -> pitch +12: %d\n", c[0], c[1]);
        check(std::abs(c[1] - 2 * c[0]) < 4, "pitch modulation moves SUB too (it stays an octave under)");
    }
    {   // PAN modulation pans the oscillators as far as SUB (once, not twice)
        auto s = make(0);
        s->settings.mix.sub_on.store(1); s->settings.mix.sub_table.store(classic(0)); s->settings.mix.sub_level.store(1);
        s->settings.osc[0].level.store(0);
        s->settings.mod.macro[0].store(0.5f); s->settings.mod.macro_dest[0].store(SD_PAN);
        Out o = run(*s, 69, 0.3);
        double rs = rmsr(o.l, S / 10, S * 3 / 10) / rmsr(o.r, S / 10, S * 3 / 10);
        s = make(0);
        s->settings.mod.macro[0].store(0.5f); s->settings.mod.macro_dest[0].store(SD_PAN);
        o = run(*s, 69, 0.3);
        double ro = rmsr(o.l, S / 10, S * 3 / 10) / rmsr(o.r, S / 10, S * 3 / 10);
        std::printf("synth: PAN +50 L/R: sub %.3f, osc %.3f\n", rs, ro);
        check(std::fabs(ro / rs - 1) < 0.05, "PAN modulation pans the oscillators once (as SUB)");
    }
    // ---- the filters ----
    auto gain = [&](int type, float cutoff, float res, int note, Out *keep_out = nullptr) {
        double g[2];
        for (int f = 0; f < 2; f++) {
            auto s = make(0);
            s->settings.osc[0].level.store(0.5f);
            s->settings.mix.flt_on.store(f); s->settings.mix.flt_type.store(type); s->settings.mix.cutoff.store(cutoff); s->settings.mix.res.store(res);
            Out o = run(*s, note, 0.5);
            g[f] = rmsr(o.l, S / 4, S / 2);
            if (f && keep_out) *keep_out = o;
        }
        return db(g[1] / g[0]);
    };
    {
        const char *names[] = {"LP 12", "LP 24", "Ladder", "BP", "HP", "Notch", "Comb"};
        const int notes[] = {45, 57, 69, 81, 93, 105, 117};   // 110 .. 7040 Hz (octaves; the cutoff at 880 = note 81)
        double r[7][7];
        for (int t = 0; t < 7; t++) {
            std::printf("synth: %-6s @880 Hz res 0:", names[t]);
            for (int n = 0; n < 7; n++) { r[t][n] = gain(t, 880, 0, notes[n]); std::printf(" %6.1f", r[t][n]); }
            std::printf(" dB (110 .. 7040 Hz)\n");
        }
        // the slopes an octave pair well past the cutoff (2 to 4 octaves away)
        const double lp12 = r[0][5] - r[0][6], lp24 = r[1][5] - r[1][6], lad = r[2][5] - r[2][6];
        const double hp = r[4][1] - r[4][0], bpl = r[3][1] - r[3][0], bph = r[3][5] - r[3][6];
        std::printf("synth: slopes per octave: LP12 %.1f, LP24 %.1f, Ladder %.1f, HP %.1f, BP %.1f / %.1f dB\n", lp12, lp24, lad, hp, bpl, bph);
        check(lp12 > 10 && lp12 < 14 && r[0][0] > -1 && r[0][1] > -1, "LP 12: flat below, -12 dB/oct above");
        check(lp24 > 21 && lp24 < 27 && r[1][0] > -1.5, "LP 24: flat below, -24 dB/oct above");
        check(lad > 21 && lad < 27 && r[2][0] > -7, "Ladder: -24 dB/oct above");
        check(hp > 10 && hp < 14 && r[4][6] > -1, "HP: flat above, -12 dB/oct below");
        check(bpl > 5 && bpl < 7 && bph > 5 && bph < 7 && r[3][3] > -1.5, "BP: 0 dB at the cutoff, -6 dB/oct each side");
        check(r[5][3] < -30 && r[5][0] > -1 && r[5][6] > -1, "Notch: deep at the cutoff, flat away from it");
        double comb_on = gain(6, 880, 0.8f, 81), comb_off = gain(6, 880, 0.8f, 76);
        std::printf("synth: Comb res 80: %.1f dB on its tooth (880 Hz), %.1f dB between (659 Hz)\n", comb_on, comb_off);
        check(comb_on - comb_off > 10, "Comb: peaks at the cutoff's harmonics");
    }
    {   // resonance: a peak at the cutoff, about the same at 50 % as before; near max still bounded, finite and clean
        for (int t : {Synth::FT_LP12, Synth::FT_LP24, Synth::FT_LADDER}) {
            double p50 = gain(t, 880, 0.5f, 81), p90 = gain(t, 880, 0.9f, 81);
            Out o;
            gain(t, 880, 1.0f, 81, &o);
            double pk = 0; for (float x : o.l) pk = std::max(pk, double(std::fabs(x)));
            std::printf("synth: type %d at the cutoff: res 50 %.1f dB, res 90 %.1f dB; res 100 peak %.2f\n", t, p50, p90, pk);
            check(p90 > p50 + 3 && p90 > 6 && pk < 8 && clean(o), "resonance peaks at the cutoff, bounded at max");
        }
        // the ladder at 100 % rings on at the cutoff by itself (input a quiet sine an octave below, its level 1 %)
        auto s = make(0);
        s->settings.osc[0].level.store(0.01f);
        s->settings.mix.flt_on.store(1); s->settings.mix.flt_type.store(Synth::FT_LADDER); s->settings.mix.cutoff.store(880); s->settings.mix.res.store(1);
        Out o = run(*s, 57, 1.0);
        const double ring = goertzel(o.l, S / 2, 8192, 880), in = goertzel(o.l, S / 2, 8192, 440);
        std::printf("synth: ladder res 100, 1 %% input at 440: 880 Hz level %.3f, 440 %.4f\n", ring, in);
        check(ring > 0.05 && ring < 4 && clean(o), "the ladder self-oscillates at max resonance (bounded)");
    }
    {   // cutoff modulation at audio-ish rates: no zipper (16-sample steps would put images round SR / 16 = 2756 Hz)
        for (int t : {Synth::FT_LP12, Synth::FT_LADDER}) {
            auto s = make(0);
            s->settings.mix.flt_on.store(1); s->settings.mix.flt_type.store(t); s->settings.mix.cutoff.store(330);
            s->settings.mod.lfo[0].rate_hz.store(30); s->settings.mod.lfo[0].dest.store(SD_CUTOFF); s->settings.mod.lfo[0].depth.store(0.5f);
            Out o = run(*s, 57, 0.6);   // a 220 Hz sine
            const size_t a = S / 5;
            const double f0 = goertzel(o.l, a, 16384, 220);
            double img = 0;
            for (double hz : {2756.25 - 220, 2756.25 + 220, 5512.5 - 220, 5512.5 + 220}) img = std::max(img, goertzel(o.l, a, 16384, hz));
            std::printf("synth: type %d, cutoff LFO 30 Hz +-2 oct: chunk-rate images %.1f dB under the tone\n", t, db(f0 / img));
            check(db(f0 / img) > 70 && clean(o), "cutoff modulation doesn't zipper");
        }
    }
    {   // nothing left after a release: no NaN, no denormals, for every type at max resonance
        bool ok = true;
        for (int t = 0; t < 7; t++) {
            auto s = make(2);
            s->settings.release.store(0.05f);
            s->settings.mix.flt_on.store(1); s->settings.mix.flt_type.store(t); s->settings.mix.cutoff.store(2000); s->settings.mix.res.store(1);
            s->settings.mix.drive.store(1);
            Out o = run(*s, 33, 0.6, 128, [](Synth &y, int b) { if (b == 100) y.note_off(33); });
            ok = ok && clean(o);
        }
        check(ok, "every filter at max resonance and drive: finite, no denormals");
    }
    {   // CPU: 12 voices, saw x 3 unison through each filter, 128-frame blocks
        for (int t : {Synth::FT_LP12, Synth::FT_LP24, Synth::FT_LADDER}) {
            auto s = make(2);
            s->settings.polyphony.store(12);
            s->settings.osc[0].unison.store(3);
            s->settings.mix.flt_on.store(t >= 0); s->settings.mix.flt_type.store(t); s->settings.mix.cutoff.store(1200); s->settings.mix.res.store(0.5f);
            s->settings.mod.lfo[0].dest.store(SD_CUTOFF); s->settings.mod.lfo[0].depth.store(0.3f);
            for (int n = 0; n < 12; n++) s->note_on(48 + n * 2, 100);
            std::vector<float> l(128), r(128);
            auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < int(10 * 44100 / 128); b++) s->render(l.data(), r.data(), 128);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("synth: CPU 12 voices x 3 unison, filter %d: %.0f ms per 10 s\n", t, ms);
        }
    }
}
}  // namespace syt

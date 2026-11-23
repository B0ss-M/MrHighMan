// Omni Sampler: DSP building blocks (filters, envelope, LFO, reverb, the FX page). 44.1 kHz, float.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>
#include "../core/model.hpp"

namespace omni {

constexpr float SR = 44100.0f;
constexpr float PI_F = 3.14159265358979f;

// Topology-preserving state variable filter (Zavalishin), 12 dB/oct. Coefficients per block.
struct Svf {
    float ic1 = 0, ic2 = 0, g = 0, k = 2, a1 = 0, a2 = 0, a3 = 0;
    void set(float hz, float res01) {
        if (hz < 16.0f) hz = 16.0f;
        if (hz > SR * 0.45f) hz = SR * 0.45f;
        g = std::tan(PI_F * hz / SR);
        k = 2.0f - 1.96f * (res01 < 0 ? 0 : res01 > 1 ? 1 : res01);   // k = 1/Q, close to self-oscillation at 1
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    // mode: 0 LP, 1 HP, 2 BP, 3 notch
    inline float tick(float x, int mode) {
        float v3 = x - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        switch (mode) {
        case 0: return v2;
        case 1: return x - k * v1 - v2;
        case 2: return v1;
        default: return x - k * v1;
        }
    }
    void reset() { ic1 = ic2 = 0; }
};

struct OnePole {
    float z = 0, a = 0;
    void set(float hz) {
        if (hz > SR * 0.45f) hz = SR * 0.45f;
        a = 1.0f - std::exp(-2.0f * PI_F * hz / SR);
    }
    inline float lp(float x) { z += a * (x - z); return z; }
    inline float hp(float x) { return x - lp(x); }
    void reset() { z = 0; }
};

// A zone filter: 1 pole (one-pole), 2 (SVF), 3 (SVF + one-pole: the Akai S1000's 18 dB), 4 (two SVFs) or more.
struct VoiceFilter {
    Svf a, b;
    OnePole p;
    int mode = 0, poles = 2;   // mode as Svf::tick
    bool on = false;
    void setup(FilterType t, int npoles) {
        on = t != FilterType::None;
        mode = t == FilterType::HighPass ? 1 : t == FilterType::BandPass ? 2 : t == FilterType::BandReject ? 3 : 0;
        poles = npoles < 1 ? 2 : npoles > 8 ? 8 : npoles;
        a.reset(); b.reset(); p.reset();
    }
    // 2.4.7: only the stages the pole count uses (the one-pole's exp and the SVF's tan ran for every filter, every block)
    void set(float hz, float res) {
        if (poles != 1) a.set(hz, res);
        if (poles >= 4) b.set(hz, res * 0.5f);
        if (poles == 1 || poles == 3) p.set(hz);
    }
    // 2.4.7: another channel's coefficients (same cutoff / resonance), its own state: a stereo zone's second channel
    // takes them instead of computing the same tan / exp again
    void copy_coefs(const VoiceFilter &o) {
        a.g = o.a.g; a.k = o.a.k; a.a1 = o.a.a1; a.a2 = o.a.a2; a.a3 = o.a.a3;
        b.g = o.b.g; b.k = o.b.k; b.a1 = o.b.a1; b.a2 = o.b.a2; b.a3 = o.b.a3;
        p.a = o.p.a;
    }
    inline float tick(float x) {
        if (!on) return x;
        if (poles == 1) return mode == 1 ? p.hp(x) : p.lp(x);
        float y = a.tick(x, mode);
        if (poles == 3) y = mode == 1 ? p.hp(y) : p.lp(y);
        else if (poles >= 4) y = b.tick(y, mode);
        return y;
    }
};

// Shape a 0..1 ramp. slope 0 = linear; > 0 slow start (exponential rise); < 0 fast start (logarithmic).
// x^e: whole exponents (the common slopes +-1, +-0.5, +-0.25 give 5, 3, 2) by multiplication, since this runs per
// sample through curved envelope stages and std::pow is costly on ARMv7.
inline float pow_curve(float x, float e) {
    int n = int(e);
    if (float(n) == e && n >= 1 && n <= 8) {
        float r = x;
        for (int k = 1; k < n; k++) r *= x;
        return r;
    }
    return std::pow(x, e);
}

inline float shape(float t, float slope) {
    if (slope == 0) return t;
    if (slope > 0) return pow_curve(t, 1.0f + 4.0f * slope);
    return 1.0f - pow_curve(1.0f - t, 1.0f - 4.0f * slope);
}

// DAHDSR envelope, evaluated per sample with a precomputed per-stage increment.
struct Env {
    enum Stage { Idle, Delay, Attack, Hold, Decay, Sustain, Release, Slope };
    Stage stage = Idle;
    float t = 0, inc = 0, level = 0, from = 0, start = 0, sustain = 1, end_level = 0;
    float s_attack = 0, s_decay = 0, s_release = 0;
    float delay_s = 0, attack_s = 0, hold_s = 0, decay_s = 0, release_s = 0;

    float peak = 1, breakpoint = -1, slope_s = 0;

    static float step(float seconds) { return seconds <= 0.0005f ? 1.0f : 1.0f / (seconds * SR); }
    void begin(const Envelope &e, float time_scale_attack, float time_scale, float add_attack, float add_release,
               float sustain_scale, float sustain_add = 0) {
        peak = float(e.attack_level);
        breakpoint = float(e.break_level);
        slope_s = float(e.slope_time) * time_scale;
        start = float(e.start_level);
        sustain = float(e.sustain) * sustain_scale + sustain_add;
        if (sustain < 0) sustain = 0;
        if (sustain > 1) sustain = 1;
        end_level = float(e.end_level);
        s_attack = float(e.attack_slope); s_decay = float(e.decay_slope); s_release = float(e.release_slope);
        delay_s = float(e.delay) * time_scale;
        attack_s = float(e.attack) * time_scale_attack + add_attack;
        hold_s = float(e.hold) * time_scale;
        decay_s = float(e.decay) * time_scale;
        release_s = float(e.release) * time_scale + add_release;
        level = start;
        enter(delay_s > 0 ? Delay : Attack);
    }
    void enter(Stage s) {
        stage = s;
        t = 0;
        from = level;
        switch (s) {
        case Delay: inc = step(delay_s); break;
        case Attack: inc = step(attack_s); break;
        case Hold: inc = step(hold_s); break;
        case Decay: inc = step(decay_s); break;
        case Slope: inc = step(slope_s); break;
        case Release: inc = step(release_s); break;
        default: inc = 0; break;
        }
        if (s == Hold && hold_s <= 0) enter(Decay);
    }
    void release() { if (stage != Idle && stage != Release) enter(Release); }
    void fast_release(float seconds) { if (stage != Idle) { release_s = seconds; s_release = 0; enter(Release); } }
    inline float tick() {
        switch (stage) {
        case Idle: return 0;
        case Delay: t += inc; if (t >= 1) enter(Attack); return level;
        case Attack:
            t += inc;
            if (t >= 1) { level = peak; enter(Hold); }
            else level = from + (peak - from) * shape(t, s_attack);
            return level;
        case Hold: t += inc; if (t >= 1) enter(Decay); return level;
        case Decay:
            t += inc;
            if (breakpoint >= 0) {
                level = t >= 1 ? breakpoint : peak + (breakpoint - peak) * shape(t, s_decay);
                if (t >= 1) enter(Slope);
            } else {
                level = t >= 1 ? sustain : peak + (sustain - peak) * shape(t, s_decay);
                if (t >= 1) stage = Sustain;
            }
            return level;
        case Slope:
            t += inc;
            level = t >= 1 ? sustain : breakpoint + (sustain - breakpoint) * shape(t, s_decay);
            if (t >= 1) stage = Sustain;
            return level;
        case Sustain: return level;
        case Release:
            t += inc;
            if (t >= 1) { level = end_level; stage = Idle; }
            else level = from + (end_level - from) * shape(t, s_release);
            return level;
        }
        return 0;
    }
    // 2.4.11 (audit M6): n samples at once, for an envelope nobody reads this chunk (the synth's filter / mod envelope
    // when unrouted): t steps exactly as n tick() calls step it (same stage changes on the same sample), but the curve
    // (shape(): the costly part) is computed once, for where it stops: the same level as n tick() calls
    inline void tick_n(int n) {
        if (breakpoint >= 0) { while (n-- > 0) tick(); return; }
        for (; n > 0; n--) {
            switch (stage) {
            case Slope: case Idle: case Sustain: n = 0; break;
            case Delay: t += inc; if (t >= 1) enter(Attack); break;
            case Attack: t += inc; if (t >= 1) { level = peak; enter(Hold); } break;
            case Hold: t += inc; if (t >= 1) enter(Decay); break;
            case Decay: t += inc; if (t >= 1) { level = sustain; stage = Sustain; } break;
            case Release: t += inc; if (t >= 1) { level = end_level; stage = Idle; } break;
            }
        }
        switch (stage) {
        case Attack: level = from + (peak - from) * shape(t, s_attack); break;
        case Decay: level = 1 + (sustain - 1) * shape(t, s_decay); break;
        case Release: level = from + (end_level - from) * shape(t, s_release); break;
        default: break;
        }
    }
    bool done() const { return stage == Idle; }
};

struct LfoState {
    float phase = 0, inc = 0, value = 0, rnd = 0, delay_left = 0, fade = 1, fade_inc = 1;
    LfoWave wave = LfoWave::Triangle;
    uint32_t seed = 22222;
    void begin(const Lfo &l, uint32_t s) {
        wave = l.wave;
        inc = float(l.rate_hz) / SR;
        phase = float(l.start_phase);
        delay_left = float(l.delay) * SR;
        fade = l.fade_in > 0 ? 0.0f : 1.0f;
        fade_inc = l.fade_in > 0 ? 1.0f / float(l.fade_in * SR) : 1.0f;
        seed = s * 2654435761u + 1;
        rnd = 0;
    }
    // advance by n samples, return the value at the end (control rate)
    float advance(int n) {
        if (delay_left > 0) { delay_left -= float(n); return 0; }
        phase += inc * float(n);
        if (phase >= 1) {
            phase -= std::floor(phase);
            seed = seed * 1664525u + 1013904223u;
            rnd = float(int32_t(seed)) / 2147483648.0f;
        }
        fade += fade_inc * float(n);
        if (fade > 1) fade = 1;
        float v;
        switch (wave) {
        case LfoWave::Sine: v = std::sin(2 * PI_F * phase); break;
        case LfoWave::Square: v = phase < 0.5f ? 1.0f : -1.0f; break;
        case LfoWave::SawUp: v = 2 * phase - 1; break;
        case LfoWave::SawDown: v = 1 - 2 * phase; break;
        case LfoWave::Random: v = rnd; break;
        default: v = phase < 0.25f ? 4 * phase : phase < 0.75f ? 2 - 4 * phase : 4 * phase - 4; break;
        }
        return value = v * fade;
    }
};

// Freeverb (Jezar, public domain) at 44.1 kHz.
struct Reverb {
    struct Comb { std::vector<float> buf; size_t i = 0; float store = 0;
        inline float tick(float x, float fb, float d1, float d2) {
            float y = buf[i];
            store = y * d2 + store * d1;
            buf[i] = x + store * fb;
            if (++i >= buf.size()) i = 0;
            return y;
        } };
    struct Allpass { std::vector<float> buf; size_t i = 0;
        inline float tick(float x) {
            float b = buf[i];
            buf[i] = x + b * 0.5f;
            if (++i >= buf.size()) i = 0;
            return b - x;
        } };
    Comb cl[8], cr[8];
    Allpass al[4], ar[4];
    float fb = 0.84f, d1 = 0.2f, d2 = 0.8f;
    Reverb() {
        static const int combs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int aps[4] = {556, 441, 341, 225};
        for (int i = 0; i < 8; i++) { cl[i].buf.assign(size_t(combs[i]), 0); cr[i].buf.assign(size_t(combs[i] + 23), 0); }
        for (int i = 0; i < 4; i++) { al[i].buf.assign(size_t(aps[i]), 0); ar[i].buf.assign(size_t(aps[i] + 23), 0); }
    }
    void set(float size01, float damp01) {
        fb = 0.7f + 0.28f * size01;
        d1 = 0.4f * damp01;
        d2 = 1 - d1;
    }
    void clear() {
        for (auto &c : cl) std::fill(c.buf.begin(), c.buf.end(), 0.f);
        for (auto &c : cr) std::fill(c.buf.begin(), c.buf.end(), 0.f);
        for (auto &a : al) std::fill(a.buf.begin(), a.buf.end(), 0.f);
        for (auto &a : ar) std::fill(a.buf.begin(), a.buf.end(), 0.f);
    }
    inline void tick(float inl, float inr, float &outl, float &outr) {
        float in = (inl + inr) * 0.015f, l = 0, r = 0;
        for (int i = 0; i < 8; i++) { l += cl[i].tick(in, fb, d1, d2); r += cr[i].tick(in, fb, d1, d2); }
        for (int i = 0; i < 4; i++) { l = al[i].tick(l); r = ar[i].tick(r); }
        outl = l; outr = r;
    }
};

// 2.2: PLATE / HALL / ROOM: Jon Dattorro's plate ("Effect Design, Part 1", J. Audio Eng. Soc. 45(9), 1997): a 120 Hz
// low cut and pre-delay, a bandwidth low-pass, four input diffusers, then a figure-of-eight tank of two halves (a
// modulated all-pass, a delay, damping, an all-pass, a delay) whose outputs are tapped at seven points per side. His
// delays are at 29761 Hz: scaled to 44.1 kHz and by the type's size (Room 0.55, Plate 1, Hall 1.5). Next to Freeverb
// (Classic) it has no metallic comb ringing. Cost on a Live II (2026-11-11): about 1.3% of a core, Freeverb 0.9%.
struct PlateReverb {
    struct Line {
        std::vector<float> b;
        int w = 0, mask = 0;
        void init(int n) { int s = 1; while (s < n + 4) s <<= 1; b.assign(size_t(s), 0.f); mask = s - 1; w = 0; }
        inline float at(int d) const { return b[size_t((w - 1 - d) & mask)]; }          // d = 0: the last one pushed
        inline float atf(float d) const { int i = int(d); float f = d - float(i); float a = at(i); return a + (at(i + 1) - a) * f; }
        inline void push(float x) { b[size_t(w)] = x; w = (w + 1) & mask; }
        void clear() { std::fill(b.begin(), b.end(), 0.f); }
    };
    // all-pass of n samples: w = x - g z, y = z + g w (Dattorro's sign convention; the tank's first one has g < 0)
    static inline float allpass(Line &L, int n, float g, float x) {
        float z = L.at(n - 1), w = x - g * z;
        L.push(w);
        return z + g * w;
    }
    static constexpr float MAX_SCALE = 1.5f * 44100.0f / 29761.0f;
    enum { IN1, IN2, IN3, IN4, AP_A, DEL_A1, AP_A2, DEL_A2, AP_B, DEL_B1, AP_B2, DEL_B2, NLINES };
    static constexpr int BASE[NLINES] = {142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163};
    // the output taps (Dattorro's table 2): line, position, sign; left then right
    struct Tap { int line, pos; float sign; };
    static constexpr Tap TAPS_L[7] = {{DEL_B1, 266, 1}, {DEL_B1, 2974, 1}, {AP_B2, 1913, -1}, {DEL_B2, 1996, 1},
                                      {DEL_A1, 1990, -1}, {AP_A2, 187, -1}, {DEL_A2, 1066, -1}};
    static constexpr Tap TAPS_R[7] = {{DEL_A1, 353, 1}, {DEL_A1, 3627, 1}, {AP_A2, 1228, -1}, {DEL_A2, 2673, 1},
                                      {DEL_B1, 2111, -1}, {AP_B2, 335, -1}, {DEL_B2, 121, -1}};
    Line line[NLINES], pre;
    int len[NLINES] = {}, tl[7] = {}, tr[7] = {};
    float exc = 16, decay = 0.5f, damp = 0.3f, bw = 0.9995f, gain = 0.6f;
    float hp_x = 0, hp_y = 0, bw_y = 0, damp_a = 0, damp_b = 0, out_a = 0, out_b = 0;
    float sa = 0, ca = 1, sb = 0, cb = 1, rot_c = 1, rot_s = 0, rot_c2 = 1, rot_s2 = 0;   // the two tank LFOs
    float t_half = 0.36f;   // one half of the tank, seconds (its decay is applied twice on the way round)
    int predelay = 0, type = 1, renorm = 0;
    PlateReverb() {
        for (int i = 0; i < NLINES; i++) line[i].init(int(float(BASE[i]) * MAX_SCALE) + 64);
        pre.init(int(0.25f * SR) + 4);
        set_type(1);
    }
    // 1 Plate, 2 Hall, 3 Room
    void set_type(int t) {
        type = t;
        const float size = t == 2 ? 1.5f : t == 3 ? 0.55f : 1.0f, s = size * 44100.0f / 29761.0f;
        for (int i = 0; i < NLINES; i++) len[i] = std::max(2, int(float(BASE[i]) * s));
        t_half = float(len[AP_A] + len[DEL_A1] + len[AP_A2] + len[DEL_A2] + len[AP_B] + len[DEL_B1] + len[AP_B2] + len[DEL_B2]) / 2 / SR;
        for (int k = 0; k < 7; k++) {
            tl[k] = std::min(len[TAPS_L[k].line] - 1, int(float(TAPS_L[k].pos) * s));
            tr[k] = std::min(len[TAPS_R[k].line] - 1, int(float(TAPS_R[k].pos) * s));
        }
        exc = (t == 2 ? 20.0f : 12.0f) * 44100.0f / 29761.0f;                // modulation depth, samples
        const float hz_a = t == 2 ? 0.7f : 1.0f, hz_b = hz_a * 0.83f;          // two rates: the halves never lock
        rot_c = std::cos(2 * float(M_PI) * hz_a / SR); rot_s = std::sin(2 * float(M_PI) * hz_a / SR);
        rot_c2 = std::cos(2 * float(M_PI) * hz_b / SR); rot_s2 = std::sin(2 * float(M_PI) * hz_b / SR);
    }
    // size 0..1 -> the decay time (RT60, exponential over the type's range: Room 0.25-2.5 s, Plate 0.5-7 s, Hall
    // 1.2-10 s; before damping), as the tank's gain per half; damp 0..1 -> high-frequency loss in the tank and at the
    // input; pre-delay in seconds
    void set(float size01, float damp01, float predelay_s) {
        const float lo = type == 2 ? 1.2f : type == 3 ? 0.25f : 0.5f, hi = type == 2 ? 10.0f : type == 3 ? 2.5f : 7.0f;
        const float rt = lo * std::pow(hi / lo, std::max(0.0f, std::min(1.0f, size01)));
        decay = std::min(0.98f, std::sqrt(std::pow(10.0f, -3.0f * t_half / rt)));
        damp = 0.0005f + 0.75f * std::max(0.0f, std::min(1.0f, damp01));
        bw = 0.9995f - 0.35f * std::max(0.0f, std::min(1.0f, damp01));
        predelay = std::max(0, std::min(pre.mask - 2, int(predelay_s * SR)));
        gain = type == 3 ? 0.75f : 0.6f;
    }
    void clear() {
        for (auto &l : line) l.clear();
        pre.clear();
        hp_x = hp_y = bw_y = damp_a = damp_b = out_a = out_b = 0;
        sa = sb = 0; ca = cb = 1;
    }
    inline void tick(float inl, float inr, float &outl, float &outr) {
        float x = (inl + inr) * 0.5f;
        hp_y = 0.983f * (hp_y + x - hp_x); hp_x = x;                          // 120 Hz low cut: no mud in the tail
        pre.push(hp_y);
        x = pre.at(predelay);
        bw_y += bw * (x - bw_y); x = bw_y;
        x = allpass(line[IN1], len[IN1], 0.75f, x);
        x = allpass(line[IN2], len[IN2], 0.75f, x);
        x = allpass(line[IN3], len[IN3], 0.625f, x);
        x = allpass(line[IN4], len[IN4], 0.625f, x);
        // the LFOs: rotating phasors, renormalised now and then
        float ns = sa * rot_c + ca * rot_s; ca = ca * rot_c - sa * rot_s; sa = ns;
        ns = sb * rot_c2 + cb * rot_s2; cb = cb * rot_c2 - sb * rot_s2; sb = ns;
        if (++renorm >= 4096) {
            renorm = 0;
            float m = 1 / std::sqrt(sa * sa + ca * ca); sa *= m; ca *= m;
            m = 1 / std::sqrt(sb * sb + cb * cb); sb *= m; cb *= m;
        }
        const float in_a = x + decay * out_b, in_b = x + decay * out_a;
        {   // half A
            Line &L = line[AP_A];
            float z = L.atf(float(len[AP_A] - 1) + exc * sa), w = in_a + 0.7f * z;
            L.push(w);
            float y = z - 0.7f * w;
            float d = line[DEL_A1].at(len[DEL_A1] - 1); line[DEL_A1].push(y);
            damp_a = d * (1 - damp) + damp_a * damp + 1e-20f;
            float u = allpass(line[AP_A2], len[AP_A2], 0.5f, damp_a * decay);
            out_a = line[DEL_A2].at(len[DEL_A2] - 1); line[DEL_A2].push(u);
        }
        {   // half B
            Line &L = line[AP_B];
            float z = L.atf(float(len[AP_B] - 1) + exc * sb), w = in_b + 0.7f * z;
            L.push(w);
            float y = z - 0.7f * w;
            float d = line[DEL_B1].at(len[DEL_B1] - 1); line[DEL_B1].push(y);
            damp_b = d * (1 - damp) + damp_b * damp + 1e-20f;
            float u = allpass(line[AP_B2], len[AP_B2], 0.5f, damp_b * decay);
            out_b = line[DEL_B2].at(len[DEL_B2] - 1); line[DEL_B2].push(u);
        }
        float l = 0, r = 0;
        for (int k = 0; k < 7; k++) {
            l += TAPS_L[k].sign * line[TAPS_L[k].line].at(tl[k]);
            r += TAPS_R[k].sign * line[TAPS_R[k].line].at(tr[k]);
        }
        outl = l * gain; outr = r * gain;
    }
};

// tanh for the drive: a rational fit (exact at 0, within 2% to |x| = 3, 1 beyond). libm's tanhf costs tens of cycles
// per call on ARMv7, and the drive ran two per sample and channel.
inline float fast_tanh(float x) {
    if (x > 3) return 1;
    if (x < -3) return -1;
    float x2 = x * x;
    return x * (27 + x2) / (27 + 9 * x2);
}

// 2.2: DRIVE at twice the sample rate: a 31-tap half-band FIR (Kaiser window, beta 8) up, the shaper on both
// samples, the same filter down; the shaper itself first-order antiderivative anti-aliased (ADAA: the mean of the
// curve between two samples, from its integral). A shaper's harmonics above 22 kHz used to fold back as inharmonic
// noise; a 7 kHz sine at full drive (Classic) had its aliases at -14 dB, now -46 dB (Tape -50, Tube -44, Fold -70). 15 samples of delay (0.34 ms) while a type is chosen. Types: 1 Classic (1.0's tanh), 2 Tape
// (a softer knee, the highs rounded), 3 Tube (asymmetric: even harmonics, the DC taken off), 4 Fold (a sine
// wavefolder). Amount 0..1 is the input gain 1..9, each type normalised to its level at full scale.
struct Drive {
    static constexpr int HB = 16;            // odd taps of the half-band (n = -15, -13 .. 15)
    float g[HB] = {};                        // h(2m - 15)
    struct Chan {
        float x[2 * HB] = {}, a[2 * HB] = {}, b[16] = {};   // input, shaped half-sample points, shaped sample points
        int xi = 0, ai = 0, bi = 0;
        float lp = 0, dc_x = 0, dc_y = 0;
        double s1 = 0, f1 = 0;   // ADAA: the last shaper input (scaled) and the curve's integral there
    } ch[2];
    int type = 1;
    float k = 1, norm = 1;
    Drive() {
        auto bessel0 = [](double x) { double s = 1, t = 1; for (int i = 1; i < 30; i++) { t *= (x / (2 * i)) * (x / (2 * i)); s += t; } return s; };
        const double beta = 8.0;
        for (int m = 0; m < HB; m++) {
            int n = 2 * m - 15;
            double sinc = std::sin(M_PI * n / 2.0) / (M_PI * n), r = double(n) / 16.0;
            g[m] = float(sinc * bessel0(beta * std::sqrt(1 - r * r)) / bessel0(beta));
        }
    }
    void clear() { ch[0] = Chan(); ch[1] = Chan(); }
    static constexpr double BIAS = 0.35, HALF_PI = 1.5707963267948966;
    // fast_tanh's integral: x^2/18 + 4/3 ln(x^2 + 3) inside +-3, then |x| on (continuous at 3)
    static inline double tanh_int(double x) {
        double a = std::fabs(x);
        return a <= 3 ? x * x / 18 + 4.0 / 3.0 * std::log(x * x + 3) : a - 3 + (0.5 + 4.0 / 3.0 * std::log(12.0));
    }
    // the curve (scaled input s = v k) and its integral, unnormalised
    inline double curve(double s) const {
        switch (type) {
        case 2: return s / (1 + std::fabs(s));                                                  // Tape
        case 3: return double(fast_tanh(float(s + BIAS))) - double(fast_tanh(float(BIAS)));    // Tube
        case 4: return std::sin(s * HALF_PI);                                                   // Fold
        default: return double(fast_tanh(float(s)));                                            // Classic
        }
    }
    inline double integral(double s) const {
        switch (type) {
        case 2: { double a = std::fabs(s); return a - std::log1p(a); }
        case 3: return tanh_int(s + BIAS) - double(fast_tanh(float(BIAS))) * s;
        case 4: return -std::cos(s * HALF_PI) / HALF_PI;
        default: return tanh_int(s);
        }
    }
    inline float shape(Chan &c, float v) const {
        double sv = double(v) * k, fv = integral(sv), d = sv - c.s1;
        double y = std::fabs(d) > 1e-5 ? (fv - c.f1) / d : curve(0.5 * (sv + c.s1));
        c.s1 = sv; c.f1 = fv;
        return float(y) * norm;
    }
    void set(int t, float amount01) {
        type = t;
        k = 1 + (type == 4 ? 3.0f : 8.0f) * std::max(0.0f, std::min(1.0f, amount01));   // Fold: up to two folds
        switch (type) {
        case 2: norm = (1 + k) / k; break;
        case 3: {   // the asymmetric curve's larger side (the negative one) at full scale
            const float tb = fast_tanh(float(BIAS));
            norm = 1 / std::max(1e-3f, std::max(fast_tanh(k + float(BIAS)) - tb, fast_tanh(k - float(BIAS)) + tb));
            break;
        }
        case 4: norm = 1; break;
        default: norm = 1 / std::tanh(k); break;
        }
    }
    inline float tick1(Chan &c, float in) {
        // up: the sample point (15 samples back... its centre) and the half-sample point between
        c.x[c.xi] = c.x[c.xi + HB] = in;
        c.xi = (c.xi + 1) % HB;
        const float *xs = c.x + c.xi;                  // xs[HB - 1] = newest
        float half = 0;
        for (int m = 0; m < HB; m++) half += g[m] * xs[HB - 1 - m];
        half *= 2;
        float pt = xs[HB - 1 - 7];
        float sa = shape(c, half), sb = shape(c, pt);
        if (type == 2) {                               // Tape: a gentle low-pass at 2x (about 9 kHz)
            c.lp += 0.5f * (sa - c.lp); sa = c.lp;
            c.lp += 0.5f * (sb - c.lp); sb = c.lp;
        }
        // down: the half-band over the shaped stream, every other sample
        c.a[c.ai] = c.a[c.ai + HB] = sa;
        c.ai = (c.ai + 1) % HB;
        c.b[c.bi] = sb;
        c.bi = (c.bi + 1) & 15;
        const float *as = c.a + c.ai;
        float y = 0.5f * c.b[(c.bi + 16 - 1 - 8) & 15];
        for (int m = 0; m < HB; m++) y += g[m] * as[HB - 1 - m];
        if (type == 3) {                               // Tube: the asymmetry's DC off (10 Hz)
            float o = y - c.dc_x + 0.9986f * c.dc_y;
            c.dc_x = y; c.dc_y = o; y = o;
        }
        return y;
    }
    inline void tick(float &l, float &r) { l = tick1(ch[0], l); r = tick1(ch[1], r); }
};

// --- the FX page (1.8): EQ, compressor, chorus, delay; textbook designs (RBJ cookbook, Reiss & McPherson "Audio
// Effects"), written here ---------------------------------------------------------------------------------------

// RBJ biquad, transposed direct form II
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline float tick(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void norm(float B0, float B1, float B2, float A0, float A1, float A2) { b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0; }
    void low_shelf(float hz, float db) {
        float A = std::pow(10.0f, db / 40), w = 2 * PI_F * hz / SR, c = std::cos(w), al = std::sin(w) / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * al;
        norm(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
             (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
    }
    void high_shelf(float hz, float db) {
        float A = std::pow(10.0f, db / 40), w = 2 * PI_F * hz / SR, c = std::cos(w), al = std::sin(w) / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * al;
        norm(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
             (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
    }
    void peak(float hz, float db, float q) {
        float A = std::pow(10.0f, db / 40), w = 2 * PI_F * hz / SR, c = std::cos(w), al = std::sin(w) / (2 * q);
        norm(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
};

// Three bands: low shelf 150 Hz, a broad mid at 1 kHz, high shelf 6 kHz; +-12 dB. Off (skipped) at 0 dB.
struct Eq3 {
    Biquad lo[2], mid[2], hi[2];
    float glo = 0, gmid = 0, ghi = 0;
    bool on = false;
    void set(float l, float m, float h) {
        on = std::fabs(l) > 0.05f || std::fabs(m) > 0.05f || std::fabs(h) > 0.05f;
        if (l != glo) { for (auto &b : lo) b.low_shelf(150, l); glo = l; }
        if (m != gmid) { for (auto &b : mid) b.peak(1000, m, 0.7f); gmid = m; }
        if (h != ghi) { for (auto &b : hi) b.high_shelf(6000, h); ghi = h; }
    }
    inline void tick(float &l, float &r) {
        l = hi[0].tick(mid[0].tick(lo[0].tick(l)));
        r = hi[1].tick(mid[1].tick(lo[1].tick(r)));
    }
};

// Feed-forward compressor, stereo-linked peak detector. One knob (amount 0..1): threshold 0 to -30 dB, ratio 1.5:1
// to 6:1, attack 5 ms, release 150 ms, makeup half the gain reduction at threshold. The detector runs per sample; the
// gain (a log and a pow) is worked out every 16 samples and glided to, which keeps it cheap on ARMv7.
struct Compressor {
    float env = 0, thr_db = 0, ratio = 1, makeup = 1, g = 1, gt = 1;
    float att = std::exp(-1.0f / (0.005f * SR)), rel = std::exp(-1.0f / (0.150f * SR));
    int count = 0;
    void set(float amount) {
        thr_db = -30 * amount;
        ratio = 1.5f + 4.5f * amount;
        makeup = std::pow(10.0f, -thr_db * (1 - 1 / ratio) * 0.5f / 20);
    }
    inline void tick(float &l, float &r) {
        float x = std::max(std::fabs(l), std::fabs(r));
        env = x > env ? att * env + (1 - att) * x : rel * env + (1 - rel) * x;
        if (--count <= 0) {
            count = 16;
            gt = makeup;
            if (env > 1e-6f) {
                float db = 20 * std::log10(env);
                if (db > thr_db) gt *= std::pow(10.0f, -(db - thr_db) * (1 - 1 / ratio) / 20);
            }
        }
        g += (gt - g) * 0.08f;
        l *= g; r *= g;
    }
};

// 2.2: the compressor's newer types (AMOUNT 0..1 sets threshold and ratio; make-up gain as Classic's: half the gain lost
// at 0 dB in):
//  2 Glue: a bus compressor. Stereo-linked RMS detector (10 ms) on a 100 Hz high-passed side chain (the bass does not
//    pump it), 6 dB soft knee, 2:1..4:1 from -10..-30 dB, 10 ms attack (transients pass), release automatic: a fast
//    (100 ms) and a slow (800 ms) one, the slower holding after long compression.
//  3 Punch: peak detector, hard knee, 4:1..10:1 from -6..-30 dB, 1 ms attack, 80 ms release: drums
struct BusComp {
    int type = 2;
    float thr_db = 0, ratio = 1, makeup = 1, g = 1;
    float hp_xl = 0, hp_yl = 0, hp_xr = 0, hp_yr = 0, ms = 0, peak = 0, red_fast = 0, red_slow = 0;
    float ms_a = std::exp(-1.0f / (0.010f * SR)), hp_a = std::exp(-2 * float(M_PI) * 100.0f / SR);
    float att = 0, rel_fast = 0, rel_slow = 0;
    int count = 0;
    float target_db = 0, red_db = 0;   // red_db: the reduction now (the GR meter)
    void set(int t, float amount) {
        type = t;
        amount = std::max(0.0f, std::min(1.0f, amount));
        if (t == 3) { thr_db = -6 - 24 * amount; ratio = 4 + 6 * amount; }
        else { thr_db = -10 - 20 * amount; ratio = 2 + 2 * amount; }
        makeup = std::pow(10.0f, -thr_db * (1 - 1 / ratio) * 0.5f / 20);
        att = std::exp(-1.0f / ((t == 3 ? 0.001f : 0.010f) * SR));
        rel_fast = std::exp(-1.0f / ((t == 3 ? 0.080f : 0.100f) * SR));
        rel_slow = std::exp(-1.0f / (0.800f * SR));
    }
    void clear() { hp_xl = hp_yl = hp_xr = hp_yr = ms = peak = red_fast = red_slow = 0; g = 1; }
    // the gain reduction (dB, >= 0) wanted for a detector level (dB)
    inline float curve(float db) const {
        const float slope = 1 - 1 / ratio, over = db - thr_db;
        if (type == 3) return over > 0 ? over * slope : 0;
        const float knee = 6;
        if (over <= -knee / 2) return 0;
        if (over >= knee / 2) return over * slope;
        const float k = over + knee / 2;
        return slope * k * k / (2 * knee);
    }
    inline void tick(float &l, float &r) {
        float dl = l, dr = r;
        if (type == 2) {   // the side chain's 100 Hz high-pass
            hp_yl = hp_a * (hp_yl + l - hp_xl); hp_xl = l; dl = hp_yl;
            hp_yr = hp_a * (hp_yr + r - hp_xr); hp_xr = r; dr = hp_yr;
            ms = ms_a * ms + (1 - ms_a) * std::max(dl * dl, dr * dr);
        } else {
            float x = std::max(std::fabs(dl), std::fabs(dr));
            peak = x > peak ? x : peak * 0.9995f;
        }
        if (--count <= 0) {
            count = 8;
            float lvl = type == 2 ? ms : peak * peak;
            target_db = lvl > 1e-12f ? curve(10 * std::log10(lvl)) : 0;
        }
        // attack towards more reduction; release: Glue's fast and slow ones (the larger holds), Punch's one
        if (target_db > red_fast) red_fast = att * red_fast + (1 - att) * target_db;
        else red_fast = rel_fast * red_fast + (1 - rel_fast) * target_db;
        float red = red_fast;
        if (type == 2) {
            if (target_db > red_slow) red_slow = 0.9999f * red_slow + 0.0001f * target_db;   // builds up slowly (~230 ms)
            else red_slow = rel_slow * red_slow + (1 - rel_slow) * target_db;
            red = std::max(red_fast, red_slow);
        }
        red_db = red;
        float gt = makeup * std::pow(10.0f, -red / 20);
        g += (gt - g) * 0.02f;
        l *= g; r *= g;
    }
};

// 2.2: LIMITER: a lookahead peak limiter at -1 dBFS (where the output's soft clip begins, so that never bends the
// waveform while this is on). The gain each sample needs is held for 64 samples (1.45 ms) and averaged over 64, so it is
// down before the peak arrives (the audio is delayed 63 samples); it recovers over 60 ms.
struct Limiter {
    static constexpr int L = 64;
    float ceiling = 0.89f, rel = std::exp(-1.0f / (0.060f * SR));
    float gq[L] = {}, dl[L] = {}, dr[L] = {}, rq[L] = {};
    int i = 0;
    float r = 1, sum = L, last_g = 1;   // last_g: the gain now (the GR meter)
    // the hold as a sliding-window minimum (a monotonic queue: gains rising from front to back, with when they came)
    float mq_v[L] = {};
    unsigned mq_t[L] = {}, t = 0;
    int mq_h = 0, mq_n = 0;
    Limiter() { clear(); }
    void clear() {
        for (int k = 0; k < L; k++) { gq[k] = 1; rq[k] = 1; dl[k] = dr[k] = 0; }
        r = 1; sum = L; i = 0; mq_h = mq_n = 0; t = 0;
    }
    inline void tick(float &l, float &rr) {
        float pk = std::max(std::fabs(l), std::fabs(rr));
        const float gn = pk > ceiling ? ceiling / pk : 1;
        gq[i] = gn;
        // the hold: the least gain of the last 64 (drop the expired front, the back ones not less than the new one)
        while (mq_n > 0 && t - mq_t[mq_h] >= unsigned(L)) { mq_h = (mq_h + 1) % L; mq_n--; }
        while (mq_n > 0 && mq_v[(mq_h + mq_n - 1) % L] >= gn) mq_n--;
        mq_v[(mq_h + mq_n) % L] = gn; mq_t[(mq_h + mq_n) % L] = t; mq_n++;
        t++;
        const float h = mq_v[mq_h];
        r = h < r ? h : r + (h - r) * (1 - rel);                  // recovery
        sum += r - rq[i]; rq[i] = r;                              // the average of the last 64
        float g = std::min(1.0f, sum / L);
        last_g = g;
        int j = (i + 1) % L;                                      // the oldest: 63 samples back
        float ol = dl[j] * g, orr = dr[j] * g;
        dl[i] = l; dr[i] = rr;
        i = j;
        if (i == 0) { sum = 0; for (int k = 0; k < L; k++) sum += rq[k]; }   // no rounding drift over hours
        l = ol; rr = orr;
    }
};

// Stereo chorus: one delay line per side read 12 ms + depth back, swept by triangle LFOs a quarter cycle apart. Linear
// interpolation; the line is 50 ms.
struct Chorus {
    std::vector<float> bl, br;
    size_t w = 0;
    float phase = 0, inc = 0, depth = 0, mix = 0;
    Chorus() : bl(size_t(0.05f * SR) + 4, 0.f), br(size_t(0.05f * SR) + 4, 0.f) {}
    void set(float rate_hz, float depth01, float mix01) { inc = rate_hz / SR; depth = depth01 * 0.008f * SR; mix = mix01; }
    void clear() { std::fill(bl.begin(), bl.end(), 0.f); std::fill(br.begin(), br.end(), 0.f); }
    inline float read(const std::vector<float> &b, float d) const {
        float p = float(w) - d;
        if (p < 0) p += float(b.size());
        size_t i = size_t(p);
        float t = p - float(i);
        size_t j = i + 1 == b.size() ? 0 : i + 1;
        return b[i] + (b[j] - b[i]) * t;
    }
    inline void tick(float &l, float &r) {
        bl[w] = l; br[w] = r;
        phase += inc;
        if (phase >= 1) phase -= 1;
        auto sn = [](float p) { return p < 0.5f ? 4 * p - 1 : 3 - 4 * p; };   // a triangle LFO, -1..1
        float s1 = sn(phase), s2 = sn(phase + 0.25f >= 1 ? phase - 0.75f : phase + 0.25f);
        float base = 0.012f * SR;
        float wl = read(bl, base + depth * (0.5f + 0.5f * s1)), wr = read(br, base + depth * (0.5f + 0.5f * s2));
        if (++w >= bl.size()) w = 0;
        l = l * (1 - 0.5f * mix) + wl * mix * 0.7f;
        r = r * (1 - 0.5f * mix) + wr * mix * 0.7f;
    }
};

// 2.2: ENSEMBLE: a bucket-brigade ensemble chorus: the mono sum into one line read at two taps swept in opposite
// directions by one triangle LFO (left and right), the wet through a 7 kHz low-pass, a 3.5 ms centre. Modes: 1 (I) 0.5 Hz
// +-1.8 ms, 2 (II) 0.83 Hz +-1.8 ms, 3 (I+II) 9 Hz +-0.25 ms. The RATE / DEPTH knobs scale the mode (50 % = the mode's
// own): rate x (knob's Hz / 0.5 Hz), depth x 2 * depth01.
struct Ensemble {
    std::vector<float> b;
    size_t w = 0;
    float phase = 0, inc = 0, centre = 0.0035f * SR, dep = 0, mix = 0, lpa = 0, lpb = 0, lp_k = 0, hp_x = 0, hp_y = 0;
    Ensemble() : b(size_t(0.02f * SR) + 4, 0.f) { lp_k = 1 - std::exp(-2 * float(M_PI) * 7000.0f / SR); }
    void set(int mode, float rate_hz, float depth01, float mix01) {
        const float r = mode == 3 ? 9.0f : mode == 2 ? 0.83f : 0.5f, d = mode == 3 ? 0.00025f : 0.0018f;
        inc = r * (rate_hz / 0.5f) / SR;
        dep = std::min(0.0032f, d * 2 * depth01) * SR;   // never past the centre
        mix = mix01;
    }
    void clear() { std::fill(b.begin(), b.end(), 0.f); lpa = lpb = hp_x = hp_y = 0; }
    inline float read(float d) const {
        float p = float(w) - d;
        if (p < 0) p += float(b.size());
        size_t i = size_t(p);
        float t = p - float(i);
        size_t j = i + 1 == b.size() ? 0 : i + 1;
        return b[i] + (b[j] - b[i]) * t;
    }
    inline void tick(float &l, float &r) {
        float m = (l + r) * 0.5f;
        hp_y = 0.995f * (hp_y + m - hp_x); hp_x = m;   // 35 Hz: no low-end wobble
        b[w] = hp_y;
        phase += inc;
        if (phase >= 1) phase -= 1;
        float tri = phase < 0.5f ? 4 * phase - 1 : 3 - 4 * phase;
        float wa = read(centre + dep * tri), wb = read(centre - dep * tri);
        if (++w >= b.size()) w = 0;
        lpa += lp_k * (wa - lpa); lpb += lp_k * (wb - lpb);
        l = l * (1 - 0.2f * mix) + lpa * mix;   // level matched to dry on real material (a Voyager pad, 2026-11-11)
        r = r * (1 - 0.2f * mix) + lpb * mix;
    }
};

// Stereo delay up to 2 s, tempo synced; feedback through a one-pole low-pass (tone); ping-pong crosses the feedback.
// The delay time glides to a new setting (no zipper or click when the tempo or division changes).
// 2.2: the delay's types: 1 Classic (1.8's clean digital delay), 2 Tape (wow 0.6 Hz and flutter 6.5 Hz on the time, the
// repeats saturated and thinned below 150 Hz: each one rounder than the last), 3 Analog (a bucket-brigade's: darker,
// 2.5 kHz low-pass, a slow 0.4 Hz drift, soft clipping, lows thinned below 250 Hz)
struct Delay {
    std::vector<float> bl, br;
    size_t w = 0;
    float cur = 0, target = 0, fb = 0, mix = 0, damp = 0, sl = 0, sr = 0;
    bool ping = false;
    int type = 1;
    float hpl_x = 0, hpl_y = 0, hpr_x = 0, hpr_y = 0, hp_a = 1, lpl = 0, lpr = 0, lp_a = 1;
    float ws = 0, wc = 1, fs = 0, fc = 1, w_rc = 1, w_rs = 0, f_rc = 1, f_rs = 0, wow = 0, flutter = 0;
    int renorm = 0;
    Delay() : bl(size_t(2.0f * SR) + 128, 0.f), br(size_t(2.0f * SR) + 128, 0.f) {}   // room for 2 s plus the drift
    void set(float seconds, float fb01, float tone01, float mix01, bool pp, int t = 1) {
        target = std::max(1.0f, std::min(float(bl.size() - 120), seconds * SR));   // + wow and flutter (at most 27) inside
        if (cur <= 0) cur = target;
        fb = fb01; damp = tone01 * 0.85f; mix = mix01; ping = pp;
        if (t != type) {
            type = t;
            const float wh = t == 3 ? 0.4f : 0.6f, fh = 6.5f;
            w_rc = std::cos(2 * float(M_PI) * wh / SR); w_rs = std::sin(2 * float(M_PI) * wh / SR);
            f_rc = std::cos(2 * float(M_PI) * fh / SR); f_rs = std::sin(2 * float(M_PI) * fh / SR);
            // pitch deviation = 2 pi f D / SR: Tape about 0.2 % wow and 0.05 % flutter, Analog 0.15 % drift
            wow = t == 2 ? 23.0f : t == 3 ? 26.0f : 0.0f;
            flutter = t == 2 ? 0.55f : 0.0f;
            hp_a = std::exp(-2 * float(M_PI) * (t == 3 ? 250.0f : 150.0f) / SR);
            lp_a = t == 3 ? 1 - std::exp(-2 * float(M_PI) * 2500.0f / SR) : 1;
        }
    }
    void clear() {
        std::fill(bl.begin(), bl.end(), 0.f); std::fill(br.begin(), br.end(), 0.f);
        sl = sr = hpl_x = hpl_y = hpr_x = hpr_y = lpl = lpr = 0;
        ws = fs = 0; wc = fc = 1;
    }
    inline float read(const std::vector<float> &b, float d) const {
        float p = float(w) - d;
        if (p < 0) p += float(b.size());
        size_t i = size_t(p);
        float t = p - float(i);
        size_t j = i + 1 == b.size() ? 0 : i + 1;
        return b[i] + (b[j] - b[i]) * t;
    }
    inline void tick(float &l, float &r) {
        cur += (target - cur) * 0.0005f;
        float d = cur;
        if (type > 1) {   // Tape / Analog: the time drifts (rotating phasors, renormalised now and then)
            float n = ws * w_rc + wc * w_rs; wc = wc * w_rc - ws * w_rs; ws = n;
            n = fs * f_rc + fc * f_rs; fc = fc * f_rc - fs * f_rs; fs = n;
            if (++renorm >= 4096) {
                renorm = 0;
                float m = 1 / std::sqrt(ws * ws + wc * wc); ws *= m; wc *= m;
                m = 1 / std::sqrt(fs * fs + fc * fc); fs *= m; fc *= m;
            }
            d = std::max(1.0f, d + wow * ws + flutter * fs);   // around the time set: the tempo sync stays exact
        }
        float dl = read(bl, d), dr = read(br, d);
        sl = dl + (sl - dl) * damp;   // one-pole low-pass in the loop
        sr = dr + (sr - dr) * damp;
        float ql = sl, qr = sr;
        if (type > 1) {
            if (type == 3) { lpl += lp_a * (ql - lpl); lpr += lp_a * (qr - lpr); ql = lpl; qr = lpr; }   // the BBD's darkness
            hpl_y = hp_a * (hpl_y + ql - hpl_x); hpl_x = ql; ql = hpl_y;                                  // thinner lows
            hpr_y = hp_a * (hpr_y + qr - hpr_x); hpr_x = qr; qr = hpr_y;
        }
        float il, ir;
        if (ping) { il = (l + r) * 0.5f + qr * fb; ir = ql * fb; }
        else { il = l + ql * fb; ir = r + qr * fb; }
        // saturation, gentle at normal levels (a 0.9 peak loses 1.4 dB on Tape, 1 dB on Analog), stronger as the repeats
        // pile up with a high feedback
        if (type == 2) { il = fast_tanh(il * 0.8f) / 0.8f; ir = fast_tanh(ir * 0.8f) / 0.8f; }        // tape saturation
        else if (type == 3) { il = fast_tanh(il * 0.7f) / 0.7f; ir = fast_tanh(ir * 0.7f) / 0.7f; }   // the BBD's clipping
        bl[w] = il; br[w] = ir;
        if (++w >= bl.size()) w = 0;
        l += dl * mix;
        r += dr * mix;
    }
};

// --- the MOD FX page (1.8.2): lo-fi, phaser, flanger, tremolo / auto-pan; textbook designs, written here ----------

// a sine-like LFO from a phase 0..1 (a cubic on a triangle, within 3 % of sin, no trig per sample), -1..1
inline float lfo_sine(float p) {
    float x = p < 0.5f ? 4 * p - 1 : 3 - 4 * p;   // triangle
    return x * (1.5f - 0.5f * x * x);
}

// Lo-fi: a sample-and-hold to a lower rate (no anti-alias filter: the aliasing is the sound) and the level quantised
// to `bits`; mix crossfades dry and wet
struct LoFi {
    float step = 1, phase = 1, hl = 0, hr = 0, q = 0, iq = 0, mix = 0;
    void set(float rate_hz, int bits, float mix01) {
        step = std::min(1.0f, rate_hz / SR);
        q = float(1 << (std::max(2, std::min(16, bits)) - 1));
        iq = 1 / q;
        mix = mix01;
    }
    inline void tick(float &l, float &r) {
        phase += step;
        if (phase >= 1) {
            phase -= 1;
            hl = std::floor(l * q + 0.5f) * iq;
            hr = std::floor(r * q + 0.5f) * iq;
        }
        l += (hl - l) * mix;
        r += (hr - r) * mix;
    }
};

// Phaser: six first-order all-passes per side whose break frequency sweeps 200 Hz up to 6 octaves (depth) on a
// sine LFO, the right side a quarter cycle on; feedback from the last stage. The coefficients are worked out every 16
// samples. Mixed half and half at full mix, which is where the notches are deepest; the wet side drops as the
// feedback rises (its resonance), so the level stays about the same.
struct Phaser {
    static constexpr int STAGES = 6;
    float zl[STAGES] = {}, zr[STAGES] = {}, fl = 0, fr = 0, al = 0, ar = 0;
    float phase = 0, inc = 0, depth = 0, fb = 0, mix = 0;
    int count = 0;
    void set(float rate_hz, float depth01, float fb01, float mix01) { inc = rate_hz / SR; depth = depth01 * 6; fb = fb01; mix = mix01; }
    void clear() { std::fill(std::begin(zl), std::end(zl), 0.f); std::fill(std::begin(zr), std::end(zr), 0.f); fl = fr = 0; }
    static float coef(float lfo, float depth) {
        float hz = 200 * std::exp2(depth * (0.5f + 0.5f * lfo));
        float t = std::tan(PI_F * std::min(hz, 16000.0f) / SR);
        return (t - 1) / (t + 1);
    }
    static inline float chain(float x, float a, float *z) {
        for (int s = 0; s < STAGES; s++) {   // y = a x + z; z = x - a y
            float y = a * x + z[s];
            z[s] = x - a * y;
            x = y;
        }
        return x;
    }
    inline void tick(float &l, float &r) {
        phase += inc;
        if (phase >= 1) phase -= 1;
        if (--count <= 0) {
            count = 16;
            al = coef(lfo_sine(phase), depth);
            ar = coef(lfo_sine(phase + 0.25f >= 1 ? phase - 0.75f : phase + 0.25f), depth);
        }
        fl = chain(l + fl * fb, al, zl);
        fr = chain(r + fr * fb, ar, zr);
        const float wet = 0.5f * mix * (1 - 0.5f * fb);
        l = l * (1 - 0.5f * mix) + fl * wet;
        r = r * (1 - 0.5f * mix) + fr * wet;
    }
};

// Flanger: a 0.3 ms + up to 6 ms (depth) delay swept by a triangle LFO, the right side a quarter cycle on, with
// feedback; mixed half and half at full mix (the comb's notches), the wet side lowered as the feedback rises
struct Flanger {
    std::vector<float> bl, br;
    size_t w = 0;
    float phase = 0, inc = 0, depth = 0, fb = 0, mix = 0, sl = 0, sr = 0;
    Flanger() : bl(size_t(0.02f * SR) + 4, 0.f), br(size_t(0.02f * SR) + 4, 0.f) {}
    void set(float rate_hz, float depth01, float fb01, float mix01) { inc = rate_hz / SR; depth = depth01 * 0.006f * SR; fb = fb01; mix = mix01; }
    void clear() { std::fill(bl.begin(), bl.end(), 0.f); std::fill(br.begin(), br.end(), 0.f); sl = sr = 0; }
    inline float read(const std::vector<float> &b, float d) const {
        float p = float(w) - d;
        if (p < 0) p += float(b.size());
        size_t i = size_t(p);
        float t = p - float(i);
        size_t j = i + 1 == b.size() ? 0 : i + 1;
        return b[i] + (b[j] - b[i]) * t;
    }
    inline void tick(float &l, float &r) {
        phase += inc;
        if (phase >= 1) phase -= 1;
        auto tri = [](float p) { return p < 0.5f ? 2 * p : 2 - 2 * p; };   // 0..1
        float base = 0.0003f * SR;
        sl = read(bl, base + depth * tri(phase));
        sr = read(br, base + depth * tri(phase + 0.25f >= 1 ? phase - 0.75f : phase + 0.25f));
        bl[w] = l + sl * fb;
        br[w] = r + sr * fb;
        if (++w >= bl.size()) w = 0;
        const float wet = 0.5f * mix * (1 - 0.6f * fb);
        l = l * (1 - 0.5f * mix) + sl * wet;
        r = r * (1 - 0.5f * mix) + sr * wet;
    }
};

// Tremolo / auto-pan on a sine or square LFO (the square's edges smoothed over about 2 ms, no clicks). Tremolo dips
// both sides to 1 - depth; auto-pan moves the sound side to side, depth = how far, with the plugin's own pan law
struct Tremolo {
    float phase = 0, inc = 0, depth = 0, g = 0;
    bool pan = false, square = false;
    float smooth = 1 - std::exp(-1.0f / (0.002f * SR));
    void set(float hz, float depth01, bool pan_mode, bool sq) { inc = hz / SR; depth = depth01; pan = pan_mode; square = sq; }
    inline void tick(float &l, float &r) {
        phase += inc;
        if (phase >= 1) phase -= 1;
        float x = square ? (phase < 0.5f ? 1.0f : -1.0f) : lfo_sine(phase);
        g += (x - g) * smooth;
        if (pan) {
            float p = g * depth;
            if (p > 0) l *= 1 - p; else r *= 1 + p;
        } else {
            float k = 1 - depth * (0.5f - 0.5f * g);
            l *= k; r *= k;
        }
    }
};

}  // namespace omni

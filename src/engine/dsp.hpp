// Omni Sampler: DSP building blocks (filters, envelope, LFO, reverb). 44.1 kHz, float.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
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
    void set(float hz, float res) {
        a.set(hz, res);
        if (poles >= 4) b.set(hz, res * 0.5f);
        p.set(hz);
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
    enum Stage { Idle, Delay, Attack, Hold, Decay, Sustain, Release };
    Stage stage = Idle;
    float t = 0, inc = 0, level = 0, from = 0, start = 0, sustain = 1, end_level = 0;
    float s_attack = 0, s_decay = 0, s_release = 0;
    float delay_s = 0, attack_s = 0, hold_s = 0, decay_s = 0, release_s = 0;

    static float step(float seconds) { return seconds <= 0.0005f ? 1.0f : 1.0f / (seconds * SR); }
    void begin(const Envelope &e, float time_scale_attack, float time_scale, float add_attack, float add_release,
               float sustain_scale, float sustain_add = 0) {
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
            if (t >= 1) { level = 1; enter(Hold); }
            else level = from + (1 - from) * shape(t, s_attack);
            return level;
        case Hold: t += inc; if (t >= 1) enter(Decay); return level;
        case Decay:
            t += inc;
            if (t >= 1) { level = sustain; stage = Sustain; }
            else level = 1 + (sustain - 1) * shape(t, s_decay);
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

}  // namespace omni

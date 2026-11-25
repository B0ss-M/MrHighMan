// Omni Sampler 2.4.40 (the user: "make sure all the knobs inside the FX BANK work and actually have an impact on the
// sound"): the page's knobs that the selected program does not use (fx_knob_usage*.inc, from the knob audit
// reports/fx-knob-audit/) drive a shared section around it. Every stage is OFF while its knob sits at the page default, so
// a default knob sounds exactly as before (projects unchanged) and costs nothing; moved, it acts over its whole range:
//   MIX      the whole FX BANK's effect (program + this section): 0 = dry, default = as the program makes it, 100 % = twice the
//            effect (a program without its own MIX); a program with its own MIX scales this section with it (dry at 0)
//   DRIVE    input drive relative to 12 dB: below = a cleaner, quieter input; above = saturation (tanh)
//   LOW / MID / HIGH   EQ after the program: low shelf 200 Hz, peak 1 kHz, high shelf 5 kHz (dB)
//   TONE     tilt after the program around the default: darker below, brighter above (+-12 dB at the ends)
//   THRESH / RATIO     dynamics after the program, two-sided around the defaults: THRESH down / RATIO up compress (at
//            THRESH, up to RATIO:1), THRESH up / RATIO down expand under THRESH (quiet parts drop); faded in, no jump
//   RATE / DEPTH       DEPTH below the default = tremolo, above = auto-pan, at RATE (RATE alone moved: a light tremolo)
//   TIME / FEEDBACK    an echo after the program: FEEDBACK away from the default sets its level (above it: repeats too),
//            TIME its time (TIME alone moved: a single echo)
//   PITCH    a pitch-shifted layer (half level) at PITCH - 7 semitones (the default 7 = no layer)
// Real time: prepare() allocates; process() never allocates; every output finite.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "omni_fx_bank.hpp"

namespace omnisampler {
namespace fxknobs {

enum Knob { K_MIX, K_RATE, K_DEPTH, K_FB, K_TIME, K_TONE, K_DRIVE, K_THR, K_RATIO, K_PITCH, K_LOW, K_MID, K_HIGH, K_COUNT };
constexpr uint16_t ALL = (1u << K_COUNT) - 1;
inline bool uses(uint16_t mask, Knob k) { return (mask >> k) & 1u; }

// the page's defaults in engine units (plugin.cpp NUM_PARAMS + apply_settings: rate = fxb_hz(39))
struct Defaults {
    float mix = .5f, rate = .05f * std::pow(800.f, .39f), depth = .65f, fb = .35f, time = 320.f, tone = .55f, drive = 12.f,
          thr = -24.f, ratio = 4.f, pitch = 7.f, low = 0.f, mid = 0.f, high = 0.f;
};
// 2.4.40 CPU (Live II): a 1024-point sine table (linear interpolation) for the LFO and the pitch window; a rational tanh
inline float fast_sin01(float ph) {   // sin(2 pi ph), ph in [0, 1)
    static const std::vector<float> T = [] { std::vector<float> t(1025); for (int i = 0; i <= 1024; i++) t[size_t(i)] = float(std::sin(2.0 * M_PI * i / 1024.0)); return t; }();
    float x = ph * 1024.f;
    int i = int(x);
    if (i < 0) i = 0; else if (i > 1023) i = 1023;
    const float f = x - float(i);
    return T[size_t(i)] + f * (T[size_t(i) + 1] - T[size_t(i)]);
}
inline float fast_tanh(float x) {   // Pade 7/6, clamped (error < 1e-3 over the range)
    if (x > 4.97f) return 1.f;
    if (x < -4.97f) return -1.f;
    const float x2 = x * x;
    return x * (135135.f + x2 * (17325.f + x2 * (378.f + x2))) / (135135.f + x2 * (62370.f + x2 * (3150.f + 28.f * x2)));
}
inline bool moved(float v, float d, float tol) { return std::isfinite(v) && std::fabs(v - d) > tol; }

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1[2] = {0, 0}, z2[2] = {0, 0};
    void clear() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    // RBJ cookbook: type 0 low shelf, 1 peak, 2 high shelf
    void set(int type, float fs, float f0, float db, float q) {
        const float A = std::pow(10.f, db / 40.f), w = 2.f * float(M_PI) * f0 / fs, cw = std::cos(w), sw = std::sin(w);
        float B0, B1, B2, A0, A1, A2;
        if (type == 1) {
            const float al = sw / (2 * q);
            B0 = 1 + al * A; B1 = -2 * cw; B2 = 1 - al * A; A0 = 1 + al / A; A1 = -2 * cw; A2 = 1 - al / A;
        } else {
            const float al = sw / 2 * std::sqrt(2.f), sA = 2 * std::sqrt(A) * al, s = type == 0 ? 1.f : -1.f;
            B0 = A * ((A + 1) - s * (A - 1) * cw + sA); B1 = s * 2 * A * ((A - 1) - s * (A + 1) * cw);
            B2 = A * ((A + 1) - s * (A - 1) * cw - sA); A0 = (A + 1) + s * (A - 1) * cw + sA;
            A1 = -s * 2 * ((A - 1) + s * (A + 1) * cw); A2 = (A + 1) + s * (A - 1) * cw - sA;
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    float tick(int ch, float x) {
        const float y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }
};

class Shared {
    float fs_ = 44100;
    std::vector<float> dryL_, dryR_, progL_, progR_, echoL_, echoR_, pitL_, pitR_;
    size_t ew_ = 0, pw_ = 0;
    Biquad lo_, mi_, hi_;
    float eqLo_ = 0, eqMi_ = 0, eqHi_ = 0;
    float tiltLp_[2] = {0, 0}, env_ = 0, lfo_ = 0, pph_ = 0, dyn_g_ = 1;
    bool echo_was_ = false, pitch_was_ = false;
    Defaults d_;
public:
    void prepare(float fs, int max_block) {
        fs_ = fs > 1000 ? fs : 44100;
        dryL_.assign(size_t(std::max(1, max_block)), 0.f);
        dryR_.assign(size_t(std::max(1, max_block)), 0.f);
        progL_.assign(dryL_.size(), 0.f);
        progR_.assign(dryL_.size(), 0.f);
        echoL_.assign(size_t(fs_ * 1.7f) + 4, 0.f);   // TIME up to 1600 ms
        echoR_.assign(echoL_.size(), 0.f);
        pitL_.assign(size_t(fs_ * .1f) + 4, 0.f);     // a 100 ms window for the pitch layer
        pitR_.assign(pitL_.size(), 0.f);
        eqLo_ = eqMi_ = eqHi_ = 1e9f;
        reset();
    }
    void reset() {
        std::fill(echoL_.begin(), echoL_.end(), 0.f); std::fill(echoR_.begin(), echoR_.end(), 0.f);
        std::fill(pitL_.begin(), pitL_.end(), 0.f); std::fill(pitR_.begin(), pitR_.end(), 0.f);
        lo_.clear(); mi_.clear(); hi_.clear();
        tiltLp_[0] = tiltLp_[1] = 0; env_ = 0; lfo_ = 0; pph_ = 0; ew_ = pw_ = 0; dyn_g_ = 1;
        echo_was_ = pitch_was_ = false;
    }
    // any stage on for these controls and this program (false: pre / post leave the signal exactly as it is)
    bool active(const fxbank::Controls &c, uint16_t used) const {
        return (!uses(used, K_MIX) && moved(c.mix, d_.mix, .005f)) || (!uses(used, K_DRIVE) && moved(c.driveDb, d_.drive, .05f)) ||
               (!uses(used, K_LOW) && std::fabs(c.lowDb) > .05f) || (!uses(used, K_MID) && std::fabs(c.midDb) > .05f) ||
               (!uses(used, K_HIGH) && std::fabs(c.highDb) > .05f) || (!uses(used, K_TONE) && moved(c.tone, d_.tone, .005f)) ||
               (!uses(used, K_THR) && moved(c.thresholdDb, d_.thr, .05f)) || (!uses(used, K_RATIO) && moved(c.ratio, d_.ratio, .02f)) ||
               (!uses(used, K_DEPTH) && moved(c.depth, d_.depth, .005f)) || (!uses(used, K_RATE) && moved(c.rateHz, d_.rate, .01f)) ||
               (!uses(used, K_FB) && moved(c.feedback, d_.fb, .005f)) || (!uses(used, K_TIME) && moved(c.timeMs, d_.time, 1.f)) ||
               (!uses(used, K_PITCH) && moved(c.pitchSemitones, d_.pitch, .05f));
    }
    // before the program: keeps the dry signal (for MIX) and drives the input
    void pre(float *l, float *r, int n, const fxbank::Controls &c, uint16_t used) {
        if (n <= 0 || dryL_.empty()) return;
        const int m = std::min(n, int(dryL_.size()));
        std::memcpy(dryL_.data(), l, sizeof(float) * size_t(m));
        std::memcpy(dryR_.data(), r, sizeof(float) * size_t(m));
        if (!uses(used, K_DRIVE) && moved(c.driveDb, d_.drive, .05f)) {
            // 2.4.40: MIX governs the shared section too: a program that uses MIX scales it (full from the default up, dry at 0)
            const float mm = uses(used, K_MIX) ? std::max(0.f, std::min(1.f, c.mix / d_.mix)) : 1.f;
            const float e = mm * std::max(-12.f, std::min(24.f, c.driveDb - d_.drive));
            const float g = std::pow(10.f, e / 20.f);
            if (e < 0) for (int i = 0; i < n; i++) { l[i] *= g; r[i] *= g; }
            else for (int i = 0; i < n; i++) { l[i] = fast_tanh(g * l[i]); r[i] = fast_tanh(g * r[i]); }
        }
    }
    // after the program
    void post(float *l, float *r, int n, const fxbank::Controls &c, uint16_t used) {
        if (n <= 0 || dryL_.empty()) return;
        const int m = std::min(n, int(dryL_.size()));
        const bool on = active(c, used);
        if (on && uses(used, K_MIX)) {   // the program's own output, before the shared stages (MIX scales them below)
            std::memcpy(progL_.data(), l, sizeof(float) * size_t(m));
            std::memcpy(progR_.data(), r, sizeof(float) * size_t(m));
        }
        const float lo = uses(used, K_LOW) ? 0.f : c.lowDb, mi = uses(used, K_MID) ? 0.f : c.midDb, hi = uses(used, K_HIGH) ? 0.f : c.highDb;
        if (std::fabs(lo) > .05f || std::fabs(mi) > .05f || std::fabs(hi) > .05f) {
            if (lo != eqLo_) { lo_.set(0, fs_, 200.f, lo, .707f); eqLo_ = lo; }
            if (mi != eqMi_) { mi_.set(1, fs_, 1000.f, mi, .7f); eqMi_ = mi; }
            if (hi != eqHi_) { hi_.set(2, fs_, 5000.f, hi, .707f); eqHi_ = hi; }
            for (int i = 0; i < n; i++) {
                float a = l[i], b = r[i];
                if (std::fabs(lo) > .05f) { a = lo_.tick(0, a); b = lo_.tick(1, b); }
                if (std::fabs(mi) > .05f) { a = mi_.tick(0, a); b = mi_.tick(1, b); }
                if (std::fabs(hi) > .05f) { a = hi_.tick(0, a); b = hi_.tick(1, b); }
                l[i] = a; r[i] = b;
            }
        } else { lo_.clear(); mi_.clear(); hi_.clear(); }
        if (!uses(used, K_TONE) && moved(c.tone, d_.tone, .005f)) {   // tilt around 800 Hz
            const float t = std::max(-1.f, std::min(1.f, (c.tone - d_.tone) / (c.tone < d_.tone ? d_.tone : 1.f - d_.tone)));
            const float gl = std::pow(10.f, -t * 12.f / 40.f), gh = std::pow(10.f, t * 12.f / 40.f);
            const float a = 1.f - std::exp(-2.f * float(M_PI) * 800.f / fs_);
            for (int i = 0; i < n; i++) {
                tiltLp_[0] += a * (l[i] - tiltLp_[0]); tiltLp_[1] += a * (r[i] - tiltLp_[1]);
                l[i] = gl * tiltLp_[0] + gh * (l[i] - tiltLp_[0]);
                r[i] = gl * tiltLp_[1] + gh * (r[i] - tiltLp_[1]);
            }
        }
        const bool thr_m = !uses(used, K_THR) && moved(c.thresholdDb, d_.thr, .05f), rat_m = !uses(used, K_RATIO) && moved(c.ratio, d_.ratio, .02f);
        if (thr_m || rat_m) {
            // two-sided around the defaults: THRESH down / RATIO up = a compressor (threshold THRESH, ratio up to RATIO);
            // THRESH up / RATIO down = an expander under THRESH (quiet parts drop, up to 3:1); faded in from the defaults
            const float comp = std::min(1.f, (thr_m ? std::max(0.f, d_.thr - c.thresholdDb) / 6.f : 0.f) + (rat_m ? std::max(0.f, c.ratio - d_.ratio) / 4.f : 0.f));
            const float expd = std::min(1.f, (thr_m ? std::max(0.f, c.thresholdDb - d_.thr) / 6.f : 0.f) + (rat_m ? std::max(0.f, d_.ratio - c.ratio) / 1.5f : 0.f));
            const float th0 = std::max(-80.f, std::min(0.f, c.thresholdDb));
            // RATIO below the default raises the expander's threshold with it (to -6 dB at 1:1), so it reaches normal material
            const float th = comp >= expd || !rat_m || c.ratio >= d_.ratio ? th0
                           : std::max(th0, d_.thr + 18.f * std::min(1.f, (d_.ratio - c.ratio) / (d_.ratio - 1.f)));
            const float rc = 1.f + (std::max(d_.ratio, std::min(40.f, c.ratio)) - 1.f) * comp, re = 1.f + 2.f * expd;
            const float att = std::exp(-1.f / (.005f * fs_)), rel = std::exp(-1.f / (.12f * fs_));
            // the gain computed every 16 samples (log / pow are the cost on the Live II), ramped in between
            for (int i0 = 0; i0 < n; i0 += 16) {
                const int i1 = std::min(n, i0 + 16);
                for (int i = i0; i < i1; i++) {
                    const float pk = std::max(std::fabs(l[i]), std::fabs(r[i]));
                    env_ = pk > env_ ? att * env_ + (1 - att) * pk : rel * env_ + (1 - rel) * pk;
                }
                const float lv = 20.f * std::log10(std::max(env_, 1e-6f));
                float gr = 0.f;
                if (comp >= expd) { if (lv > th) gr = (th + (lv - th) / rc) - lv; }
                else if (lv < th) gr = std::max(-60.f, (lv - th) * (re - 1.f));
                const float g1 = std::pow(10.f, gr / 20.f), g0 = dyn_g_, step = (g1 - g0) / float(i1 - i0);
                for (int i = i0; i < i1; i++) { const float g = g0 + step * float(i - i0 + 1); l[i] *= g; r[i] *= g; }
                dyn_g_ = g1;
            }
        } else { env_ = 0; dyn_g_ = 1; }
        const bool dep_m = !uses(used, K_DEPTH) && moved(c.depth, d_.depth, .005f), rate_m = !uses(used, K_RATE) && moved(c.rateHz, d_.rate, .01f);
        if (dep_m || rate_m) {   // DEPTH below default = tremolo, above = auto-pan; RATE alone: a light tremolo
            const float dd = dep_m ? c.depth - d_.depth : 0.f;
            const bool pan = dd > 0;
            const float amt = std::min(1.f, dep_m ? std::fabs(dd) / (pan ? 1.f - d_.depth : d_.depth) : .3f);
            const float hz = std::max(.02f, std::min(40.f, c.rateHz)), inc = hz / fs_;
            for (int i = 0; i < n; i++) {
                lfo_ += inc; if (lfo_ >= 1.f) lfo_ -= 1.f;
                const float s = fast_sin01(lfo_);
                if (pan) { const float p = amt * s; l[i] *= 1.f - std::max(0.f, p); r[i] *= 1.f + std::min(0.f, p); }
                else { const float g = 1.f - amt * (.5f + .5f * s); l[i] *= g; r[i] *= g; }
            }
        }
        const bool fb_m = !uses(used, K_FB) && moved(c.feedback, d_.fb, .005f), time_m = !uses(used, K_TIME) && moved(c.timeMs, d_.time, 1.f);
        if (fb_m || time_m) {   // an echo after the program
            if (!echo_was_) { std::fill(echoL_.begin(), echoL_.end(), 0.f); std::fill(echoR_.begin(), echoR_.end(), 0.f); }
            const float f = fb_m ? c.feedback - d_.fb : 0.f;
            const float level = std::min(.6f, (fb_m ? std::fabs(f) / .35f * .5f : 0.f) + (time_m ? .35f : 0.f));
            const float fbk = f > 0 ? std::min(.85f, f / .6f * .85f) : 0.f;
            const size_t N = echoL_.size(), dly = std::min(N - 2, size_t(std::max(1.f, std::min(1600.f, c.timeMs)) * fs_ / 1000.f));
            for (int i = 0; i < n; i++) {
                const size_t rd = (ew_ + N - dly) % N;
                const float el = echoL_[rd], er = echoR_[rd];
                echoL_[ew_] = l[i] + fbk * el; echoR_[ew_] = r[i] + fbk * er;
                if (++ew_ == N) ew_ = 0;
                l[i] += level * el; r[i] += level * er;
            }
        }
        echo_was_ = fb_m || time_m;
        if (!uses(used, K_PITCH) && moved(c.pitchSemitones, d_.pitch, .05f)) {   // a pitch-shifted layer, half level
            if (!pitch_was_) { std::fill(pitL_.begin(), pitL_.end(), 0.f); std::fill(pitR_.begin(), pitR_.end(), 0.f); }
            const float st = std::max(-24.f, std::min(24.f, c.pitchSemitones - d_.pitch));
            const size_t N = pitL_.size();
            const float win = float(N - 3), rate = 1.f - std::pow(2.f, st / 12.f);   // delay change per sample
            for (int i = 0; i < n; i++) {
                pitL_[pw_] = l[i]; pitR_[pw_] = r[i];
                pph_ += rate; if (pph_ < 0) pph_ += win; if (pph_ >= win) pph_ -= win;
                float sl = 0, sr = 0;
                for (int tap = 0; tap < 2; tap++) {
                    float d = pph_ + tap * win * .5f; if (d >= win) d -= win;
                    const float wgt = fast_sin01(.5f * d / win);   // crossfade window (half a sine)
                    const float pos = float(pw_) - (d + 1.f);
                    const float fp = pos < 0 ? pos + float(N) : pos;
                    const size_t i0 = size_t(fp) % N, i1 = (i0 + N - 1) % N;
                    const float fr = fp - std::floor(fp);
                    sl += wgt * (pitL_[i0] + fr * (pitL_[i1] - pitL_[i0]));
                    sr += wgt * (pitR_[i0] + fr * (pitR_[i1] - pitR_[i0]));
                }
                if (++pw_ == N) pw_ = 0;
                l[i] += .5f * sl; r[i] += .5f * sr;
            }
        }
        pitch_was_ = !uses(used, K_PITCH) && moved(c.pitchSemitones, d_.pitch, .05f);
        if (on) {
            if (uses(used, K_MIX)) {   // 2.4.40: the shared stages follow the program's MIX (full from the default up, dry at 0)
                const float mm = std::max(0.f, std::min(1.f, c.mix / d_.mix));
                if (mm < 1.f)
                    for (int i = 0; i < m; i++) { l[i] = progL_[size_t(i)] + mm * (l[i] - progL_[size_t(i)]); r[i] = progR_[size_t(i)] + mm * (r[i] - progR_[size_t(i)]); }
            } else if (moved(c.mix, d_.mix, .005f)) {   // MIX the program does not use: the whole FX BANK's effect scaled (0 dry .. 2x)
                const float k = std::max(0.f, std::min(2.f, c.mix / d_.mix));
                for (int i = 0; i < m; i++) { l[i] = dryL_[size_t(i)] + k * (l[i] - dryL_[size_t(i)]); r[i] = dryR_[size_t(i)] + k * (r[i] - dryR_[size_t(i)]); }
            }
        }
        if (on)   // finite (only when a stage ran; the mix's own check follows in the sampler)
            for (int i = 0; i < n; i++) {
                if (!std::isfinite(l[i])) l[i] = 0;
                if (!std::isfinite(r[i])) r[i] = 0;
            }
    }
};

}  // namespace fxknobs
}  // namespace omnisampler

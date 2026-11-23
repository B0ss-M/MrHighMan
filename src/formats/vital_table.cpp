// Omni Sampler 2.4.13: Vital's wavetables and LFO shapes (formats/vital_table.hpp). Our own reading of the file format
// (no Vital code): what each field does was measured on the files and is approximate where Vital's math is unknown [G].
//
// A wavetable: "groups" (summed), each a source and modifiers applied in order; every component has "keyframes" at
// positions 0..256 that are interpolated ("interpolation_style": 0 none = the keyframe before, 1 linear, 2 cubic -> linear).
// Component "type": a name, or (older files) a number: 0 Wave Source, 1 Line Source, 2 Audio File Source, the modifiers
// told apart by their fields.
//   Wave Source / Shepard Tone Source: wave_data = base64 of 2048 float32; "interpolation" 0 time, 1 spectral.
//   Line Source: keyframe "line" {points, powers, smooth} (y 0..1, 0 = the top); older: points right in the keyframe,
//     y -1..1.
//   Audio File Source: audio_file = base64 int16 mono; window_size = samples per cycle; keyframe start_position (samples),
//     window_fade (the loop crossfade, 0..1); fade_style 0 blend (start glides between keyframes), 1 none, 2 time, 3
//     spectral (cycles cross-faded); phase_style 1 clear / 2 vocode (both: phases zeroed [G]); normalize_gain: each cycle
//     to full scale.
//   Phase Shift {phase radians, mix; style 0 all, 1 even/odd, 2 harmonic, 3 harmonic even/odd}; Wave Window {left_position,
//   right_position, window_shape 0 cos 1 half sine 2 linear 3 square 4 wiggle}; Frequency Filter {cutoff = log2 harmonic,
//   shape 0..1, style 0 low 1 band 2 high 3 comb, normalize}; Slew Limiter {up_run_rise, down_run_rise: a fraction of the
//   cycle to rise the whole range [G]}; Wave Folder {fold_boost}; Wave Warp {horizontal_power, vertical_power,
//   horizontal_asymmetric, vertical_asymmetric}.
#include "vital_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include "../core/parse.hpp"

namespace omni {
namespace {

constexpr int N = VITAL_CYCLE;
constexpr size_t MAX_AUDIO_BYTES = size_t(64) << 20;
constexpr size_t MAX_KEYFRAMES = 300, MAX_COMPONENTS = 32, MAX_GROUPS = 16, MAX_POINTS = 1024;
using Cx = std::complex<double>;
using Cycle = std::vector<double>;

double num(const Json &j, const char *k, double def) {
    const Json &v = j[k];
    if (v.type == Json::Number) return std::isfinite(v.num) ? v.num : def;
    if (v.type == Json::Bool) return v.b ? 1 : 0;
    return def;
}
double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

// Vital's curve between two points: power 0 straight, else exponential [G: the usual (e^pt - 1) / (e^p - 1)]
double power_scale(double t, double p) {
    p = clampd(p, -50, 50);
    if (std::fabs(p) < 1e-3) return t;
    return std::expm1(p * t) / std::expm1(p);
}

void fft(std::vector<Cx> &a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * M_PI / double(len) * (inverse ? 1 : -1);
        const Cx wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            Cx w(1);
            for (size_t j = 0; j < len / 2; j++) {
                const Cx u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (Cx &x : a) x /= double(n);
}
std::vector<Cx> spectrum(const Cycle &c) {
    std::vector<Cx> a(c.begin(), c.end());
    fft(a, false);
    return a;
}
// back to a real cycle; X is treated as the half spectrum of a real signal (bins 1..N/2-1 mirrored)
Cycle from_spectrum(std::vector<Cx> X) {
    X[0] = Cx(X[0].real(), 0);
    X[N / 2] = Cx(X[N / 2].real(), 0);
    for (int k = 1; k < N / 2; k++) X[size_t(N - k)] = std::conj(X[size_t(k)]);
    fft(X, true);
    Cycle c(N);
    for (int i = 0; i < N; i++) c[size_t(i)] = X[size_t(i)].real();
    return c;
}

double rms(const Cycle &c) { double s = 0; for (double v : c) s += v * v; return std::sqrt(s / double(c.size())); }
double peak(const Cycle &c) { double p = 0; for (double v : c) p = std::max(p, std::fabs(v)); return p; }

// the keyframes around a position: a, b and how far from a to b (0 when not interpolated)
struct Around { const Json *a = nullptr, *b = nullptr; double t = 0; };
struct Comp {
    std::string kind;
    const Json *c = nullptr;
    std::vector<const Json *> kfs;   // sorted by position
    int style = 1;                   // interpolation_style
    Around at(double pos) const {
        Around r;
        if (kfs.empty()) return r;
        r.a = r.b = kfs.front();
        if (pos <= num(*kfs.front(), "position", 0)) return r;
        r.a = r.b = kfs.back();
        if (pos >= num(*kfs.back(), "position", 0)) return r;
        for (size_t i = 0; i + 1 < kfs.size(); i++) {
            const double p0 = num(*kfs[i], "position", 0), p1 = num(*kfs[i + 1], "position", 0);
            if (pos >= p0 && pos < p1) {
                r.a = kfs[i];
                r.b = kfs[i + 1];
                r.t = style == 0 || p1 - p0 < 1e-9 ? 0 : (pos - p0) / (p1 - p0);
                return r;
            }
        }
        return r;
    }
    // a keyframe number at pos, interpolated
    double val(double pos, const char *k, double def) const {
        const Around r = at(pos);
        if (!r.a) return def;
        const double a = num(*r.a, k, def), b = num(*r.b, k, def);
        return a + (b - a) * r.t;
    }
};

std::string kind_of(const Json &c) {
    const Json &t = c["type"];
    if (t.type == Json::String) return t.str;
    const int n = t.as_int(-1);
    if (n == 0) return "Wave Source";
    if (n == 1) return "Line Source";
    if (n == 2) return "Audio File Source";
    // older numbered modifiers: by their fields
    static const Json none;
    const Json &kfs = c["keyframes"];
    const Json &k = kfs.type == Json::Array && !kfs.arr.empty() ? kfs.arr[0] : none;
    if (k.has("wave_data")) return "Shepard Tone Source";
    if (k.has("phase")) return "Phase Shift";
    if (k.has("left_position") || c.has("window_shape")) return "Wave Window";
    if (k.has("cutoff")) return "Frequency Filter";
    if (k.has("up_run_rise")) return "Slew Limiter";
    if (k.has("fold_boost")) return "Wave Folder";
    if (k.has("horizontal_power")) return "Wave Warp";
    return "type " + std::to_string(n);
}

Cycle wave_data(const Json &kf) {
    const std::vector<uint8_t> b = vital_base64(kf["wave_data"].as_str(), size_t(N) * 4);
    const size_t n = b.size() / 4;
    if (n < 2) return {};
    std::vector<float> f(n);
    std::memcpy(f.data(), b.data(), n * 4);   // float32 LE (the MPC is LE)
    Cycle c(N);
    for (int i = 0; i < N; i++) {   // (always 2048 in the files; any other length resampled)
        const double x = double(i) * double(n) / N;
        const size_t i0 = size_t(x) % n, i1 = (i0 + 1) % n;
        const double a = std::isfinite(f[i0]) ? f[i0] : 0, b2 = std::isfinite(f[i1]) ? f[i1] : 0;
        c[size_t(i)] = a + (b2 - a) * (x - std::floor(x));
    }
    return c;
}

// two cycles blended: in time, or (spectral) magnitudes and phases blended
Cycle blend(const Cycle &a, const Cycle &b, double t, bool spectral) {
    if (t <= 0 || b.empty()) return a;
    if (t >= 1 || a.empty()) return b;
    if (!spectral) {
        Cycle c(N);
        for (int i = 0; i < N; i++) c[size_t(i)] = a[size_t(i)] + (b[size_t(i)] - a[size_t(i)]) * t;
        return c;
    }
    std::vector<Cx> A = spectrum(a), B = spectrum(b), C(static_cast<size_t>(N));
    for (int k = 0; k <= N / 2; k++) {
        const double ma = std::abs(A[size_t(k)]), mb = std::abs(B[size_t(k)]);
        const double pa = std::arg(A[size_t(k)]);
        double d = std::arg(B[size_t(k)]) - pa;
        d -= 2 * M_PI * std::floor((d + M_PI) / (2 * M_PI));   // the shorter way round
        C[size_t(k)] = std::polar(ma + (mb - ma) * t, pa + d * t);
    }
    return from_spectrum(C);
}

Cycle line_cycle(const VitalLine &l, bool) {
    Cycle c(N);
    for (int i = 0; i < N; i++) {
        const double v = vital_line_value(l, double(i) / N);   // 0..1, 1 = the top
        c[size_t(i)] = 2 * v - 1;
    }
    return c;
}
bool line_of(const Json &kf, VitalLine &l, bool &old_range) {
    old_range = !kf.has("line");
    if (!vital_line_parse(old_range ? kf : kf["line"], l)) return false;
    if (old_range)   // y -1..1 in the older files: onto 0..1 with the same sense (smaller = higher [G])
        for (double &y : l.y) y = (clampd(y, -1, 1) + 1) * 0.5;
    return true;
}

// ---- the sources ----
struct Audio { std::vector<float> s; };
double sample_at(const Audio &au, double x) {
    const size_t n = au.s.size();
    if (x <= 0) return au.s[0];
    if (x >= double(n - 1)) return au.s[n - 1];
    const size_t i = size_t(x);
    const double f = x - double(i);
    return au.s[i] + (au.s[i + 1] - au.s[i]) * f;
}
// one cycle read from start (samples), window samples long, made to loop: over the last `fade` of it the read crossfades
// into the cycle before (x - window), which meets the first sample. Near the file's start (no cycle before) the read and
// the cycle after (x + window) cross over the whole cycle instead, which also meets at both ends
Cycle audio_cycle(const Audio &au, double start, double window, double fade) {
    Cycle c(N);
    const double n = double(au.s.size());
    start = clampd(start, 0, std::max(0.0, n - 1));
    fade = clampd(fade, 0, 1);
    const bool back = start >= window;
    for (int i = 0; i < N; i++) {
        const double t = double(i) / N, x = start + t * window;
        if (fade < 1e-6) c[size_t(i)] = sample_at(au, x);
        else if (back) {
            const double w = clampd((t - (1 - fade)) / fade, 0, 1);
            c[size_t(i)] = sample_at(au, x) * (1 - w) + sample_at(au, x - window) * w;
        } else c[size_t(i)] = sample_at(au, x) * t + sample_at(au, x + window) * (1 - t);
    }
    return c;
}

Cycle clear_phase(const Cycle &c) {
    std::vector<Cx> X = spectrum(c);
    for (int k = 1; k <= N / 2; k++) X[size_t(k)] = Cx(0, -std::abs(X[size_t(k)]));   // every harmonic a sine at phase 0
    X[0] = 0;
    return from_spectrum(X);
}

// ---- the modifiers ----
void phase_shift(Cycle &c, double phase, double mix, int style) {
    std::vector<Cx> X = spectrum(c);
    for (int k = 1; k < N / 2; k++) {
        double ph = phase;
        if (style == 1) ph = k % 2 ? -phase : phase;
        else if (style == 2) ph = phase * k;
        else if (style == 3) ph = (k % 2 ? -phase : phase) * k;
        X[size_t(k)] *= std::polar(1.0, ph);
    }
    const Cycle s = from_spectrum(X);
    mix = clampd(mix, 0, 1);
    for (int i = 0; i < N; i++) c[size_t(i)] += (s[size_t(i)] - c[size_t(i)]) * mix;
}
void wave_window(Cycle &c, double left, double right, int shape) {
    left = clampd(left, 0, 1);
    right = clampd(right, left, 1);
    auto f = [shape](double u) {
        u = clampd(u, 0, 1);
        switch (shape) {
        case 1: return std::sin(M_PI / 2 * u);
        case 2: return u;
        case 3: return u >= 1 ? 1.0 : 0.0;
        case 4: return std::fabs(std::sin(M_PI * 1.5 * u)) * u;
        default: return 0.5 - 0.5 * std::cos(M_PI * u);
        }
    };
    for (int i = 0; i < N; i++) {
        const double t = double(i) / N;
        double w = 1;
        if (t < left) w = f(t / std::max(1e-9, left));
        else if (t > right) w = f((1 - t) / std::max(1e-9, 1 - right));
        c[size_t(i)] *= w;
    }
}
void freq_filter(Cycle &c, double cutoff, double shape, int style, bool normalize) {
    const double before = rms(c);
    std::vector<Cx> X = spectrum(c);
    const double fc = std::exp2(clampd(cutoff, -2, 11)), order = 1 + 7 * clampd(shape, 0, 1);
    for (int k = 1; k <= N / 2; k++) {
        const double r = double(k) / fc;
        double g;
        switch (style) {
        case 1: { const double o = std::log2(r); g = std::exp(-o * o * (0.5 + 8 * clampd(shape, 0, 1))); break; }
        case 2: g = 1 / (1 + std::pow(1 / r, 2 * order)); break;
        case 3: g = std::pow(0.5 + 0.5 * std::cos(2 * M_PI * r), order); break;
        default: g = 1 / (1 + std::pow(r, 2 * order)); break;
        }
        X[size_t(k)] *= std::isfinite(g) ? g : 0;
    }
    c = from_spectrum(X);
    const double after = rms(c);
    if (normalize && after > 1e-9 && before > 1e-9) for (double &v : c) v *= before / after;
}
void slew(Cycle &c, double up, double down) {
    const double mu = up > 1e-4 ? 2.0 / std::max(1.0, up * N) : 1e9, md = down > 1e-4 ? 2.0 / std::max(1.0, down * N) : 1e9;
    if (mu > 1e8 && md > 1e8) return;
    Cycle out = c;
    double prev = c[size_t(N - 1)];
    for (int pass = 0; pass < 2; pass++)   // twice round: the start settles onto the end
        for (int i = 0; i < N; i++) {
            const double x = c[size_t(i)];
            prev = clampd(x, prev - md, prev + mu);
            out[size_t(i)] = prev;
        }
    c = out;
}
void fold(Cycle &c, double boost) {
    boost = clampd(boost, 0, 64);
    for (double &v : c) v = std::sin(v * boost * M_PI / 2);
}
void warp(Cycle &c, double hp, double vp, bool hasym, bool vasym) {
    if (std::fabs(hp) > 1e-3) {
        const Cycle in = c;
        for (int i = 0; i < N; i++) {
            const double t = double(i) / N;
            double u;
            if (hasym) u = power_scale(t, hp);
            else u = t < 0.5 ? 0.5 * power_scale(2 * t, hp) : 1 - 0.5 * power_scale(2 - 2 * t, hp);
            const double x = clampd(u, 0, 1) * N;
            const size_t i0 = size_t(x) % size_t(N), i1 = (i0 + 1) % size_t(N);
            c[size_t(i)] = in[i0] + (in[i1] - in[i0]) * (x - std::floor(x));
        }
    }
    if (std::fabs(vp) > 1e-3)
        for (double &v : c) {
            if (vasym) v = power_scale((clampd(v, -1, 1) + 1) * 0.5, vp) * 2 - 1;
            else v = (v < 0 ? -1 : 1) * power_scale(std::fabs(clampd(v, -1, 1)), vp);
        }
}

// ---- one group at a position ----
struct Group {
    std::vector<Comp> comps;
    Audio audio;   // the group's Audio File Source's samples (decoded once)
    bool has_audio = false;
};

Cycle source_at(const Group &g, const Comp &s, double pos) {
    const Around r = s.at(pos);
    if (!r.a) return {};
    if (s.kind == "Wave Source" || s.kind == "Shepard Tone Source") {
        const bool spectral = num(*s.c, "interpolation", 0) >= 1;
        return blend(wave_data(*r.a), r.t > 0 ? wave_data(*r.b) : Cycle(), r.t, spectral);
    }
    if (s.kind == "Line Source") {
        VitalLine la, lb;
        bool olda = false, oldb = false;
        if (!line_of(*r.a, la, olda)) return {};
        if (r.t > 0 && line_of(*r.b, lb, oldb) && lb.x.size() == la.x.size()) {   // the points glide (same count)
            for (size_t i = 0; i < la.x.size(); i++) {
                la.x[i] += (lb.x[i] - la.x[i]) * r.t;
                la.y[i] += (lb.y[i] - la.y[i]) * r.t;
                la.power[i] += (lb.power[i] - la.power[i]) * r.t;
            }
            return line_cycle(la, olda);
        }
        if (r.t > 0 && !lb.x.empty()) return blend(line_cycle(la, olda), line_cycle(lb, oldb), r.t, false);
        return line_cycle(la, olda);
    }
    if (s.kind == "Audio File Source" && g.has_audio) {
        double window = num(*s.c, "window_size", 0);
        if (window < 2) window = num(*r.a, "window_size", N);
        window = clampd(window, 16, double(g.audio.s.size()));
        const int fade_style = int(num(*s.c, "fade_style", 0));
        Cycle c;
        if (fade_style == 0) {   // the start glides
            const double st = num(*r.a, "start_position", 0) + (num(*r.b, "start_position", 0) - num(*r.a, "start_position", 0)) * r.t;
            c = audio_cycle(g.audio, st, window, num(*r.a, "window_fade", 1));
        } else {
            c = audio_cycle(g.audio, num(*r.a, "start_position", 0), window, num(*r.a, "window_fade", 1));
            if (fade_style >= 2 && r.t > 0)
                c = blend(c, audio_cycle(g.audio, num(*r.b, "start_position", 0), window, num(*r.b, "window_fade", 1)), r.t, fade_style == 3);
        }
        if (int(num(*s.c, "phase_style", 0)) >= 1) c = clear_phase(c);
        if (num(*s.c, "normalize_gain", 0) > 0.5) { const double p = peak(c); if (p > 1e-9) for (double &v : c) v /= p; }
        return c;
    }
    return {};
}

void modify(const Comp &m, Cycle &c, double pos) {
    if (m.kfs.empty()) {   // a modifier without keyframes: its defaults [G]
        if (m.kind == "Wave Folder") fold(c, 1);
        return;
    }
    if (m.kind == "Phase Shift") phase_shift(c, m.val(pos, "phase", 0), m.val(pos, "mix", 1), int(num(*m.c, "style", 0)));
    else if (m.kind == "Wave Window") wave_window(c, m.val(pos, "left_position", 0), m.val(pos, "right_position", 1), int(num(*m.c, "window_shape", 0)));
    else if (m.kind == "Frequency Filter") freq_filter(c, m.val(pos, "cutoff", 10), m.val(pos, "shape", 0.5), int(num(*m.c, "style", 0)), num(*m.c, "normalize", 1) > 0.5);
    else if (m.kind == "Slew Limiter") slew(c, m.val(pos, "up_run_rise", 0), m.val(pos, "down_run_rise", 0));
    else if (m.kind == "Wave Folder") fold(c, m.val(pos, "fold_boost", 1));
    else if (m.kind == "Wave Warp") warp(c, m.val(pos, "horizontal_power", 0), m.val(pos, "vertical_power", 0),
                                         num(*m.c, "horizontal_asymmetric", 0) > 0.5, num(*m.c, "vertical_asymmetric", 0) > 0.5);
}

bool is_source(const std::string &k) {
    return k == "Wave Source" || k == "Line Source" || k == "Audio File Source" || k == "Shepard Tone Source";
}
bool is_modifier(const std::string &k) {
    return k == "Phase Shift" || k == "Wave Window" || k == "Frequency Filter" || k == "Slew Limiter" || k == "Wave Folder" || k == "Wave Warp";
}

}  // namespace

std::vector<uint8_t> vital_base64(const std::string &s, size_t limit) {
    static const std::array<int8_t, 256> T = [] {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) t[uint8_t(a[i])] = int8_t(i);
        return t;
    }();
    std::vector<uint8_t> out;
    out.reserve(std::min(limit, s.size() / 4 * 3));
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : s) {
        const int v = T[c];
        if (v < 0) continue;   // '=', line breaks
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out.size() >= limit) break;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return out;
}

bool vital_line_parse(const Json &j, VitalLine &out) {
    out = VitalLine();
    const Json &pts = j["points"];
    if (pts.type != Json::Array || pts.arr.size() < 2) return false;
    std::vector<std::array<double, 3>> p;
    const Json &pw = j["powers"];
    for (size_t i = 0; i + 1 < pts.arr.size() && p.size() < MAX_POINTS; i += 2) {
        const double x = pts.arr[i].as_num(), y = pts.arr[i + 1].as_num();
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        const size_t pi = i / 2;
        const double w = pw.type == Json::Array && pi < pw.arr.size() ? pw.arr[pi].as_num() : 0;
        p.push_back({clampd(x, 0, 1), y, std::isfinite(w) ? w : 0});
    }
    if (p.empty()) return false;
    std::stable_sort(p.begin(), p.end(), [](const std::array<double, 3> &a, const std::array<double, 3> &b) { return a[0] < b[0]; });
    for (auto &q : p) { out.x.push_back(q[0]); out.y.push_back(q[1]); out.power.push_back(q[2]); }
    out.smooth = num(j, "smooth", 0) > 0.5;
    out.name = j["name"].as_str();
    return true;
}

double vital_line_value(const VitalLine &l, double ph) {
    const size_t n = l.x.size();
    if (n == 0) return 0.5;
    if (n == 1) return 1 - clampd(l.y[0], 0, 1);
    ph -= std::floor(ph);
    double x0, y0, x1, y1, pw;
    if (ph < l.x[0] || ph >= l.x[n - 1]) {   // round from the last point to the first
        x0 = l.x[n - 1] - (ph < l.x[0] ? 1 : 0); y0 = l.y[n - 1];
        x1 = l.x[0] + (ph < l.x[0] ? 0 : 1); y1 = l.y[0];
        pw = l.power[n - 1];
    } else {
        size_t i = 0;
        while (i + 2 < n && !(ph < l.x[i + 1])) i++;
        x0 = l.x[i]; y0 = l.y[i]; x1 = l.x[i + 1]; y1 = l.y[i + 1]; pw = l.power[i];
    }
    double t = x1 - x0 > 1e-9 ? clampd((ph - x0) / (x1 - x0), 0, 1) : 1;
    t = power_scale(t, pw);
    if (l.smooth) t = 0.5 - 0.5 * std::cos(M_PI * t);
    const double y = y0 + (y1 - y0) * t;
    return 1 - clampd(y, 0, 1);
}

std::vector<float> vital_lfo_table(const VitalLine &l) {
    std::vector<float> t(size_t(VITAL_LFO_TAB) + 1);
    for (int i = 0; i < VITAL_LFO_TAB; i++) t[size_t(i)] = float(2 * vital_line_value(l, double(i) / VITAL_LFO_TAB) - 1);
    t[size_t(VITAL_LFO_TAB)] = t[0];
    return t;
}

bool vital_wavetable_render(const Json &table, int max_frames, std::vector<float> &cycles, int &frames, std::vector<std::string> *notes) {
    cycles.clear();
    frames = 0;
    const Json &groups = table["groups"];
    if (groups.type != Json::Array || groups.arr.empty()) return false;
    std::vector<Group> gs;
    bool moves = false;
    auto note = [notes](const std::string &s) {
        if (notes && std::find(notes->begin(), notes->end(), s) == notes->end()) notes->push_back(s);
    };
    for (size_t gi = 0; gi < groups.arr.size() && gi < MAX_GROUPS; gi++) {
        const Json &comps = groups.arr[gi]["components"];
        if (comps.type != Json::Array) continue;
        Group g;
        bool source = false;
        for (size_t ci = 0; ci < comps.arr.size() && ci < MAX_COMPONENTS; ci++) {
            const Json &c = comps.arr[ci];
            Comp m;
            m.c = &c;
            m.kind = kind_of(c);
            if (!is_source(m.kind) && !is_modifier(m.kind)) { note("Vital wavetable: " + m.kind + " not read"); continue; }
            if (is_source(m.kind) && source) { note("Vital wavetable: a group's second source not read"); continue; }
            m.style = c.has("interpolation_style") ? int(num(c, "interpolation_style", 1)) : 1;
            const Json &kfs = c["keyframes"];
            if (kfs.type == Json::Array)
                for (size_t k = 0; k < kfs.arr.size() && k < MAX_KEYFRAMES; k++) m.kfs.push_back(&kfs.arr[k]);
            std::stable_sort(m.kfs.begin(), m.kfs.end(), [](const Json *a, const Json *b) { return num(*a, "position", 0) < num(*b, "position", 0); });
            if (is_source(m.kind) && m.kfs.empty()) continue;
            if (m.kfs.size() > 1) moves = true;   // (interpolated, or steps from keyframe to keyframe)
            if (m.kind == "Audio File Source") {
                const std::vector<uint8_t> b = vital_base64(c["audio_file"].as_str(), MAX_AUDIO_BYTES);
                const size_t n = b.size() / 2;
                if (n < 16) { note("Vital wavetable: an audio file could not be read"); continue; }
                g.audio.s.resize(n);
                for (size_t i = 0; i < n; i++) g.audio.s[i] = float(int16_t(uint16_t(b[2 * i] | (b[2 * i + 1] << 8)))) / 32768.0f;
                g.has_audio = true;
            }
            if (m.kind == "Shepard Tone Source") note("Vital wavetable: Shepard tone played as its wave");
            if (m.kind == "Line Source" && m.style == 2) m.style = 1;
            source = source || is_source(m.kind);
            g.comps.push_back(std::move(m));
        }
        if (source) gs.push_back(std::move(g));
    }
    if (gs.empty()) return false;
    frames = moves ? std::max(1, std::min(max_frames, 256)) : 1;
    cycles.assign(size_t(frames) * N, 0.0f);
    bool any = false;
    for (int f = 0; f < frames; f++) {
        const double pos = frames > 1 ? 256.0 * f / (frames - 1) : 0;
        Cycle sum(N, 0.0);
        for (const Group &g : gs) {
            Cycle c;
            for (const Comp &m : g.comps) {
                if (is_source(m.kind)) { c = source_at(g, m, pos); continue; }
                if (!c.empty()) modify(m, c, pos);
            }
            if (c.size() != size_t(N)) continue;
            for (int i = 0; i < N; i++) sum[size_t(i)] += std::isfinite(c[size_t(i)]) ? c[size_t(i)] : 0;
            any = true;
        }
        for (int i = 0; i < N; i++) cycles[size_t(f) * N + size_t(i)] = float(clampd(sum[size_t(i)], -64, 64));
    }
    if (!any) { cycles.clear(); frames = 0; return false; }
    return true;
}

bool vital_wavetable_from_text(const std::string &text, int osc, int max_frames, std::vector<float> &cycles, int &frames,
                               std::string &name, std::string &err) {
    try {
        const Json j = parse_json(text);
        const Json *t = &j;
        if (osc > 0) {
            const Json &w = j["settings"]["wavetables"];
            if (w.type != Json::Array || size_t(osc - 1) >= w.arr.size()) { err = "Vital: the preset has no wavetable " + std::to_string(osc); return false; }
            t = &w.arr[size_t(osc - 1)];
        }
        name = (*t)["name"].as_str();
        if (!vital_wavetable_render(*t, max_frames, cycles, frames, nullptr)) { err = "Vital: the wavetable could not be read"; return false; }
        return true;
    } catch (const std::exception &e) {
        err = std::string("Vital: ") + e.what();
        return false;
    }
}

bool vital_lfo_from_text(const std::string &text, int lfo, VitalLine &out, std::string &err) {
    try {
        const Json j = parse_json(text);
        const Json *l = &j;
        if (lfo > 0) {
            const Json &a = j["settings"]["lfos"];
            if (a.type != Json::Array || size_t(lfo - 1) >= a.arr.size()) { err = "Vital: the preset has no LFO " + std::to_string(lfo); return false; }
            l = &a.arr[size_t(lfo - 1)];
        }
        if (!vital_line_parse(*l, out)) { err = "Vital: the LFO shape could not be read"; return false; }
        return true;
    } catch (const std::exception &e) {
        err = std::string("Vital: ") + e.what();
        return false;
    }
}

bool vital_ref_parse(const std::string &ref, const char *kind, int &index, std::string &path) {
    const size_t kl = std::strlen(kind);
    if (ref.size() < kl + 4 || ref.compare(0, kl, kind) || ref[kl] != ':') return false;
    size_t i = kl + 1;
    int n = 0, digits = 0;
    while (i < ref.size() && std::isdigit(static_cast<unsigned char>(ref[i])) && digits < 3) { n = n * 10 + (ref[i] - '0'); i++; digits++; }
    if (!digits || i >= ref.size() || ref[i] != ':' || i + 1 >= ref.size()) return false;
    index = n;
    path = ref.substr(i + 1);
    return true;
}

}  // namespace omni

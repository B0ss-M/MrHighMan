// Omni Sampler: automatic loop points for sustaining samples that have none.
//
// The loop end is an upward zero crossing near 90% of the sample; the start is the upward zero crossing in the
// middle part of the sample whose surrounding waveform best matches the one before the end (least squared
// difference over +-256 frames, both channels), with the loop at least 0.1 s long. Cheap enough for the loader
// thread on ARM: a few hundred candidates of 512 frames each.
#include "autoloop.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace omni {

namespace {

constexpr int WIN = 256;

float frame(const Pcm &p, int64_t i) {
    const int ch = p.channels;
    float v = p.data[size_t(i) * ch];
    if (ch > 1) v = 0.5f * (v + p.data[size_t(i) * ch + 1]);
    return v;
}

// upward zero crossings in [a, b)
std::vector<int64_t> crossings(const Pcm &p, int64_t a, int64_t b, size_t limit) {
    std::vector<int64_t> out;
    float prev = frame(p, a);
    for (int64_t i = a + 1; i < b; i++) {
        float v = frame(p, i);
        if (prev < 0 && v >= 0) out.push_back(i);
        prev = v;
    }
    if (out.size() > limit) {   // thin evenly
        std::vector<int64_t> t;
        double step = double(out.size()) / double(limit);
        for (size_t k = 0; k < limit; k++) t.push_back(out[size_t(double(k) * step)]);
        out.swap(t);
    }
    return out;
}

double mismatch(const Pcm &p, int64_t s, int64_t e) {
    double d = 0, energy = 1e-9;
    const int ch = p.channels;
    for (int k = -WIN; k < WIN; k++)
        for (int c = 0; c < ch && c < 2; c++) {
            float a = p.data[size_t(s + k) * ch + c], b = p.data[size_t(e + k) * ch + c];
            d += double(a - b) * double(a - b);
            energy += double(b) * double(b);
        }
    return d / energy;
}

}  // namespace

bool find_loop(const Pcm &p, int64_t start, int64_t stop, int64_t &loop_start, int64_t &loop_end) {
    const int64_t frames = p.frames();
    stop = std::min(stop <= 0 ? frames : stop, frames);
    start = std::max<int64_t>(0, start);
    const int64_t n = stop - start;
    const int64_t min_len = std::max<int64_t>(p.rate / 10, 2 * WIN);
    if (n < p.rate * 3 / 10 || n < 4 * min_len) return false;
    // the end: the upward zero crossing nearest 90%
    std::vector<int64_t> ends = crossings(p, start + n * 80 / 100, std::min(stop - WIN - 1, start + n * 97 / 100), 64);
    if (ends.empty()) return false;
    int64_t target = start + n * 90 / 100;
    int64_t e = *std::min_element(ends.begin(), ends.end(), [&](int64_t a, int64_t b) { return std::llabs(a - target) < std::llabs(b - target); });
    // the start: best match among crossings in 25%..(end - min length)
    int64_t lo = std::max(start + WIN + 1, start + n * 25 / 100), hi = e - min_len;
    if (hi <= lo) return false;
    std::vector<int64_t> starts = crossings(p, lo, hi, 400);
    if (starts.empty()) return false;
    double best = 1e30;
    int64_t bs = -1;
    for (int64_t s : starts) {
        double m = mismatch(p, s, e);
        if (m < best) { best = m; bs = s; }
    }
    if (bs < 0 || best > 0.5) return false;   // nothing similar enough: better no loop than a click
    loop_start = bs;
    loop_end = e - 1;   // inclusive: the frame before the matching crossing
    return true;
}

int auto_loop(Instrument &inst, const std::vector<PcmPtr> &pcm) {
    int added = 0;
    for (size_t i = 0; i < inst.zones.size() && i < pcm.size(); i++) {
        Zone &z = inst.zones[i];
        const Pcm *p = pcm[i].get();
        if (!p || !z.loops.empty() || z.one_shot || z.reverse || z.trigger == Trigger::Release) continue;
        if (z.amp_env.set && z.amp_env.sustain < 0.05) continue;   // decays to silence while held: a drum, a pluck
        int64_t ls, le;
        if (!find_loop(*p, z.start, z.stop, ls, le)) continue;
        Loop l;
        l.start = ls;
        l.end = le;
        z.loops.push_back(l);
        added++;
    }
    return added;
}

}  // namespace omni

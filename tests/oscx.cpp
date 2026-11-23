// Omni Sampler: oscillator extractor. Reads ROMs (or anything else) with Omni's own readers and writes each short
// forward loop that holds a steady wave as a single cycle the SYNTH's oscillators load (2048 samples, 16-bit mono WAV).
//   oscx <out dir> <name> <file or folder>...
// The loop is periodic over its length L, so its DFT bins are exact: a loop of k cycles only has energy at multiples
// of k. One cycle = those bins as harmonics 1, 2, 3...: rebuilt at 2048 samples, band-limited, no resampling error.
// Kept: loops of 8..8192 frames, at most 32 cycles, 97% of the energy on the cycle's harmonics, not near-duplicates.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <sys/stat.h>
#include "../src/formats/format.hpp"

using namespace omni;
using cd = std::complex<double>;

static const int N = 2048, H = 1023;

struct Kept { std::vector<double> mag; };
struct Cand { std::vector<double> y, mag; int hmax; int64_t L, k; std::string sname; };

static std::string clean(std::string s) {
    for (char &c : s) if (!(isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '+' || c == '.' || c == '_')) c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s[0] == ' ') s.erase(0, 1);
    std::string o;   // runs of spaces to one
    for (char c : s) if (!(c == ' ' && !o.empty() && o.back() == ' ')) o += c;
    return o.empty() ? "Wave" : o.substr(0, 40);
}

static bool write_wav(const std::string &path, const std::vector<double> &x) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const char clm[] = "<!>2048 00000000 wavetable (Omni oscx)";
    uint32_t clm_len = uint32_t(sizeof clm - 1 + 1) & ~1u;
    std::fwrite("RIFF", 1, 4, f); u32(uint32_t(4 + 8 + 16 + 8 + clm_len + 8 + x.size() * 2)); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1); u32(44100); u32(88200); u16(2); u16(16);
    std::fwrite("clm ", 1, 4, f); u32(clm_len); std::fwrite(clm, 1, sizeof clm - 1, f); if ((sizeof clm - 1) & 1) std::fputc(0, f);
    std::fwrite("data", 1, 4, f); u32(uint32_t(x.size() * 2));
    for (double v : x) { int s = int(std::lround(v * 32767)); s = std::max(-32768, std::min(32767, s)); u16(uint16_t(int16_t(s))); }
    std::fclose(f);
    return true;
}

int main(int argc, char **argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: oscx <out dir> <name> <file or folder>...\n"); return 2; }
    register_all();
    Vfs vfs;
    const std::string out = argv[1], set = argv[2];
    mkdir(out.c_str(), 0755);
    std::vector<std::string> files;
    std::function<void(const std::string &, int)> walk = [&](const std::string &p, int depth) {
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            if (depth > 3) return;
            for (auto &e : vfs.list(p)) walk(p + "/" + e.name, depth + 1);
        } else files.push_back(p);
    };
    for (int i = 3; i < argc; i++) walk(argv[i], 0);
    std::vector<Kept> kept;
    std::set<std::string> names;
    std::set<std::string> seen_samples;
    int n_loops = 0, n_written = 0;
    for (auto &path : files) {
        try {
            Location loc = vfs.resolve(path);
            const FormatReader *r = find_reader(*loc.volume, loc.inner);
            if (!r) continue;
            std::vector<PresetInfo> presets = r->list(loc.volume, loc.inner);
            std::fprintf(stderr, "%s: %s, %zu presets\n", path.c_str(), r->name, presets.size());
            for (auto &pi : presets) {
                Instrument inst;
                try { inst = r->load(loc.volume, loc.inner, pi.index); } catch (const std::exception &) { continue; }
                std::vector<Cand> cands;
                for (auto &z : inst.zones) {
                    if (z.loops.empty() || z.loops[0].type != LoopType::Forward || !z.sample) continue;
                    const int64_t ls = z.loops[0].start, le = z.loops[0].end + 1, L = le - ls;
                    if (L < 8 || L > 4096 || ls < 0) continue;
                    char id[64];
                    std::snprintf(id, sizeof id, "%p/%lld/%lld", (void *)z.sample.get(), (long long)ls, (long long)L);
                    std::string key = z.sample->name + "|" + std::to_string(z.sample->frames) + "|" + id;
                    if (!seen_samples.insert(key).second) continue;
                    PcmPtr p;
                    try { p = z.sample->decode(); } catch (const std::exception &) { continue; }
                    if (!p || le > p->frames()) continue;
                    n_loops++;
                    std::vector<double> x(static_cast<size_t>(L));
                    for (int64_t i = 0; i < L; i++) x[size_t(i)] = p->data[size_t((ls + i) * p->channels)] / 32768.0;
                    // the loop's spectrum (bins 1..L/2; DC dropped)
                    const int64_t B = L / 2;
                    std::vector<cd> X(static_cast<size_t>(B + 1));
                    double tot = 0;
                    for (int64_t b = 1; b <= B; b++) {
                        cd s = 0;
                        const double w = -2 * M_PI * double(b) / double(L);
                        for (int64_t i = 0; i < L; i++) s += x[size_t(i)] * cd(std::cos(w * double(i)), std::sin(w * double(i)));
                        X[size_t(b)] = s;
                        tot += std::norm(s);
                    }
                    if (tot <= 0) continue;
                    double rms = std::sqrt(2 * tot) / double(L);
                    if (rms < 0.004) continue;   // silence
                    // the cycles in the loop: the lowest strong bin, then the gcd of the strong bins
                    double mx = 0;
                    for (int64_t b = 1; b <= B; b++) mx = std::max(mx, std::abs(X[size_t(b)]));
                    int64_t k = 0;
                    for (int64_t b = 1; b <= B; b++)
                        if (std::abs(X[size_t(b)]) > 0.08 * mx) k = k ? std::gcd(k, b) : b;
                    if (k < 1 || k > 32) continue;
                    double on = 0;
                    for (int64_t b = k; b <= B; b += k) on += std::norm(X[size_t(b)]);
                    if (on < 0.97 * tot) continue;   // not a steady wave
                    // one cycle: harmonic h = bin h*k
                    std::vector<double> y(N, 0.0), mag(64, 0.0);
                    int hmax = int(std::min<int64_t>(H, B / k));
                    for (int h = 1; h <= hmax; h++) {
                        cd c = X[size_t(h * k)];
                        if (h < 64) mag[size_t(h)] = std::abs(c);
                        if (h == hmax && (L % 2 == 0) && (h * k == B)) c *= 0.5;   // Nyquist bin
                        const double a = 2 * std::abs(c) / double(L), ph = std::arg(c);
                        if (a < 1e-6) continue;
                        for (int i = 0; i < N; i++) y[size_t(i)] += a * std::cos(2 * M_PI * h * i / N + ph);
                    }
                    double pk = 0;
                    for (double v : y) pk = std::max(pk, std::fabs(v));
                    if (pk < 1e-4) continue;
                    for (double &v : y) v *= 0.95 / pk;
                    double nm = std::sqrt(std::inner_product(mag.begin(), mag.end(), mag.begin(), 0.0));
                    for (double &v : mag) v /= nm;
                    cands.push_back({y, mag, hmax, L, k, z.sample->name});
                }
                // the richest first; a preset's other loops only when they sound different (a multisample's octave
                // copies of one wave are dropped); near-duplicates of anything already written are skipped
                std::stable_sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.hmax > b.hmax; });
                std::vector<const Cand *> mine;
                for (auto &c : cands) {
                    auto sim = [&](const std::vector<double> &m) { return std::inner_product(c.mag.begin(), c.mag.end(), m.begin(), 0.0); };
                    bool dup = false;
                    for (auto *o : mine) if (sim(o->mag) > 0.9) { dup = true; break; }
                    for (auto &kk : kept) if (!dup && sim(kk.mag) > 0.99) dup = true;
                    if (dup) continue;
                    mine.push_back(&c);
                    kept.push_back({c.mag});
                }
                for (auto *c : mine) {
                    std::string nmz = clean(mine.size() == 1 || c->sname.empty() ? pi.name : c->sname);
                    std::string base = nmz;
                    for (int d = 2; names.count(base); d++) base = nmz + " " + std::to_string(d);
                    names.insert(base);
                    char fn[512];
                    std::snprintf(fn, sizeof fn, "%s/%03d %s.wav", out.c_str(), n_written + 1, base.c_str());
                    if (write_wav(fn, c->y)) n_written++;
                    std::printf("%03d %-40s loop %5lld, %2lld cycles, harmonics %4d, %s\n", n_written, base.c_str(), (long long)c->L,
                                (long long)c->k, c->hmax, pi.name.c_str());
                }
            }
        } catch (const std::exception &e) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), e.what());
        }
    }
    std::fprintf(stderr, "%s: %d short loops, %d oscillators written to %s\n", set.c_str(), n_loops, n_written, out.c_str());
    return 0;
}

#include "synth.hpp"
#include "sampler.hpp"   // 2.3.4: Program (SAMPLE oscillators read a slot's zones)

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <mutex>
#include "../core/audio.hpp"
#include "../core/parse.hpp"
#include "../core/vfs.hpp"

namespace omni {

namespace {

constexpr double TAU = 6.283185307179586;

// in-place radix-2 FFT of n (a power of 2) complex values; inverse without the 1/n
void fft(std::vector<std::complex<double>> &a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = TAU / double(len) * (inverse ? 1 : -1);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; k++) {
                std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// a single cycle from harmonic amplitudes (sine phase): amps[k] for harmonic k (amps[0] ignored)
std::vector<float> from_harmonics(const std::vector<double> &amps) {
    std::vector<std::complex<double>> x(WT_SIZE);
    for (size_t k = 1; k < amps.size() && k < size_t(WT_SIZE / 2); k++) {
        // sin(2 pi k t) = (e^{i..} - e^{-i..}) / 2i: bin k gets -i a/2, bin N-k gets +i a/2
        x[k] = std::complex<double>(0, -amps[k] / 2);
        x[WT_SIZE - k] = std::complex<double>(0, amps[k] / 2);
    }
    fft(x, true);
    std::vector<float> out(WT_SIZE);
    for (int i = 0; i < WT_SIZE; i++) out[size_t(i)] = float(x[size_t(i)].real());
    return out;
}

std::vector<double> saw_amps(int n = WT_SIZE / 2 - 1) { std::vector<double> a(size_t(n) + 1); for (int k = 1; k <= n; k++) a[size_t(k)] = 1.0 / k; return a; }
std::vector<double> square_amps(int n = WT_SIZE / 2 - 1) { std::vector<double> a(size_t(n) + 1); for (int k = 1; k <= n; k += 2) a[size_t(k)] = 1.0 / k; return a; }
std::vector<double> tri_amps(int n = WT_SIZE / 2 - 1) {
    std::vector<double> a(size_t(n) + 1);
    for (int k = 1; k <= n; k += 2) a[size_t(k)] = ((k / 2) % 2 ? -1.0 : 1.0) / (double(k) * k);
    return a;
}
std::vector<double> pulse_amps(double w, int n = WT_SIZE / 2 - 1) {
    std::vector<double> a(size_t(n) + 1);
    for (int k = 1; k <= n; k++) a[size_t(k)] = std::sin(M_PI * k * w) / k;
    return a;
}

// ---- the factory set -----------------------------------------------------------------------------------------------
const char *const CLASSIC[] = {"Sine", "Triangle", "Saw", "Square", "Pulse 25%", "Pulse 10%", "Soft Saw", "Organ"};
const char *const TABLES[] = {"Basic Shapes", "PWM Sweep", "Harmonic Build", "Odd Build", "Vowels", "Sync Sweep",
                              "Wavefold", "FM Bells", "Spectral Comb", "Resonant Sweep"};
constexpr int N_CLASSIC = int(sizeof CLASSIC / sizeof CLASSIC[0]), N_TABLES = int(sizeof TABLES / sizeof TABLES[0]);
constexpr int TF = 64;   // frames in a factory wavetable

std::vector<float> classic_cycle(int i) {
    switch (i) {
    case 0: { std::vector<double> a(2); a[1] = 1; return from_harmonics(a); }
    case 1: return from_harmonics(tri_amps());
    case 2: return from_harmonics(saw_amps());
    case 3: return from_harmonics(square_amps());
    case 4: return from_harmonics(pulse_amps(0.25));
    case 5: return from_harmonics(pulse_amps(0.10));
    case 6: { std::vector<double> a(1024); for (int k = 1; k < 1024; k++) a[size_t(k)] = std::pow(double(k), -1.6); return from_harmonics(a); }
    default: {   // drawbars 16', 8', 5 1/3', 4', 2 2/3', 2': harmonics 1, 2, 3, 4, 6, 8
        std::vector<double> a(9);
        a[1] = 0.8; a[2] = 0.7; a[3] = 0.5; a[4] = 0.45; a[6] = 0.3; a[8] = 0.25;
        return from_harmonics(a);
    }
    }
}

// the frames of factory wavetable i, t = 0..1 across the table
std::vector<float> table_frame(int i, double t) {
    std::vector<float> c(WT_SIZE);
    auto fill = [&](auto f) { for (int s = 0; s < WT_SIZE; s++) c[size_t(s)] = float(f(double(s) / WT_SIZE)); };
    switch (i) {
    case 0: {   // Basic Shapes: sine -> triangle -> saw -> square
        static std::vector<float> sh[4];
        static std::once_flag once;
        std::call_once(once, [] { sh[0] = classic_cycle(0); sh[1] = classic_cycle(1); sh[2] = classic_cycle(2); sh[3] = classic_cycle(3); });
        double m = t * 3; int a = std::min(2, int(m)); double f = m - a;
        // each shape at the same loudness before blending
        auto rms = [](const std::vector<float> &v) { double s = 0; for (float x : v) s += double(x) * x; return std::sqrt(s / double(v.size())); };
        double ga = 1 / rms(sh[a]), gb = 1 / rms(sh[a + 1]);
        for (int s = 0; s < WT_SIZE; s++) c[size_t(s)] = float(sh[a][size_t(s)] * ga * (1 - f) + sh[a + 1][size_t(s)] * gb * f);
        return c;
    }
    case 1: return from_harmonics(pulse_amps(0.5 - 0.47 * t));
    case 2: { int n = 1 + int(t * 63 + 0.5); std::vector<double> a(size_t(n) + 1); for (int k = 1; k <= n; k++) a[size_t(k)] = 1.0 / k; return from_harmonics(a); }
    case 3: { int n = 1 + 2 * int(t * 40 + 0.5); std::vector<double> a(size_t(n) + 1); for (int k = 1; k <= n; k += 2) a[size_t(k)] = 1.0 / k; return from_harmonics(a); }
    case 4: {   // Vowels: A E I O U formants on a 110 Hz voice
        static const double F[5][3] = {{800, 1150, 2900}, {400, 1600, 2700}, {250, 1750, 2600}, {450, 800, 2830}, {325, 700, 2530}};
        double m = t * 4; int a = std::min(3, int(m)); double f = m - a;
        std::vector<double> amps(200);
        for (int k = 1; k < 200; k++) {
            double hz = 110.0 * k, v = 0;
            for (int j = 0; j < 3; j++) {
                double fc = F[a][j] * (1 - f) + F[a + 1][j] * f, bw = 60 + 40 * j, g = j == 0 ? 1 : j == 1 ? 0.6 : 0.3;
                v += g / (1 + ((hz - fc) / bw) * ((hz - fc) / bw));
            }
            amps[size_t(k)] = v * std::pow(double(k), -0.5);
        }
        return from_harmonics(amps);
    }
    case 5: { double r = 1 + 7 * t; fill([&](double x) { double p = x * r; p -= std::floor(p); return (2 * p - 1) * (1 - x); }); return c; }
    case 6: { double g = 1 + 5 * t; fill([&](double x) { return std::sin(M_PI / 2 * g * std::sin(TAU * x)); }); return c; }
    case 7: { double ix = 8 * t; fill([&](double x) { return std::sin(TAU * x + ix * std::sin(TAU * 3 * x)); }); return c; }
    case 8: {   // Spectral Comb: a comb moving through 128 harmonics
        std::vector<double> a(129);
        for (int k = 1; k <= 128; k++) a[size_t(k)] = std::pow(double(k), -0.5) * (0.5 + 0.5 * std::cos(TAU * (k / 8.0 + t)));
        return from_harmonics(a);
    }
    default: { double r = 1 + 15 * t; fill([&](double x) { return (1 - x) * std::sin(TAU * x * r); }); return c; }
    }
}

std::mutex cache_mutex;
std::map<int, std::weak_ptr<const Wavetable>> cache;   // factory tables built and still in use (key: classic i / 100 + table i)

WavetablePtr cached(int key, const std::string &name, int frames, std::vector<float> (*gen)(int, double), int i) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (auto p = cache[key].lock()) return p;
    std::vector<float> cycles;
    cycles.reserve(size_t(frames) * WT_SIZE);
    for (int f = 0; f < frames; f++) {
        auto c = gen(i, frames > 1 ? double(f) / (frames - 1) : 0.0);
        cycles.insert(cycles.end(), c.begin(), c.end());
    }
    auto p = build_wavetable(name, cycles, frames);
    cache[key] = p;
    return p;
}

std::vector<float> classic_gen(int i, double) { return classic_cycle(i); }

}  // namespace

// ---- building ------------------------------------------------------------------------------------------------------
WavetablePtr build_wavetable(const std::string &name, const std::vector<float> &cycles, int frames) {
    frames = std::max(1, std::min(WT_MAX_FRAMES, frames));
    auto w = std::make_shared<Wavetable>();
    w->name = name;
    w->frames = frames;
    w->data.assign(size_t(frames) * WT_MIPS * (WT_SIZE + 1), 0.f);
    std::vector<std::complex<double>> spec(WT_SIZE), x(WT_SIZE);
    double peak = 0;
    for (int f = 0; f < frames; f++) {
        for (int s = 0; s < WT_SIZE; s++) {
            size_t i = size_t(f) * WT_SIZE + size_t(s);
            spec[size_t(s)] = i < cycles.size() ? cycles[i] : 0.0f;
        }
        fft(spec, false);
        spec[0] = 0;   // no DC
        for (int m = 0; m < WT_MIPS; m++) {
            const int keep = std::min(WT_SIZE / 2 - 1, (WT_SIZE / 2) >> m);   // the highest harmonic kept
            std::fill(x.begin(), x.end(), std::complex<double>(0));
            for (int k = 1; k <= keep; k++) { x[size_t(k)] = spec[size_t(k)]; x[size_t(WT_SIZE - k)] = spec[size_t(WT_SIZE - k)]; }
            fft(x, true);
            float *dst = w->data.data() + (size_t(f) * WT_MIPS + size_t(m)) * (WT_SIZE + 1);
            for (int s = 0; s < WT_SIZE; s++) {
                dst[s] = float(x[size_t(s)].real() / WT_SIZE);
                if (m == 0) peak = std::max(peak, double(std::fabs(dst[s])));
            }
            dst[WT_SIZE] = dst[0];
        }
    }
    if (peak > 1e-9) {
        const float g = float(1 / peak);
        for (float &v : w->data) v *= g;
    }
    return w;
}

WavetablePtr load_wavetable_file(const std::string &path) {
    BlobPtr blob = open_host_file(path);
    PcmPtr pcm = decode_audio(*blob);
    if (!pcm || pcm->frames() < 8) throw ParseError("no audio in " + path);
    // the cycle length: a Serum-style "clm " chunk ("<!>2048 ...")
    int cyc = 0;
    {
        std::vector<uint8_t> head = blob->head(1 << 16);
        for (size_t i = 12; i + 8 < head.size(); i++)
            if (!std::memcmp(&head[i], "clm ", 4)) {
                std::string s(head.begin() + long(i) + 8, head.begin() + long(std::min(head.size(), i + 40)));
                size_t at = s.find("<!>");
                if (at != std::string::npos) cyc = std::atoi(s.c_str() + at + 3);
                break;
            }
    }
    const int64_t n = pcm->frames();
    const int ch = pcm->channels;
    if (cyc < 16 || cyc > 65536) cyc = n % WT_SIZE == 0 ? WT_SIZE : (n <= 8192 ? int(n) : WT_SIZE);
    int frames = int(std::min<int64_t>(WT_MAX_FRAMES, std::max<int64_t>(1, n / cyc)));
    std::vector<float> cycles(size_t(frames) * WT_SIZE);
    for (int f = 0; f < frames; f++)
        for (int s = 0; s < WT_SIZE; s++) {   // each cycle resampled to WT_SIZE (linear), the first channel
            double pos = double(s) * cyc / WT_SIZE;
            int64_t i0 = int64_t(f) * cyc + int64_t(pos), i1 = std::min<int64_t>(n - 1, i0 + 1);
            double fr = pos - std::floor(pos);
            double a = pcm->data[size_t(std::min<int64_t>(n - 1, i0) * ch)] / 32768.0, b = pcm->data[size_t(i1 * ch)] / 32768.0;
            cycles[size_t(f) * WT_SIZE + size_t(s)] = float(a + (b - a) * fr);
        }
    std::string name = path.substr(path.find_last_of('/') + 1);
    return build_wavetable(name, cycles, frames);
}

bool natural_less(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && isdigit((unsigned char)a[i2])) i2++;
            while (j2 < b.size() && isdigit((unsigned char)b[j2])) j2++;
            unsigned long long x = std::strtoull(a.substr(i, i2 - i).c_str(), nullptr, 10), y = std::strtoull(b.substr(j, j2 - j).c_str(), nullptr, 10);
            if (x != y) return x < y;
            i = i2; j = j2;
            continue;
        }
        const int ca = std::tolower((unsigned char)a[i]), cb = std::tolower((unsigned char)b[j]);
        if (ca != cb) return ca < cb;
        i++; j++;
    }
    return a.size() - i < b.size() - j;
}

WavetablePtr load_wavetable_frames(const std::string &name, const std::vector<std::string> &files) {
    std::vector<float> cycles;
    int frames = 0;
    for (const std::string &path : files) {
        if (frames >= WT_MAX_FRAMES) break;
        try {
            PcmPtr pcm = decode_audio(*open_host_file(path));
            if (!pcm || pcm->frames() < 8) continue;
            const int64_t n = std::min<int64_t>(pcm->frames(), 65536);
            const int ch = pcm->channels;
            for (int s = 0; s < WT_SIZE; s++) {   // the whole file is one cycle, resampled to WT_SIZE (linear)
                double pos = double(s) * double(n) / WT_SIZE;
                int64_t i0 = int64_t(pos), i1 = (i0 + 1) % n;
                double fr = pos - std::floor(pos);
                double a = pcm->data[size_t(i0 * ch)] / 32768.0, b = pcm->data[size_t(i1 * ch)] / 32768.0;
                cycles.push_back(float(a + (b - a) * fr));
            }
            frames++;
        } catch (...) {}
    }
    if (!frames) throw ParseError("no readable WAVs");
    return build_wavetable(name, cycles, frames);
}

int factory_classic_count() { return N_CLASSIC; }
int factory_table_count() { return N_TABLES; }
const char *factory_classic_name(int i) { return i >= 0 && i < N_CLASSIC ? CLASSIC[i] : ""; }
const char *factory_table_name(int i) { return i >= 0 && i < N_TABLES ? TABLES[i] : ""; }
WavetablePtr factory_classic(int i) { i = std::max(0, std::min(N_CLASSIC - 1, i)); return cached(i, CLASSIC[i], 1, classic_gen, i); }
WavetablePtr factory_table(int i) { i = std::max(0, std::min(N_TABLES - 1, i)); return cached(100 + i, TABLES[i], TF, table_frame, i); }

// ---- voices --------------------------------------------------------------------------------------------------------
Synth::Voice *Synth::alloc() {
    const int limit = std::max(1, std::min(SYN_MAX_VOICES - 4, settings.polyphony.load()));   // spares for fade-outs
    int sounding = 0;
    Voice *oldest = nullptr, *free_v = nullptr;
    for (auto &v : voices_) {
        if (!v.on) { if (!free_v) free_v = &v; continue; }
        if (v.stealing) continue;
        sounding++;
        if (!oldest || (v.released && !oldest->released) || (v.released == oldest->released && v.age < oldest->age)) oldest = &v;
    }
    if (sounding >= limit && oldest) { oldest->stealing = true; }   // it fades over 5 ms (render)
    if (free_v) return free_v;
    return oldest;   // no spare: restart the oldest at once
}

void Synth::start(Voice &v, int note, int vel) {
    v = Voice();
    v.on = true;
    v.note = note;
    v.age = ++age_;
    const float vs = settings.vel_sens.load();
    v.vel = (1 - vs) + vs * float(vel) / 127.0f;
    Envelope e;
    e.set = true;
    e.attack = settings.attack.load();
    e.decay = settings.decay.load();
    e.sustain = settings.sustain.load();
    e.release = std::max(0.004f, settings.release.load());
    v.amp.begin(e, 1, 1, 0, 0, 1);
    for (int o = 0; o < SYN_OSCS; o++) {
        const SynthOscSettings &os = settings.osc[o];
        const int u_n = std::max(1, std::min(SYN_MAX_UNISON, os.unison.load()));
        const int pm = os.phase_mode.load();
        const double base = os.phase.load() / 360.0, rnd = os.rand.load();
        for (int u = 0; u < u_n; u++) {
            double p = base;
            if (pm == PM_RANDOM) p += rnd * rand01();
            else if (pm == PM_SPREAD) p += double(u) / u_n + rnd * rand01() / u_n;
            else if (pm == PM_FREE) p = free_ph_[o] + double(u) / u_n;
            v.ph[o][u] = p - std::floor(p);
            v.start_ph[o][u] = v.ph[o][u];
        }
    }
    {   // 2.3.1: the filter envelope; a noise seed of its own
        const SynthMixSettings &mx = settings.mix;
        Envelope fe;
        fe.set = true;
        fe.attack = mx.fe_attack.load();
        fe.decay = mx.fe_decay.load();
        fe.sustain = mx.fe_sustain.load();
        fe.release = std::max(0.004f, mx.fe_release.load());
        v.fenv.begin(fe, 1, 1, 0, 0, 1);
        v.noise = 0x9e3779b9u * uint32_t(v.age) + 1;
        v.sub_ph = 0;
    }
    {   // 2.3.2: the mod envelope; the LFOs start at their beginning (retrig, one-shot)
        const SynthModSettings &md = settings.mod;
        Envelope me;
        me.set = true;
        me.attack = md.me_attack.load();
        me.decay = md.me_decay.load();
        me.sustain = md.me_sustain.load();
        me.release = std::max(0.004f, md.me_release.load());
        v.menv.begin(me, 1, 1, 0, 0, 1);
        for (int k = 0; k < SYN_XENVS; k++) {   // 2.4.17: MOD ENV 2 / 3
            const SynthModEnvSettings &xs = md.mex[k];
            Envelope xe;
            xe.set = true;
            xe.attack = xs.attack.load();
            xe.decay = xs.decay.load();
            xe.sustain = xs.sustain.load();
            xe.release = std::max(0.004f, xs.release.load());
            v.xenv[k].begin(xe, 1, 1, 0, 0, 1);
        }
        for (int k = 0; k < SYN_LFOS; k++) {
            v.lfo_ph[k] = 0;
            v.lfo_rnd[k][0] = rand01() * 2 - 1;
            v.lfo_rnd[k][1] = rand01() * 2 - 1;
        }
    }
    v.drift = (rand01() * 2 - 1) * settings.voice.drift.load() * 0.25f;   // 2.3.3: up to +-25 ct
    // 2.4.5: analog wander starts somewhere different for every voice; the cutoff tolerance is this voice's own
    for (int k = 0; k < 2; k++) { v.wander[k] = rand01() * 2 - 1; v.wander_to[k] = rand01() * 2 - 1; }
    v.wander_n = 0;
    v.cut_tol = rand01() * 2 - 1;
    v.rnd = rand01() * 2 - 1;
    start_samples(v, vel);   // 2.3.4
}

// 2.3.4: what each SAMPLE oscillator plays for this note: a slot's zone (its key and velocity, its attack layer) or the
// oscillator's file; POSITION moves the start into the sample
void Synth::start_samples(Voice &v, int vel) {
    for (int o = 0; o < SYN_OSCS; o++) {
        Voice::SamplePlay &sp = v.smp[o];
        sp = Voice::SamplePlay();
        const SynthOscSettings &os = settings.osc[o];
        if (!os.on.load() || os.type.load() != OT_SAMPLE) continue;
        const int slot = os.sample_slot.load();
        if (slot >= 0) {
            if (!prog_) continue;
            for (int zi : prog_->by_key[size_t(v.note & 127)]) {
                const Zone &z = prog_->inst.zones[size_t(zi)];
                if (z.slot != slot || z.trigger != Trigger::Attack || vel < z.vel_lo || vel > z.vel_hi) continue;
                const Pcm *pcm = prog_->pcm[size_t(zi)].get();
                if (!pcm || pcm->frames() < 2) continue;
                sp.pcm = pcm;
                sp.from_program = true;
                sp.root = z.root - z.tune;
                sp.start = std::max<int64_t>(0, std::min<int64_t>(z.start, pcm->frames() - 1));
                sp.end = z.stop > 0 ? std::min<int64_t>(z.stop, pcm->frames()) : pcm->frames();
                if (!z.loops.empty() && !z.one_shot) {
                    sp.loop_start = std::max<int64_t>(sp.start, z.loops[0].start);
                    sp.loop_end = std::min<int64_t>(sp.end, z.loops[0].end + 1);
                    sp.loop_type = z.loops[0].type;               // 2.4.30 (audit F9)
                    sp.until_release = z.loops[0].until_release;
                }
                break;
            }
        } else if (const SynthSample *f = os.sample.load(std::memory_order_acquire)) {
            if (!f->zones.empty()) {   // 2.4.33: a multisample: the zone the note and velocity pick
                const SynthSample::Zone *z = nullptr;
                for (const SynthSample::Zone &c : f->zones)
                    if (v.note >= c.lokey && v.note <= c.hikey && vel >= c.lovel && vel <= c.hivel && c.pcm && c.pcm->frames() >= 2) { z = &c; break; }
                if (!z) continue;
                sp.pcm = z->pcm.get();
                sp.file = f;
                sp.root = z->root;
                sp.start = std::max<int64_t>(0, std::min<int64_t>(z->start, z->pcm->frames() - 1));
                sp.end = z->end > 0 ? std::min<int64_t>(z->end, z->pcm->frames()) : z->pcm->frames();
                sp.loop_start = z->loop_start;
                sp.loop_end = z->loop_end;
                sp.loop_type = z->loop_type;
            } else {
                if (!f->pcm || f->pcm->frames() < 2) continue;
                sp.pcm = f->pcm.get();
                sp.file = f;
                sp.root = f->root - f->tune;
                sp.start = 0;
                sp.end = f->pcm->frames();
                sp.loop_start = f->loop_start;
                sp.loop_end = f->loop_end;
                sp.loop_type = f->loop_type;
            }
        }
        if (!sp.pcm) continue;
        if (sp.end <= sp.start + 1) { sp.pcm = nullptr; continue; }
        // a file's loop comes from its own chunk: kept inside the audio (a loop end past it read beyond the data)
        sp.loop_end = std::min(sp.loop_end, sp.end);
        sp.loop_start = std::max(sp.loop_start, sp.start);
        if (sp.loop_end - sp.loop_start < 2) sp.loop_start = sp.loop_end = 0;
        sp.complex_loop = sp.loop_end > 0 && (sp.loop_type != LoopType::Forward || sp.until_release);
        const double first = double(sp.start) + std::max(0.0f, std::min(0.95f, os.pos.load())) * double(sp.end - sp.start);
        for (int u = 0; u < SYN_MAX_UNISON; u++) { sp.pos[u] = first; sp.done[u] = false; }
    }
}

void Synth::fade_program() {
    for (auto &v : voices_)
        if (v.on && (v.smp[0].from_program || v.smp[1].from_program)) v.stealing = true;
}

int Synth::program_voices() const {
    int n = 0;
    for (auto &v : voices_) if (v.on && (v.smp[0].from_program || v.smp[1].from_program)) n++;
    return n;
}

// 2.3.3: glide from another note to this voice's own (over the glide time)
void Synth::glide_to(Voice &v, int from_note) {
    const float t = settings.voice.glide.load();
    if (t <= 0.0005f || from_note < 0) { v.glide = 0; v.glide_step = 0; return; }
    v.glide = float(from_note - v.note) + v.glide;
    v.glide_step = std::fabs(v.glide) / (t * SR);
}

void Synth::note_on(int key, int vel) {
    if (!settings.on.load()) return;
    const SynthVoiceSettings &vs = settings.voice;
    if (key < vs.key_lo.load() || key > vs.key_hi.load()) return;   // 2.3.3: outside the synth's key range
    const int note = std::max(0, std::min(127, key + vs.transpose.load()));
    const int mode = vs.mode.load();
    const bool legato_only = vs.glide_legato.load() != 0;
    const bool others_held = n_held_ > 0;
    hold(key);
    if (mode != 0) {   // MONO / LEGATO: one voice; the newest key plays
        Voice *cur = nullptr;
        for (auto &v : voices_) if (v.on && !v.stealing && !v.released && (!cur || v.age > cur->age)) cur = &v;
        if (cur) {
            const int from = cur->note;
            if (mode == 2) {   // LEGATO: the envelopes go on; only the pitch moves
                cur->note = note; cur->key = key;
                cur->glide += float(from - note);   // jump there now, then glide (or not) from the old pitch
                if (settings.voice.glide.load() > 0.0005f) cur->glide_step = std::fabs(cur->glide) / (settings.voice.glide.load() * SR);
                else { cur->glide = 0; cur->glide_step = 0; }
                cur->age = ++age_;
            } else {           // MONO: it starts again (the glide from the old note when on)
                const float carried = cur->glide;
                start(*cur, note, vel);
                cur->key = key;
                cur->glide = carried;
                glide_to(*cur, from);
            }
            last_note_ = note;
            return;
        }
    }
    Voice *v = alloc();
    if (!v) return;
    start(*v, note, vel);
    v->key = key;
    if (!legato_only || others_held) glide_to(*v, last_note_);
    last_note_ = note;
}

void Synth::note_off(int key) {
    const int mode = settings.voice.mode.load();
    drop(key);
    for (auto &v : voices_) {
        if (!v.on || v.released || v.key != key) continue;
        if (mode != 0 && n_held_ > 0) {   // MONO / LEGATO: back to the last key still held (no new attack)
            const int k2 = held_[n_held_ - 1];
            const int note = std::max(0, std::min(127, k2 + settings.voice.transpose.load()));
            v.glide += float(v.note - note);
            v.note = note; v.key = k2;
            if (settings.voice.glide.load() > 0.0005f) v.glide_step = std::fabs(v.glide) / (settings.voice.glide.load() * SR);
            else { v.glide = 0; v.glide_step = 0; }
            last_note_ = note;
            continue;
        }
        if (pedal_) { v.held_by_pedal = true; continue; }
        v.released = true;
        v.amp.release();
        v.fenv.release();
        v.menv.release(); for (Env &x : v.xenv) x.release();
    }
}

void Synth::sustain(bool down) {
    pedal_ = down;
    if (!down)
        for (auto &v : voices_)
            if (v.on && v.held_by_pedal) { v.held_by_pedal = false; v.released = true; v.amp.release(); v.fenv.release(); v.menv.release(); for (Env &x : v.xenv) x.release(); }
}

void Synth::all_off(bool hard) {
    n_held_ = 0;   // 2.3.3
    pedal_ = false;   // as the sampler's sustain_ (PANIC left the synth's pedal down: later notes never released)
    for (auto &v : voices_) {
        if (!v.on) continue;
        v.held_by_pedal = false;
        if (hard) v.stealing = true;
        else { v.released = true; v.amp.release(); v.fenv.release(); v.menv.release(); for (Env &x : v.xenv) x.release(); }
    }
}

namespace {
inline double warp_phase(int mode, double ph, float a) {
    switch (mode) {
    case Synth::WM_BEND: { double k = 0.99 * a; return ph + k * ph * (1 - ph); }   // pulls the cycle's middle later; monotonic for k < 1
    case Synth::WM_PWM: { double w = 0.5 - 0.48 * a; return ph < w ? ph * 0.5 / w : 0.5 + (ph - w) * 0.5 / (1 - w); }
    case Synth::WM_MIRROR: { double m = ph < 0.5 ? 2 * ph : 2 - 2 * ph; return ph + a * (m * 0.5 - ph); }
    case Synth::WM_SYNC: { double p = ph * (1 + 7 * a); return p - std::floor(p); }
    default: return ph;
    }
}
inline float read(const float *c, double ph) {
    const double x = ph * WT_SIZE;
    int i = int(x);
    if (i >= WT_SIZE) i = WT_SIZE - 1;
    if (i < 0) i = 0;
    const float f = float(x - i);
    return c[i] + (c[i + 1] - c[i]) * f;
}
}  // namespace

double synth_warp_phase(int mode, double ph, float amount) { return warp_phase(mode, ph, amount); }

// ---- 2.3.5: the MACRO engines --------------------------------------------------------------------------------------
namespace {
const char *const MACRO_NAMES[] = {"Virtual Analog", "Waveshaper", "FM 2-Op", "Formant", "Additive", "String", "Modal", "Chord", "Noise"};
constexpr int N_MACRO = int(sizeof MACRO_NAMES / sizeof MACRO_NAMES[0]);
// polyBLEP: the step's correction near a discontinuity (t: phase 0..1, dt: the increment)
inline float blep(double t, double dt) {
    if (t < dt) { t /= dt; return float(t + t - t * t - 1); }
    if (t > 1 - dt) { t = (t - 1) / dt; return float(t * t + t + t + 1); }
    return 0;
}
inline float saw_bl(double ph, double dt) { return float(2 * ph - 1) - blep(ph, dt); }
inline float sq_bl(double ph, double dt, double pw) {
    double ph2 = ph + 1 - pw; ph2 -= std::floor(ph2);
    return (ph < pw ? 1.0f : -1.0f) + blep(ph, dt) - blep(ph2, dt);
}
inline void adv(double &ph, double inc) { ph += inc; if (ph >= 1) ph -= std::floor(ph); }
}  // namespace

int macro_engine_count() { return N_MACRO; }
const char *macro_engine_name(int i) { return i >= 0 && i < N_MACRO ? MACRO_NAMES[i] : ""; }

// one sample of a MACRO oscillator: h harmonics, t timbre, m morph (0..1); about -1..1
float Synth::macro_tick(Voice &v, int o, int engine, double inc, float h, float t, float m) {
    Voice::MacroPlay &p = v.mac[o];
    if (p.engine != engine) {   // the engine changed under a held note: String with Modal's state divided by ks_len 0
        if (p.engine >= 0) p = Voice::MacroPlay();
        p.engine = engine;
    }
    const double dt = std::max(1e-6, inc);
    const bool fresh = inc != p.ck_inc || h != p.ck_h || t != p.ck_t || m != p.ck_m;   // 2.4.5
    if (fresh) { p.ck_inc = inc; p.ck_h = h; p.ck_t = t; p.ck_m = m; }
    float *cf = p.cf;
    auto noise = [&v]() { v.noise = v.noise * 1664525u + 1013904223u; return float(int32_t(v.noise)) * (1.0f / 2147483648.0f); };
    switch (engine) {
    case 0: {   // VIRTUAL ANALOG: two oscillators an interval apart (h: 0..12 st); t: saw -> square -> thin pulse; m: sync
        if (fresh) cf[0] = float(std::exp2(double(h)) * (1 + 3 * m));
        const double dt2 = std::min(0.45, dt * cf[0]);
        auto shape = [t](double ph, double d) {
            if (t < 0.5f) { const float k = t * 2; return saw_bl(ph, d) * (1 - k) + sq_bl(ph, d, 0.5) * k; }
            return sq_bl(ph, d, 0.5 - (t - 0.5) * 0.8);
        };
        const float y = 0.5f * (shape(p.ph[0], dt) + shape(p.ph[1], dt2));
        const double before = p.ph[0];
        adv(p.ph[0], dt); adv(p.ph[1], dt2);
        if (m > 0.01f && p.ph[0] < before) p.ph[1] = 0;   // sync: the second restarts with the first
        return y;
    }
    case 1: {   // WAVESHAPER: a triangle folded (t: amount), h: asymmetry, m: the plain sine mixed in
        const float tri = float(p.ph[0] < 0.5 ? 4 * p.ph[0] - 1 : 3 - 4 * p.ph[0]);
        const float x = tri + h * 0.6f;
        const float y = std::sin(1.5707963f * (1 + t * 7) * x) * (1 - m) + std::sin(6.2831853f * float(p.ph[0])) * m;
        adv(p.ph[0], dt);
        return y * 0.9f;
    }
    case 2: {   // FM 2-OP: h picks the ratio, t the index, m the modulator's feedback
        static const float R[] = {0.5f, 1, 1.5f, 2, 2.5f, 3, 4, 5, 6, 7};
        const float ratio = R[std::min(9, int(h * 9.99f))];
        const float mod = std::sin(6.2831853f * float(p.ph[1]) + m * 1.5f * p.y[0]);
        p.y[0] = mod;
        const float y = std::sin(6.2831853f * float(p.ph[0]) + t * 10.0f * mod);
        adv(p.ph[0], dt); adv(p.ph[1], std::min(0.45, dt * ratio));
        return y;
    }
    case 3: {   // FORMANT: VOSIM-like: two formants restarted every cycle (h: vowel a e i o u, t: formant shift, m: width)
        static const float F[5][2] = {{800, 1150}, {400, 1600}, {250, 1750}, {450, 800}, {325, 700}};
        if (fresh) {
            const float vm = h * 4; const int a = std::min(3, int(vm)); const float f = vm - float(a);
            const float sh = std::exp2((t - 0.5f) * 2);
            cf[0] = std::min(0.45f, (F[a][0] * (1 - f) + F[a + 1][0] * f) * sh / SR);
            cf[1] = std::min(0.45f, (F[a][1] * (1 - f) + F[a + 1][1] * f) * sh / SR);
        }
        const double before = p.ph[0];
        adv(p.ph[0], dt);
        if (p.ph[0] < before) { p.ph[1] = p.ph[2] = 0; p.y[0] = 1; }
        const float y = p.y[0] * (std::sin(6.2831853f * float(p.ph[1])) * 0.7f + std::sin(6.2831853f * float(p.ph[2])) * 0.45f);
        p.y[0] *= 1 - (0.0004f + (1 - m) * 0.004f);
        adv(p.ph[1], double(cf[0])); adv(p.ph[2], double(cf[1]));
        return y;
    }
    case 4: {   // ADDITIVE: 8 harmonics (h: tilt, t: even ones down, m: a peak moving up the series)
        if (fresh) {   // cf[0..7]: the harmonics' gains (0 above Nyquist), cf[8]: 1.3 / their sum
            const float tilt = 0.25f + 0.73f * h, peak = 1 + 7 * m;
            float a = 1, norm = 0;
            for (int n = 1; n <= 8; n++, a *= tilt) {
                float g = 0;
                if (dt * n < 0.45) {
                    g = a * ((n % 2 == 0) ? 1 - t : 1.0f) + 0.8f * std::exp(-(float(n) - peak) * (float(n) - peak));
                }
                cf[n - 1] = g; norm += g;
            }
            cf[8] = norm > 0 ? 1.3f / norm : 0;
        }
        // the harmonics by the angle-addition recurrence: one sin and cos, then sin(nx) = 2cos(x)sin((n-1)x) - sin((n-2)x)
        const float x = 6.2831853f * float(p.ph[0]);
        const float s1 = std::sin(x), c2 = 2 * std::cos(x);
        float sp = 0, sc = s1, y = cf[0] * s1;
        for (int n = 1; n < 8; n++) { const float sn = c2 * sc - sp; sp = sc; sc = sn; y += cf[n] * sn; }
        adv(p.ph[0], dt);
        return y * cf[8];
    }
    case 5: {   // STRING: Karplus-Strong (t: brightness, h: pluck position, m: decay)
        if (!p.excited) {
            p.excited = true;
            p.ks_len = std::max(4, std::min(Voice::MacroPlay::KS - 2, int(1.0 / dt)));
            float lp = 0;
            for (int i = 0; i < p.ks_len; i++) { lp += (0.15f + 0.85f * t) * (noise() - lp); p.ks[i] = lp; }
            const int off = std::max(1, int(p.ks_len * (0.05f + 0.45f * h)));
            for (int i = p.ks_len - 1; i >= off; i--) p.ks[i] -= p.ks[i - off];   // the pluck position's comb
            p.ks_w = 0;
        }
        const int r = p.ks_w, r1 = (r + 1) % p.ks_len;
        const float y = p.ks[r];
        const float damp = 0.4f + 0.55f * t, decay = 0.985f + 0.0148f * m;
        p.ks[r] = decay * (y * damp + p.ks[r1] * (1 - damp));
        p.ks_w = r1;
        return y * 1.6f;
    }
    case 6: {   // MODAL: four resonators struck at the note (h: material, t: brightness, m: decay)
        static const float RS[4][4] = {{1, 3.99f, 10.65f, 18.0f}, {1, 2.76f, 5.40f, 8.93f}, {1, 1.47f, 2.09f, 2.56f}, {1, 2.32f, 4.25f, 6.63f}};
        if (inc != p.last_inc || h != p.last_h || m != p.last_m) {
            p.last_inc = inc; p.last_h = h; p.last_m = m;
            const int a = std::min(3, int(h * 3.99f));
            for (int k = 0; k < 4; k++) {
                const float fr = float(inc) * RS[a][k];
                const float rr = fr < 0.45f ? std::exp(-1.0f / ((0.05f + 2.5f * m) * SR / (1 + k * 1.5f))) : 0;
                p.c1[k] = 2 * rr * std::cos(6.2831853f * std::min(0.45f, fr));
                p.c2[k] = -rr * rr;
                // a struck resonator rings at 1 / sin(w): scaled out so the level holds across notes and materials
                p.g[k] = fr < 0.45f ? 1.0f / float(k + 1) * (k == 0 ? 1 : 0.4f + 0.6f * t) * std::sin(6.2831853f * fr) : 0;
            }
        }
        float x = 0;
        if (!p.excited) { p.excited = true; x = 1; }
        if (t > 0.6f && p.strike < 64) { x += noise() * (t - 0.6f) * 0.08f; p.strike++; }   // a bright mallet's noisy strike
        float y = 0;
        for (int k = 0; k < 4; k++) {
            const float z = p.c1[k] * p.y[2 * k] + p.c2[k] * p.y[2 * k + 1] + x;
            p.y[2 * k + 1] = p.y[2 * k]; p.y[2 * k] = z;
            y += z * p.g[k];
        }
        return y * 0.6f;   // (levels matched across the engines, 2026-11-12)
    }
    case 7: {   // CHORD: four oscillators (h: the chord, t: saw -> square, m: detune between them)
        static const int C[7][4] = {{0, 12, 19, 24}, {0, 7, 12, 19}, {0, 4, 7, 12}, {0, 3, 7, 12}, {0, 4, 7, 10}, {0, 5, 7, 12}, {0, 4, 7, 11}};
        const int c = std::min(6, int(h * 6.99f));
        float y = 0;
        if (fresh) for (int k = 0; k < 4; k++) cf[k] = float(std::exp2((C[c][k] + (k - 1.5) * m * 0.15) / 12.0));
        for (int k = 0; k < 4; k++) {
            const double d = std::min(0.45, dt * cf[k]);
            y += saw_bl(p.ph[k], d) * (1 - t) + sq_bl(p.ph[k], d, 0.5) * t;
            adv(p.ph[k], d);
        }
        return y * 0.75f;
    }
    default: {   // NOISE: white through a resonant filter at the note (t: its pitch +-2 oct, h: resonance, m: low-pass -> band-pass)
        // 2.4.32 (user bug: a pop, then the SYNTH silent until RESET): this Chamberlin filter diverges when
        // f * (f + 2q) >= 4 (a high pitch with low resonance: inf, then NaN in the whole mix). f is held at 85 % of that
        // bound with a margin when the controls change; settings that were stable keep their sound
        if (fresh) {
            const float fc = std::min(0.24f, float(dt) * std::exp2((t - 0.5f) * 4));
            const float q = 1.9f - 1.85f * h;
            cf[0] = std::min(2 * std::sin(3.1415927f * fc), std::sqrt(q * q + 3.4f) - q);
            cf[1] = q;
        }
        const float f = cf[0], q = cf[1];
        const float in = noise();
        p.y[1] += f * p.y[0];
        const float hp = in - p.y[1] - q * p.y[0];
        p.y[0] += f * hp;
        return (p.y[1] * (1 - m) + p.y[0] * m) * (0.6f + h) * 6.0f;
    }
    }
}

// 2.3.2: the LFOs' tempo divisions (beats per cycle): the sampler's LFO list (design.py LFO_SYNC), 0 = free
static const double SYNC_BEATS[] = {0, 16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.0 / 6, 1.5, 0.75, 0.375};
constexpr int SYNC_COUNT = int(sizeof SYNC_BEATS / sizeof SYNC_BEATS[0]);

double Synth::lfo_hz(const SynthLfoSettings &s) const {
    const int sy = s.sync.load();
    if (sy > 0 && sy < SYNC_COUNT) return tempo_ / 60.0 / SYNC_BEATS[sy];
    return std::max(0.01f, s.rate_hz.load());
}

// -1..1 at phase ph (0..1); rnd: the sample & hold value now, the next (random glides between them); tab: CUSTOM's
float Synth::lfo_value(int shape, double ph, const float *rnd, const float *tab) const {
    switch (shape) {
    case SYN_LFO_CUSTOM:   // 2.4.13
        if (tab) {
            const double x = std::min(std::max(ph, 0.0), 1.0) * SYN_LFO_TAB;
            const int i = std::min(int(x), SYN_LFO_TAB - 1);
            return tab[i] + (tab[i + 1] - tab[i]) * float(x - i);
        }
        return float(std::sin(TAU * ph));
    case 1: return float(ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph);
    case 2: return float(2 * ph - 1);
    case 3: return ph < 0.5 ? 1.0f : -1.0f;
    case 4: return rnd[0];
    case 5: { float t = float(ph); t = t * t * (3 - 2 * t); return rnd[0] + (rnd[1] - rnd[0]) * t; }
    default: return float(std::sin(TAU * ph));
    }
}

void Synth::render(float *l, float *r, int frames) {
    // 2.4.2: at most MAX_BLOCK frames per pass (the free-running LFOs' values are worked out per 16-sample chunk ahead)
    while (frames > MAX_BLOCK) { render(l, r, MAX_BLOCK); l += MAX_BLOCK; r += MAX_BLOCK; frames -= MAX_BLOCK; }
    for (int o = 0; o < SYN_OSCS; o++) { free_ph_[o] += 0.0137 * frames; free_ph_[o] -= std::floor(free_ph_[o]); }
    int active = 0;
    const float level = settings.level.load();
    const float bend_semi = bend_ * settings.bend_range.load();
    const SynthMixSettings &mx = settings.mix;
    const SynthModSettings &md = settings.mod;
    // ---- per block: what does not change between voices ----
    struct OscBlock {
        const Wavetable *t = nullptr;
        const SynthSample *file = nullptr;   // a SAMPLE oscillator's file now (voices still on an older one: silent)
        bool on = false, sample = false;   // 2.3.4: sample: a SAMPLE oscillator (each voice its own audio)
        bool macro = false;                // 2.3.5: a MACRO oscillator (one engine per voice, no unison)
        int engine = 0;
        int u_n = 1, wmode = 0;
        float pos = 0, warp = 0, pan = 0, spread = 0, gain = 1, det = 0;
        float lvl = 0;                     // 2.4.35: the oscillator's level (LEVEL A / B add to it)
        double pitch = 0;
        float pl[SYN_MAX_UNISON], pr[SYN_MAX_UNISON];   // 2.4.6: each unison voice's pan gains (x gain x sqrt 2), once a block
    } ob[SYN_OSCS];
    for (int o = 0; o < SYN_OSCS; o++) {
        const SynthOscSettings &os = settings.osc[o];
        OscBlock &b = ob[o];
        const int type = os.type.load();
        b.t = os.table.load(std::memory_order_acquire);
        b.sample = type == OT_SAMPLE;
        if (b.sample) b.file = os.sample.load(std::memory_order_acquire);
        b.macro = type == OT_MACRO;   // 2.3.5
        b.engine = os.macro_engine.load();
        b.on = os.on.load() && (b.sample || b.macro || (b.t && (type == OT_CLASSIC || type == OT_WAVETABLE)));
        if (!b.on) continue;
        b.u_n = type == OT_MACRO ? 1 : std::max(1, std::min(SYN_MAX_UNISON, os.unison.load()));
        b.wmode = os.warp_mode.load();
        b.warp = os.warp.load();
        b.pos = os.pos.load();
        b.pitch = os.octave.load() * 12 + os.semi.load() + os.fine.load() / 100.0 + bend_semi;
        b.det = os.detune.load() * 100.0f;
        b.spread = os.spread.load();
        b.pan = os.pan.load();
        b.lvl = os.level.load();
        b.gain = b.lvl / std::sqrt(float(b.u_n));
        for (int u = 0; u < b.u_n; u++) {   // 2.4.6: was cos + sin per unison voice per 16-sample chunk per note
            const float upos = b.u_n == 1 ? 0.0f : float(u) / float(b.u_n - 1) * 2 - 1;
            const float ang = (std::max(-1.0f, std::min(1.0f, b.pan + upos * b.spread)) + 1) * float(M_PI) / 4;
            b.pl[u] = std::cos(ang) * b.gain * 1.41421356f;
            b.pr[u] = std::sin(ang) * b.gain * 1.41421356f;
        }
    }
    const float bal0 = mx.balance.load();
    const float fm0 = mx.fm.load(), fbk = mx.feedback.load() * 1.5f, ring = mx.ring.load();
    const bool sync = mx.sync.load() != 0 && ob[0].on && ob[1].on;
    const float ring_lv = ob[0].on && ob[1].on ? std::sqrt(std::max(0.0f, ob[0].gain * std::sqrt(float(ob[0].u_n)) * ob[1].gain * std::sqrt(float(ob[1].u_n)))) : 0.0f;
    const Wavetable *subt = mx.sub_on.load() ? mx.sub_table.load(std::memory_order_acquire) : nullptr;
    const int sub_oct = std::max(1, std::min(2, mx.sub_oct.load()));
    float sub_gl = 0, sub_gr = 0, n_gl = 0, n_gr = 0;
    auto eqpan = [](float p, float g, float &gl, float &gr) {
        const float ang = (std::max(-1.0f, std::min(1.0f, p)) + 1) * float(M_PI) / 4;
        gl = std::cos(ang) * g * 1.41421356f; gr = std::sin(ang) * g * 1.41421356f;
    };
    const float sub_pan0 = mx.sub_pan.load(), sub_lv0 = mx.sub_level.load();   // 2.4.9: (SUB PAN modulated per chunk)
    eqpan(sub_pan0, sub_lv0, sub_gl, sub_gr);
    const bool noise_on = mx.noise_on.load() != 0;
    const int ncol = mx.noise_color.load();
    const bool nkey = mx.noise_key.load() != 0;
    eqpan(mx.noise_pan.load(), mx.noise_level.load(), n_gl, n_gr);
    const float ntone0 = mx.noise_tone.load();
    const float ntone_k = 1 - std::exp(-2 * float(M_PI) * (200.0f * std::pow(100.0f, ntone0)) / SR);
    const bool flt = mx.flt_on.load() != 0;
    const int ftype = std::max(0, std::min(6, mx.flt_type.load()));
    const int route = flt ? mx.route.load() : 0;
    const float res0 = mx.res.load(), drive = mx.drive.load(), fmix = mx.mix.load();
    const float fat = mx.fat.load(), fat_k = 1 - std::exp(-2 * float(M_PI) * 150.0f / SR);
    const float pan0 = mx.pan.load();
    // the modulation sources' settings; the free-running LFOs' values for each chunk of this block (2.4.2: they were
    // one value per block, a staircase at fast rates; and the MATRIX's LFO RATE reaches them too: the newest voice's)
    constexpr int CHUNK = 16;   // modulation, increments and filter coefficients every 16 samples
    // 2.3.3: the MATRIX's routes (2.4.17: first, so an LFO nothing reads is not run)
    int mx_src[SYN_ROUTES], mx_dst[SYN_ROUTES], n_routes = 0;
    float mx_amt[SYN_ROUTES];
    for (int k = 0; k < SYN_ROUTES; k++) {
        const int sr = md.mx_src[k].load(), ds = md.mx_dst[k].load();
        const float am = md.mx_amt[k].load();
        if (sr <= SS_OFF || sr >= SS_COUNT || ds <= SD_OFF || ds >= SD_COUNT || am == 0) continue;
        mx_src[n_routes] = sr; mx_dst[n_routes] = ds; mx_amt[n_routes] = am; n_routes++;
    }
    static constexpr int LFO_SRC[SYN_LFOS] = {SS_LFO1, SS_LFO2, SS_LFO3, SS_LFO4};
    static constexpr int LFO_RATE_DST[SYN_LFOS] = {SD_LFO1_RATE, SD_LFO2_RATE, SD_LFO3_RATE, SD_LFO4_RATE};
    int lfo_shape[SYN_LFOS], lfo_trig[SYN_LFOS], lfo_dest[SYN_LFOS];
    float lfo_depth[SYN_LFOS], lfo_inc[SYN_LFOS];
    bool lfo_used[SYN_LFOS];   // 2.4.17: its own destination, or a route reads it (else it costs nothing)
    const float *lfo_tab[SYN_LFOS];   // 2.4.13: CUSTOM's table (read once a block)
    float glfo_val[SYN_LFOS][MAX_BLOCK / CHUNK + 1];
    for (int k = 0; k < SYN_LFOS; k++) {
        const SynthLfoSettings &ls = md.lfo[k];
        lfo_shape[k] = ls.shape.load(); lfo_trig[k] = ls.trig.load(); lfo_dest[k] = ls.dest.load();
        lfo_tab[k] = ls.custom.load(std::memory_order_acquire);
        lfo_depth[k] = ls.depth.load();
        lfo_inc[k] = float(lfo_hz(ls) / SR);
        lfo_used[k] = lfo_dest[k] > 0 && lfo_dest[k] < SD_COUNT;
        for (int r = 0; r < n_routes; r++) lfo_used[k] = lfo_used[k] || mx_src[r] == LFO_SRC[k];
        if (!lfo_used[k] || lfo_trig[k] != 0) continue;   // (retriggered LFOs run per voice)
        for (int c = 0, c0 = 0; c0 < frames; c++, c0 += CHUNK) {
            glfo_val[k][c] = lfo_value(lfo_shape[k], glfo_ph_[k], glfo_rnd_[k], lfo_tab[k]);
            glfo_ph_[k] += double(lfo_inc[k]) * glfo_rate_mod_[k] * std::min(CHUNK, frames - c0);
            while (glfo_ph_[k] >= 1) { glfo_ph_[k] -= 1; glfo_rnd_[k][0] = glfo_rnd_[k][1]; glfo_rnd_[k][1] = rand01() * 2 - 1; }
        }
    }
    const bool me_on = md.me_on.load() != 0;
    const int me_dest = md.me_dest.load();
    const float me_amt = md.me_amt.load();
    // 2.4.17: MOD ENV 2 / 3
    bool xe_on[SYN_XENVS];
    int xe_dest[SYN_XENVS];
    float xe_amt[SYN_XENVS];
    for (int k = 0; k < SYN_XENVS; k++) {
        xe_on[k] = md.mex[k].on.load() != 0;
        xe_dest[k] = md.mex[k].dest.load();
        xe_amt[k] = md.mex[k].amt.load();
        if (!(xe_dest[k] > 0 && xe_dest[k] < SD_COUNT)) xe_on[k] = false;   // (its own route: needs a destination)
    }
    float macro_dst[SD_COUNT] = {};
    for (int k = 0; k < 4; k++) {
        const int d = md.macro_dest[k].load();
        if (d > 0 && d < SD_COUNT) macro_dst[d] += md.macro[k].load();
    }
    float mac[4];
    for (int k = 0; k < 4; k++) mac[k] = md.macro[k].load();
    const Voice *newest = nullptr;   // 2.4.2: whose MATRIX LFO RATE the free-running LFOs follow
    for (const auto &v : voices_) if (v.on && !v.stealing && (!newest || v.age > newest->age)) newest = &v;

    for (auto &v : voices_) {
        if (!v.on) continue;
        active++;
        // a file swapped out is freed 2 s later (plugin.cpp syn_retired_s): a voice held longer must not read it
        for (int o = 0; o < SYN_OSCS; o++) {
            Voice::SamplePlay &sp = v.smp[o];
            if (sp.pcm && !sp.from_program && sp.file != ob[o].file) sp.pcm = nullptr;
        }
        double sub_inc = 0; int sub_mip = 0;
        float nbp_f = 0;
        const float base_cut = mx.cutoff.load() * std::exp2(mx.keytrack.load() * float(v.note - 60) / 12.0f)
                               * std::exp2(mx.vel.load() * 4.0f * (v.vel - 1.0f));
        const float env_oct = mx.env_amt.load() * 6.0f;
        // 2.4.11 (audit M6): an envelope nobody reads runs a chunk at a time (Env::tick_n), not per sample
        bool fenv_need = flt && env_oct != 0, menv_need = me_on && me_dest > 0 && me_dest < SD_COUNT;
        for (int k = 0; k < n_routes; k++) { fenv_need = fenv_need || mx_src[k] == SS_FLT_ENV; menv_need = menv_need || mx_src[k] == SS_MOD_ENV; }
        const float drift_amt = settings.voice.drift.load();
        bool alive = true;
        for (int c0 = 0; c0 < frames && alive; c0 += CHUNK) {
            const int n = std::min(CHUNK, frames - c0);
            // 2.4.5: analog wander: each voice's pitch (up to +-6 ct) and cutoff (+-0.12 oct, plus its own +-0.2 oct
            // tolerance) drift slowly to a new random target every 0.15..0.65 s, smoothed over about 0.4 s
            float wander_st = 0, wander_oct = 0;
            if (drift_amt > 0) {
                if (--v.wander_n <= 0) {
                    v.wander_n = int(SR * (0.15f + 0.5f * rand01()) / CHUNK);
                    for (int k = 0; k < 2; k++) v.wander_to[k] = rand01() * 2 - 1;
                }
                const float wk = float(n) / (0.4f * SR);
                for (int k = 0; k < 2; k++) v.wander[k] += (v.wander_to[k] - v.wander[k]) * wk;
                wander_st = drift_amt * 0.06f * v.wander[0];
                wander_oct = drift_amt * (0.12f * v.wander[1] + 0.2f * v.cut_tol);
            }
            // ---- modulation for this chunk ----
            float dst[SD_COUNT];
            std::copy(macro_dst, macro_dst + SD_COUNT, dst);
            float lfo_val[SYN_LFOS] = {};
            for (int k = 0; k < SYN_LFOS; k++) {   // 2.4.17: the LFOs something reads (was: both always)
                if (!lfo_used[k]) continue;
                if (lfo_trig[k] == 0) lfo_val[k] = glfo_val[k][c0 / CHUNK];
                else {
                    lfo_val[k] = lfo_value(lfo_shape[k], v.lfo_ph[k], v.lfo_rnd[k], lfo_tab[k]);
                    double ph = v.lfo_ph[k] + double(lfo_inc[k]) * n * v.lfo_rate_mod[k];
                    if (ph >= 1) {
                        if (lfo_trig[k] == 2) ph = 0.999999;   // one-shot: it holds at the end
                        else { ph -= std::floor(ph); v.lfo_rnd[k][0] = v.lfo_rnd[k][1]; v.lfo_rnd[k][1] = rand01() * 2 - 1; }
                    }
                    v.lfo_ph[k] = ph;
                }
                if (lfo_dest[k] > 0 && lfo_dest[k] < SD_COUNT) dst[lfo_dest[k]] += lfo_val[k] * lfo_depth[k];
            }
            if (me_on && me_dest > 0 && me_dest < SD_COUNT) dst[me_dest] += v.menv.level * me_amt;
            // 2.4.17: MOD ENV 2 / 3: this chunk's level, then on by the chunk (Env::tick_n)
            float xe_val[SYN_XENVS];
            for (int k = 0; k < SYN_XENVS; k++) {
                xe_val[k] = v.xenv[k].level;
                if (xe_on[k]) dst[xe_dest[k]] += xe_val[k] * xe_amt[k];
                v.xenv[k].tick_n(n);
            }
            for (int k = 0; k < n_routes; k++) {   // 2.3.3: the MATRIX
                float sv = 0;
                switch (mx_src[k]) {
                case SS_LFO1: sv = lfo_val[0]; break;
                case SS_LFO2: sv = lfo_val[1]; break;
                case SS_LFO3: sv = lfo_val[2]; break;   // 2.4.17
                case SS_LFO4: sv = lfo_val[3]; break;
                case SS_MOD_ENV2: sv = xe_val[0]; break;
                case SS_MOD_ENV3: sv = xe_val[1]; break;
                case SS_MOD_ENV: sv = v.menv.level; break;
                case SS_AMP_ENV: sv = v.amp.level; break;
                case SS_FLT_ENV: sv = v.fenv.level; break;
                case SS_VEL: sv = v.vel; break;
                case SS_NOTE: sv = float(v.note - 60) / 60.0f; break;
                case SS_WHEEL: sv = wheel_; break;
                case SS_AT: sv = at_; break;
                case SS_MACRO1: case SS_MACRO2: case SS_MACRO3: case SS_MACRO4: sv = mac[mx_src[k] - SS_MACRO1]; break;
                case SS_RANDOM: sv = v.rnd; break;
                default: break;
                }
                dst[mx_dst[k]] += sv * mx_amt[k];
            }
            for (int k = 0; k < SYN_LFOS; k++)
                v.lfo_rate_mod[k] = lfo_used[k] && dst[LFO_RATE_DST[k]] != 0 ? std::exp2(3.0f * std::max(-1.0f, std::min(1.0f, dst[LFO_RATE_DST[k]]))) : 1.0f;
            // ---- 2.3.3: glide (2.4.9: after the modulation: GLIDE +1 makes it 8x slower, -1 8x faster) ----
            if (v.glide != 0) {
                float st = v.glide_step * float(n);
                if (dst[SD_GLIDE] != 0) st *= std::exp2(-3.0f * std::max(-1.0f, std::min(1.0f, dst[SD_GLIDE])));
                v.glide = std::fabs(v.glide) <= st ? 0.0f : v.glide - (v.glide > 0 ? st : -st);
            }
            const double pitch_mod = dst[SD_PITCH] * 12.0 + v.glide + v.drift + wander_st;
            {   // SUB and keyed NOISE: at the note's pitch with its modulation (2.4.2: they missed the PITCH destination)
                const double note_hz = 440.0 * std::exp2((v.note + pitch_mod - 69 + bend_semi) / 12.0);
                if (subt) {
                    sub_inc = std::min(0.45, note_hz / (sub_oct == 2 ? 4 : 2) / SR);
                    double m = sub_inc * WT_SIZE * 1.15;
                    sub_mip = m <= 1 ? 0 : std::min(WT_MIPS - 1, int(std::ceil(std::log2(m))));
                }
                if (nkey) nbp_f = 2 * std::sin(float(M_PI) * float(std::min(note_hz, 8000.0)) / SR);
            }
            // the oscillators: frames, warp, increments, mips, gains
            struct OscChunk { int fa = 0, fb = 0; float pos = 0, ff = 0, warp = 0, gl[SYN_MAX_UNISON], gr[SYN_MAX_UNISON], mf[SYN_MAX_UNISON]; double inc[SYN_MAX_UNISON]; int ma[SYN_MAX_UNISON]; } oc[SYN_OSCS];
            const float bal = std::max(-1.0f, std::min(1.0f, bal0 + dst[SD_BALANCE])), ga = bal <= 0 ? 1 : 1 - bal, gb = bal >= 0 ? 1 : 1 + bal;
            for (int o = 0; o < SYN_OSCS; o++) {
                const OscBlock &b = ob[o];
                if (!b.on) continue;
                OscChunk &q = oc[o];
                const float pos = std::max(0.0f, std::min(1.0f, b.pos + dst[o ? SD_POS_B : SD_POS_A]));
                q.pos = pos;
                const float scan = b.sample || b.macro ? 0.0f : pos * float(b.t->frames - 1);
                const int frames_t = b.sample || b.macro ? 1 : b.t->frames;
                q.fa = std::min(frames_t - 1, int(scan));
                q.fb = std::min(frames_t - 1, q.fa + 1);
                q.ff = scan - float(q.fa);
                q.warp = std::max(0.0f, std::min(1.0f, b.warp + dst[o ? SD_WARP_B : SD_WARP_A]));
                // (2.4.2: PAN modulation is the output's; 2.4.6: the pan gains are the block's, b.pl / b.pr)
                // 2.4.35: LEVEL A / B (the MATRIX): this chunk's level = the stored one + the modulation (0..1); only when routed
                const float dl = dst[o ? SD_LEVEL_B : SD_LEVEL_A];
                float gain_c = b.gain, lv_k = 1;
                if (dl != 0) {
                    const float lv = std::max(0.0f, std::min(1.0f, b.lvl + dl));
                    gain_c = lv / std::sqrt(float(b.u_n));
                    lv_k = b.lvl > 1e-6f ? lv / b.lvl : 0;
                }
                const bool from_zero = dl != 0 && b.lvl <= 1e-6f;   // stored silent, faded in: its pan gains computed here
                for (int u = 0; u < b.u_n; u++) {
                    const float upos = b.u_n == 1 ? 0.0f : float(u) / float(b.u_n - 1) * 2 - 1;
                    const float det = std::max(0.0f, b.det + dst[o ? SD_DET_B : SD_DET_A] * 100.0f);   // 2.3.3: + the MATRIX
                    const double semis = v.note - 69 + b.pitch + pitch_mod + upos * det * 0.5 / 100.0;
                    if (b.sample) {   // 2.3.4: frames per output sample: the note against the sample's root, at its rate
                        const Voice::SamplePlay &sp = v.smp[o];
                        q.inc[u] = sp.pcm ? std::min(16.0, std::exp2((semis + 69 - sp.root) / 12.0) * double(sp.pcm->rate) / SR) : 0;
                    } else
                    q.inc[u] = std::min(0.45, 440.0 * std::exp2(semis / 12.0) / SR);
                    double m = q.inc[u] * WT_SIZE * (b.wmode == WM_SYNC ? 1 + 7 * q.warp : 1) * 1.15;
                    m = m <= 1 ? 0 : std::log2(m);
                    m = std::min(double(WT_MIPS - 1) - 0.001, m);
                    q.ma[u] = int(m);
                    q.mf[u] = float(m - int(m));
                    const float g = o ? gb : ga;
                    const float dsp = dst[o ? SD_SPREAD_B : SD_SPREAD_A];
                    if ((dsp != 0 && b.u_n > 1) || from_zero) {   // 2.4.9: SPREAD modulated: this voice's pan gains (only when routed)
                        const float ang = (std::max(-1.0f, std::min(1.0f, b.pan + upos * std::max(0.0f, std::min(1.0f, b.spread + dsp)))) + 1) * float(M_PI) / 4;
                        q.gl[u] = std::cos(ang) * gain_c * 1.41421356f * g;
                        q.gr[u] = std::sin(ang) * gain_c * 1.41421356f * g;
                    } else {
                        q.gl[u] = b.pl[u] * g * lv_k;
                        q.gr[u] = b.pr[u] * g * lv_k;
                    }
                }
            }
            const float fm = std::max(0.0f, fm0 + dst[SD_FM]) * 2.0f;
            // 2.4.9: FEEDBACK, RING, NOISE TONE and SUB PAN as destinations (block values unless routed)
            const float fbk_c = dst[SD_FEEDBACK] != 0 ? std::max(0.0f, std::min(1.5f, fbk + dst[SD_FEEDBACK] * 1.5f)) : fbk;
            const float ring_c = dst[SD_RING] != 0 ? std::max(0.0f, std::min(1.0f, ring + dst[SD_RING])) : ring;
            const float ntone_c = dst[SD_NOISE_TONE] != 0 && noise_on
                ? 1 - std::exp(-2 * float(M_PI) * (200.0f * std::exp2(6.643856f * std::max(0.0f, std::min(1.0f, ntone0 + dst[SD_NOISE_TONE])))) / SR)
                : ntone_k;
            float sub_gl_c = sub_gl, sub_gr_c = sub_gr;
            if (dst[SD_SUB_PAN] != 0 && subt) eqpan(sub_pan0 + dst[SD_SUB_PAN], sub_lv0, sub_gl_c, sub_gr_c);
            // the filter's coefficients for this chunk (envelope, LFO / env / macro on cutoff and resonance)
            // 2.4.2: the targets for the end of this chunk; the filter glides to them sample by sample (Voice::fc*: the
            // coefficients now and their step), so a fast sweep (envelope, LFO, MATRIX) moves smoothly, not in 16-sample
            // steps. Topology-preserving (zero-delay feedback) filters after V. Zavalishin, "The Art of VA Filter Design",
            // and A. Simper (Cytomic), "Solving the continuous SVF equations using trapezoidal integration": both
            // published maths, written here from the equations.
            int fc_lo = 0, fc_hi = 0;   // the coefficients this type uses (Voice::FC_*)
            if (flt) {
                const float res = std::max(0.0f, std::min(1.0f, res0 + dst[SD_RES] * 0.5f));
                const float fc = std::max(20.0f, std::min(0.45f * SR, base_cut * std::exp2(env_oct * v.fenv.level + dst[SD_CUTOFF] * 4 + wander_oct)));
                const float g = std::tan(float(M_PI) * fc / SR);
                float tg[Voice::FC_N];
                if (ftype == FT_LADDER) {
                    const float G = g / (1 + g), kl = 4.2f * res;
                    tg[Voice::FC_G] = G; tg[Voice::FC_KL] = kl; tg[Voice::FC_INV] = 1 / (1 + kl * G * G * G * G);
                    fc_lo = Voice::FC_G; fc_hi = Voice::FC_INV + 1;
                } else if (ftype == FT_COMB) {
                    tg[Voice::FC_CD] = std::max(2.0f, std::min(float(COMB_LEN - 3), SR / fc));   // fractional: no steps
                    tg[Voice::FC_CFB] = 0.97f * res;
                    fc_lo = Voice::FC_CD; fc_hi = Voice::FC_CFB + 1;
                } else {
                    // LP 24: two SVFs; the second's damping no lower than 0.8 (its peak at most +1.9 dB), so the resonance
                    // isn't squared (two Q 25 peaks were +56 dB: it blew up near max). Up to 61 % it is as it was
                    const float k = 2 - 1.96f * res, k2 = std::max(k, 0.8f);
                    const float a1 = 1 / (1 + g * (g + k)), b1 = 1 / (1 + g * (g + k2));
                    tg[Voice::FC_K] = k; tg[Voice::FC_A1] = a1; tg[Voice::FC_A2] = g * a1; tg[Voice::FC_A3] = g * g * a1;
                    tg[Voice::FC_K2] = k2; tg[Voice::FC_B1] = b1; tg[Voice::FC_B2] = g * b1; tg[Voice::FC_B3] = g * g * b1;
                    fc_lo = Voice::FC_K; fc_hi = Voice::FC_B3 + 1;
                }
                const bool jump = !v.fc_set || v.fc_type != ftype;   // the first chunk of a note / a new type: no glide
                for (int c = fc_lo; c < fc_hi; c++) {
                    if (jump) v.fc[c] = tg[c];
                    v.fcd[c] = (tg[c] - v.fc[c]) / float(n);
                }
                v.fc_set = true; v.fc_type = ftype;
            }
            const float lvl = std::max(0.0f, 1 + dst[SD_LEVEL]);
            // 2.3.3: SUB / NOISE level and DRIVE from the MATRIX
            const float sub_m = std::max(0.0f, 1 + dst[SD_SUB]), noise_m = std::max(0.0f, 1 + dst[SD_NOISE]);
            const float drv = std::max(0.0f, std::min(1.0f, drive + dst[SD_DRIVE]));
            const float dkc = 1 + 7 * drv, dnc = drv > 0.001f ? 1 / std::tanh(dkc) : 1;
            // 2.4.5: with DRIVE up the filter saturates inside too (the ladder's third stage input, the SVF's band state):
            // the resonance rounds off and sings instead of growing clean. Linear below about +-1 at low drive
            const float nlg = drv > 0.001f ? 0.08f + 0.17f * drv : 0, nli = nlg > 0 ? 1 / nlg : 0;   // linear to about +-4..12
            float out_gl, out_gr;
            eqpan(pan0 + dst[SD_PAN], 1.0f, out_gl, out_gr);
            // ---- the samples ----
            if (!fenv_need) v.fenv.tick_n(n);   // 2.4.11: unread this chunk: its timing kept, its per-sample cost not
            if (!menv_need) v.menv.tick_n(n);
            for (int i = c0; i < c0 + n; i++) {
                float env = v.amp.tick();
                if (fenv_need) v.fenv.tick();
                if (menv_need) v.menv.tick();
                if (v.stealing) { v.fade -= 1.0f / (0.005f * SR); if (v.fade <= 0) { v.on = false; alive = false; break; } env *= v.fade; }
                if (v.amp.done()) { v.on = false; alive = false; break; }
                float al = 0, ar = 0, bl = 0, br = 0, am = 0, bm = 0;
                for (int o = 0; o < SYN_OSCS; o++) {
                    const OscBlock &b = ob[o];
                    if (!b.on) continue;
                    const OscChunk &q = oc[o];
                    float osum = 0, ol = 0, orr = 0;
                    bool wrapped = false;
                    if (b.macro) {   // 2.3.5: a MACRO engine (POSITION harmonics, WARP timbre, SPREAD morph)
                        const float smp = macro_tick(v, o, b.engine, q.inc[0], q.pos, q.warp, b.spread);
                        v.last[o] = smp;
                        if (o == 0) { al = smp * q.gl[0]; ar = smp * q.gr[0]; am = smp; } else { bl = smp * q.gl[0]; br = smp * q.gr[0]; bm = smp; }
                        continue;
                    }
                    if (b.sample) {   // 2.3.4: a SAMPLE oscillator: its audio, pitched, looping or once through
                        Voice::SamplePlay &sp = v.smp[o];
                        if (sp.pcm) {
                            const int ch = sp.pcm->channels;
                            // 2.4.30 (audit F9): a loop with UNTIL RELEASE stops looping at note-off (the sample plays on to its end)
                            const bool cx = sp.complex_loop, loop_on = sp.loop_end > 0 && !(sp.until_release && v.released);
                            const int16_t *d = sp.pcm->data.data();
                            for (int u = 0; u < b.u_n; u++) {
                                if (sp.done[u]) continue;
                                double p = sp.pos[u];
                                const int64_t k = int64_t(p);
                                const float f = float(p - double(k));
                                int64_t k1 = k + 1;
                                if (!cx) { if (sp.loop_end > 0 && k1 >= sp.loop_end) k1 = sp.loop_start; }
                                else if (loop_on && k1 >= sp.loop_end) k1 = sp.loop_type == LoopType::Forward ? sp.loop_start : k;   // (Backward / Alternating: no wrap to read)
                                if (k1 >= sp.end) k1 = k;
                                float a = d[size_t(k) * size_t(ch)], c = d[size_t(k1) * size_t(ch)];
                                if (ch > 1) { a = (a + d[size_t(k) * size_t(ch) + 1]) * 0.5f; c = (c + d[size_t(k1) * size_t(ch) + 1]) * 0.5f; }
                                const float smp = (a + (c - a) * f) * (1.0f / 32768.0f);
                                osum += smp;
                                ol += smp * q.gl[u];
                                orr += smp * q.gr[u];
                                if (!cx) {
                                    p += q.inc[u];
                                    if (sp.loop_end > 0) { while (p >= double(sp.loop_end)) p -= double(sp.loop_end - sp.loop_start); }
                                    else if (p >= double(sp.end - 1)) sp.done[u] = true;
                                } else {   // Backward / Alternating / UNTIL RELEASE (as the sampler's voices: sampler.cpp, loop / end handling)
                                    if (!loop_on) sp.back[u] = false;   // released out of a backward leg: on, forwards, to the end
                                    p += sp.back[u] ? -q.inc[u] : q.inc[u];
                                    if (loop_on) {
                                        const double ls = double(sp.loop_start), le = double(sp.loop_end), len = le - ls;
                                        if (sp.loop_type == LoopType::Forward) { while (p >= le) p -= len; }
                                        else if (sp.loop_type == LoopType::Backward) {
                                            if (!sp.back[u] && p >= le) { p = le - (p - le) - 1; sp.back[u] = true; }
                                            else if (sp.back[u] && p < ls) p += len;
                                        } else {
                                            if (!sp.back[u] && p >= le - 1) { p = 2.0 * (le - 1) - p; sp.back[u] = true; }
                                            else if (sp.back[u] && p < ls) { p = 2.0 * ls - p; sp.back[u] = false; }
                                        }
                                        if (sp.loop_type != LoopType::Forward) p = std::max(ls, std::min(p, le - 1e-6));   // (a step longer than the loop: stay inside it)
                                    } else if (p >= double(sp.end - 1)) sp.done[u] = true;
                                }
                                sp.pos[u] = p;
                            }
                        }
                        v.last[o] = osum / float(b.u_n);
                        if (o == 0) { al = ol; ar = orr; am = v.last[0]; } else { bl = ol; br = orr; bm = v.last[1]; }
                        continue;
                    }
                    for (int u = 0; u < b.u_n; u++) {
                        double ph = v.ph[o][u];
                        double pw = b.wmode == WM_OFF ? ph : warp_phase(b.wmode, ph, q.warp);
                        double pm = 0;
                        if (b.wmode == WM_FM) pm += q.warp * 2.0 * v.last[1 - o];
                        if (o == 0 && (fm > 0 || fbk_c > 0)) pm += fm * v.last[1] + fbk_c * v.last[0];
                        if (pm != 0) { pw += pm; pw -= std::floor(pw); }
                        const int ma = q.ma[u], mb = ma + 1 < WT_MIPS ? ma + 1 : ma;
                        const float a0 = read(b.t->cycle(q.fa, ma), pw), b0 = read(b.t->cycle(q.fb, ma), pw);
                        float smp = a0 + (b0 - a0) * q.ff;
                        if (q.mf[u] > 0.001f) {
                            const float a1_ = read(b.t->cycle(q.fa, mb), pw), b1 = read(b.t->cycle(q.fb, mb), pw);
                            smp += (a1_ + (b1 - a1_) * q.ff - smp) * q.mf[u];
                        }
                        osum += smp;
                        ol += smp * q.gl[u];
                        orr += smp * q.gr[u];
                        ph += q.inc[u];
                        if (ph >= 1) { ph -= 1; if (u == 0) wrapped = true; }
                        v.ph[o][u] = ph;
                    }
                    v.last[o] = osum / float(b.u_n);
                    if (o == 0) { al = ol; ar = orr; am = v.last[0]; if (sync && wrapped) for (int u = 0; u < ob[1].u_n; u++) v.ph[1][u] = v.start_ph[1][u]; }
                    else { bl = ol; br = orr; bm = v.last[1]; }
                }
                float oscl = al + bl, oscr = ar + br;
                if (ring_c > 0) {   // 2.3.6: the ring follows both oscillators' LEVELs (silent with either at 0)
                    const float rm = am * bm * 2 * ring_lv;
                    oscl += ring_c * (rm - oscl); oscr += ring_c * (rm - oscr);
                }
                float subl = 0, subr = 0;
                if (subt) {
                    const float s_ = read(subt->cycle(0, sub_mip), v.sub_ph);
                    subl = s_ * sub_gl_c * sub_m; subr = s_ * sub_gr_c * sub_m;
                    v.sub_ph += sub_inc; if (v.sub_ph >= 1) v.sub_ph -= 1;
                }
                float nl = 0, nr = 0;
                if (noise_on) {
                    v.noise = v.noise * 1664525u + 1013904223u;
                    float w = float(int32_t(v.noise)) * (1.0f / 2147483648.0f), nz = w;
                    if (ncol == 1) {
                        float *p = v.pink;
                        p[0] = 0.99886f * p[0] + w * 0.0555179f; p[1] = 0.99332f * p[1] + w * 0.0750759f;
                        p[2] = 0.96900f * p[2] + w * 0.1538520f; p[3] = 0.86650f * p[3] + w * 0.3104856f;
                        p[4] = 0.55000f * p[4] + w * 0.5329522f; p[5] = -0.7616f * p[5] - w * 0.0168980f;
                        nz = (p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + w * 0.5362f) * 0.11f;
                        p[6] = w * 0.115926f;
                    } else if (ncol == 2) {
                        v.crack *= 0.92f;
                        if ((v.noise >> 20) < 6) v.crack = w > 0 ? 1.0f : -1.0f;
                        nz = v.crack;
                    }
                    if (nkey) { v.nbp2 += nbp_f * v.nbp1; float hp = nz - v.nbp2 - 0.125f * v.nbp1; v.nbp1 += nbp_f * hp; nz = v.nbp1 * 0.5f; }
                    v.ntone[0] += ntone_c * (nz - v.ntone[0]);
                    nz = v.ntone[0];
                    nl = nz * n_gl * noise_m; nr = nz * n_gr * noise_m;
                }
                float in_l = 0, in_r = 0, by_l = 0, by_r = 0;
                ((route & 3) ? in_l : by_l) += oscl; ((route & 3) ? in_r : by_r) += oscr;
                if ((route & 3) && (route & 3) != 3) {
                    in_l -= oscl; in_r -= oscr; by_l += oscl; by_r += oscr;
                    const float fl = (route & 1) ? al : bl, fr = (route & 1) ? ar : br;
                    in_l += fl; in_r += fr; by_l -= fl; by_r -= fr;
                }
                ((route & 4) ? in_l : by_l) += subl; ((route & 4) ? in_r : by_r) += subr;
                ((route & 8) ? in_l : by_l) += nl; ((route & 8) ? in_r : by_r) += nr;
                float outl = by_l, outr = by_r;
                if (flt) {
                    for (int c = fc_lo; c < fc_hi; c++) v.fc[c] += v.fcd[c];   // 2.4.2: the coefficients glide
                    const float *F = v.fc;
                    for (int c = 0; c < 2; c++) {
                        float x = c ? in_r : in_l;
                        if (drv > 0.001f) x = fast_tanh(x * dkc) * dnc;
                        float y = x;
                        if (ftype == FT_LADDER) {   // four TPT one-poles, the loop solved (Zavalishin ch. 5); tanh at its input
                            float *st = v.lad[c];
                            const float G = F[Voice::FC_G], kl = F[Voice::FC_KL];
                            const float S = G * G * G * st[0] + G * G * st[1] + G * st[2] + st[3];
                            const float G4 = G * G * G * G;
                            const float y4 = (G4 * x + S * (1 - G)) * F[Voice::FC_INV];
                            float u = 2 * fast_tanh(0.5f * (x - kl * y4));   // 2.4.2: gentler (it squashed the resonance: +3 dB at 90 %)
                            for (int si = 0; si < 4; si++) {
                                if (nlg > 0 && si == 2) u = fast_tanh(u * nlg) * nli;   // one stage: the cost of each is a tanh per sample and channel
                                const float vv = (u - st[si]) * G, yy = vv + st[si];
                                st[si] = yy + vv;
                                u = yy;
                            }
                            y = u * (1 + 0.5f * kl);
                        } else if (ftype == FT_COMB) {   // 2.4.2: a fractional delay (linear), so the tuning glides
                            float *cb = v.comb[c];
                            const float d = F[Voice::FC_CD], comb_fb = F[Voice::FC_CFB];
                            const int di = int(d);
                            const float fr = d - float(di);
                            int rd = v.comb_w - di; if (rd < 0) rd += COMB_LEN;
                            int rd2 = rd - 1; if (rd2 < 0) rd2 += COMB_LEN;
                            y = x + comb_fb * (cb[rd] + (cb[rd2] - cb[rd]) * fr);
                            cb[v.comb_w] = y;
                            y *= 1 - 0.5f * comb_fb;
                        } else {
                            const int stages = ftype == FT_LP24 ? 2 : 1;
                            float in = x;
                            for (int sg = 0; sg < stages; sg++) {   // Simper's SVF (trapezoidal)
                                float *ic = v.svf[sg][c];
                                const float *A = F + (sg ? Voice::FC_B1 : Voice::FC_A1), k_svf = F[Voice::FC_K];
                                const float a1 = A[0], a2 = A[1], a3 = A[2];
                                const float v3 = in - ic[1], v1 = a1 * ic[0] + a2 * v3, v2 = ic[1] + a2 * ic[0] + a3 * v3;
                                ic[0] = 2 * v1 - ic[0]; ic[1] = 2 * v2 - ic[1];
                                if (nlg > 0) ic[0] = fast_tanh(ic[0] * nlg) * nli;
                                switch (ftype) {
                                case FT_BP: in = v1 * k_svf; break;
                                case FT_HP: in = in - k_svf * v1 - v2; break;
                                case FT_NOTCH: in = in - k_svf * v1; break;
                                default: in = v2; break;
                                }
                            }
                            y = in;
                        }
                        if (fat > 0) { v.fat_lp[c] += fat_k * ((c ? in_r : in_l) - v.fat_lp[c]); y += fat * v.fat_lp[c]; }
                        const float dry = c ? in_r : in_l;
                        (c ? outr : outl) += dry + (y - dry) * fmix;
                    }
                    if (ftype == FT_COMB) { if (++v.comb_w >= COMB_LEN) v.comb_w = 0; }
                } else { outl += in_l; outr += in_r; }
                const float gg = env * v.vel * level * lvl * 0.5f;
                l[i] += outl * gg * out_gl;
                r[i] += outr * gg * out_gr;
            }
        }
    }
    for (int k = 0; k < SYN_LFOS; k++) glfo_rate_mod_[k] = newest ? newest->lfo_rate_mod[k] : 1.0f;
    this->active.store(active, std::memory_order_relaxed);
}

}  // namespace omni

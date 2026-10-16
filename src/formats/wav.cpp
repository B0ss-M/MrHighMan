// Omni Sampler: a single audio file as an instrument: one zone over the keyboard, root and loop from the file's own
// metadata (WAV smpl/inst, AIFF INST/MARK), C3 otherwise.
//
// Single cycles and wavetables (1.7): a WAV that is one cycle (256, 512, 600, 1024, 2048 or 4096 frames, no loop) plays
// as an oscillator: looped, tuned so the cycle sounds at the key's pitch, with a synth envelope the ADSR faders shape. A
// wavetable (frames marked by a Serum "clm " or Surge "srge" chunk) plays its cycles with the WT position crossfading
// between them. Either way the keyboard is split by octaves into band-limited copies (harmonics above what the octave's
// top note can carry removed by FFT), so high notes don't alias.
// 1.8.1: Surge / Bitwig .wt files too, and wavetable WAVs with no chunk (many Serum packs, WaveEdit's 64 x 256): see
// guess_cycle. A single cycle that loops over the whole file (e.g. 600 frames looped 0-599) is an oscillator too, its
// pitch from its length.
#include <cmath>
#include <complex>
#include <cstring>
#include "format.hpp"

namespace omni {
namespace {

std::vector<PresetInfo> list_audio(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

// a wavetable's cycle length from its WAV chunks: Serum "clm " ("<!>2048 ..."), Surge "srge" (version, cycle length)
int wavetable_cycle(const Blob &b) {
    std::vector<uint8_t> h = b.head(1 << 16);
    if (h.size() >= 12 && !std::memcmp(h.data(), "vawt", 4)) return int(le32(&h[4]));   // .wt
    if (h.size() < 12 || std::memcmp(h.data(), "RIFF", 4) || std::memcmp(h.data() + 8, "WAVE", 4)) return 0;
    for (size_t p = 12; p + 8 <= h.size();) {
        uint32_t n = le32(&h[p + 4]);
        const uint8_t *c = &h[p + 8];
        size_t avail = h.size() - (p + 8);
        if (!std::memcmp(&h[p], "clm ", 4) && n >= 4 && avail >= 4 && !std::memcmp(c, "<!>", 3)) {
            int v = 0;
            for (size_t i = 3; i < std::min<size_t>(n, avail) && isdigit(c[i]); i++) v = v * 10 + (c[i] - '0');
            return v;
        }
        if (!std::memcmp(&h[p], "srge", 4) && n >= 8 && avail >= 8) return int(le32(c + 4));
        if (!std::memcmp(&h[p], "data", 4)) break;   // the metadata chunks come before the audio
        p += 8 + n + (n & 1);
    }
    return 0;
}

bool single_cycle_size(int64_t frames);

// A wavetable WAV with no chunk to say so (mono, no loop, more than one cycle's worth). In order: a Folderinfo.txt beside
// it whose first line is the cycle length ("[2048]", as KRC's Serum folders have); the smallest cycle length (256 ...
// 4096, 2-256 cycles) at which 9 in 10 cycles either end where they start (the jump back no bigger than 3 of their own
// steps) or match the cycle next to them (correlation 0.9+), at least half of them not near silent; else WaveEdit's
// layout, 64 x 256 (16384 16-bit frames). Old samplers' drum hits (block-sized, silent tails) and recordings fail.
int guess_cycle(VolumePtr vol, const std::string &path, const Blob &b, const AudioInfo &ai) {
    if (ai.channels != 1 || !ai.loops.empty() || single_cycle_size(ai.frames) || ai.frames < 512 || ai.frames > 256 * 4096 ||
        ai.frames % 256) return 0;
    auto fits = [&](int64_t n) { return n > 1 && ai.frames % n == 0 && ai.frames / n >= 2 && ai.frames / n <= 256; };
    try {
        std::vector<uint8_t> t = vol->open(path_join(path_dir(path), "Folderinfo.txt"))->head(64);
        size_t i = 0;
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
        if (i < t.size() && t[i] == '[') {
            int64_t n = 0;
            for (i++; i < t.size() && isdigit(t[i]); i++) n = n * 10 + (t[i] - '0');
            if (i < t.size() && t[i] == ']' && fits(n)) return int(n);
        }
    } catch (const std::exception &) {}
    PcmPtr p = decode_audio(b);
    const std::vector<int16_t> &x = p->data;
    if (int64_t(x.size()) < ai.frames) return 0;
    for (int n : {256, 512, 1024, 2048, 4096}) {
        if (!fits(n)) continue;
        int count = int(ai.frames / n);
        std::vector<double> energy(static_cast<size_t>(count), 0.0), corr(static_cast<size_t>(count), 0.0);   // corr[c]: cycle c against c + 1
        for (int c = 0; c < count; c++) {
            double e = 0;
            for (int i = 0; i < n; i++) e += double(x[size_t(c * n + i)]) * x[size_t(c * n + i)];
            energy[size_t(c)] = std::sqrt(e);
        }
        for (int c = 0; c + 1 < count; c++) {
            double d = 0;
            for (int i = 0; i < n; i++) d += double(x[size_t(c * n + i)]) * x[size_t((c + 1) * n + i)];
            double den = energy[size_t(c)] * energy[size_t(c + 1)];
            corr[size_t(c)] = den > 0 ? d / den : 0;
        }
        int good = 0, loud = 0;
        for (int c = 0; c < count; c++) {
            const int16_t *f = &x[size_t(c * n)];
            double steps = 0;
            for (int i = 0; i + 1 < n; i++) steps += std::abs(f[i + 1] - f[i]);
            bool wraps = std::abs(f[n - 1] - f[0]) <= 3 * steps / (n - 1) + 64;
            bool alike = (c > 0 && corr[size_t(c - 1)] >= 0.9) || (c + 1 < count && corr[size_t(c)] >= 0.9);
            if (wraps || alike) good++;
            if (energy[size_t(c)] / std::sqrt(double(n)) > 330) loud++;
        }
        if (good * 10 >= count * 9 && loud * 2 >= count) return n;
    }
    if (ai.frames == 16384 && ai.bits == 16) return 256;
    return 0;
}

bool single_cycle_size(int64_t frames) {
    for (int s : {256, 512, 600, 1024, 2048, 4096}) if (frames == s) return true;
    return false;
}

// in-place complex FFT (n a power of two); inverse with inv
void fft(std::vector<std::complex<double>> &a, bool inv) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2 * M_PI / double(len) * (inv ? 1 : -1);
        std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; k++) {
                std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inv) for (auto &x : a) x /= double(n);
}

// One cycle with only harmonics 1..h (and no DC offset change): FFT for a power of two, else a direct DFT (600 frames).
void band_limit(std::vector<double> &x, int h) {
    size_t n = x.size();
    if (h >= int(n / 2)) return;
    if ((n & (n - 1)) == 0) {
        std::vector<std::complex<double>> a(x.begin(), x.end());
        fft(a, false);
        for (size_t k = size_t(h) + 1; k < n - size_t(h); k++) a[k] = 0;
        fft(a, true);
        for (size_t i = 0; i < n; i++) x[i] = a[i].real();
        return;
    }
    std::vector<double> y(n, 0);
    for (int k = 0; k <= h; k++) {   // rebuild from harmonics 0..h
        double re = 0, im = 0;
        for (size_t i = 0; i < n; i++) { double ph = 2 * M_PI * double(k) * double(i) / double(n); re += x[i] * std::cos(ph); im -= x[i] * std::sin(ph); }
        double sc = (k == 0 ? 1.0 : 2.0) / double(n);
        for (size_t i = 0; i < n; i++) { double ph = 2 * M_PI * double(k) * double(i) / double(n); y[i] += sc * (re * std::cos(ph) - im * std::sin(ph)); }
    }
    x = y;
}

// the file's cycles with harmonics above h removed (h <= 0: as they are)
SampleRefPtr cycles_ref(VolumePtr vol, const std::string &path, int size, int count, int h, const AudioInfo &ai) {
    auto r = std::make_shared<SampleRef>();
    r->name = path_stem(path);
    r->key = "wt:" + path + ":" + std::to_string(h);
    r->rate = ai.rate; r->channels = ai.channels; r->frames = int64_t(size) * count;
    r->decode = [vol, path, size, count, h]() {
        PcmPtr src = decode_audio(*vol->open(path));
        if (h <= 0) return src;
        auto out = std::make_shared<Pcm>(*src);
        int ch = src->channels;
        std::vector<double> x(size_t(size), 0);
        for (int f = 0; f < count; f++)
            for (int c = 0; c < ch; c++) {
                size_t base = size_t(f) * size_t(size) * size_t(ch) + size_t(c);
                for (int i = 0; i < size; i++) x[size_t(i)] = base + size_t(i) * ch < src->data.size() ? src->data[base + size_t(i) * ch] : 0;
                band_limit(x, h);
                for (int i = 0; i < size; i++)
                    if (base + size_t(i) * ch < out->data.size())
                        out->data[base + size_t(i) * ch] = int16_t(std::lrint(std::max(-32768.0, std::min(32767.0, x[size_t(i)]))));
            }
        return PcmPtr(out);
    };
    return r;
}

// a cycle (or wavetable) as an oscillator: band-limited copies across the keyboard by octaves, tuned to pitch
void oscillator_zones(Instrument &inst, VolumePtr vol, const std::string &path, const AudioInfo &ai, int size, int count) {
    double f0 = double(ai.rate) / double(size);   // the pitch the cycle plays at its own rate
    double tune = 12.0 * std::log2(440.0 / f0);   // with root 69 (A3): any key then plays its own pitch
    Envelope env;   // a synth envelope for the ADSR faders to shape (their Decay and Sustain scale these)
    env.set = true; env.attack = 0.003; env.decay = 1.0; env.sustain = 1.0; env.release = 0.25;
    for (int lo = 0; lo < 128;) {
        int hi = lo == 0 ? 35 : std::min(127, lo + 11);
        double f_hi = 440.0 * std::pow(2.0, (hi - 69) / 12.0);
        int h = int(0.45 * 44100.0 / f_hi);   // the highest harmonic under Nyquist (with a little room) at the top key
        Zone z;
        z.name = path_stem(path);
        z.sample = cycles_ref(vol, path, size, count, h >= size / 2 ? 0 : std::max(1, h), ai);
        z.key_lo = lo; z.key_hi = hi; z.root = 69; z.tune = tune;
        z.amp_env = env;
        if (count > 1) { z.wt_size = size; z.wt_count = count; }
        else { Loop l; l.start = 0; l.end = size - 1; z.loops.push_back(l); }
        inst.zones.push_back(z);
        lo = hi + 1;
    }
}

Instrument load_audio(VolumePtr vol, const std::string &path, int) {
    BlobPtr blob = vol->open(path);
    AudioInfo ai = probe_audio(*blob);
    Instrument inst;
    inst.name = path_stem(path);
    std::string ext = path_ext(path);
    int cyc = ext == "wav" || ext == "wave" || ext == "wt" ? wavetable_cycle(*blob) : 0;
    if (!cyc && (ext == "wav" || ext == "wave")) cyc = guess_cycle(vol, path, *blob, ai);
    // a loop over the whole of a cycle-sized file: the cycle itself
    bool whole_loop = ai.loops.size() == 1 && ai.loops[0].start == 0 && ai.loops[0].end == ai.frames - 1;
    if (cyc > 1 && ai.frames >= cyc && ai.frames % cyc == 0 && ai.frames / cyc <= 1024) {
        oscillator_zones(inst, vol, path, ai, cyc, int(ai.frames / cyc));
        inst.format = ai.frames / cyc > 1 ? "Wavetable (" + std::to_string(ai.frames / cyc) + " cycles)" : "Single cycle";
    } else if ((ai.loops.empty() || whole_loop) && single_cycle_size(ai.frames)) {
        oscillator_zones(inst, vol, path, ai, int(ai.frames), 1);
        inst.format = "Single cycle";
    } else {
        Zone z;
        z.name = inst.name;
        z.sample = file_sample_ref(vol, path);
        z.sample->rate = ai.rate;
        z.sample->channels = ai.channels;
        z.sample->frames = ai.frames;
        z.root = ai.root >= 0 ? ai.root : 60;
        z.tune = ai.fine / 100.0;
        if (ai.key_lo >= 0 && ai.key_hi >= ai.key_lo && !(ai.key_lo == 0 && ai.key_hi == 0)) { z.key_lo = ai.key_lo; z.key_hi = ai.key_hi; }
        if (ai.vel_lo >= 1 && ai.vel_hi >= ai.vel_lo) { z.vel_lo = ai.vel_lo; z.vel_hi = ai.vel_hi; }
        z.loops = ai.loops;
        z.gain_db = ai.gain_db;
        if (z.loops.empty() && ai.frames < ai.rate * 4) z.one_shot = false;
        inst.zones.push_back(z);
        inst.format = ext == "ncw" ? "NI NCW" : ext == "flac" ? "FLAC" : ext == "ogg" ? "Ogg Vorbis" : ext[0] == 'a' ? "AIFF" : "WAV";
    }
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_wav_folder() {
    register_reader({"Audio file", "wav wave aif aiff aifc flac ogg ncw wt", nullptr, list_audio, load_audio});
}

}  // namespace omni

// Omni Sampler: E-mu Emax and Emax II banks (.em1 .eb1 .em2 .eb2 .emx .ez1 .ez2, floppy and hard disk images).
// A bank is a memory dump: up to 100 presets, their voices and the (companded, Emax) or 16-bit (Emax II) audio.
// Banks are searched for anywhere in the file (an EM1 file has a signature in front, a floppy the OS, a hard disk
// up to 35 banks). Translated from ConvertWithMoss's EmaxDetector / EmaxConstants / EmaxModel (LGPL-3.0).
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include "format.hpp"

namespace omni {
namespace {
#include "emax_tables.inc"

constexpr int PARAMETER_SIZE = 0x7000, MEM_EMAX = 0x80000, MEM_EMAX2_MAX = 0x400000;
constexpr int CPU_BASE = 0x8000, CPU_END = CPU_BASE + PARAMETER_SIZE, NUM_PRESETS = 100;
constexpr int SELECTED_PRESET = 0xD0, SEQUENCE_TABLE = 0xD4, NUM_SEQ = 51;
constexpr int DIR_BOTTOM = 0x1A0, DIR_TOP = 0x1A4, MEM_USED = 0x1A8, PRESET_HEAP = 0x1AC;
constexpr int KEY_AREAS = 0x23, KEY_MAP = 0x24, VOICE_TABLE = 0x7C, NUM_KEYS = 88, KEY_OFFSET = 21, VOICE_SIZE = 32, ENTRY = 32;

int32_t i32(const std::vector<uint8_t> &d, size_t o) { return o + 4 <= d.size() ? int32_t(le32(&d[o])) : -1; }
int i16(const std::vector<uint8_t> &d, size_t o) { return o + 2 <= d.size() ? le16(&d[o]) : -1; }

int model_bytes(const std::vector<uint8_t> &d, size_t bank) {   // 1 = Emax (8-bit companded), 2 = Emax II
    int frames = 0;
    for (int i = 0; i < NUM_SEQ; i++) {
        int v = i32(d, bank + SEQUENCE_TABLE + size_t(i) * 4);
        if (v > frames && v <= MEM_EMAX2_MAX) frames = v;
    }
    return frames > MEM_EMAX ? 2 : 1;
}

// header bytes from bank: at least PRESET_HEAP + 4
bool is_bank(const std::vector<uint8_t> &d, size_t off, uint64_t file_size, uint64_t file_off) {
    if (i32(d, off + DIR_TOP) != CPU_END - ENTRY) return false;
    int bottom = i32(d, off + DIR_BOTTOM);
    if (bottom > CPU_END || bottom < CPU_BASE + PRESET_HEAP || (CPU_END - bottom) % ENTRY) return false;
    int sel = i32(d, off + SELECTED_PRESET);
    if (sel < 0 || sel >= NUM_PRESETS) return false;
    int first = i16(d, off);
    if (first < CPU_BASE + PRESET_HEAP || first >= CPU_END) return false;
    int used = i32(d, off + MEM_USED);
    if (used < 0 || used > MEM_EMAX2_MAX) return false;
    return file_off + PARAMETER_SIZE + uint64_t(used) * model_bytes(d, off) <= file_size;
}

std::vector<uint64_t> find_banks(const Blob &b) {
    static std::mutex m;
    static std::map<std::string, std::vector<uint64_t>> cache;
    std::vector<uint64_t> out;
    uint64_t size = b.size();
    if (size < PARAMETER_SIZE) return out;
    if (size <= (16u << 20)) {
        std::vector<uint8_t> d = b.all();
        for (uint64_t off = 0; off + PARAMETER_SIZE <= size;) {
            if (is_bank(d, size_t(off), size, off)) {
                out.push_back(off);
                off += PARAMETER_SIZE + uint64_t(i32(d, size_t(off) + MEM_USED)) * model_bytes(d, size_t(off));
            } else off++;
        }
        return out;
    }
    // hard disk images: banks sit on sector boundaries
    for (uint64_t off = 0; off + PARAMETER_SIZE <= size;) {
        std::vector<uint8_t> h = b.bytes(off, 0x200);
        if (is_bank(h, 0, size, off)) {
            out.push_back(off);
            off += PARAMETER_SIZE + uint64_t(i32(h, MEM_USED)) * model_bytes(h, 0);
            off = (off + 511) / 512 * 512;
        } else off += 512;
    }
    (void)m; (void)cache;
    return out;
}

int field(const uint8_t *v, int offset, int width) {
    int value = 0;
    for (int i = 0; i < width; i++) {
        int bit = offset + i;
        if ((v[bit / 8] >> (bit % 8)) & 1) value |= 1 << i;
    }
    return value;
}
int sfield(const uint8_t *v, int offset, int width) {
    int value = field(v, offset, width), sign = 1 << (width - 1);
    return (value & sign) ? value - (sign << 1) : value;
}
template <size_t N> double tab(const double (&t)[N], int i) { return t[clampi(i, 0, int(N) - 1)]; }

Envelope emax_env(const uint8_t *v, int off) {
    Envelope e;
    e.set = true;
    e.attack = tab(EMAX_ENVELOPE_ATTACK_TIME, field(v, off, 5));
    e.hold = tab(EMAX_ENVELOPE_HOLD_TIME, field(v, off + 5, 5));
    e.decay = tab(EMAX_ENVELOPE_DECAY_TIME, field(v, off + 10, 5));
    e.sustain = tab(EMAX_ENVELOPE_SUSTAIN_LEVEL, field(v, off + 15, 5));
    e.release = tab(EMAX_ENVELOPE_DECAY_TIME, field(v, off + 20, 5));
    return e;
}

int16_t expand(uint8_t value) {
    int chord = value >> 4 & 7, step = value & 15;
    int magnitude = (((step << 1) + 33) << chord) - 33;
    int scaled = magnitude * 4;
    return int16_t(value & 0x80 ? -scaled : scaled);
}

struct EmaxSample { bool ok = false; int start = 0, end = 0, loop_start = 0, loop_end = 0, flags = 0, rate = 0; };

struct BankView {
    uint64_t file_off = 0;
    std::vector<uint8_t> p;     // the parameter memory
    int bytes = 1;
    std::vector<EmaxSample> samples;
};

BankView read_bank(const Blob &b, uint64_t off) {
    BankView v;
    v.file_off = off;
    v.p = b.bytes(off, PARAMETER_SIZE);
    v.bytes = model_bytes(v.p, 0);
    int n = (CPU_END - i32(v.p, DIR_BOTTOM)) / ENTRY;
    int used = i32(v.p, MEM_USED), prev = 0;
    for (int i = 0; i < n; i++) {
        size_t e = size_t(PARAMETER_SIZE - ENTRY * (i + 1));
        EmaxSample s;
        s.start = i32(v.p, e + 4); s.end = i32(v.p, e + 8);
        if (s.start < prev || s.end <= s.start || s.end > used) { v.samples.push_back(s); continue; }
        prev = s.end;
        s.ok = true;
        s.loop_start = i32(v.p, e + 12); s.loop_end = i32(v.p, e + 16);
        s.flags = v.p[e + 28];
        int ri = v.p[e + 29];
        s.rate = EMAX_SAMPLE_RATES[ri < 8 ? ri : 0];
        v.samples.push_back(s);
    }
    return v;
}

int num_voices(const std::vector<uint8_t> &p, size_t preset, int areas) {
    int n = 0;
    for (int a = 0; a < areas; a++)
        for (int f : {2, 3}) {
            int voice = p[preset + VOICE_TABLE + size_t(a) * 4 + size_t(f)];
            if (voice != 0xFF && voice + 1 > n) n = voice + 1;
        }
    return n;
}
size_t voice_off(int areas, int voice) { return size_t(VOICE_TABLE + areas * 4 + voice * VOICE_SIZE); }

bool printable(uint8_t c) { return c >= 0x20 && c <= 0x7E; }

bool is_preset(const BankView &v, int preset) {
    if (preset < PRESET_HEAP || preset + VOICE_TABLE > PARAMETER_SIZE) return false;
    const auto &p = v.p;
    int areas = p[size_t(preset) + KEY_AREAS];
    if (!areas || !printable(p[size_t(preset)])) return false;
    for (int k = 0; k < NUM_KEYS; k++) {
        int a = p[size_t(preset) + KEY_MAP + size_t(k)];
        if (a != 0xFF && a >= areas) return false;
    }
    int nv = num_voices(p, size_t(preset), areas);
    if (!nv || preset + VOICE_TABLE + areas * 4 + nv * VOICE_SIZE > PARAMETER_SIZE) return false;
    for (int i = 0; i < nv; i++) {
        size_t vo = size_t(preset) + voice_off(areas, i);
        if (p[vo + 13] >= v.samples.size() || p[vo + 12] >= NUM_KEYS) return false;
    }
    return true;
}

std::vector<PresetInfo> list_emax(VolumePtr vol, const std::string &path) {
    BlobPtr b = vol->open(path);
    std::vector<uint64_t> banks = find_banks(*b);
    std::vector<PresetInfo> out;
    for (size_t bi = 0; bi < banks.size(); bi++) {
        BankView v = read_bank(*b, banks[bi]);
        for (int slot = 0; slot < NUM_PRESETS; slot++) {
            int preset = i16(v.p, size_t(slot) * 2) - CPU_BASE;
            if (!is_preset(v, preset)) continue;
            std::string name = trim(std::string(reinterpret_cast<const char *>(&v.p[size_t(preset)]), 12));
            char num[48];
            if (banks.size() > 1) std::snprintf(num, sizeof num, "B%zu %02d ", bi + 1, slot);
            else std::snprintf(num, sizeof num, "%02d ", slot);
            out.push_back({std::string(num) + (name.empty() ? "Preset" : name), int(bi * 100 + size_t(slot))});
        }
    }
    if (out.empty()) throw ParseError("no Emax presets in this file");
    return out;
}

Instrument load_emax(VolumePtr vol, const std::string &path, int index) {
    BlobPtr blob = vol->open(path);
    std::vector<uint64_t> banks = find_banks(*blob);
    size_t bi = size_t(index / 100);
    int slot = index % 100;
    if (bi >= banks.size()) throw ParseError("no such bank");
    BankView v = read_bank(*blob, banks[bi]);
    int preset = i16(v.p, size_t(slot) * 2) - CPU_BASE;
    if (!is_preset(v, preset)) throw ParseError("no such preset");
    const auto &p = v.p;
    int areas = p[size_t(preset) + KEY_AREAS];
    Instrument inst;
    inst.format = v.bytes == 2 ? "E-mu Emax II bank" : "E-mu Emax bank";
    inst.name = trim(std::string(reinterpret_cast<const char *>(&p[size_t(preset)]), 12));
    if (inst.name.empty()) inst.name = path_stem(path);
    inst.groups = {Group{"Layer 1"}, Group{"Layer 2"}};
    std::map<int, SampleRefPtr> refs;
    uint64_t audio = v.file_off + PARAMETER_SIZE;
    int bytes = v.bytes;
    for (int key = 0; key < NUM_KEYS;) {
        int area = p[size_t(preset) + KEY_MAP + size_t(key)];
        if (area == 0xFF) { key++; continue; }
        int last = key;
        while (last + 1 < NUM_KEYS && p[size_t(preset) + KEY_MAP + size_t(last + 1)] == area) last++;
        size_t entry = size_t(preset) + VOICE_TABLE + size_t(area) * 4;
        for (int layer = 0; layer < 2; layer++) {
            int voice = p[entry + size_t(layer == 0 ? 2 : 3)];
            if (voice == 0xFF) continue;
            const uint8_t *vr = &p[size_t(preset) + voice_off(areas, voice)];
            int si = vr[13];
            if (si >= int(v.samples.size()) || !v.samples[size_t(si)].ok) continue;
            const EmaxSample &s = v.samples[size_t(si)];
            auto r = refs.find(si);
            if (r == refs.end()) {
                auto ref = std::make_shared<SampleRef>();
                ref->name = "Sample " + std::to_string(si + 1);
                ref->rate = s.rate; ref->channels = 1; ref->frames = s.end - s.start;
                ref->key = "emax:" + std::to_string(reinterpret_cast<uintptr_t>(blob.get())) + ":" + std::to_string(banks[bi]) + ":" + std::to_string(si);
                uint64_t at = audio + uint64_t(s.start) * bytes;
                int frames = s.end - s.start, rate = s.rate;
                ref->decode = [blob, at, frames, rate, bytes]() -> PcmPtr {
                    if (bytes == 2) {
                        std::vector<uint8_t> raw = blob->bytes(at, size_t(frames) * 2);
                        return pcm_from_raw(raw.data(), frames, 1, Enc::S16LE, rate);
                    }
                    std::vector<uint8_t> raw = blob->bytes(at, size_t(frames));
                    auto pcm = std::make_shared<Pcm>();
                    pcm->rate = rate; pcm->channels = 1; pcm->data.resize(size_t(frames));
                    for (int i = 0; i < frames; i++) pcm->data[size_t(i)] = expand(raw[size_t(i)]);
                    return pcm;
                };
                r = refs.emplace(si, ref).first;
            }
            Zone z;
            z.name = "Sample " + std::to_string(si + 1);
            z.group = layer;
            z.sample = r->second;
            z.key_lo = key + KEY_OFFSET; z.key_hi = last + KEY_OFFSET;
            z.root = vr[12] + KEY_OFFSET;
            int pan = field(vr, 196, 4);
            if (pan != 0 && pan != 8) z.pan = clampd((8 - pan) / 7.0, -1, 1);
            z.gain_db = -field(vr, 214, 5) * 1.5;
            z.tune = sfield(vr, 47, 5) * 3.0 / 100.0;
            if (field(vr, 191, 1)) z.key_tracking = 0;
            z.amp_env = emax_env(vr, 0);
            z.amp_env.delay = tab(EMAX_VOICE_DELAY_TIME, field(vr, 208, 6));
            double vr_range = EMAX_VELOCITY_TO_LEVEL[clampi(field(vr, 68, 4), 0, 15)];
            if (vr_range > 0) z.amp_vel_depth = clampd(vr_range / MAX_VOLUME_DEPTH, 0, 1);
            else z.amp_vel_depth = 0;
            double rate = tab(EMAX_LFO_RATE, field(vr, 25, 7)), delay = tab(EMAX_LFO_DELAY_TIME, field(vr, 32, 6));
            int to_pitch = field(vr, 43, 4), to_vol = field(vr, 64, 4);
            if (to_pitch > 0) { z.pitch_lfo_depth = clampd(to_pitch * 13.0 / MAX_ENVELOPE_DEPTH, 0, 1); z.pitch_lfo.set = true; z.pitch_lfo.rate_hz = rate; z.pitch_lfo.delay = delay; }
            if (to_vol > 0) { z.amp_lfo_depth = clampd(to_vol * 1.6 / MAX_VOLUME_DEPTH, 0, 1); z.amp_lfo.set = true; z.amp_lfo.rate_hz = rate; z.amp_lfo.delay = delay; }
            int cutoff = field(vr, 128, 7), env_amt = sfield(vr, 144, 7), tracking = field(vr, 192, 4), res = field(vr, 136, 7), lfo_cut = field(vr, 204, 4);
            if (!(cutoff >= 120 && env_amt == 0 && tracking == 0 && res == 0 && lfo_cut == 0)) {
                Filter &f = z.filter;
                f.type = FilterType::LowPass;
                f.poles = 4;
                f.cutoff = std::min(MAX_CUTOFF_HZ, tab(EMAX_CUTOFF_FREQUENCY, cutoff));
                f.resonance = clampd(tab(EMAX_RESONANCE, res) / MAX_RESONANCE_DB, 0, 1);
                f.key_tracking = clampd(tracking * 5.9 / 100.0, 0, 1);
                if (env_amt) { f.env_depth = clampd(env_amt * 240.0 / MAX_ENVELOPE_DEPTH, -1, 1); f.env = emax_env(vr, 160); }
                int vc = field(vr, 52, 4);
                if (vc > 0) f.vel_depth = clampd(vc / 15.0, 0, 1);
                if (lfo_cut > 0) { f.lfo_depth = clampd(lfo_cut * 340.0 / MAX_ENVELOPE_DEPTH, 0, 1); f.lfo.set = true; f.lfo.rate_hz = rate; f.lfo.delay = delay; }
            }
            if ((s.flags & 1) && s.loop_end > s.loop_start && s.loop_start >= s.start && s.loop_end <= s.end) {
                Loop l; l.start = s.loop_start - s.start; l.end = s.loop_end - s.start;
                z.loops.push_back(l);
            }
            inst.zones.push_back(z);
        }
        key = last + 1;
    }
    finish_instrument(inst);
    return inst;
}

bool probe_emax(const std::string &name, const uint8_t *h, size_t n, uint64_t size) {
    std::string ext = path_ext(name);
    if (ext != "img") return true;   // the Emax extensions: the bank search decides
    return size == 819200 || size == 1638400 || size == 737280 || size == 1474560;
}

}  // namespace

void register_emu_emax() {
    register_reader({"E-mu Emax bank", "em1 eb1 em2 eb2 emx em1fd em2fd ez1 ez2 img", probe_emax, list_emax, load_emax});
}

}  // namespace omni

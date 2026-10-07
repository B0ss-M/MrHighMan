// Omni Sampler: E-mu Emulator II disks (.eii / .emuiifd raw images, .hfe through the floppy container) and bank
// files (.eii from Sound Designer / EMXP). Up to 100 voices and 100 presets over 61 keys; companded 8-bit audio at
// 27777 Hz. Translated from ConvertWithMoss's Emulator2Detector / Emulator2Constants / Emulator2VoiceSettings
// (LGPL-3.0).
#include <cmath>
#include <map>
#include "format.hpp"

namespace omni {

int16_t emu_expand(uint8_t value);

namespace {
#include "e2_tables.inc"

constexpr int TRACK = 3584, IMAGE_SIZE = 80 * 2 * TRACK, BANK_OFFSET = 22 * TRACK, BANK_ADDR = 0x9600;
constexpr int BANK_MEM = 0x80000 - BANK_ADDR, NUM_KEYS = 61, LOWEST = 36, UNITY = 0x10;
constexpr int KEY_MAP_VOICE_ID = 0x49, KEY_MAP_TRANSPOSE = 0x86, SELECTED_PRESET = 0x2AB, VOICE_LIST = 0x2CF, ID_BASE = 0x9B;
constexpr int VOICE_TABLE = 0x500, VOICE_SIZE = 0x100, MAX_VOICES = 100, RATE = 27777;
constexpr int PRESET_HEADER = 9, ENTRIES = 9 + 12 + 14, ENTRY = 5, ENTRY2 = 3;

template <size_t N> double lookup(const double (&t)[N][2], double key) {
    if (key <= t[0][0]) return t[0][1];
    for (size_t i = 1; i < N; i++)
        if (key <= t[i][0]) { double f = (key - t[i - 1][0]) / (t[i][0] - t[i - 1][0]); return t[i - 1][1] + f * (t[i][1] - t[i - 1][1]); }
    return t[N - 1][1];
}
double attack_time(int hi, int lo) { return lo == 0 ? 0 : 2.55 * hi / lo; }
double decay_time(int hi, int lo) { return lo == 0 ? (hi == 0xFF ? 100 : 0) : 2.55 * hi / (256 - lo); }
double vcf_time(double c) {
    if (c <= 0) return 0;
    const auto &t = E2_VCF_TIME;
    if (c <= t[0][0]) return t[0][1] * c / t[0][0];
    for (size_t i = 1; i < sizeof t / sizeof t[0]; i++)
        if (c <= t[i][0]) { double f = std::log(c / t[i - 1][0]) / std::log(t[i][0] / t[i - 1][0]); return t[i - 1][1] * std::pow(t[i][1] / t[i - 1][1], f); }
    return c;
}
int lfo_index(const uint8_t *v, int off) { int page = v[off + 1]; return page < 0x1C ? 0 : (page - 0x1C) * 4 + v[off]; }

void apply_voice(Zone &z, const uint8_t *v) {
    z.tune = int8_t(v[0x1A]) * (100.0 / 64) / 100.0;
    int level = v[0x40];
    z.gain_db = -lookup(E2_ATTENUATION_DB, level);
    z.amp_vel_depth = clampd(lookup(E2_VELOCITY_LEVEL_DB, level - double(v[0x40 + 15])) / 96.0, 0, 1);
    Envelope &a = z.amp_env;
    a.set = true;
    a.attack = attack_time(v[0x70], v[0x80]);
    a.decay = decay_time(v[0xA5], v[0xA7]);
    a.sustain = clampd(1 - lookup(E2_VCA_SUSTAIN_DB, v[0xA9]) / 100.0, 0, 1);
    a.release = decay_time(v[0xB3], v[0xB5]);
    a.time_vel_tracking = (v[0x1B] >> 4) / 15.0;
    double rate = 0.0669 * std::pow(2.0, v[0x1D] / 15.67), delay = std::max(0, v[0x1C] - 1) / 100.0;
    double lp = lookup(E2_LFO_PITCH_CENTS, lfo_index(v, 0x28));
    if (lp > 0) { z.pitch_lfo_depth = lp / MAX_ENVELOPE_DEPTH; z.pitch_lfo.set = true; z.pitch_lfo.rate_hz = rate; z.pitch_lfo.delay = delay; }
    double ll = lookup(E2_LFO_LEVEL_DB, lfo_index(v, 0x2C));
    if (ll > 0) { z.amp_lfo_depth = ll / MAX_VOLUME_DEPTH; z.amp_lfo.set = true; z.amp_lfo.rate_hz = rate; z.amp_lfo.delay = delay; }
    int cutoff = v[0x30];
    if (cutoff >= 0xF9) return;
    double vel_cut = -59.4 * (cutoff - v[0x30 + 15]);
    int amt = v[0x25];
    double env = amt == 4 ? 0 : lookup(E2_ENVELOPE_AMOUNT_CENTS, amt);
    if (v[0x2F] & 0x80) env = -env;
    double lf = lookup(E2_LFO_FILTER_CENTS, lfo_index(v, 0x2A));
    int q = v[0xF0];
    Filter &f = z.filter;
    f.type = FilterType::LowPass;
    f.poles = 4;
    f.cutoff = std::min(MAX_CUTOFF_HZ, lookup(E2_CUTOFF_HZ, cutoff));
    f.resonance = (q < 0x17 ? 0 : lookup(E2_RESONANCE_DB, q)) / MAX_RESONANCE_DB;
    f.key_tracking = std::min(1.0, (v[0x1F] >> 4) / 8.0);
    f.vel_depth = clampd(-vel_cut / 9600.0, -1, 1);   // the table drops the cutoff towards low velocities
    f.env_depth = env / MAX_ENVELOPE_DEPTH;
    if (env != 0) {
        f.env.set = true;
        f.env.attack = vcf_time(attack_time(v[0x50], v[0x60]));
        f.env.decay = vcf_time(decay_time(v[0x90], v[0x92]));
        f.env.sustain = lookup(E2_VCF_SUSTAIN_LEVEL, std::min<int>(v[0x2E], 0x1F));
        f.env.release = vcf_time(decay_time(v[0x9E], v[0xA0]));
        f.env.time_vel_tracking = (v[0x1B] & 15) / 15.0;
    }
    if (lf > 0) { f.lfo_depth = lf / MAX_ENVELOPE_DEPTH; f.lfo.set = true; f.lfo.rate_hz = rate; f.lfo.delay = delay; }
}

std::string e2_name(const std::vector<uint8_t> &d, size_t o, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && o + i < d.size(); i++) { int c = d[o + i]; s += c >= 0x80 ? '?' : (c < 0x20 || c == 0x7F ? ' ' : char(c)); }
    return trim(s);
}
int addr(const std::vector<uint8_t> &d, size_t o) { return o + 2 < d.size() ? int(le24(&d[o])) : -1; }
int word(const std::vector<uint8_t> &d, size_t o) { return o + 1 < d.size() ? le16(&d[o]) : -1; }

struct Voice { std::string name; int start = 0, frames = 0; bool loop = false; int loop_start = 0, loop_len = 0; size_t rec = 0; };
struct Range { int first = 0, keys = 0, voice = 0, transpose = 0, voice2 = 0, transpose2 = 0; };
struct Preset { std::string name; std::vector<Range> ranges; };

struct Bank {
    const std::vector<uint8_t> &d;
    int off, end;
    std::vector<Voice> voices;
    std::vector<Preset> presets;
    Bank(const std::vector<uint8_t> &data, int o) : d(data), off(o), end(int(std::min<size_t>(data.size(), size_t(o) + BANK_MEM))) { heap(); }

    bool is_voice(int p) const { return size_t(p + VOICE_SIZE) <= d.size() && d[size_t(p)] == 0x04 && d[size_t(p) + 1] == 0x03; }
    bool is_preset(int p, int len) const {
        if (size_t(p + ENTRIES) > d.size() || !(d[size_t(p + PRESET_HEADER - 1)] & 0x80)) return false;
        for (int i = 0; i < PRESET_HEADER - 1; i++) if (d[size_t(p + i)] > 0x0F) return false;
        int e = p + ENTRIES, stop = std::min(p + len, int(d.size())), keys = 0;
        while (e + ENTRY <= stop) {
            int first = d[size_t(e)], mode = first >> 6;
            if (mode == 0) break;
            int count = first & 0x3F;
            keys += count;
            if (!count || keys > NUM_KEYS) return false;
            e += ENTRY + (mode == 3 ? ENTRY2 : 0);
        }
        return true;
    }
    void heap() {
        int table = off + VOICE_TABLE, p = table;
        while (p + VOICE_SIZE <= end) {
            if (is_voice(p)) { if (int(voices.size()) >= MAX_VOICES) break; voice(p); p += VOICE_SIZE; continue; }
            int len = word(d, size_t(p));
            if (len <= ENTRIES || !is_preset(p + 2, len)) break;
            p = read_presets(p + 2, len);
            int slot = (p - table + VOICE_SIZE - 1) / VOICE_SIZE;
            p = table + slot * VOICE_SIZE;
        }
    }
    void voice(int r) {
        Voice v;
        v.rec = size_t(r);
        v.name = e2_name(d, size_t(r + 0xBA), 12);
        if (v.name.empty()) v.name = "Voice " + std::to_string(voices.size() + 1);
        int region = addr(d, size_t(r + 0xC7)), slot = addr(d, size_t(r + 0xCA)), play = addr(d, size_t(r + 0xD3));
        if (play < region || play >= region + slot) play = region;
        int ls = addr(d, size_t(r + 0xCD)) - play, ll = addr(d, size_t(r + 0xD0)), plen = addr(d, size_t(r + 0xD6));
        bool loop_on = (d[size_t(r + 0xC6)] & 4) && ll > 2;
        int pad = loop_on ? 4 : 2;
        long n = 0x7FFFFFFF;
        if (slot > 2) n = region + slot - 2 - play;
        if (plen > pad) n = std::min<long>(n, plen - pad);
        if (loop_on) n = std::min<long>(n, ls + ll);
        if (n == 0x7FFFFFFF) n = 0;
        int avail = end - off - play;
        if (n > avail) n = std::max(0, avail);
        v.start = play;
        v.frames = int(n);
        v.loop = loop_on && ls >= 0 && ls + 2 < v.frames;
        v.loop_start = ls;
        v.loop_len = std::min(ll, v.frames - ls);
        voices.push_back(v);
    }
    int read_presets(int start, int len) {
        int p = start;
        while (len > ENTRIES && p + len <= end && presets.size() < 100 && is_preset(p, len)) {
            Preset pr;
            pr.name = e2_name(d, size_t(p + PRESET_HEADER), 12);
            int e = p + ENTRIES, key = 0;
            while (e + ENTRY <= p + len && key < NUM_KEYS) {
                int first = d[size_t(e)], mode = first >> 6, count = first & 0x3F;
                if (mode == 0 || !count) break;
                Range rg;
                rg.first = key;
                rg.keys = std::min(count, NUM_KEYS - key);
                rg.voice = d[size_t(e + 2)];
                rg.transpose = d[size_t(e + 3)];
                e += ENTRY;
                if (mode == 3) {
                    if (e + ENTRY2 <= p + len) { rg.voice2 = d[size_t(e)]; rg.transpose2 = d[size_t(e + 1)]; }
                    e += ENTRY2;
                }
                if (rg.voice > 0 || rg.voice2 > 0) pr.ranges.push_back(rg);
                key += count;
            }
            presets.push_back(pr);
            int next = word(d, size_t(p + len - 2));
            p += len;
            len = next;
        }
        return p;
    }
    bool selected(Preset &pr) const {
        pr.name = e2_name(d, size_t(off + SELECTED_PRESET + PRESET_HEADER), 12);
        auto tr = [&](int k) { return int(d[size_t(off + KEY_MAP_TRANSPOSE + k)]); };
        for (int key = 0; key < NUM_KEYS;) {
            int id = d[size_t(off + KEY_MAP_VOICE_ID + key)];
            if (id < ID_BASE) { key++; continue; }
            int last = key;
            while (last + 1 < NUM_KEYS && d[size_t(off + KEY_MAP_VOICE_ID + last + 1)] == id && tr(last + 1) == tr(last) + 1) last++;
            Range rg;
            rg.first = key; rg.keys = last - key + 1; rg.voice = -(id - ID_BASE + 1); rg.transpose = tr(key);
            pr.ranges.push_back(rg);
            key = last + 1;
        }
        return !pr.ranges.empty();
    }
    const Voice *get(int number) const {
        if (!number) return nullptr;
        int index;
        if (number < 0) index = -number - 1;
        else {
            size_t le = size_t(off + VOICE_LIST + number - 1);
            int id = number <= 100 && le < d.size() ? d[le] : 0;
            index = id >= ID_BASE ? id - ID_BASE : number - 1;
        }
        return index >= 0 && index < int(voices.size()) ? &voices[size_t(index)] : nullptr;
    }
};

int find_bank_offset(const std::vector<uint8_t> &d) {
    std::map<int, int> votes;
    int first_page = (BANK_ADDR + VOICE_TABLE) >> 8;
    for (size_t p = 0; p < 0x2000 && p + VOICE_SIZE <= d.size(); p++) {
        if (d[p] != 0x04 || d[p + 1] != 0x03) continue;
        int page = d[p + 0x23];
        if (page < first_page || page != d[p + 0x27]) continue;
        long o = long(p) - (VOICE_TABLE + long(page - first_page) * VOICE_SIZE);
        if (o >= 0) votes[int(o)]++;
    }
    int best = 0, n = 0;
    for (auto &v : votes) if (v.second > n) { n = v.second; best = v.first; }
    return best;
}

struct Loaded { std::vector<uint8_t> d; int off; };
Loaded read(VolumePtr vol, const std::string &path) {
    Loaded l;
    l.d = vol->open(path)->all(8 << 20);
    if (l.d.size() == size_t(IMAGE_SIZE)) l.off = BANK_OFFSET;
    else if (l.d.size() >= size_t(VOICE_TABLE + VOICE_SIZE) && l.d.size() <= size_t(IMAGE_SIZE)) l.off = find_bank_offset(l.d);
    else throw ParseError("not an Emulator II disk or bank");
    return l;
}

std::vector<PresetInfo> list_e2(VolumePtr vol, const std::string &path) {
    Loaded l = read(vol, path);
    Bank b(l.d, l.off);
    if (b.voices.empty()) throw ParseError("no voices in this Emulator II bank");
    std::vector<PresetInfo> out;
    for (size_t i = 0; i < b.presets.size(); i++) out.push_back({b.presets[i].name.empty() ? "Preset " + std::to_string(i + 1) : b.presets[i].name, int(i)});
    if (out.empty()) out.push_back({"Selected preset", -1});
    return out;
}

Instrument load_e2(VolumePtr vol, const std::string &path, int index) {
    BlobPtr blob = vol->open(path);
    Loaded l = read(vol, path);
    Bank b(l.d, l.off);
    Preset pr;
    if (index >= 0 && size_t(index) < b.presets.size()) pr = b.presets[size_t(index)];
    else if (!b.selected(pr)) throw ParseError("no presets in this Emulator II bank");
    Instrument inst;
    inst.format = "E-mu Emulator II bank";
    inst.name = pr.name.empty() ? path_stem(path) : pr.name;
    inst.groups = {Group{"Layer 1"}, Group{"Layer 2"}};
    std::map<const Voice *, SampleRefPtr> refs;
    int off = l.off;
    for (int layer = 0; layer < 2; layer++)
        for (auto &rg : pr.ranges) {
            const Voice *v = b.get(layer == 0 ? rg.voice : rg.voice2);
            if (!v || v->start <= 0 || v->frames <= 0 || size_t(off + v->start + v->frames) > l.d.size()) continue;
            auto it = refs.find(v);
            if (it == refs.end()) {
                auto ref = std::make_shared<SampleRef>();
                ref->name = v->name; ref->rate = RATE; ref->channels = 1; ref->frames = v->frames;
                ref->key = "e2:" + std::to_string(reinterpret_cast<uintptr_t>(blob.get())) + ":" + std::to_string(v->start);
                uint64_t at = uint64_t(off + v->start);
                int frames = v->frames;
                ref->decode = [blob, at, frames]() {
                    std::vector<uint8_t> raw = blob->bytes(at, size_t(frames));
                    auto p = std::make_shared<Pcm>();
                    p->rate = RATE; p->channels = 1; p->data.resize(size_t(frames));
                    for (int i = 0; i < frames; i++) p->data[size_t(i)] = emu_expand(raw[size_t(i)]);
                    return PcmPtr(p);
                };
                it = refs.emplace(v, ref).first;
            }
            Zone z;
            z.name = v->name;
            z.group = layer;
            z.sample = it->second;
            z.key_lo = LOWEST + rg.first; z.key_hi = LOWEST + rg.first + rg.keys - 1;
            z.root = LOWEST + rg.first + UNITY - (layer == 0 ? rg.transpose : rg.transpose2);
            apply_voice(z, &l.d[v->rec]);
            if (v->loop) { Loop lp; lp.start = v->loop_start; lp.end = v->loop_start + v->loop_len - 1; z.loops.push_back(lp); }
            inst.zones.push_back(z);
        }
    finish_instrument(inst);
    return inst;
}

bool probe_e2(const std::string &name, const uint8_t *, size_t, uint64_t size) {
    std::string ext = path_ext(name);
    if (ext == "eii") return size <= uint64_t(IMAGE_SIZE);
    return size == uint64_t(IMAGE_SIZE);
}

}  // namespace

void register_emu_e2() {
    register_reader({"E-mu Emulator II bank", "eii emuiifd img", probe_e2, list_e2, load_e2});
}

}  // namespace omni

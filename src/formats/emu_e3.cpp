// Omni Sampler: E-mu Emulator III / IIIX / ESI-32 / ESI-2000 / ESI-4000 banks (.e3b .e3x .esi, and inside E-mu
// disk images). Translated from ConvertWithMoss's Emulator3Detector, Emulator3Constants, Emulator3BankFormat and
// Emulator3SampleIndexRepair (LGPL-3.0; the format was reverse-engineered by the emu3bm project). Each preset
// becomes an instrument: 88 keys map to note zones with a primary and a secondary layer; linked presets add
// their layers. Floppy-disk bank sets are not read.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include "format.hpp"

namespace omni {
namespace {
#include "emu3_tables.inc"

enum class E3Fmt { E3X, ESI, E3 };
struct Fmt {
    E3Fmt f;
    bool compact() const { return f == E3Fmt::E3; }
    int preset_table() const { return compact() ? 0x6C : 0x17CA; }
    int sample_table() const { return compact() ? 0x204 : 0x1BD2; }
    int preset_area() const { return compact() ? 0x74A : 0x2B72; }
    int preset_bias() const { return compact() ? 0x1A6FE : 0; }
    int max_presets() const { return compact() ? 100 : 256; }
    int max_samples() const { return compact() ? 99 : 999; }
    int index_mask() const { return compact() ? 0xFF : 0x3FFF; }
};

bool get_format(const std::vector<uint8_t> &d, Fmt &out) {
    if (d.size() < 16 || d[15] != 0) return false;
    std::string id(reinterpret_cast<const char *>(d.data()), 15);
    if (id == "EMULATOR 3X    ") out.f = E3Fmt::E3X;
    else if (id == "EMU SI-32 v3   ") out.f = E3Fmt::ESI;
    else if (id == "EMULATOR THREE ") out.f = E3Fmt::E3;
    else return false;
    return true;
}

constexpr int PRESET_SIZE = 142, NOTE_ZONE = 4, ZONE_SIZE = 48, SAMPLE_HEADER = 92, NUM_KEYS = 88, KEY_OFFSET = 21;
constexpr uint32_t SAMPLE_ADDRESS_OFFSET = 0x400000;

uint32_t u32(const std::vector<uint8_t> &d, size_t o) { return o + 4 <= d.size() ? le32(&d[o]) : 0; }
int u16(const std::vector<uint8_t> &d, size_t o) { return o + 2 <= d.size() ? le16(&d[o]) : 0; }
std::string e3_name(const std::vector<uint8_t> &d, size_t o) {
    std::string s;
    for (size_t i = 0; i < 16 && o + i < d.size(); i++) s += (d[o + i] >= 32 && d[o + i] < 127) ? char(d[o + i]) : ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

struct E3Sample {
    std::string name;
    uint64_t data_off = 0, right_off = 0;
    int frames = 0, rate = 44100;
    bool stereo = false, loop = false, alt = false, in_release = false, reverse = false;
    int loop_start = 0, loop_end = 0;
};

bool parse_sample(const std::vector<uint8_t> &d, int64_t addr, E3Sample &s) {
    if (addr < 0 || size_t(addr) + SAMPLE_HEADER > d.size()) return false;
    size_t o = size_t(addr);
    int opt = u16(d, o + 0x3A);
    s.stereo = (opt & 0x60) == 0x60;
    bool left = opt & 0x20;
    int64_t start = u32(d, o + (left ? 0x14 : 0x18)), end = u32(d, o + (left ? 0x1C : 0x20));
    int64_t start_r = u32(d, o + 0x18), end_r = u32(d, o + 0x20);
    int64_t frames = (end + 2 - start) / 2;
    if (s.stereo) frames = std::min(frames, (end_r + 2 - start_r) / 2);
    int rate = int(u32(d, o + 0x34));
    bool bad_r = s.stereo && (start_r < SAMPLE_HEADER || o + end_r + 2 > d.size());
    if (frames <= 0 || rate <= 0 || start < SAMPLE_HEADER || o + end + 2 > d.size() || bad_r) return false;
    s.name = e3_name(d, o);
    s.data_off = o + uint64_t(start);
    s.right_off = o + uint64_t(start_r);
    s.frames = int(frames);
    s.rate = rate;
    int lt = opt & 3;
    if (lt) {
        s.alt = lt >= 2;
        s.loop_start = int((int64_t(u32(d, o + (left ? 0x24 : 0x28))) - start) / 2);
        s.loop_end = int((int64_t(u32(d, o + (left ? 0x2C : 0x30))) - start) / 2);
        s.loop = s.loop_start >= 0 && s.loop_end > s.loop_start && s.loop_start < s.frames;
        s.loop_end = std::min(s.loop_end, s.frames - 1);
        s.in_release = opt & 8;
    }
    s.reverse = opt & 4;
    return true;
}

bool preset_present(const std::vector<uint8_t> &d, const Fmt &f, int i) {
    return u32(d, size_t(f.preset_table() + i * 4)) != u32(d, size_t(f.preset_table() + (i + 1) * 4));
}
int64_t preset_offset(const std::vector<uint8_t> &d, const Fmt &f, int i) {
    int64_t a = int64_t(f.preset_area()) + int64_t(u32(d, size_t(f.preset_table() + i * 4))) - f.preset_bias();
    return a < 0 || size_t(a) + PRESET_SIZE > d.size() ? -1 : a;
}

// --- Emulator3SampleIndexRepair -------------------------------------------------------------------------------

struct Note { int pc, octave; bool acc; };

bool parse_note(const std::string &name0, Note &n) {
    std::string name = name0;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    // upper: ([A-G])\s?([#bx])?\s?(-?\d)\s*$   lower: ([a-g])([#bx])?(-?\d)$
    for (int pass = 0; pass < 2; pass++) {
        size_t i = name.size();
        if (!i || !isdigit((unsigned char)name[i - 1])) return false;
        int digit = name[i - 1] - '0';
        i--;
        bool neg = i > 0 && name[i - 1] == '-';
        if (neg) i--;
        if (pass == 0 && i > 0 && name[i - 1] == ' ') i--;
        char acc = 0;
        if (i > 0 && (name[i - 1] == '#' || name[i - 1] == 'b' || name[i - 1] == 'x')) {
            // 'b' is an accidental only if a note letter precedes it
            if (i > 1 && ((pass == 0 && name[i - 2] >= 'A' && name[i - 2] <= 'G') || (pass == 0 && name[i - 2] == ' ') ||
                          (pass == 1 && name[i - 2] >= 'a' && name[i - 2] <= 'g'))) { acc = name[i - 1]; i--; }
        }
        if (pass == 0 && acc && i > 0 && name[i - 1] == ' ') i--;
        if (i == 0) continue;
        char letter = name[i - 1];
        bool ok = pass == 0 ? (letter >= 'A' && letter <= 'G') : (letter >= 'a' && letter <= 'g');
        if (!ok) continue;
        static const int PC[7] = {9, 11, 0, 2, 4, 5, 7};
        int pc = PC[toupper(letter) - 'A'];
        if (acc) pc = acc == 'b' ? pc - 1 : pc + 1;
        n.pc = (pc + 12) % 12;
        n.octave = neg ? -digit : digit;
        n.acc = acc != 0;
        return true;
    }
    return false;
}

bool note_matches(int key, const Note &n) {
    int kpc = key % 12;
    if (n.pc != kpc && (n.acc || (n.pc + 1) % 12 != kpc)) return false;
    for (int base = 1; base <= 2; base++)
        if (std::abs(n.pc + (n.octave + base) * 12 - key) <= 13) return true;
    return false;
}

std::string normalize(const std::string &t) {
    std::string o;
    for (char c : t) if (isalnum((unsigned char)c)) o += char(tolower((unsigned char)c));
    return o;
}

double affinity(const std::string &preset, const std::vector<std::string> &targets) {
    if (targets.empty()) return 0;
    static const std::set<std::string> generic = {"loop", "wave", "the", "and", "link", "new", "old", "big", "low", "high"};
    std::set<std::string> tokens;
    std::string p = preset;
    for (char &c : p) if (c == ':' || c == '/' || c == '-') c = ' ';
    size_t a = 0;
    while (a <= p.size()) {
        size_t b = p.find(' ', a);
        if (b == std::string::npos) b = p.size();
        std::string t = normalize(p.substr(a, b - a));
        bool digits = !t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return isdigit((unsigned char)c); });
        if (t.size() >= 3 && !generic.count(t) && !digits) tokens.insert(t);
        a = b + 1;
    }
    if (tokens.empty()) return 0;
    int matched = 0;
    for (auto &n : targets) {
        std::string nn = normalize(n);
        for (auto &t : tokens) if (nn.find(t) != std::string::npos) { matched++; break; }
    }
    return matched / double(targets.size());
}

std::map<int, int> resolve_preset(const std::vector<uint8_t> &d, size_t po, const std::map<int, std::string> &names,
                                  const std::map<int, Note> &notes, int max_slot, int mask) {
    std::set<std::pair<int, int>> pairs;
    int nnz = d[po + 0x35];
    size_t nzo = po + PRESET_SIZE, zo = nzo + size_t(nnz) * NOTE_ZONE;
    int max_zone = -1;
    for (int i = 0; i < nnz; i++) {
        size_t nz = nzo + size_t(i) * NOTE_ZONE;
        if (nz + NOTE_ZONE > d.size()) break;
        for (int f : {2, 3}) if (d[nz + size_t(f)] != 0xFF) max_zone = std::max(max_zone, int(d[nz + size_t(f)]));
    }
    for (int z = 0; z <= max_zone; z++) {
        size_t zz = zo + size_t(z) * ZONE_SIZE;
        if (zz + ZONE_SIZE > d.size()) break;
        int st = u16(d, zz + 1) & mask;
        if (st) pairs.insert({d[zz] + KEY_OFFSET, st});
    }
    if (pairs.empty()) return {};
    std::vector<int> roots, stored, lows;
    bool as_is = true;
    for (auto &p : pairs) {
        roots.push_back(p.first); stored.push_back(p.second); lows.push_back((p.second - 1) % 256 + 1);
        if (!names.count(p.second)) as_is = false;
    }
    struct Cand { bool as_is; int page; };
    std::vector<Cand> cands;
    if (as_is) cands.push_back({true, 0});
    int max_page = (max_slot - 1) / 256;
    for (int page = 0; page <= max_page; page++) {
        bool feas = true, ident = as_is;
        for (size_t i = 0; i < lows.size(); i++) {
            int slot = lows[i] + page * 256;
            if (!names.count(slot)) feas = false;
            if (slot != stored[i]) ident = false;
        }
        if (feas && !ident) cands.push_back({false, page});
    }
    std::map<int, int> mapping;
    if (cands.empty()) {
        for (size_t i = 0; i < stored.size(); i++) {
            if (names.count(stored[i])) continue;
            for (int page = 0; page <= max_page; page++)
                if (names.count(lows[i] + page * 256)) { mapping[stored[i]] = lows[i] + page * 256; break; }
        }
        return mapping;
    }
    std::string pname = e3_name(d, po);
    struct Score { int hits, parseable, distinct; double aff; };
    std::vector<Score> sc;
    for (auto &c : cands) {
        Score s{0, 0, 0, 0};
        std::set<int> hit_roots;
        std::vector<std::string> targets;
        for (size_t i = 0; i < roots.size(); i++) {
            int slot = c.as_is ? stored[i] : lows[i] + c.page * 256;
            auto n = names.find(slot);
            if (n != names.end()) targets.push_back(n->second);
            auto nt = notes.find(slot);
            if (nt == notes.end()) continue;
            s.parseable++;
            if (note_matches(roots[i], nt->second)) { s.hits++; hit_roots.insert(roots[i]); }
        }
        s.distinct = int(hit_roots.size());
        s.aff = affinity(pname, targets);
        sc.push_back(s);
    }
    const Score &base = sc[0];
    size_t best = 0;
    for (size_t i = 1; i < sc.size(); i++) if (sc[i].hits > sc[best].hits) best = i;
    if (sc[best].hits < 3 && base.hits == 0 && base.parseable >= 2) {
        size_t aff = 0;
        bool unique = true;
        for (size_t i = 1; i < sc.size(); i++) {
            if (sc[i].aff > sc[aff].aff) { aff = i; unique = true; }
            else if (i != aff && sc[i].aff == sc[aff].aff) unique = false;
        }
        if (sc[aff].aff >= 0.5 && (unique || cands.size() == 1)) best = aff;
    }
    if (best == 0) return {};
    const Score &b = sc[best];
    if (base.aff > b.aff) return {};
    bool decisive = b.hits >= 3 && (b.distinct >= 3 || b.aff > base.aff) && b.hits >= base.hits + 2 &&
                    b.hits * 10 >= 6 * std::max(b.parseable, 1) && b.hits >= 2 * std::max(base.hits, 1);
    bool perfect_small = roots.size() <= 2 && b.hits == int(roots.size()) && b.parseable == int(roots.size()) && base.hits == 0 && base.parseable >= 1;
    bool aff_decisive = base.hits == 0 && base.parseable >= 2 && b.aff >= 0.5 && base.aff == 0;
    if (!(decisive || perfect_small || aff_decisive)) return {};
    const Cand &w = cands[best];
    if (w.as_is) return {};
    for (size_t i = 0; i < roots.size(); i++) {
        int slot = lows[i] + w.page * 256;
        if (slot != stored[i]) mapping[stored[i]] = slot;
    }
    return mapping;
}

// --- bank -------------------------------------------------------------------------------------------------------

struct E3Bank {
    std::vector<uint8_t> d;
    Fmt fmt;
    std::map<int, E3Sample> samples;
    std::set<int> empty_slots, linked;
    std::map<size_t, std::map<int, int>> repairs;
    std::vector<int> presets;   // slots of the top-level presets
};

E3Bank parse_bank(BlobPtr blob) {
    E3Bank b;
    b.d = blob->all(1u << 30);
    if (!get_format(b.d, b.fmt)) throw ParseError("not an Emulator III bank");
    const auto &d = b.d;
    const Fmt &f = b.fmt;
    if (size_t(f.sample_table() + (f.max_samples() + 1) * 4) > d.size()) throw ParseError("truncated Emulator III bank");
    int64_t preset_area = int64_t(u32(d, size_t(f.preset_table() + f.max_presets() * 4))) - f.preset_bias();
    int64_t sample_area = f.preset_area() + 1 + preset_area;
    for (int i = 0; i < f.max_samples(); i++) {
        uint32_t e = u32(d, size_t(f.sample_table() + i * 4));
        if (!e) { b.empty_slots.insert(i + 1); continue; }
        E3Sample s;
        if (parse_sample(d, sample_area + int64_t(e) - SAMPLE_ADDRESS_OFFSET, s)) {
            if (trim(s.name).empty()) s.name = "Sample " + std::to_string(i + 1);
            b.samples[i + 1] = s;
        }
    }
    std::vector<size_t> offsets;
    for (int i = 0; i < f.max_presets(); i++) {
        if (!preset_present(d, f, i)) continue;
        int64_t po = preset_offset(d, f, i);
        if (po < 0) continue;
        offsets.push_back(size_t(po));
        int link = d[size_t(po) + 0x31];
        if (link > 0 && link - 1 < f.max_presets() && link - 1 != i) b.linked.insert(link - 1);
    }
    std::map<int, std::string> names;
    std::map<int, Note> notes;
    int max_slot = 0;
    for (auto &s : b.samples) {
        names[s.first] = s.second.name;
        max_slot = std::max(max_slot, s.first);
        Note n;
        if (parse_note(s.second.name, n)) notes[s.first] = n;
    }
    if (!names.empty())
        for (size_t po : offsets) {
            auto m = resolve_preset(d, po, names, notes, max_slot, f.index_mask());
            if (!m.empty()) b.repairs[po] = m;
        }
    for (int i = 0; i < f.max_presets(); i++)
        if (preset_present(d, f, i) && !b.linked.count(i) && preset_offset(d, f, i) >= 0) b.presets.push_back(i);
    return b;
}

Envelope e3_env(const std::vector<uint8_t> &d, size_t o) {
    Envelope e;
    e.set = true;
    e.attack = E3_ENVELOPE_TIME[clampi(d[o], 0, 127)];
    e.hold = E3_ENVELOPE_TIME[clampi(d[o + 1], 0, 127)];
    e.decay = E3_ENVELOPE_TIME[clampi(d[o + 2], 0, 127)];
    e.sustain = clampd(int8_t(d[o + 3]) / 127.0, 0, 1);
    e.release = E3_ENVELOPE_TIME[clampi(d[o + 4], 0, 127)];
    return e;
}

void e3_lfo(Lfo &l, const std::vector<uint8_t> &d, size_t z) {
    l.set = true;
    int shape = d[z + 45] & 3;
    l.wave = shape == 1 ? LfoWave::Sine : shape == 2 ? LfoWave::SawUp : shape == 3 ? LfoWave::Square : LfoWave::Triangle;
    l.rate_hz = E3_LFO_RATE[clampi(d[z + 9], 0, 127)];
    double delay = E3_TIME_21_69[clampi(d[z + 10], 0, 127)];
    if (delay > 0) l.delay = delay;
}

SampleRefPtr sample_ref(BlobPtr blob, const E3Bank &b, int index, int channels_sel) {
    const E3Sample &s = b.samples.at(index);
    auto r = std::make_shared<SampleRef>();
    r->name = s.name;
    r->rate = s.rate;
    r->frames = s.frames;
    r->channels = channels_sel == 0 ? 2 : 1;
    r->key = "e3:" + std::to_string(reinterpret_cast<uintptr_t>(blob.get())) + ":" + std::to_string(index) + ":" + std::to_string(channels_sel);
    uint64_t lo = s.data_off, ro = s.right_off;
    int frames = s.frames, rate = s.rate;
    r->decode = [blob, lo, ro, frames, rate, channels_sel]() -> PcmPtr {
        if (channels_sel == 0) {
            std::vector<uint8_t> l = blob->bytes(lo, size_t(frames) * 2), rr = blob->bytes(ro, size_t(frames) * 2);
            auto p = std::make_shared<Pcm>();
            p->rate = rate; p->channels = 2; p->data.resize(size_t(frames) * 2);
            for (int i = 0; i < frames; i++) { p->data[2 * size_t(i)] = int16_t(le16(&l[2 * size_t(i)])); p->data[2 * size_t(i) + 1] = int16_t(le16(&rr[2 * size_t(i)])); }
            return p;
        }
        std::vector<uint8_t> c = blob->bytes(channels_sel == 2 ? ro : lo, size_t(frames) * 2);
        return pcm_from_raw(c.data(), frames, 1, Enc::S16LE, rate);
    };
    return r;
}

void parse_layers(BlobPtr blob, const E3Bank &b, size_t po, Instrument &inst, std::map<std::string, SampleRefPtr> &refs,
                  std::set<int> &missing) {
    const auto &d = b.d;
    int nnz = d[po + 0x35];
    if (!nnz) return;
    size_t nzo = po + PRESET_SIZE, zo = nzo + size_t(nnz) * NOTE_ZONE;
    if (zo > d.size()) return;
    auto rep = b.repairs.find(po);
    std::vector<Zone> layer_zones[2];
    for (int layer = 0; layer < 2; layer++) {
        int field = layer == 0 ? 2 : 3;
        for (int nzi = 0; nzi < nnz; nzi++) {
            size_t nz = nzo + size_t(nzi) * NOTE_ZONE;
            if (nz + NOTE_ZONE > d.size()) break;
            int zi = d[nz + size_t(field)];
            if (zi == 0xFF) continue;
            int klo = -1, khi = -1;
            for (int k = 0; k < NUM_KEYS; k++)
                if (d[po + 0x36 + size_t(k)] == nzi) { if (klo < 0) klo = k; khi = k; }
            if (klo < 0) continue;
            size_t z = zo + size_t(zi) * ZONE_SIZE;
            if (z + ZONE_SIZE > d.size()) continue;
            int stored = u16(d, z + 1) & b.fmt.index_mask();
            if (!stored) continue;
            int si = stored;
            if (rep != b.repairs.end()) { auto it = rep->second.find(stored); if (it != rep->second.end()) si = it->second; }
            auto sit = b.samples.find(si);
            if (sit == b.samples.end()) { if (!b.empty_slots.count(si)) missing.insert(si); continue; }
            const E3Sample &s = sit->second;
            int flags = d[z + 47];
            bool dl = flags & 0x40, dr = flags & 0x80;
            int sel = !s.stereo ? 1 : (!dl && !dr) ? 0 : dl ? 2 : 1;
            std::string key = std::to_string(si) + ":" + std::to_string(sel);
            if (!refs.count(key)) refs[key] = sample_ref(blob, b, si, sel);
            Zone zn;
            zn.name = s.name;
            zn.sample = refs[key];
            zn.key_lo = klo + KEY_OFFSET; zn.key_hi = khi + KEY_OFFSET;
            zn.root = d[z] + KEY_OFFSET;
            zn.stop = s.frames;
            zn.tune = int8_t(d[z + 41]) * 1.5625 / 100.0;
            int level = int8_t(d[z + 40]);
            zn.gain_db = level <= 0 ? -96 : 20.0 * std::log10(level / 127.0);
            zn.pan = E3_PANORAMA[clampi(d[z + 44], 0, 127)] / 100.0;
            if (flags & 0x02) zn.key_tracking = 0;
            if (s.loop && !(flags & 0x20)) {
                Loop l;
                l.type = s.alt ? LoopType::Alternating : LoopType::Forward;
                l.start = s.loop_start; l.end = s.loop_end;
                l.until_release = !s.in_release;
                zn.loops.push_back(l);
            }
            zn.reverse = s.reverse;
            zn.amp_env = e3_env(d, z + 4);
            zn.amp_vel_depth = clampd(int8_t(d[z + 28]) / 127.0, 0, 1);
            if (d[z + 26] == 1) {
                double depth = int8_t(d[z + 25]) / 127.0;
                if (depth != 0) { zn.pitch_env = e3_env(d, z + 20); zn.pitch_env_depth = clampd(depth, -1, 1); }
            }
            // filter
            int fts = d[z + 45];
            int fidx = b.fmt.f == E3Fmt::ESI ? fts >> 3 : 1;
            FilterType ft = fidx < 8 ? E3_ESI_FILTER_TYPES[fidx] : FilterType::None;
            bool has_filter = false;
            if (ft != FilterType::None) {
                int cut = d[z + 12], q = d[z + 13] & 0x7F;
                double env_depth = clampd(int8_t(d[z + 14]) / 127.0, -1, 1);
                double kt = clampd(int8_t(d[z + 42]) / 127.0 * 2.0, 0, 1);
                double vd = clampd(int8_t(d[z + 32]) / 127.0, -1, 1);
                double hz = E3_CUTOFF_FREQUENCY[clampi(cut, 0, 255)];
                double lowest = std::min(hz, hz * std::pow(2.0, kt * (zn.key_lo - zn.root) / 12.0));
                if (!(ft == FilterType::LowPass && env_depth >= 0 && vd >= 0 && lowest >= 20000.0)) {
                    has_filter = true;
                    zn.filter.type = ft;
                    zn.filter.poles = fidx < 8 ? E3_ESI_FILTER_POLES[fidx] : 4;
                    zn.filter.cutoff = std::min(hz, MAX_CUTOFF_HZ);
                    zn.filter.resonance = q / 127.0;
                    zn.filter.key_tracking = kt;
                    zn.filter.vel_depth = vd;
                    if (env_depth != 0) { zn.filter.env_depth = env_depth; zn.filter.env = e3_env(d, z + 15); }
                }
            }
            int l2p = int8_t(d[z + 36]), l2a = int8_t(d[z + 37]), l2c = int8_t(d[z + 38]);
            if (l2p) { e3_lfo(zn.pitch_lfo, d, z); zn.pitch_lfo_depth = clampd(l2p / 127.0, -1, 1) * 100.0 / MAX_ENVELOPE_DEPTH; }
            if (l2a) { e3_lfo(zn.amp_lfo, d, z); zn.amp_lfo_depth = clampd(l2a / 127.0, -1, 1) * 24.0 / MAX_VOLUME_DEPTH; }
            if (l2c && has_filter) { e3_lfo(zn.filter.lfo, d, z); zn.filter.lfo_depth = clampd(l2c / 127.0, -1, 1) * 5100.0 / MAX_ENVELOPE_DEPTH; }
            int bend = int8_t(d[po + 0x2C]);
            if (bend > 0) { zn.bend_up = bend * 100; zn.bend_down = -bend * 100; }
            layer_zones[layer].push_back(zn);
            if (flags & 0x08) {   // chorus: a second voice detuned by +-7 cents
                Zone ch = zn;
                layer_zones[layer].back().tune -= 0.07;
                ch.tune += 0.07;
                layer_zones[layer].push_back(ch);
            }
        }
    }
    bool both = !layer_zones[0].empty() && !layer_zones[1].empty();
    for (int layer = 0; layer < 2; layer++) {
        if (layer_zones[layer].empty()) continue;
        int group = int(inst.groups.size());
        inst.groups.push_back(Group{"Layer " + std::to_string(group + 1)});
        int lo = d[po + size_t(layer == 0 ? 0x2D : 0x2F)], hi = d[po + size_t(layer == 0 ? 0x2E : 0x30)];
        for (auto &z : layer_zones[layer]) {
            z.group = group;
            if (both && hi != 0 && hi <= 127 && lo <= hi) { z.vel_lo = std::max(1, lo); z.vel_hi = hi; }
            inst.zones.push_back(z);
        }
    }
}

std::vector<PresetInfo> list_e3(VolumePtr vol, const std::string &path) {
    E3Bank b = parse_bank(vol->open(path));
    std::vector<PresetInfo> out;
    for (int i : b.presets) {
        std::string n = e3_name(b.d, size_t(preset_offset(b.d, b.fmt, i)));
        char num[8];
        std::snprintf(num, sizeof num, "%03d ", i);
        out.push_back({std::string(num) + (trim(n).empty() ? "Preset" : trim(n)), i});
    }
    if (out.empty()) throw ParseError("no presets in this bank");
    return out;
}

Instrument load_e3(VolumePtr vol, const std::string &path, int index) {
    BlobPtr blob = vol->open(path);   // the samples decode from the file; only the parse holds the bank in memory
    E3Bank b = parse_bank(blob);
    Instrument inst;
    inst.format = b.fmt.f == E3Fmt::ESI ? "E-mu ESI bank" : b.fmt.f == E3Fmt::E3X ? "E-mu Emulator IIIX bank" : "E-mu Emulator III bank";
    inst.groups.clear();
    int64_t po0 = preset_offset(b.d, b.fmt, index);
    if (po0 < 0) throw ParseError("no such preset");
    inst.name = trim(e3_name(b.d, size_t(po0)));
    if (inst.name.empty()) inst.name = path_stem(path);
    std::map<std::string, SampleRefPtr> refs;
    std::set<int> missing, visited;
    int cur = index;
    while (cur >= 0 && visited.insert(cur).second) {
        if (!preset_present(b.d, b.fmt, cur)) break;
        int64_t po = preset_offset(b.d, b.fmt, cur);
        if (po < 0) break;
        parse_layers(blob, b, size_t(po), inst, refs, missing);
        int link = b.d[size_t(po) + 0x31];
        cur = link > 0 && link - 1 < b.fmt.max_presets() ? link - 1 : -1;
    }
    for (int m : missing) inst.warnings.push_back("missing sample #" + std::to_string(m));
    finish_instrument(inst);
    return inst;
}

bool probe_e3(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    if (n < 16 || h[15] != 0) return false;
    return !std::memcmp(h, "EMULATOR 3X    ", 15) || !std::memcmp(h, "EMU SI-32 v3   ", 15) || !std::memcmp(h, "EMULATOR THREE ", 15);
}

}  // namespace

void register_emu_e3() {
    register_reader({"E-mu Emulator III bank", "e3b e3x esi eiii", probe_e3, list_e3, load_e3});
}

}  // namespace omni

// Omni Sampler: the Alesis HR-16, HR-16:B and SR-16 drum machines, played from their sound ROM and OS dumps (MAME's
// hr16b and sr16 sets, or the same chips under any name: they are known by the CRC-32 of their first 4 KB and size).
// Tapping a chip lists the machine's kit, then its voices one by one. Decoded 2026-10-21:
//   - Sound ROMs (MAME alesis_a.cpp, the DM3AG): two 512 KB chips (HR-16 U16 then U15, SR-16 U6 then U5) are one 1 MB
//     space of signed 8-bit samples at 48 kHz. A sound starts at full scale (the sample << 8) and each single 0x80 in
//     it halves the gain from there on (one step, at most 8); three 0x80 in a row end it.
//   - HR-16:B OS v2.00 (its address lines 0-7 wired in reverse): 49 u16 big-endian sound starts (address / 16) at
//     0x6BFD, then the 49 voice names, 16 characters each (MONSTER KICK ... JAM BLOCK), in the same order.
//   - SR-16 OS v1.04: the voices from 0xCD20, one after another: name (8 characters), a byte (bit 7: plays one of its
//     layers at random, bits 0-6 the layer count), then 8 bytes a layer: type (0 / 1 always, 2 / 3 / 4 the soft /
//     medium / loud hit), level (signed), start (u16 big-endian, address / 16: velocity layers start later in the same
//     sample), pitch (12 = as sampled), output / pan (0 centre, 8 / 16 the left / right of a stereo room pair ...),
//     u16. A few voices are followed by a stray layer, skipped. 234 voices from 68 samples.
//     The level and pitch steps are estimates (0.25 dB, a quarter tone: Hi / Mid / Lo toms sit 8 and 6 steps apart).
//   - The HR-16 sound ROM pair dumped here (U16 DEFA9D0D / U15 39F48DFB) is not MAME's and matches no table in the
//     v1.09 OS beside it: its 50 sounds are listed by number.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include "../third_party/tinf/tinf.h"
#include "format.hpp"

namespace omni {
namespace {

constexpr int RATE = 48000;
constexpr uint32_t SOUND_CHIP = 0x80000;

enum Part { HR16_U16, HR16_U15, HR16B_U16, HR16B_U15, HR16B_OS, SR16_U6, SR16_U5, SR16_OS, N_PARTS };
struct PartInfo { uint32_t head_crc, size; const char *name; };
const PartInfo PARTS[N_PARTS] = {
    {0xDEFA9D0D, SOUND_CHIP, "HR-16 sound ROM U16"}, {0x39F48DFB, SOUND_CHIP, "HR-16 sound ROM U15"},
    {0x223A0B2C, SOUND_CHIP, "HR-16:B sound ROM U16 (2-27-0008)"}, {0xCE0838BF, SOUND_CHIP, "HR-16:B sound ROM U15 (2-27-0007)"},
    {0xCEA39A5C, 0x8000, "HR-16:B OS v2.00"},
    {0x2DB0537C, SOUND_CHIP, "SR-16 sound ROM U6"}, {0x87E4E0F2, SOUND_CHIP, "SR-16 sound ROM U5"},
    {0xE8BE166B, 0x10000, "SR-16 OS v1.04"},
};

int part_of(const uint8_t *h, size_t n, uint64_t size) {
    if (n < 4096) return -1;
    uint32_t crc = tinf_crc32(h, 4096);
    for (int k = 0; k < N_PARTS; k++)
        if (PARTS[k].size == size && PARTS[k].head_crc == crc) return k;
    return -1;
}

struct Layer {
    uint32_t start = 0;            // byte in the 1 MB sound space
    int vel_lo = 0, vel_hi = 127;
    double tune = 0, gain_db = 0, pan = 0, rand_lo = 0, rand_hi = 1;
};
struct Voice { std::string name; int cat = 0; std::vector<Layer> layers; };
struct Machine {
    std::string name;
    BlobPtr chip[2];
    std::vector<Voice> voices;
    bool chromatic = false;        // numbered sounds: one key each from C2 up, not the GM drum keys
};
struct Folder { std::vector<std::shared_ptr<const Machine>> machines; };

std::mutex cache_mutex;
std::map<std::string, std::shared_ptr<const Folder>> folders;

// ---- names to drum categories and GM keys ----

enum Cat { OTHER, KICK, SNARE, RIM, CLAP, HAT_CLOSED, HAT_OPEN, HAT_PEDAL, TOM, CRASH, RIDE, COWBELL, TAMB, CABASA,
           SHAKER, CONGA, BONGO, TIMBALE, AGOGO, CLAVE, BLOCK, TRIANGLE, CHINA };

bool has(const std::string &n, const char *w) { return n.find(w) != std::string::npos; }

int category(std::string n) {
    for (char &c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (has(n, "open") && has(n, "hat")) return HAT_OPEN;
    if ((has(n, "foot") && has(n, "hat")) || has(n, "pedal")) return HAT_PEDAL;
    if (has(n, "hat")) return HAT_CLOSED;
    if (has(n, "kick") || has(n, "kik") || has(n, "kk") || has(n, "foot") || has(n, "stomp")) return KICK;
    if (has(n, "side") || has(n, "stick") || has(n, "stik") || has(n, "rim")) return RIM;
    if (has(n, "snare") || has(n, "snr") || has(n, "sn") || has(n, "picolo")) return SNARE;
    if (has(n, "clap") || has(n, "snap")) return CLAP;
    if (has(n, "china")) return CHINA;
    if (has(n, "ride")) return RIDE;
    if (has(n, "crash") || has(n, "cym")) return CRASH;
    if (has(n, "cow")) return COWBELL;
    if (has(n, "tamb")) return TAMB;
    if (has(n, "cabasa")) return CABASA;
    if (has(n, "shak") || has(n, "maraca")) return SHAKER;
    if (has(n, "conga")) return CONGA;
    if (has(n, "bongo")) return BONGO;
    if (has(n, "timbal")) return TIMBALE;
    if (has(n, "agogo")) return AGOGO;
    if (has(n, "clave")) return CLAVE;
    if (has(n, "block")) return BLOCK;
    if (has(n, "triangle")) return TRIANGLE;
    if (has(n, "tom")) return TOM;
    return OTHER;
}

const std::vector<int> &gm_keys(int cat) {
    static const std::vector<std::vector<int>> keys = {
        {}, {36, 35}, {38, 40}, {37}, {39}, {42}, {46}, {44}, {41, 43, 45, 47, 48, 50}, {49, 57, 55}, {51, 59, 53},
        {56}, {54}, {69}, {70, 82}, {63, 62, 64}, {60, 61}, {65, 66}, {67, 68}, {75}, {76, 77}, {80, 81}, {52},
    };
    return keys[size_t(cat)];
}

// ---- the sound chips ----

uint8_t byte_at(const Machine &m, uint32_t a, std::vector<uint8_t> &buf, uint32_t &buf_at) {
    if (a < buf_at || a >= buf_at + buf.size()) {
        buf_at = a & ~uint32_t(0xFFF);
        int c = int(buf_at / SOUND_CHIP);
        uint32_t in = buf_at % SOUND_CHIP;
        buf = c < 2 ? m.chip[c]->bytes(in, std::min<uint32_t>(0x10000, SOUND_CHIP - in)) : std::vector<uint8_t>();
        if (buf.empty()) throw ParseError("Alesis sound past the end of its ROMs");
    }
    return buf[a - buf_at];
}

// the DM3AG's playback of a sound from byte a: full scale, one gain step down at each single 0x80, three end it
std::vector<int16_t> decode_from(const Machine &m, uint32_t a) {
    std::vector<int16_t> out;
    std::vector<uint8_t> buf;
    uint32_t buf_at = 0;
    int shift = 8, run = 0;
    for (; a < 2 * SOUND_CHIP && out.size() < size_t(RATE) * 30; a++) {
        uint8_t b = byte_at(m, a, buf, buf_at);
        if (b == 0x80) {
            if (++run == 1 && shift) shift--;
            if (run >= 3) break;
            continue;
        }
        run = 0;
        out.push_back(int16_t(int(int8_t(b)) * (1 << shift)));
    }
    return out;
}

// the sound starts of a ROM pair: its first byte, and after each run of three or more 0x80
std::vector<uint32_t> block_starts(const Machine &m) {
    std::vector<uint32_t> out;
    int run = 0;
    for (int c = 0; c < 2; c++) {
        std::vector<uint8_t> d = m.chip[c]->all(SOUND_CHIP);
        for (size_t i = 0; i < d.size(); i++) {
            if (d[i] == 0x80) { run++; continue; }
            if (run >= 3 || (c == 0 && i == 0)) out.push_back(uint32_t(c * SOUND_CHIP + i));
            run = 0;
        }
    }
    return out;
}

std::string trim_name(const uint8_t *p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) s += p[i] >= 32 && p[i] < 127 ? char(p[i]) : ' ';
    return trim(s);
}

// ---- the OS tables ----

void hr16b_voices(Machine &m, const std::vector<uint8_t> &raw) {
    std::vector<uint8_t> os(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        size_t lo = i & 0xFF, rev = 0;
        for (int b = 0; b < 8; b++) rev |= ((lo >> b) & 1) << (7 - b);
        os[(i & ~size_t(0xFF)) | rev] = raw[i];
    }
    const size_t TABLE = 0x6BFD, NAMES = 0x6C5F, N = 49;
    std::map<uint32_t, bool> seen;
    for (size_t k = 0; k < N; k++) {
        uint32_t start = uint32_t(os[TABLE + 2 * k] << 8 | os[TABLE + 2 * k + 1]) * 16;
        if (seen[start]) continue;                   // JAM BLOCK three times over: one sample
        seen[start] = true;
        Voice v;
        v.name = trim_name(&os[NAMES + 16 * k], 16);
        v.cat = category(v.name);
        Layer l;
        l.start = start;
        v.layers.push_back(l);
        m.voices.push_back(std::move(v));
    }
}

void sr16_voices(Machine &m, const std::vector<uint8_t> &os) {
    auto is_name = [&](size_t at) {
        bool letter = false;
        for (size_t i = 0; i < 8; i++) {
            if (os[at + i] < 32 || os[at + i] > 126) return false;
            letter |= std::isalpha(os[at + i]) != 0;
        }
        return letter;
    };
    size_t at = 0xCD20;
    int stray = 0;
    while (at + 17 <= os.size()) {
        int n = os[at + 8] & 0x7F;
        if (!is_name(at) || n < 1 || n > 8) {
            if (++stray > 3) break;                  // a stray layer after a voice, else the table's end
            at += 8;
            continue;
        }
        stray = 0;
        Voice v;
        v.name = trim_name(&os[at], 8);
        v.cat = category(v.name);
        bool random = os[at + 8] & 0x80;
        for (int k = 0; k < n; k++) {
            const uint8_t *x = &os[at + 9 + 8 * size_t(k)];
            Layer l;
            l.start = uint32_t(x[2] << 8 | x[3]) * 16;
            if (x[0] >= 2 && x[0] <= 4) { l.vel_lo = (x[0] - 2) * 43; l.vel_hi = x[0] == 4 ? 127 : l.vel_lo + 42; }
            l.gain_db = int8_t(x[1]) * 0.25;
            l.tune = (x[4] - 12) * 0.5;
            static const double PAN[8] = {0, -0.7, 0.7, -0.35, 0.35, -0.15, 0.15, 0};
            l.pan = PAN[(x[5] >> 3) & 7];
            if (random) { l.rand_lo = double(k) / n; l.rand_hi = double(k + 1) / n; }
            v.layers.push_back(l);
        }
        m.voices.push_back(std::move(v));
        at += 9 + 8 * size_t(n);
    }
}

void numbered_voices(Machine &m) {
    std::vector<uint32_t> starts = block_starts(m);
    char nm[32];
    for (size_t i = 0; i < starts.size(); i++) {
        std::snprintf(nm, sizeof nm, "Sound %02zu", i + 1);
        Voice v;
        v.name = nm;
        Layer l;
        l.start = starts[i];
        v.layers.push_back(l);
        m.voices.push_back(std::move(v));
    }
    m.chromatic = true;
}

std::shared_ptr<const Folder> scan(Volume &vol, const std::string &dir) {
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = folders.find(dir);
        if (it != folders.end()) return it->second;
    }
    std::string path[N_PARTS];
    for (const DirEntry &e : vol.list(dir)) {
        if (e.dir || (e.size != SOUND_CHIP && e.size != 0x8000 && e.size != 0x10000)) continue;
        try {
            std::vector<uint8_t> h = vol.open(path_join(dir, e.name))->head(4096);
            int k = part_of(h.data(), h.size(), e.size);
            if (k >= 0 && path[k].empty()) path[k] = path_join(dir, e.name);
        } catch (const std::exception &) {}
    }
    auto f = std::make_shared<Folder>();
    auto machine = [&](const char *name, Part a, Part b, Part os, void (*voices)(Machine &, const std::vector<uint8_t> &)) {
        if (path[a].empty() || path[b].empty()) return;
        auto m = std::make_shared<Machine>();
        m->name = name;
        m->chip[0] = vol.open(path[a]);
        m->chip[1] = vol.open(path[b]);
        if (os != N_PARTS && !path[os].empty()) voices(*m, vol.open(path[os])->all(PARTS[os].size));
        if (m->voices.empty()) numbered_voices(*m);    // no OS beside them: the ROMs' own sounds, by number
        f->machines.push_back(m);
    };
    machine("Alesis HR-16", HR16_U16, HR16_U15, N_PARTS, nullptr);
    machine("Alesis HR-16:B", HR16B_U16, HR16B_U15, HR16B_OS, hr16b_voices);
    machine("Alesis SR-16", SR16_U6, SR16_U5, SR16_OS, sr16_voices);
    std::lock_guard<std::mutex> lock(cache_mutex);
    folders[dir] = f;
    return f;
}

std::string missing_parts(Volume &vol, const std::string &dir) {
    std::string out;
    bool here[N_PARTS] = {};
    for (const DirEntry &e : vol.list(dir)) {
        if (e.dir) continue;
        try {
            std::vector<uint8_t> h = vol.open(path_join(dir, e.name))->head(4096);
            int k = part_of(h.data(), h.size(), e.size);
            if (k >= 0) here[k] = true;
        } catch (const std::exception &) {}
    }
    for (auto [a, b] : {std::pair<int, int>{HR16_U16, HR16_U15}, {HR16B_U16, HR16B_U15}, {SR16_U6, SR16_U5}})
        if (here[a] != here[b]) out = std::string("put ") + PARTS[here[a] ? b : a].name + " beside " + PARTS[here[a] ? a : b].name;
    return out;
}

// ---- zones ----

SampleRefPtr sample(const std::shared_ptr<const Machine> &m, uint32_t start, const std::string &name) {
    auto r = std::make_shared<SampleRef>();
    r->name = name;
    r->rate = RATE;
    r->channels = 1;
    r->frames = 0;
    r->key = "alesis:" + m->name + ":" + std::to_string(start);
    r->decode = [m, start]() -> PcmPtr {
        auto p = std::make_shared<Pcm>();
        p->rate = RATE;
        p->channels = 1;
        p->data = decode_from(*m, start);
        if (p->data.empty()) throw ParseError("empty Alesis sound");
        return p;
    };
    return r;
}

void add_voice(const std::shared_ptr<const Machine> &m, const Voice &v, int key, bool across, Instrument &inst) {
    for (const Layer &l : v.layers) {
        Zone z;
        z.name = v.name;
        z.sample = sample(m, l.start, v.name);
        z.one_shot = true;
        z.root = across ? 60 : key;
        z.key_lo = across ? 0 : key;
        z.key_hi = across ? 127 : key;
        z.vel_lo = l.vel_lo;
        z.vel_hi = l.vel_hi;
        z.tune = l.tune;
        z.gain_db = l.gain_db;
        z.pan = l.pan;
        if (l.rand_lo > 0 || l.rand_hi < 1) { z.play_logic = PlayLogic::Random; z.rand_lo = l.rand_lo; z.rand_hi = l.rand_hi; }
        inst.zones.push_back(std::move(z));
    }
}

// kits: HR-16 voices on the GM drum keys; the SR-16's 234 in banks of 61 from C2; numbered sounds from C2
constexpr size_t BANK = 61;
size_t kit_count(const Machine &m) { return m.chromatic || m.voices.size() <= 64 ? 1 : (m.voices.size() + BANK - 1) / BANK; }
bool gm_kit(const Machine &m) { return !m.chromatic && m.voices.size() <= 64; }

Instrument load_kit(const std::shared_ptr<const Machine> &m, size_t kit) {
    Instrument inst;
    inst.format = "Drum machine ROM";
    inst.library = m->name;
    if (gm_kit(*m)) {
        inst.name = m->name + " kit";
        bool taken[128] = {};
        std::vector<std::pair<size_t, int>> placed;
        std::vector<size_t> later;
        for (size_t i = 0; i < m->voices.size(); i++) {
            int key = -1;
            for (int k : gm_keys(m->voices[i].cat)) if (!taken[k]) { key = k; break; }
            if (key < 0) { later.push_back(i); continue; }
            taken[key] = true;
            placed.push_back({i, key});
        }
        for (size_t i : later) {                     // the rest from C5 up, then down from B1
            int key = -1;
            for (int k = 72; k < 128 && key < 0; k++) if (!taken[k]) key = k;
            for (int k = 35; k >= 0 && key < 0; k--) if (!taken[k]) key = k;
            if (key < 0) continue;
            taken[key] = true;
            placed.push_back({i, key});
        }
        bool open = false;
        for (auto &p : placed) open |= m->voices[p.first].cat == HAT_OPEN;
        for (auto &p : placed) {
            size_t before = inst.zones.size();
            add_voice(m, m->voices[p.first], p.second, false, inst);
            int c = m->voices[p.first].cat;
            if (open && (c == HAT_CLOSED || c == HAT_OPEN || c == HAT_PEDAL))
                for (size_t z = before; z < inst.zones.size(); z++) inst.zones[z].exclusive_group = 1;
        }
    } else {
        size_t first = m->chromatic ? 0 : kit * BANK, last = m->chromatic ? m->voices.size() : std::min(m->voices.size(), first + BANK);
        inst.name = m->name + (m->chromatic ? " sounds" : " voices " + std::to_string(first + 1) + "-" + std::to_string(last));
        for (size_t i = first; i < last && 36 + int(i - first) < 128; i++) add_voice(m, m->voices[i], 36 + int(i - first), false, inst);
    }
    return inst;
}

// ---- the reader ----

bool probe(const std::string &, const uint8_t *h, size_t n, uint64_t size) { return part_of(h, n, size) >= 0; }

std::vector<PresetInfo> list(VolumePtr vol, const std::string &path) {
    std::shared_ptr<const Folder> f = scan(*vol, path_dir(path));
    std::vector<PresetInfo> out;
    for (size_t mi = 0; mi < f->machines.size(); mi++) {
        const Machine &m = *f->machines[mi];
        int base = int(mi) * 10000;
        for (size_t k = 0; k < kit_count(m); k++) {
            size_t first = k * BANK, last = std::min(m.voices.size(), first + BANK);
            std::string nm = gm_kit(m) || m.chromatic ? m.name + " kit (" + std::to_string(m.voices.size()) + ")"
                                                      : m.name + " voices " + std::to_string(first + 1) + "-" + std::to_string(last);
            out.push_back({nm, base + int(k)});
        }
        for (size_t i = 0; i < m.voices.size(); i++) out.push_back({m.name + ": " + m.voices[i].name, base + 1000 + int(i)});
    }
    if (out.empty()) {
        std::string why = missing_parts(*vol, path_dir(path));
        throw ParseError(why.empty() ? "no Alesis sound ROM pair here" : why);
    }
    return out;
}

Instrument load(VolumePtr vol, const std::string &path, int index) {
    std::shared_ptr<const Folder> f = scan(*vol, path_dir(path));
    size_t mi = size_t(index / 10000);
    int k = index % 10000;
    if (mi >= f->machines.size()) throw ParseError("no such Alesis kit here");
    const auto &m = f->machines[mi];
    Instrument inst;
    if (k < 1000) {
        if (size_t(k) >= kit_count(*m)) throw ParseError("no such Alesis kit");
        inst = load_kit(m, size_t(k));
    } else {
        size_t v = size_t(k - 1000);
        if (v >= m->voices.size()) throw ParseError("no such Alesis voice");
        inst.name = m->name + ": " + m->voices[v].name;
        inst.format = "Drum machine ROM";
        inst.library = m->name;
        add_voice(m, m->voices[v], 60, true, inst);   // one voice across the keyboard, C4 at its own pitch
    }
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_alesis_drum() {
    register_reader({"Alesis drum ROM", "", probe, list, load});
}

}  // namespace omni

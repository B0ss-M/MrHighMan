// Omni Sampler: the Kurzweil K2000's ROM programs, played from its ROM dumps: a setup ROM pair (the programs, keymaps
// and samples; MAME's k2000 set: k2j-k2rj_su_hi / _lo, or the K2000R setup ROM, or the v3.54 K2VPhi / K2VPlo) with
// the sound ROM chips beside it (the base 8 MB: k2m1h ... k2m4l; the RM-126 Orchestral block: k2xxrom1-1 ... 1-4).
// Tapping a setup chip or a sound chip lists the programs, then the keymaps no program uses.
// Decoded 2026-10-21 from the ROMs above:
//   - A setup ROM is a hi / lo byte pair (hi = even bytes, so the hi chip starts "SO", the lo chip "RM"): "SROM", u32
//     end, then from 0x20 the objects in one block (a negative int32 size, as in the K2000's .krz files: see
//     ConvertWithMoss's KURZWEIL_FORMAT.md, from KurzFiler). Inside the block the objects follow one another (a .krz
//     file puts each in its own block): u16 hash (type << 10 | id), u16 size, u16 name offset, name, data; the next
//     object at size, rounded up to even. Types: 36 program, 37 keymap, 38 sample.
//   - Program, keymap and sample objects read as in ConvertWithMoss's Kurzweil reader (LGPL-3.0): program segments
//     (tag, fixed length), a layer from its LYR segment (keys, velocity window) and CAL (transpose, keymap); keymaps
//     with 8 dynamic levels, each pointing at a table of entries (tuning, volume, sample, sub-sample); sample headers
//     of 32 bytes (root, flags, volume in 0.5 dB, start / alt start / loop / end, period in ns).
//   - Sample addresses are 16-bit words: the base ROM at 0 - 0x3FFFFF (each pair of chips 1M words, hi chip the high
//     byte), the Orchestral block at 0x1000000 - 0x13FFFFF (each chip 1M words, little endian). The setup ROMs found
//     hold no objects for the RM-226 Contemporary block (k2xxrom2-*), so it is not read.
//   - The loop runs from loop to end (inclusive). Header flags: 0x04 drums and hits (one shot), 0x80 no loop,
//     0x01 reversed; a loop at or past the end means none.
//   - CAL byte 3 is the key tracking: 0x2B normal, 0 fixed at C4 + transpose (the drum programs: 94 of 120 one-entry
//     drum layers then play their hit at its root, exactly).
// Not read: the programs' effects, LFOs, pitch envelope (their depths and routing are not confirmed), the keymaps'
// volume column. 2.4.18: the amp envelope (ENC / ENV) is read and played; the filter (algorithm, F1-F3, ENV2 or
// velocity) is decoded but not applied (k2_sound: its other modulation is not confirmed).
// 2.4.18: the K2000's own files (.KRZ: factory and FARM banks, the GM sets) - decoded from Kurzweil's downloads:
//   - "PRAM", u32 offset of the sample data (= the file's size when it holds none), u32 the sample data's full size
//     (bigger than the file's when a second floppy holds the rest: GMSET3's DISK2, its own "PRAM" header of 36 bytes),
//     then from 0x20 the objects, one block each (the setup ROM's layout; the list ends at a size >= 0).
//   - Objects the file holds have ids 200 and up; a program's keymap or a keymap's sample not in the file is the ROM's
//     (ids under 200, and the Orchestral block's). The ROM is found in the file's folder or above it, or in a folder
//     named for the K2000 there (".../Kurzweil K2000/Presets/<bank>/x.krz" finds ".../Kurzweil K2000").
//   - A sample header with flag 0x10 plays from the file's sample data (16-bit big-endian words, its start / loop /
//     end counted from the data's start: 70 of 70 such headers in the downloads fit their file); without it, from the
//     ROM's words (the factory banks' "RAM" samples are ROM samples with other loops and roots).
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include "../third_party/tinf/tinf.h"
#include "format.hpp"

namespace omni {
namespace {

constexpr uint32_t CHIP_WORDS = 0x100000;
constexpr uint32_t ORCH_BASE = 0x1000000;
constexpr int N_BASE = 8, N_ORCH = 4, N_CONT = 4, N_CHIPS = N_BASE + N_ORCH + N_CONT;
constexpr uint32_t CONT_BASE = ORCH_BASE + N_ORCH * CHIP_WORDS;   // 2.4.18: the Contemporary block, right after

struct ChipInfo { uint32_t head_crc; uint64_t size; const char *name; };
// the base ROM in pairs (hi, lo) by word order, then the Orchestral block
const ChipInfo SOUND[N_CHIPS] = {
    {0xAB1183BD, 0x100000, "k2m1h (U38)"}, {0x1B2462A5, 0x100000, "k2m1l (U43)"},
    {0xF6498FFF, 0x100000, "k2m2h (U39)"}, {0x5AAEFCA0, 0x100000, "k2m2l (U44)"},
    {0x7A087C1E, 0x100000, "k2m3h (U40)"}, {0xEEA8999B, 0x100000, "k2m3l (U46)"},
    {0xB5933EDD, 0x100000, "k2m4h (U42)"}, {0x2F0C88CE, 0x100000, "k2m4l (U47)"},
    {0x4051117E, 0x200000, "k2xxrom1-1"}, {0x555E685C, 0x200000, "k2xxrom1-2"},
    {0x022CD0DA, 0x200000, "k2xxrom1-3"}, {0x4E759D48, 0x200000, "k2xxrom1-4"},
    // 2.4.18: the RM-226 Contemporary block (little endian, as the Orchestral; the K2VX setup ROM's 800s use it)
    {0xC9601F4A, 0x200000, "k2xxrom2-1"}, {0x723FE59D, 0x200000, "k2xxrom2-2"},
    {0x2BE5B56A, 0x200000, "k2xxrom2-3"}, {0x55BDB667, 0x200000, "k2xxrom2-4"},
};

int sound_chip(const uint8_t *h, size_t n, uint64_t size) {
    if (n < 4096) return -1;
    uint32_t crc = tinf_crc32(h, 4096);
    for (int k = 0; k < N_CHIPS; k++)
        if (SOUND[k].size == size && SOUND[k].head_crc == crc) return k;
    return -1;
}

// a setup ROM chip: 0 hi ("SO..."), 1 lo ("RM..."), else -1
int setup_half(const uint8_t *h, size_t n, uint64_t size) {
    if (n < 16 || size < 0x10000 || size > 0x200000 || size % 0x10000) return -1;
    // byte 16 of each chip is a byte of the first block's (negative) size: 0xFF on the hi chip, 0xC0-0xFF on the lo
    if (h[0] == 'S' && h[1] == 'O' && h[2] == 0 && h[16] == 0xFF) return 0;
    if (h[0] == 'R' && h[1] == 'M' && h[16] >= 0xC0) return 1;
    return -1;
}

struct KHeader { int root = 60, flags = 0, vol = 0; uint32_t start = 0, loop = 0, end = 0; int period = 0; };
struct KSample { std::string name; bool stereo = false; std::vector<KHeader> headers; };
struct KEntry {
    int tune = 0, vol = 0, sample = 0, sub = 0;
    bool operator==(const KEntry &o) const { return tune == o.tune && vol == o.vol && sample == o.sample && sub == o.sub; }
};
struct KKeymap { std::string name; int base_pitch = 0, cents = 100; std::vector<std::vector<KEntry>> tables; int level[8] = {}; };
struct KLayer {
    int lo = 0, hi = 127, vel_lo = 0, vel_hi = 127, keymap = 0, transpose = 0; double key_tracking = 1;
    // 2.4.18 the layer's sound (ConvertWithMoss KURZWEIL_FORMAT.md, from mpc2emu's K2000R diffs): ENC [1] (1 = the
    // samples' natural envelope), ENV / ENV2 (the amp and filter envelopes: 7 time / level pairs), CAL [29] (the DSP
    // algorithm), the F1 / F2 / F3 pages (filter type, cutoff, its modulation; resonance; separation)
    int enc_mode = -1, alg = -1;
    bool has_env = false, has_env2 = false, has_f1 = false, has_f2 = false, has_f3 = false;
    uint8_t env[15] = {}, env2[15] = {}, f1[15] = {}, f2[15] = {}, f3[15] = {};
};
struct KProgram { int id = 0; std::string name; std::vector<KLayer> layers; };

struct Rom {
    std::string key;
    BlobPtr chip[N_CHIPS];
    std::vector<KProgram> programs;
    std::map<int, KKeymap> keymaps;
    std::map<int, KSample> samples;
    std::vector<int> spare_keymaps;    // keymaps no program uses, in id order
    // 2.4.18 a .KRZ file: its sample data (16-bit big-endian words) and whether a K2000 ROM was found for it
    BlobPtr ram;
    uint64_t ram_off = 0, ram_words = 0;
    std::vector<std::shared_ptr<const Rom>> under;   // the ROMs a .KRZ file's objects sit over, searched in turn (shared)
    bool rom_found = true;
    std::string krz_note;               // e.g. the second disk of a two-disk file is missing
};
using RomPtr = std::shared_ptr<const Rom>;

// an object by id: the file's own first, then its ROM's (2.4.18)
const KKeymap *keymap_of(const Rom &r, int id) {
    auto it = r.keymaps.find(id);
    if (it != r.keymaps.end()) return &it->second;
    for (const auto &u : r.under) if (const KKeymap *k = keymap_of(*u, id)) return k;
    return nullptr;
}
const KSample *sample_of(const Rom &r, int id) {
    auto it = r.samples.find(id);
    if (it != r.samples.end()) return &it->second;
    for (const auto &u : r.under) if (const KSample *sm = sample_of(*u, id)) return sm;
    return nullptr;
}

std::mutex cache_mutex;
std::map<std::string, RomPtr> roms;

int s8(uint8_t b) { return int(int8_t(b)); }
int s16(const uint8_t *p) { return int(int16_t(uint16_t(p[0] << 8 | p[1]))); }
uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

int segment_length(int tag) {
    if (tag == 8 || tag == 9) return 15;
    if (tag == 15) return 7;
    switch (tag & 0xF8) {
    case 16: return 7;
    case 24: return 3;
    case 32: return 15;
    case 64: return 31;
    case 80: return 15;
    case 104: return 7;
    case 120: return 31;
    default: return 0;
    }
}

KProgram read_program(const uint8_t *d, size_t n) {
    KProgram p;
    KLayer *cur = nullptr;
    for (size_t at = 0; at < n;) {
        int tag = d[at++];
        int len = segment_length(tag);
        if (!tag || !len || at + size_t(len) > n) break;
        const uint8_t *x = d + at;
        at += size_t(len);
        if (tag == 9) {
            KLayer l;
            l.lo = x[3] & 0x7F;
            l.hi = x[4] & 0x7F;
            l.vel_lo = (x[5] >> 3 & 7) * 16;
            l.vel_hi = (7 - (x[5] & 7)) * 16 + 15;
            p.layers.push_back(l);
            cur = &p.layers.back();
        } else if (tag == 64 && cur) {
            cur->transpose = s8(x[1]);
            // key tracking: 43 (the default) = 100 cents a key, 0 = fixed at C4 (drum kits); steps between are taken
            // as linear and those above 43 as 100 cents, a guess (no factory program needs them to be right)
            cur->key_tracking = x[3] >= 43 ? 1.0 : x[3] / 43.0;
            cur->keymap = x[7] << 8 | x[8];
            if (!cur->keymap) cur->keymap = x[11] << 8 | x[12];
            cur->alg = x[29];
        } else if (cur && tag == 32) cur->enc_mode = x[1];
        else if (cur && (tag == 33 || tag == 34)) {
            std::memcpy(tag == 33 ? cur->env : cur->env2, x, 15);
            (tag == 33 ? cur->has_env : cur->has_env2) = true;
        } else if (cur && tag >= 0x50 && tag <= 0x52) {
            std::memcpy(tag == 0x50 ? cur->f1 : tag == 0x51 ? cur->f2 : cur->f3, x, 15);
            (tag == 0x50 ? cur->has_f1 : tag == 0x51 ? cur->has_f2 : cur->has_f3) = true;
        }
    }
    return p;
}

bool read_keymap(const uint8_t *d, size_t n, KKeymap &k) {
    if (n < 28) return false;
    int sample = s16(d), method = s16(d + 2);
    k.base_pitch = s16(d + 4);
    k.cents = s16(d + 6);
    int entries = s16(d + 8) + 1;
    int size = (method & 0x10 ? 2 : method & 0x08 ? 1 : 0) + (method & 4 ? 1 : 0) + (method & 2 ? 2 : 0) + (method & 1 ? 1 : 0);
    if (entries <= 0 || entries > 1024 || !size) return false;
    std::map<int, int> table_of;   // byte offset -> table index
    for (int lv = 0; lv < 8; lv++) {
        int off = s16(d + 12 + 2 * lv) - (8 - lv) * 2;
        if (off < 0) return false;
        auto it = table_of.find(off);
        if (it == table_of.end()) it = table_of.emplace(off, int(table_of.size())).first;
        k.level[lv] = it->second;
    }
    k.tables.resize(table_of.size());
    for (const auto &t : table_of) {
        std::vector<KEntry> &es = k.tables[size_t(t.second)];
        size_t at = 28 + size_t(t.first);
        for (int i = 0; i < entries; i++) {
            KEntry e;
            e.sample = sample;
            if (at + size_t(size) <= n) {   // a short last table (the K2000R's English Horn) leaves its end unused
                const uint8_t *x = d + at;
                if (method & 0x10) { e.tune = s16(x); x += 2; } else if (method & 0x08) e.tune = s8(*x++);
                if (method & 4) e.vol = s8(*x++);
                if (method & 2) { e.sample = s16(x); x += 2; }
                if (method & 1) e.sub = *x;
            }
            es.push_back(e);
            at += size_t(size);
        }
    }
    return true;
}

bool read_sample(const uint8_t *d, size_t n, KSample &s) {
    if (n < 8) return false;
    int count = s16(d + 2) + 1, offset = s16(d + 4);
    s.stereo = d[6] & 1;
    if (count <= 0 || offset < 0) return false;
    for (int k = 0; k < count; k++) {
        size_t at = 4 + size_t(offset) + 32 * size_t(k);
        if (at + 32 > n) break;
        const uint8_t *x = d + at;
        KHeader h;
        h.root = x[0];
        h.flags = x[1];
        h.vol = s8(x[2]);
        h.start = u32(x + 8);
        h.loop = u32(x + 16);
        h.end = u32(x + 20);
        h.period = int(u32(x + 28));
        s.headers.push_back(h);
    }
    return !s.headers.empty();
}

std::string paired_key(const std::string &hi, const std::string &lo) { return hi + "\n" + lo; }

// the objects from `first` (a list of blocks, each a negative int32 size then objects) up to `limit`: programs, keymaps
// and samples into rom (an id already there is replaced: a file's objects over the ROM's); the keymaps programs use
void read_objects(const std::vector<uint8_t> &d, size_t first, size_t limit, Rom &rom, std::set<int> &used, bool replace) {
    limit = std::min(limit, d.size());
    for (size_t block = first; block + 4 <= limit;) {
        int32_t bs = int32_t(u32(&d[block]));
        if (bs >= 0 || uint64_t(-int64_t(bs)) > limit - block) break;
        size_t end = block + size_t(-int64_t(bs));
        for (size_t at = block + 4; at + 6 <= end;) {
            int hash = d[at] << 8 | d[at + 1], size = d[at + 2] << 8 | d[at + 3], name_ofs = d[at + 4] << 8 | d[at + 5];
            if (!hash || size < 6 || at + size_t(size) > end || name_ofs + 4 > size) break;
            int type = hash >> 10, id = hash & 0x3FF;
            std::string name;
            for (size_t k = at + 6; k < at + 4 + size_t(name_ofs) && d[k]; k++) name += char(d[k] >= 32 && d[k] < 127 ? d[k] : ' ');
            name = trim(name);
            const uint8_t *data = &d[at + 4 + size_t(name_ofs)];
            size_t n = size_t(size - name_ofs - 4);
            if (type == 36) {
                KProgram p = read_program(data, n);
                p.id = id;
                p.name = name.empty() ? "Program " + std::to_string(id) : name;
                for (const KLayer &ly : p.layers) used.insert(ly.keymap);
                rom.programs.push_back(std::move(p));
            } else if (type == 37) {
                KKeymap k;
                k.name = name.empty() ? "Keymap " + std::to_string(id) : name;
                if (read_keymap(data, n, k) && (replace || !rom.keymaps.count(id))) rom.keymaps[id] = std::move(k);
            } else if (type == 38) {
                KSample sm;
                sm.name = name;
                if (read_sample(data, n, sm) && (replace || !rom.samples.count(id))) rom.samples[id] = std::move(sm);
            }
            at += size_t(size) + (size & 1);
        }
        block = end;
    }
}


// 2.4.18: a setup ROM's hi and lo chips paired by what they hold, not by size alone: a folder with two pairs of one
// size (the K2VX v5.00 and the K2000R v1.0 setups, both 512 KB a chip) paired the K2VX's hi with the K2000R's lo.
// The score is the objects the interleaved first 32 KB parse into (a wrong pair stops at its first object)
int pair_score(const std::vector<uint8_t> &h, const std::vector<uint8_t> &l) {
    std::vector<uint8_t> d(2 * std::min(h.size(), l.size()));
    for (size_t i = 0; 2 * i < d.size(); i++) { d[2 * i] = h[i]; d[2 * i + 1] = l[i]; }
    if (d.size() < 0x28 || std::memcmp(d.data(), "SROM", 4)) return 0;
    int n = 0;
    for (size_t block = 0x20; block + 4 <= d.size();) {
        int32_t bs = int32_t(u32(&d[block]));
        if (bs >= 0) break;
        const size_t end = std::min(d.size(), block + size_t(-int64_t(bs)));
        for (size_t at = block + 4; at + 6 <= end;) {
            int hash = d[at] << 8 | d[at + 1], size = d[at + 2] << 8 | d[at + 3], name_ofs = d[at + 4] << 8 | d[at + 5];
            if (!hash || size < 6 || name_ofs + 4 > size || (hash >> 10) < 1) return n;
            if (at + size_t(size) > end) return n;
            n++;
            at += size_t(size) + (size & 1);
        }
        if (end >= d.size()) break;
        block = end;
    }
    return n;
}

struct SetupPair { std::string hi, lo; uint64_t size = 0; int score = 0; };

// the folder's sound chips (into sound) and its setup pairs, the biggest first (then the best parsed)
std::vector<SetupPair> scan_rom_dir(Volume &vol, const std::string &dir, std::string *sound) {
    std::vector<std::pair<std::string, uint64_t>> his, los;
    for (const DirEntry &e : vol.list(dir)) {
        if (e.dir || e.size < 0x10000 || e.size > 0x200000) continue;
        std::string p = path_join(dir, e.name);
        try {
            std::vector<uint8_t> h = vol.open(p)->head(4096);
            int k = sound_chip(h.data(), h.size(), e.size);
            if (k >= 0) { if (sound && sound[k].empty()) sound[k] = p; continue; }
            int half = setup_half(h.data(), h.size(), e.size);
            if (half == 0) his.push_back({p, e.size});
            if (half == 1) los.push_back({p, e.size});
        } catch (const std::exception &) {}
    }
    std::vector<SetupPair> out;
    std::set<std::string> used_lo;
    for (const auto &h : his) {
        std::vector<uint8_t> hb;
        try { hb = vol.open(h.first)->head(16384); } catch (const std::exception &) { continue; }
        SetupPair best;
        for (const auto &l : los) {
            if (l.second != h.second || used_lo.count(l.first)) continue;
            std::vector<uint8_t> lb;
            try { lb = vol.open(l.first)->head(16384); } catch (const std::exception &) { continue; }
            const int sc = pair_score(hb, lb);
            if (sc > best.score) best = {h.first, l.first, h.second, sc};
        }
        if (best.score > 0) { used_lo.insert(best.lo); out.push_back(best); }
    }
    std::sort(out.begin(), out.end(), [](const SetupPair &a, const SetupPair &b) {
        return a.size != b.size ? a.size > b.size : a.score != b.score ? a.score > b.score : a.hi < b.hi;
    });
    return out;
}

RomPtr open_rom_pair(Volume &vol, const SetupPair &sp, const std::string *sound) {
    const std::string key = paired_key(sp.hi, sp.lo);
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = roms.find(key);
        if (it != roms.end()) return it->second;
    }
    auto rom = std::make_shared<Rom>();
    rom->key = key;
    for (int k = 0; k < N_CHIPS; k++)
        if (!sound[k].empty()) rom->chip[k] = vol.open(sound[k]);
    std::vector<uint8_t> h = vol.open(sp.hi)->all(0x200000), l = vol.open(sp.lo)->all(0x200000);
    std::vector<uint8_t> d(2 * std::min(h.size(), l.size()));
    for (size_t i = 0; 2 * i < d.size(); i++) { d[2 * i] = h[i]; d[2 * i + 1] = l[i]; }
    if (d.size() < 0x28 || std::memcmp(d.data(), "SROM", 4)) throw ParseError("not a K2000 setup ROM");
    std::set<int> used;
    read_objects(d, 0x20, d.size(), *rom, used, true);
    if (rom->programs.empty() && rom->keymaps.empty()) throw ParseError("no programs in the K2000 setup ROM");
    std::sort(rom->programs.begin(), rom->programs.end(), [](const KProgram &a, const KProgram &b) { return a.id < b.id; });
    for (const auto &k : rom->keymaps)
        if (!used.count(k.first) && k.second.name != "Silence") rom->spare_keymaps.push_back(k.first);
    std::lock_guard<std::mutex> lock(cache_mutex);
    roms[key] = rom;
    return rom;
}

// the setup the tapped chip belongs to (else the folder's biggest)
RomPtr open_rom_in(Volume &vol, const std::string &dir, const std::string &path) {
    std::string sound[N_CHIPS];
    const std::vector<SetupPair> pairs = scan_rom_dir(vol, dir, sound);
    if (pairs.empty()) throw ParseError("K2000 setup ROM missing: put its hi and lo chips (k2j-k2rj_su_hi / _lo, or the K2000R setup ROM) beside the sound ROMs");
    const SetupPair *pick = &pairs[0];
    for (const SetupPair &sp : pairs) if (sp.hi == path || sp.lo == path) { pick = &sp; break; }
    return open_rom_pair(vol, *pick, sound);
}

// 2.4.18: every setup in the folder (for a .KRZ file's ROM objects: the K2VX's has the Contemporary 800s, the
// K2000R's the Orchestral 900s, the K2000's the base), the most objects first
std::vector<RomPtr> open_all_roms(Volume &vol, const std::string &dir) {
    std::string sound[N_CHIPS];
    std::vector<RomPtr> out;
    for (const SetupPair &sp : scan_rom_dir(vol, dir, sound)) {
        try { out.push_back(open_rom_pair(vol, sp, sound)); } catch (const std::exception &) {}
    }
    std::stable_sort(out.begin(), out.end(), [](const RomPtr &a, const RomPtr &b) { return a->keymaps.size() + a->samples.size() > b->keymaps.size() + b->samples.size(); });
    return out;
}

RomPtr open_rom(Volume &vol, const std::string &path) { return open_rom_in(vol, path_dir(path), path); }

// whether the chips holding these words are all there
bool has_words(const Rom &rom, uint32_t first, uint32_t last) {
    for (uint32_t w = first & ~(CHIP_WORDS - 1); w <= last; w += CHIP_WORDS) {
        if (w < 4 * CHIP_WORDS) {
            int pair = int(w / CHIP_WORDS);
            if (!rom.chip[2 * pair] || !rom.chip[2 * pair + 1]) return false;
        } else if (w >= ORCH_BASE && w < ORCH_BASE + (N_ORCH + N_CONT) * CHIP_WORDS) {   // (Orchestral, Contemporary)
            if (!rom.chip[N_BASE + int((w - ORCH_BASE) / CHIP_WORDS)]) return false;
        } else return false;
        if (w + CHIP_WORDS < w) break;
    }
    return true;
}

void read_words(const Rom &rom, uint32_t first, size_t count, int16_t *out, int stride) {
    while (count) {
        uint32_t in_chip = first & (CHIP_WORDS - 1);
        size_t n = std::min<size_t>(count, CHIP_WORDS - in_chip);
        if (first < 4 * CHIP_WORDS) {
            int pair = int(first / CHIP_WORDS);
            std::vector<uint8_t> h = rom.chip[2 * pair]->bytes(in_chip, n), l = rom.chip[2 * pair + 1]->bytes(in_chip, n);
            for (size_t i = 0; i < n; i++) out[i * size_t(stride)] = int16_t(uint16_t(h[i] << 8 | l[i]));
        } else {
            int c = N_BASE + int((first - ORCH_BASE) / CHIP_WORDS);
            std::vector<uint8_t> b = rom.chip[c]->bytes(2 * uint64_t(in_chip), 2 * n);
            for (size_t i = 0; i < n; i++) out[i * size_t(stride)] = int16_t(uint16_t(b[2 * i + 1] << 8 | b[2 * i]));
        }
        out += n * size_t(stride);
        first += uint32_t(n);
        count -= n;
    }
}

// 2.4.18: single-cycle waves (organ drawbars, synth waves: 44-85 words) loop from a point BEFORE their start
// (Drawbars 1-3: loop 4192502, start 4192544, end 4192587, in every setup ROM): the loop was dropped (it needed
// loop >= start), so the wave played its few frames and stopped (a click). Such a sample is now built as start..end
// then the whole loop (loop..end) again, the loop on that second part: it plays as the K2000 does (from the start,
// then round the loop) without a loop start before the zone's start (the sampler clamps those).
bool loops_ok(const KHeader &h) { return !(h.flags & 0x84) && h.loop < h.end; }
bool loop_before(const KHeader &h) { return loops_ok(h) && h.loop < h.start && h.start - h.loop <= CHIP_WORDS; }
uint32_t first_word(const KHeader &h) { return loop_before(h) ? h.loop : h.start; }
// the frames a header's sample has as built (start..end, plus the loop again when it begins before the start)
int64_t built_frames(const KHeader &h) {
    return int64_t(h.end - h.start) + 1 + (loop_before(h) ? int64_t(h.end - h.loop) + 1 : 0);
}

// 2.4.18: a .KRZ file's own sample words (16-bit big-endian, counted from its sample data)
void read_ram(const Rom &rom, uint32_t first, size_t count, int16_t *out, int stride) {
    std::vector<uint8_t> b = rom.ram->bytes(rom.ram_off + 2 * uint64_t(first), 2 * count);
    for (size_t i = 0; i < count && 2 * i + 1 < b.size(); i++) out[i * size_t(stride)] = int16_t(uint16_t(b[2 * i] << 8 | b[2 * i + 1]));
}

SampleRefPtr rom_pcm(const RomPtr &rom, const KHeader &left, const KHeader *right, const std::string &name, bool ram = false) {
    auto r = std::make_shared<SampleRef>();
    r->name = name;
    r->rate = left.period > 0 ? int(std::lround(1e9 / left.period)) : 44100;
    r->channels = right ? 2 : 1;
    const KHeader L = left, R = right ? *right : KHeader{};
    int64_t frames = built_frames(left);
    if (right) frames = std::min(frames, built_frames(*right));
    r->frames = frames;
    const uint32_t a = left.start, b = right ? right->start : 0;
    int rate = r->rate;
    r->key = std::string(ram ? "k2000ram:" : "k2000:") + rom->key + ":" + std::to_string(a) + ":" + std::to_string(b) + ":" + std::to_string(frames);
    r->decode = [rom, L, R, frames, rate, stereo = right != nullptr, ram]() -> PcmPtr {
        auto p = std::make_shared<Pcm>();
        p->rate = rate;
        p->channels = stereo ? 2 : 1;
        p->data.resize(size_t(frames) * (stereo ? 2 : 1));
        auto rd = ram ? read_ram : read_words;
        const int st = stereo ? 2 : 1;
        // one channel: start..end, then (a loop before the start) loop..end again; never past `frames`
        auto channel = [&](const KHeader &h, int16_t *out) {
            const size_t body = std::min<size_t>(size_t(frames), size_t(h.end - h.start) + 1);
            rd(*rom, h.start, body, out, st);
            if (loop_before(h) && size_t(frames) > body)
                rd(*rom, h.loop, std::min<size_t>(size_t(frames) - body, size_t(h.end - h.loop) + 1), out + body * size_t(st), st);
        };
        channel(L, p->data.data());
        if (stereo) channel(R, p->data.data() + 1);
        return p;
    };
    return r;
}

// a zone from a header; false (and a note in missing) when its chips are not there
bool make_zone(const RomPtr &rom, const KSample &s, int sub, Zone &z, std::set<std::string> &missing,
               std::map<int64_t, SampleRefPtr> &refs, int sample_id) {
    int index = sub - 1;
    if (s.stereo) index &= ~1;
    if (index < 0 || index >= int(s.headers.size())) return false;
    const KHeader &h = s.headers[size_t(index)];
    const KHeader *right = s.stereo && index + 1 < int(s.headers.size()) ? &s.headers[size_t(index) + 1] : nullptr;
    if (h.end <= h.start || (right && right->end <= right->start)) return false;
    const bool ram = (h.flags & 0x10) && rom->ram;   // 2.4.18: a .KRZ file's own sample
    if (ram) {
        if (h.end >= rom->ram_words || (right && right->end >= rom->ram_words)) {
            missing.insert(rom->krz_note.empty() ? "sample data past the end of the file" : rom->krz_note);
            return false;
        }
    } else if (!rom->rom_found) {
        missing.insert("the K2000 ROM (its setup and sound chips: a \"Kurzweil K2000\" folder above this file)");
        return false;
    } else if (!has_words(*rom, first_word(h), h.end) || (right && !has_words(*rom, first_word(*right), right->end))) {
        missing.insert(h.start >= CONT_BASE + N_CONT * CHIP_WORDS ? "a ROM block not read"
                       : h.start >= CONT_BASE ? "the Contemporary ROM (k2xxrom2-1 ... 2-4)"
                       : h.start >= ORCH_BASE ? "the Orchestral ROM (k2xxrom1-1 ... 1-4)" : "base sound ROM chips (k2m1h ... k2m4l)");
        return false;
    }
    int64_t key = int64_t(sample_id) << 16 | index;
    SampleRefPtr &ref = refs[key];
    if (!ref) ref = rom_pcm(rom, h, right, s.name.empty() ? "Sample " + std::to_string(sample_id) : s.name, ram);
    z.sample = ref;
    z.root = std::min(127, h.root);
    z.gain_db = h.vol * 0.5;
    z.reverse = h.flags & 1;
    if (loop_before(h)) {   // (the loop again after the body: see built_frames)
        Loop l;
        l.start = int64_t(h.end - h.start) + 1;
        l.end = built_frames(h) - 1;
        z.loops.push_back(l);
    } else if (loops_ok(h) && h.loop >= h.start) {
        Loop l;
        l.start = int64_t(h.loop - h.start);
        l.end = int64_t(h.end - h.start);
        z.loops.push_back(l);
    }
    z.one_shot = h.flags & 4;
    return true;
}

// ---- 2.4.18 the layer's envelopes and filter onto Omni's zone ----
// a time byte: steps on the editor's grid + 3 (0-2 s in 0.02 s, 2-5 s in 0.04, 5-10 s in 0.1, 10-15 s in 0.5,
// 15-25 s in 1, 25-60 s in 5). 2.4.19b (user: "release times are off on all the presets, way too long"): the byte is
// SIGNED - Rel1 is mostly -1, -7, -13, -24, -28 (1264 of 4389 layers -1): read unsigned they were 30-60 s (Ballad
// Organ 60 s); its size on the grid gives releases of median 0.3-0.5 s, 90 % under 2 s, as factory programs sound
double k2_time(uint8_t b) {
    const int v = std::abs(int(int8_t(b)));
    int n = v > 3 ? v - 3 : 0;
    static const struct { int steps; double step; } GRID[] = {{100, 0.02}, {75, 0.04}, {50, 0.1}, {10, 0.5}, {10, 1.0}, {7, 5.0}};
    double t = 0;
    for (const auto &g : GRID) { const int k = std::min(n, g.steps); t += k * g.step; n -= k; if (!n) break; }
    return t;
}
// 7 (time, level %) stages: 3 attack, 1 decay, 3 release; a stage with time and level 0 is unused (keeps the level)
// -> Omni's attack (to the highest attack level), decay (the rest of the attack and the decay stage), sustain (the
// level they end on, of that peak), release (the release stages' time). false when it has no level at all
bool k2_envelope(const uint8_t *e, Envelope &out) {
    double t[7], lv[7];
    bool used[7];
    for (int i = 0; i < 7; i++) {
        t[i] = k2_time(e[2 * i]);
        lv[i] = std::max(0.0, std::min(100.0, double(int8_t(e[2 * i + 1]))));
        used[i] = e[2 * i] || e[2 * i + 1];
    }
    double peak = 0, level = 0, attack = 0, decay = 0;
    int peak_at = -1;
    for (int i = 0; i < 3; i++) if (used[i] && lv[i] > peak) { peak = lv[i]; peak_at = i; }
    if (peak <= 0) return false;
    for (int i = 0; i <= peak_at; i++) if (used[i]) attack += t[i];
    level = peak;
    for (int i = peak_at + 1; i < 4; i++) if (used[i]) { decay += t[i]; level = lv[i]; }
    double release = 0;   // (to silence: the stages after one that reaches 0 % are never heard)
    for (int i = 4; i < 7; i++) if (used[i]) { release += t[i]; if (lv[i] <= 0) break; }
    out.set = true;
    out.attack = attack;
    out.decay = decay;
    out.sustain = level / peak;
    out.release = std::max(0.015, release);   // (0 s on the device is a short fade, not a click)
    return true;
}
// F1 [6]: the cutoff modulation depth, piece-wise linear through 0 = 0, 40 = 1200, 127 = 10800 cents (signed)
double k2_depth_cents(uint8_t b) {
    const int v = int8_t(b), a = std::abs(v);
    const double c = a <= 40 ? a * 30.0 : 1200.0 + (a - 40) * (9600.0 / 87.0);
    return v < 0 ? -c : c;
}
void k2_sound(const KLayer &l, Zone &z) {
    if (l.enc_mode == 0 && l.has_env) {   // the program's own amp envelope (mode 1: the samples' natural one, as they are)
        Envelope e;
        if (k2_envelope(l.env, e)) z.amp_env = e;
    }
    // The filter is decoded but NOT applied (2.4.18): over the downloads' 14367 filtered zones the cutoff byte alone
    // gives a median of 466 Hz (10 % under 50 Hz), and F1 [3] (0, 50, 20, 25 ...) and the page's second modulation
    // source, which open it on the device (key tracking ...), are not confirmed: applied, most of these sounds would
    // play muffled. Set K2_FILTERS once a recording of the device confirms them.
    constexpr bool K2_FILTERS = false;
    if (!K2_FILTERS || !l.has_f1 || l.alg < 0) return;
    const int ft = l.f1[0];
    FilterType type = FilterType::None;
    int poles = 2;
    bool res = true;
    if (l.alg == 16 && ft == 15) { type = FilterType::LowPass; poles = 1; res = false; }
    else if (l.alg == 5 && ft == 2) { type = FilterType::LowPass; poles = 2; }
    else if (l.alg == 5 && ft == 3) { type = FilterType::BandPass; poles = 2; res = false; }
    else if (l.alg == 1 && ft == 50) { type = FilterType::LowPass; poles = 4; }
    else if (l.alg == 1 && ft == 54) { type = FilterType::HighPass; poles = 4; }
    else if (l.alg == 1 && (ft == 55 || ft == 56)) { type = ft == 55 ? FilterType::BandPass : FilterType::BandReject; poles = 4; }
    if (type == FilterType::None) return;   // (no filter, or a DSP function Omni has no match for: left out)
    Filter f;
    f.type = type;
    f.poles = poles;
    f.cutoff = std::max(16.0, std::min(MAX_CUTOFF_HZ, 440.0 * std::pow(2.0, (int8_t(l.f1[1]) - 9) / 12.0)));
    if (res && l.has_f2 && l.f2[0] == 16) f.resonance = std::max(0.0, std::min(1.0, l.f2[1] * 0.5 / MAX_RESONANCE_DB));
    const int src = l.f1[5];
    if (src == 121 && l.has_env2) {   // ENV2, the filter envelope
        Envelope e;
        if (k2_envelope(l.env2, e)) { f.env = e; f.env_depth = std::max(-1.0, std::min(1.0, k2_depth_cents(l.f1[6]) / MAX_ENVELOPE_DEPTH)); }
    } else if (src == 100) f.vel_depth = std::max(-1.0, std::min(1.0, k2_depth_cents(l.f1[6]) / 9600.0));   // the attack velocity
    z.filter = f;
}

void add_keymap(const RomPtr &rom, const KKeymap &k, const KLayer &layer, Instrument &inst, std::set<std::string> &missing,
                std::map<int64_t, SampleRefPtr> &refs) {
    int group = int(inst.groups.size());
    inst.groups.push_back(Group{k.name});
    for (size_t t = 0; t < k.tables.size(); t++) {
        // each run of dynamic levels on this table is a velocity range
        for (int lv = 0; lv < 8; lv++) {
            if (k.level[lv] != int(t) || (lv > 0 && k.level[lv - 1] == int(t))) continue;
            int top = lv;
            while (top + 1 < 8 && k.level[top + 1] == int(t)) top++;
            int vel_lo = std::max(lv * 16, layer.vel_lo), vel_hi = std::min(top * 16 + 15, layer.vel_hi);
            if (vel_lo > vel_hi) continue;
            const std::vector<KEntry> &es = k.tables[t];
            for (size_t i = 0; i < es.size();) {
                if (!es[i].sub) { i++; continue; }
                size_t j = i;
                while (j + 1 < es.size() && es[j + 1] == es[i]) j++;
                // entry e covers the pitches from base + e * cents up to the next entry's; the first and last
                // entries reach the ends of the keyboard (a one-entry keymap, cents 32767, is one sample on all keys)
                auto first_key = [&](size_t e) { return 12 + int(std::ceil((k.base_pitch + double(e) * k.cents) / 100.0)); };
                int key_lo = i == 0 ? 0 : first_key(i), key_hi = j + 1 == es.size() ? 127 : first_key(j + 1) - 1;
                int lo = std::max({key_lo, layer.lo, 0}), hi = std::min({key_hi, layer.hi, 127});
                const KSample *s = sample_of(*rom, es[i].sample);
                // "Silence" (3 zero words) fills the unused keys of the K2000R's drum keymaps
                if (s && s->name == "Silence") { i = j + 1; continue; }
                Zone z;
                if (lo <= hi && s && make_zone(rom, *s, es[i].sub, z, missing, refs, es[i].sample)) {
                    z.name = s->name.empty() ? k.name : s->name;
                    z.key_lo = lo;
                    z.key_hi = hi;
                    z.vel_lo = vel_lo;
                    z.vel_hi = vel_hi;
                    // the K2000 tracks around C4: (key - 60) * kt + 60 + transpose + tuning - root
                    z.key_tracking = layer.key_tracking;
                    z.tune = es[i].tune / 100.0 + layer.transpose + (60 - z.root) * (1 - layer.key_tracking);
                    z.group = group;
                    k2_sound(layer, z);   // 2.4.18
                    inst.zones.push_back(std::move(z));
                }
                i = j + 1;
            }
        }
    }
}

// ---- the reader ----

bool probe_rom(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    return sound_chip(h, n, size) >= 0 || setup_half(h, n, size) >= 0;
}

std::vector<PresetInfo> list_rom(VolumePtr vol, const std::string &path) {
    RomPtr rom = open_rom(*vol, path);
    std::vector<PresetInfo> out;
    char num[24];
    for (size_t i = 0; i < rom->programs.size(); i++) {
        std::snprintf(num, sizeof num, "%03d ", rom->programs[i].id);
        out.push_back({num + rom->programs[i].name, int(i)});
    }
    for (size_t i = 0; i < rom->spare_keymaps.size(); i++) {
        int id = rom->spare_keymaps[i];
        std::snprintf(num, sizeof num, "Keymap %03d ", id);
        out.push_back({num + rom->keymaps.at(id).name, int(rom->programs.size() + i)});
    }
    return out;
}

Instrument load_rom(VolumePtr vol, const std::string &path, int index) {
    RomPtr rom = open_rom(*vol, path);
    size_t n_programs = rom->programs.size();
    if (index < 0 || size_t(index) >= n_programs + rom->spare_keymaps.size()) throw ParseError("no such K2000 program");
    Instrument inst;
    inst.format = "Kurzweil K2000 ROM";
    inst.library = "Kurzweil K2000";
    inst.groups.clear();
    std::set<std::string> missing;
    std::map<int64_t, SampleRefPtr> refs;
    if (size_t(index) < n_programs) {
        const KProgram &p = rom->programs[size_t(index)];
        inst.name = p.name;
        inst.category = "Programs";
        for (const KLayer &l : p.layers) {
            auto k = rom->keymaps.find(l.keymap);
            if (k == rom->keymaps.end()) { inst.warnings.push_back("keymap " + std::to_string(l.keymap) + " not in the setup ROM"); continue; }
            add_keymap(rom, k->second, l, inst, missing, refs);
        }
    } else {
        int id = rom->spare_keymaps[size_t(index) - n_programs];
        const KKeymap &k = rom->keymaps.at(id);
        inst.name = k.name;
        inst.category = "Keymaps";
        add_keymap(rom, k, KLayer{}, inst, missing, refs);
    }
    for (const std::string &m : missing) inst.warnings.push_back("needs " + m);
    if (inst.groups.empty()) inst.groups.push_back(Group{});
    if (inst.zones.empty()) throw ParseError("nothing to play: " + inst.name + (missing.empty() ? "" : " (needs " + *missing.begin() + ")"));
    finish_instrument(inst);
    return inst;
}

// ---- 2.4.18 the .KRZ files ----

std::string lower(std::string t) { for (char &c : t) c = char(std::tolower(static_cast<unsigned char>(c))); return t; }

// a folder holding a K2000 setup ROM pair (hi and lo chips)
bool has_setup(Volume &vol, const std::string &dir) {
    bool hi = false, lo = false;
    int looked = 0;
    try {
        for (const DirEntry &e : vol.list(dir)) {
            if (e.dir || e.size < 0x10000 || e.size > 0x200000 || e.size % 0x10000) continue;
            if (++looked > 96) break;   // (a big folder of other things: not a ROM folder)
            try {
                std::vector<uint8_t> h = vol.open(path_join(dir, e.name))->head(32);
                int half = setup_half(h.data(), h.size(), e.size);
                hi = hi || half == 0;
                lo = lo || half == 1;
            } catch (const std::exception &) {}
            if (hi && lo) return true;
        }
    } catch (const std::exception &) {}
    return false;
}

// the K2000 ROM for a .KRZ file: its folder, then each folder above it (4 up) and the folders there named for the
// K2000 or Kurzweil ("" when none)
std::string find_rom_dir(Volume &vol, const std::string &file_dir) {
    static std::mutex m;
    static std::map<std::string, std::string> found;   // a file's folder -> the ROM's (or "")
    {
        std::lock_guard<std::mutex> lock(m);
        auto it = found.find(file_dir);
        if (it != found.end()) return it->second;
    }
    std::string dir = file_dir, hit;
    for (int up = 0; up <= 4 && hit.empty(); up++) {
        if (has_setup(vol, dir)) { hit = dir; break; }
        try {
            for (const DirEntry &e : vol.list(dir)) {
                if (!e.dir) continue;
                const std::string n = lower(e.name);
                if (n.find("k2000") == std::string::npos && n.find("k2vx") == std::string::npos && n.find("kurzweil") == std::string::npos) continue;
                const std::string sub = path_join(dir, e.name);
                if (sub != file_dir && has_setup(vol, sub)) { hit = sub; break; }
            }
        } catch (const std::exception &) {}
        const std::string parent = path_dir(dir);
        if (parent == dir || parent.empty()) break;
        dir = parent;
    }
    std::lock_guard<std::mutex> lock(m);
    found[file_dir] = hit;
    return hit;
}

std::mutex krz_mutex;
std::map<std::string, RomPtr> krzs;

RomPtr open_krz(Volume &vol, const std::string &path) {
    BlobPtr blob = vol.open(path);
    const std::string key = path + "#" + std::to_string(blob->size());
    {
        std::lock_guard<std::mutex> lock(krz_mutex);
        auto it = krzs.find(key);
        if (it != krzs.end()) return it->second;
    }
    std::vector<uint8_t> h = blob->head(0x20);
    if (h.size() < 0x20 || std::memcmp(h.data(), "PRAM", 4)) throw ParseError("not a K2000 file");
    const uint64_t osize = u32(&h[4]), full = u32(&h[8]);
    if (osize < 0x20 || osize > blob->size()) throw ParseError("K2000 file: bad header");
    if (osize > (8u << 20)) throw ParseError("K2000 file: object area too big");
    std::vector<uint8_t> d = blob->head(size_t(osize));
    auto out = std::make_shared<Rom>();
    // the ROM's objects and chips first, the file's over them
    const std::string rom_dir = find_rom_dir(vol, path_dir(path));
    if (!rom_dir.empty()) {
        try {
            out->under = open_all_roms(vol, rom_dir);
            if (out->under.empty()) out->rom_found = false;
            else {
                for (int k = 0; k < N_CHIPS; k++) out->chip[k] = out->under[0]->chip[k];
                out->key = out->under[0]->key + "\n";
            }
        } catch (const std::exception &) { out->rom_found = false; }
    } else out->rom_found = false;
    out->key += path;
    std::set<int> used;
    read_objects(d, 0x20, d.size(), *out, used, true);
    std::sort(out->programs.begin(), out->programs.end(), [](const KProgram &a, const KProgram &b) { return a.id < b.id; });
    if (out->programs.empty())   // (a file of keymaps only: they are its presets)
        for (const auto &k : out->keymaps) if (k.first >= 200 && k.second.name != "Silence") out->spare_keymaps.push_back(k.first);
    if (out->programs.empty() && out->spare_keymaps.empty()) throw ParseError("no programs in this K2000 file");
    out->ram = blob;
    out->ram_off = osize;
    out->ram_words = (blob->size() - osize) / 2;
    if (full > blob->size() - osize)
        out->krz_note = "the rest of its samples (a two-disk file: join DISK2's part to this one)";
    std::lock_guard<std::mutex> lock(krz_mutex);
    if (krzs.size() >= 24) krzs.clear();   // (a few banks' objects; the ROM's own are shared)
    krzs[key] = out;
    return out;
}

bool probe_krz(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    return n >= 12 && size >= 0x24 && !std::memcmp(h, "PRAM", 4) && u32(h + 4) >= 0x20 && u32(h + 4) <= size;
}

std::vector<PresetInfo> list_krz(VolumePtr vol, const std::string &path) {
    RomPtr rom = open_krz(*vol, path);
    std::vector<PresetInfo> out;
    char num[24];
    for (size_t i = 0; i < rom->programs.size(); i++) {
        std::snprintf(num, sizeof num, "%03d ", rom->programs[i].id);
        out.push_back({num + rom->programs[i].name, int(i)});
    }
    for (size_t i = 0; i < rom->spare_keymaps.size(); i++) {
        const int id = rom->spare_keymaps[i];
        std::snprintf(num, sizeof num, "Keymap %03d ", id);
        out.push_back({num + rom->keymaps.at(id).name, int(rom->programs.size() + i)});
    }
    return out;
}

Instrument load_krz(VolumePtr vol, const std::string &path, int index) {
    RomPtr rom = open_krz(*vol, path);
    const size_t n_programs = rom->programs.size();
    if (index < 0 || size_t(index) >= n_programs + rom->spare_keymaps.size()) throw ParseError("no such K2000 program");
    Instrument inst;
    inst.format = "Kurzweil K2000 file";
    inst.library = "Kurzweil K2000";
    inst.groups.clear();
    std::set<std::string> missing;
    std::map<int64_t, SampleRefPtr> refs;
    if (size_t(index) < n_programs) {
        const KProgram &p = rom->programs[size_t(index)];
        inst.name = p.name;
        inst.category = "Programs";
        for (const KLayer &l : p.layers) {
            const KKeymap *k = keymap_of(*rom, l.keymap);
            if (!l.keymap) {   // a layer of the K2000's DSP oscillators (VAST synth): no samples to play
                missing.insert("its synth layers (the K2000's DSP oscillators, no samples: not played)");
                continue;
            }
            if (!k) {
                missing.insert(!rom->rom_found ? "the K2000 ROM (its setup and sound chips: a \"Kurzweil K2000\" folder above this file)"
                                               : "keymap " + std::to_string(l.keymap) + " (not in this file or the ROM: a ROM block not read)");
                continue;
            }
            add_keymap(rom, *k, l, inst, missing, refs);
        }
    } else {
        const KKeymap &k = rom->keymaps.at(rom->spare_keymaps[size_t(index) - n_programs]);
        inst.name = k.name;
        inst.category = "Keymaps";
        add_keymap(rom, k, KLayer{}, inst, missing, refs);
    }
    for (const std::string &m : missing) inst.warnings.push_back("needs " + m);
    if (inst.groups.empty()) inst.groups.push_back(Group{});
    if (inst.zones.empty()) throw ParseError("nothing to play: " + inst.name + (missing.empty() ? "" : " (needs " + *missing.begin() + ")"));
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_kurzweil_k2000() {
    register_reader({"Kurzweil K2000 ROM", "", probe_rom, list_rom, load_rom});
    register_reader({"Kurzweil K2000 file", "krz", probe_krz, list_krz, load_krz});   // 2.4.18
}

}  // namespace omni

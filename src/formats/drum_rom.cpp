// Omni Sampler: vintage drum machine sound chips (LinnDrum, Oberheim DMX / DX, Roland TR-707, Sequential Drumtraks,
// E-mu Drumulator, Simmons SDS, Solton, Roland TR-505 / TR-626, Yamaha PSR-60 / PSR-70, and the third-party EPROMs
// made for them). A chip dump is known by the CRC-32
// of its first 4 KB and its size (tests/gen_drum_roms.py builds the table from MAME's sets and the archive.org
// dumps), so any file name works: .bin, MAME's names (21kick.u7, hn61256p_c71_15179694.ic34 ...). Tapping a chip
// lists the kits its folder holds (every set whose chips are there) and their sounds one by one; a kit lays its
// sounds out on the GM drum keys, closed / open hats choking each other. The formats, rates and chip layouts are in
// the generator's notes.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include "../third_party/tinf/tinf.h"
#include "format.hpp"

namespace omni {
namespace {

enum DrumFmt : uint8_t { DAC76, U8, S8, U8NEG, U6, RYP4 };
struct DrumChip { uint32_t crc, size; uint8_t xform; };      // xform: see transformed()
struct DrumRun { uint16_t first, count; };                       // chips DRUM_RUN_CHIPS[first ..], one after another
struct DrumSound { const char *name; uint8_t cat, fmt, stride, phase; uint32_t rate; uint16_t run; uint32_t offset, frames, decay_ms; };
struct DrumSet { const char *name; uint16_t first, count; };

#include "drum_rom_table.inc"

constexpr size_t N_CHIPS = sizeof DRUM_CHIPS / sizeof DRUM_CHIPS[0];
constexpr size_t N_SETS = sizeof DRUM_SETS / sizeof DRUM_SETS[0];
constexpr uint32_t MAX_CHIP = 262144;

// the table's chip for a dump: its first 4 KB's CRC and its size (DRUM_CHIPS is sorted by both)
int chip_of(const uint8_t *head, size_t n, uint64_t size) {
    if (size > MAX_CHIP || size < 1024 || n < std::min<uint64_t>(size, 4096)) return -1;
    uint32_t crc = tinf_crc32(head, unsigned(std::min<uint64_t>(size, 4096)));
    const DrumChip *end = DRUM_CHIPS + N_CHIPS;
    const DrumChip *it = std::lower_bound(DRUM_CHIPS, end, DrumChip{crc, uint32_t(size), 0}, [](const DrumChip &a, const DrumChip &b) {
        return a.crc != b.crc ? a.crc < b.crc : a.size < b.size;
    });
    return it != end && it->crc == crc && it->size == size ? int(it - DRUM_CHIPS) : -1;
}

int16_t sample_of(uint8_t fmt, uint8_t b) {
    static const int LEV[8] = {0, 33, 99, 231, 495, 1023, 2079, 4191};
    switch (fmt) {
    case DAC76: {                                                 // AM6070 / DAC-76 companding: sign, segment, step
        int c = (b >> 4) & 7, v = LEV[c] + (b & 15) * (2 << c);
        return int16_t((b & 0x80 ? v : -v) * 4);
    }
    case U8: return int16_t((int(b) - 128) * 256);
    case S8: return int16_t(int8_t(b) * 256);
    case U8NEG: return int16_t(std::min(32767, (128 - int(b)) * 256));   // TR-707: larger values more negative
    case U6: return int16_t((int(b & 0xFC) - 128) * 256);                  // TR-707 cymbals: the top 6 bits
    case RYP4: {                                                  // Yamaha YM2154: sign, shift, mantissa (MAME ym2154)
        int v = (0x21 | (b & 15) << 1) << ((b >> 4) & 7);
        return int16_t((b & 0x80 ? -v : v) * 4);
    }
    default: return 0;
    }
}

// A chip's bytes as its sounds' offsets count them. 1: the raw TR-626 mask ROM (address lines 6 and 8 swapped,
// two sounds on the even / odd bytes of 8 KB, cymbals in 2 or 4 blocks of 8 KB read in turn) laid out plainly, as in
// the community's TR-626_Sound_Rom_UnScrambled.bin (byte for byte).
std::vector<uint8_t> transformed(std::vector<uint8_t> d, uint8_t xform) {
    if (xform != 1 || d.size() != 0x40000) return d;
    std::vector<uint8_t> s(d.size());
    for (size_t a = 0; a < d.size(); a++) s[a] = d[(a & ~size_t(0x140)) | ((a >> 6) & 1) << 8 | ((a >> 8) & 1) << 6];
    std::vector<uint8_t> out = s;
    for (size_t b : {0x8000, 0xA000, 0xC000, 0x28000, 0x2A000, 0x2C000})
        for (size_t k = 0; k < 4096; k++) { out[b + k] = s[b + 2 * k]; out[b + 4096 + k] = s[b + 2 * k + 1]; }
    for (auto [b, nb] : {std::pair<size_t, size_t>{0x10000, 4}, {0x18000, 2}, {0x30000, 4}, {0x38000, 2}})
        for (size_t k = 0; k < nb * 8192; k++) out[b + k] = s[b + (k % nb) * 8192 + k / nb];
    return out;
}

// ---- a folder's chips ----

struct Folder {
    std::map<int, std::string> chips;                           // table chip -> file
    std::vector<int> sets;                                      // the sets found there
};

const std::vector<std::vector<int>> &set_chips() {              // each set's chips, and how many sets use each chip
    static std::vector<std::vector<int>> v = [] {
        std::vector<std::vector<int>> out(N_SETS);
        for (size_t s = 0; s < N_SETS; s++) {
            std::set<int> c;
            for (int i = DRUM_SETS[s].first; i < DRUM_SETS[s].first + DRUM_SETS[s].count; i++) {
                const DrumRun &r = DRUM_RUNS[DRUM_SOUNDS[i].run];
                for (int k = 0; k < r.count; k++) c.insert(DRUM_RUN_CHIPS[r.first + k]);
            }
            out[s].assign(c.begin(), c.end());
        }
        return out;
    }();
    return v;
}

bool has_run(const Folder &f, const DrumRun &r) {
    for (int k = 0; k < r.count; k++)
        if (!f.chips.count(DRUM_RUN_CHIPS[r.first + k])) return false;
    return true;
}

std::mutex folders_mutex;
std::map<std::string, std::shared_ptr<const Folder>> folders;

std::shared_ptr<const Folder> scan(Volume &vol, const std::string &dir) {
    {
        std::lock_guard<std::mutex> lock(folders_mutex);
        auto it = folders.find(dir);
        if (it != folders.end()) return it->second;
    }
    auto f = std::make_shared<Folder>();
    for (const DirEntry &e : vol.list(dir)) {
        if (e.dir || e.size > MAX_CHIP || e.size < 1024) continue;
        try {
            std::vector<uint8_t> h = vol.open(path_join(dir, e.name))->head(4096);
            int c = chip_of(h.data(), h.size(), e.size);
            if (c >= 0) f->chips.emplace(c, path_join(dir, e.name));
        } catch (const std::exception &) {}
    }
    // a set is there when one of its own chips (no other set's) is, or, if it has none, when all of its chips are
    std::map<int, int> users;
    for (const auto &cs : set_chips())
        for (int c : cs) users[c]++;
    for (size_t s = 0; s < N_SETS; s++) {
        const std::vector<int> &cs = set_chips()[s];
        bool own = false, any_own = false, all = true;
        for (int c : cs) {
            bool here = f->chips.count(c) != 0;
            if (users[c] == 1) { any_own = true; own |= here; }
            all &= here;
        }
        bool found = any_own ? own : all;
        if (!found) continue;
        bool playable = false;
        for (int i = DRUM_SETS[s].first; i < DRUM_SETS[s].first + DRUM_SETS[s].count && !playable; i++)
            playable = has_run(*f, DRUM_RUNS[DRUM_SOUNDS[i].run]);
        if (playable) f->sets.push_back(int(s));
    }
    std::lock_guard<std::mutex> lock(folders_mutex);
    folders[dir] = f;
    return f;
}

std::vector<int> sounds_of(const Folder &f, int set) {
    std::vector<int> out;
    for (int i = DRUM_SETS[set].first; i < DRUM_SETS[set].first + DRUM_SETS[set].count; i++)
        if (has_run(f, DRUM_RUNS[DRUM_SOUNDS[i].run])) out.push_back(i);
    return out;
}

SampleRefPtr sample(VolumePtr vol, const Folder &f, const DrumSound &s) {
    const DrumRun &run = DRUM_RUNS[s.run];
    std::vector<std::string> paths;
    std::vector<uint8_t> xforms;
    for (int k = 0; k < run.count; k++) {
        paths.push_back(f.chips.at(DRUM_RUN_CHIPS[run.first + k]));
        xforms.push_back(DRUM_CHIPS[DRUM_RUN_CHIPS[run.first + k]].xform);
    }
    auto r = std::make_shared<SampleRef>();
    r->name = s.name;
    r->rate = int(s.rate);
    r->channels = 1;
    r->frames = s.frames;
    r->key = "drumrom:" + paths[0] + ":" + std::to_string(run.count) + ":" + std::to_string(s.offset) + ":" +
             std::to_string(s.stride) + ":" + std::to_string(s.phase) + ":" + std::to_string(s.fmt);
    const uint8_t fmt = s.fmt, stride = s.stride, phase = s.phase;
    const uint32_t offset = s.offset, frames = s.frames, rate = s.rate;
    r->decode = [vol, paths, xforms, fmt, stride, phase, offset, frames, rate]() -> PcmPtr {
        std::vector<uint8_t> data;
        for (size_t i = 0; i < paths.size(); i++) {
            std::vector<uint8_t> d = transformed(vol->open(paths[i])->all(MAX_CHIP), xforms[i]);
            data.insert(data.end(), d.begin(), d.end());
        }
        auto pcm = std::make_shared<Pcm>();
        pcm->rate = int(rate);
        pcm->channels = 1;
        pcm->data.resize(frames);
        for (uint32_t k = 0; k < frames; k++) {
            size_t at = size_t(offset) + phase + size_t(k) * stride;
            if (at >= data.size()) throw ParseError("drum ROM sound past the end of its chips");
            pcm->data[k] = sample_of(fmt, data[at]);
        }
        return pcm;
    };
    return r;
}

// ---- kits ----

Zone zone_of(VolumePtr vol, const Folder &f, const DrumSound &s) {
    Zone z;
    z.name = s.name;
    z.sample = sample(vol, f, s);
    z.one_shot = true;
    if (s.decay_ms) {                                           // a closed hat: the open one's sample, cut short
        z.amp_env.set = true;
        z.amp_env.decay = s.decay_ms / 1000.0;
        z.amp_env.sustain = 0;
        z.amp_env.release = s.decay_ms / 1000.0;
        z.one_shot = false;
    }
    return z;
}

// 2.4.11 (user): the whole kit on ONE layer, one sound per key with no gaps, from C1 (36) up, grouped by kind so the
// keys read like a drum map: kicks, snares, rim, claps, hats (closed, pedal, open), toms, cymbals, then percussion and
// effects (was: GM keys, with gaps). Closed / pedal / open hats still choke each other.
Instrument load_kit(VolumePtr vol, const Folder &f, int set) {
    Instrument inst;
    inst.name = DRUM_SETS[set].name;
    inst.format = "Drum machine ROM";
    static const int ORDER[] = {1, 2, 3, 4, 5, 7, 6, 8, 9, 10, 22, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 25, 23, 0, 24};
    auto rank = [](int cat) {
        for (size_t i = 0; i < sizeof ORDER / sizeof ORDER[0]; i++) if (ORDER[i] == cat) return int(i);
        return 99;
    };
    std::vector<int> snd = sounds_of(f, set);
    std::stable_sort(snd.begin(), snd.end(), [&](int a, int b) { return rank(DRUM_SOUNDS[a].cat) < rank(DRUM_SOUNDS[b].cat); });
    bool open_hat = false;
    for (int i : snd) open_hat |= DRUM_SOUNDS[i].cat == 6;
    int key = 36;
    for (int i : snd) {
        if (key > 127) { inst.warnings.push_back(std::string("no key left for ") + DRUM_SOUNDS[i].name); continue; }
        const DrumSound &s = DRUM_SOUNDS[i];
        Zone z = zone_of(vol, f, s);
        z.key_lo = z.key_hi = z.root = key++;
        if (open_hat && (s.cat == 5 || s.cat == 6 || s.cat == 7)) z.exclusive_group = 1;
        inst.zones.push_back(std::move(z));
    }
    return inst;
}

// ---- the reader ----

bool probe_chip(const std::string &, const uint8_t *h, size_t n, uint64_t size) { return chip_of(h, n, size) >= 0; }

std::vector<PresetInfo> list_chip(VolumePtr vol, const std::string &path) {
    std::shared_ptr<const Folder> f = scan(*vol, path_dir(path));
    std::vector<PresetInfo> out;
    for (int s : f->sets) {
        std::vector<int> snd = sounds_of(*f, s);
        out.push_back({std::string(DRUM_SETS[s].name) + " kit (" + std::to_string(snd.size()) + ")", s * 1000});
        for (size_t k = 0; k < snd.size(); k++)
            out.push_back({std::string(DRUM_SETS[s].name) + ": " + DRUM_SOUNDS[snd[k]].name, s * 1000 + 1 + int(k)});
    }
    return out;
}

Instrument load_chip(VolumePtr vol, const std::string &path, int index) {
    std::shared_ptr<const Folder> f = scan(*vol, path_dir(path));
    int set = index / 1000, k = index % 1000;
    if (std::find(f->sets.begin(), f->sets.end(), set) == f->sets.end()) throw ParseError("no such drum ROM kit here");
    Instrument inst;
    if (k == 0) inst = load_kit(vol, *f, set);
    else {
        std::vector<int> snd = sounds_of(*f, set);
        if (k > int(snd.size())) throw ParseError("no such drum ROM sound");
        const DrumSound &s = DRUM_SOUNDS[snd[size_t(k - 1)]];
        inst.name = std::string(DRUM_SETS[set].name) + ": " + s.name;
        inst.format = "Drum machine ROM";
        Zone z = zone_of(vol, *f, s);
        z.root = 60;                                            // one sound across the keyboard, C4 at its own pitch
        inst.zones.push_back(std::move(z));
    }
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_drum_rom() {
    // probe only (no extension: MAME's names end in .u7, .ic34 ...); registered late so other readers' .bin go first
    register_reader({"Drum machine ROM", "", probe_chip, list_chip, load_chip});
}

}  // namespace omni

// Omni Sampler: Korg M1 / M1R / M1EX SysEx (.syx): program banks, single programs and combinations, played with the M1's
// ROM multisounds. An M1 file holds settings only; the samples are searched for (find_near), in this order:
//   - the M1's own sample ROM, dumped as the eight 512 KB EPROMs ic36.bin ... ic43.bin (MAME's korgm1 set). Where each
//     multisound and drum sound lies in it comes from the M1 OS (korg_m1_rom.inc, made by tests/gen_m1_rom.py): 16-bit
//     words at 31250 Hz, the 23 DWGS waves as single cycles. The M1EX card bank (100+) is not in these chips.
//   - Korg's own copy of the multisounds, the TRINITY "M1" sample disk (TFD-4S: 00_PIANO.KMP ... 85_DWGS_.KMP and the
//     drum sounds 01_KICK_.KSF ... 44_METRO.KSF).
// What the format turned out to be (decoded on ~100 banks, 7025 programs; no Korg table was at hand):
//   - Messages F0 42 3n 19 <fn>: 40 one program (data from byte 5), 4C 100 programs, 4D 100 combinations, 51 global
//     (data from byte 6). Korg 7-bit packing: a byte of high bits, then 7 data bytes. 50 all data (packed from byte 8:
//     global, 100 combinations at 861, 100 programs at 13261, then the songs; checked on Factory.syx, Bankc.syx,
//     Universal Collection.syx), 48 sequencer data (songs only). A file may start with a Mac header (MacBinary,
//     ORIGPROG.SYX) before its first message. A bank without kits takes them from a global dump beside it
//     (ORIGPROG.SYX + ORIGGLOB.SYX).
//   - Program, 143 bytes: name 0-9; 10 oscillator mode (0 single, 1 double, 2 drums); 11 bit 4 / 6: OSC1 / OSC2 plays
//     the M1EX card bank (multisound 100 + n); 12 / 14 OSC1 / OSC2 multisound (0-99 ROM, 100+ a PCM card; for drums,
//     12 = kit 0-3); 13 / 15 octave (-1 16', 0 8', +1 4'); 16 OSC2 interval
//     (semitones); 17 OSC2 detune (cents). Per oscillator a 40-byte block from 63 / 103 whose VDA (amp) EG is at +29:
//     attack time, attack level, decay time, break point, slope time, sustain level, release time (0-99). Checked on
//     organs (0 99 99 99 99 99 0), pianos (slope to 0), strings (attack 12-18, sustain 82), bells (decay, sustain 0);
//     octaves on programs named 16' / 8'. Filter, levels and velocity are not identified: the sampler's own apply.
//   - Multisound n of the ROM is the disk's KMP "<n - NT before n>_*.KMP": the disk leaves out the 14 "NT" (no
//     transient) variants, which play their parent multisound from its loop start (the ROM has them).
//   - Combination, 124 bytes: name 0-9, mode 10 (0 single, 1 layer, 2 split, 3 velocity switch, 4 multi); from 36,
//     (program: 0-99 the M1 memory's, 100-199 the card's)
//     8 timbres of 11 bytes: program, level 0-99, transpose, detune (cents), pan, top key, bottom key, top velocity,
//     bottom velocity, (channel), flags (bit 4: off).
//   - Global, 861 bytes: from 21, 4 drum kits x 30 keys x 7 bytes: drum sound (0 "--", 1-44 the M1's as the manual
//     numbers them, 45-85 the M1EX's), key, pan, tune, decay, level, -. (0 is a third of all keys in the collection's
//     kits: empty, not Kick 1.)
//     Pan (here and in timbres): 0 A (left), 1-9 = 9:1 ... 1:9, 10 B (right), 11 / 12 C / D (the effect outs: centre).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include "../core/parse.hpp"
#include "format.hpp"

namespace omni {
namespace {

constexpr size_t PROG = 143, COMBI = 124, KIT_KEYS = 30, KIT_ENTRY = 7, GLOBAL_KITS = 21, GLOBAL = 861;
constexpr size_t ALL_COMBIS = GLOBAL, ALL_PROGS = ALL_COMBIS + 100 * COMBI;   // in an all-data dump
const int NT[] = {24, 43, 47, 49, 53, 55, 57, 59, 62, 65, 67, 68, 75, 76};   // ROM multisounds the disk leaves out
const char *const MODES[] = {"Single", "Layer", "Split", "Velocity Switch", "Multi"};

bool is_nt(int ms) { for (int n : NT) if (n == ms) return true; return false; }
int sx(uint8_t v) { return int(int8_t(v)); }

std::vector<uint8_t> unpack(const uint8_t *p, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n * 7 / 8 + 7);
    for (size_t i = 0; i < n; i += 8) {
        uint8_t hi = p[i];
        for (size_t j = 1; j < 8 && i + j < n; j++) out.push_back(uint8_t(p[i + j] | (((hi >> (j - 1)) & 1) << 7)));
    }
    return out;
}

// one key of a drum kit: drum sound (0-43 the M1's, 44+ the M1EX's or, on a Plus+1 card, the Plus+1's), the key it is
// on, the key it plays as (its pitch), pan -1..1, fine tune, level
struct DrumHit { int sound, key, root; double pan, cents, level_db; };

struct Bank {
    std::vector<std::vector<uint8_t>> progs, combis;
    std::vector<std::vector<DrumHit>> kits;   // empty: no kits in the file (no global dump)
    bool card = false;                        // a Plus+1 program card: its card bank is the Plus+1's
    bool songs = false;                       // sequencer data (function 48) in the file
};

double pan_of(int v) {
    if (v < 0 || v > 10) return 0;   // C / D: the effect outputs
    return (v - 5) / 5.0;
}

bool is_m1(const uint8_t *h, size_t n) { return n >= 6 && h[0] == 0xF0 && h[1] == 0x42 && (h[2] & 0xF0) == 0x30 && h[3] == 0x19; }

Bank read_bank(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(size_t(4) << 20);
    Bank b;
    std::vector<uint8_t> global;
    for (size_t i = 0; i < d.size();) {
        if (d[i] != 0xF0) { i++; continue; }
        size_t e = i + 1;
        while (e < d.size() && d[e] != 0xF7) e++;
        if (e - i > 6 && is_m1(&d[i], e - i)) {
            uint8_t fn = d[i + 4];
            if (fn == 0x40) {
                std::vector<uint8_t> p = unpack(&d[i + 5], e - i - 5);
                if (p.size() >= PROG) b.progs.emplace_back(p.begin(), p.begin() + PROG);
            } else if (fn == 0x50) {
                // all data: 2 more bytes, then (packed) the global data, 100 combinations, 100 programs, the songs
                std::vector<uint8_t> p = unpack(&d[i + 8], e - i - 8);
                if (p.size() >= ALL_PROGS + 100 * PROG) {
                    global.assign(p.begin(), p.begin() + GLOBAL);
                    for (size_t k = 0; k < 100; k++) {
                        b.combis.emplace_back(p.begin() + ALL_COMBIS + k * COMBI, p.begin() + ALL_COMBIS + (k + 1) * COMBI);
                        b.progs.emplace_back(p.begin() + ALL_PROGS + k * PROG, p.begin() + ALL_PROGS + (k + 1) * PROG);
                    }
                }
            } else if (fn == 0x48) {
                b.songs = true;
            } else if (fn == 0x4C || fn == 0x4D || fn == 0x51) {
                std::vector<uint8_t> p = unpack(&d[i + 6], e - i - 6);
                size_t sz = fn == 0x4C ? PROG : COMBI;
                if (fn == 0x51) { if (p.size() >= GLOBAL_KITS + 4 * KIT_KEYS * KIT_ENTRY) global = p; }
                else for (size_t k = 0; k + sz <= p.size() && k / sz < 100; k += sz) (fn == 0x4C ? b.progs : b.combis).emplace_back(p.begin() + k, p.begin() + k + sz);
            }
        }
        i = e + 1;
    }
    // a global dump of zeros (a blank one is common) has no kits
    bool any = false;
    for (size_t k = GLOBAL_KITS; k < global.size(); k++) any |= global[k] != 0;
    if (any)
        for (size_t kit = 0; kit < 4; kit++) {
            b.kits.emplace_back();
            for (size_t k = 0; k < KIT_KEYS; k++) {
                const uint8_t *e = &global[GLOBAL_KITS + (kit * KIT_KEYS + k) * KIT_ENTRY];
                if (e[0] == 0) continue;   // "--": no sound on this key
                // a relative level, -99..+99: about +-12 dB
                b.kits.back().push_back({e[0] - 1, e[1], e[1], pan_of(e[2]), double(sx(e[3])), clampi(sx(e[5]), -99, 99) * 0.12});
            }
        }
    return b;
}

// A program card image (the Plus+1's, 32 KB): "KORG" ..., from 41 the 4 drum kits (30 keys x 10 bytes: sound, bank
// (0x80: a Plus+1 sound, 100 + n), key, pan, (decay?), the key it plays as, fine tune (cents), -, level, -; FF FF =
// empty), from 1241 100 combinations, from 13641 100 programs; the same records as SysEx, not packed.
constexpr size_t CARD = 32768, CARD_KITS = 41, CARD_KIT_ENTRY = 10, CARD_COMBIS = 1241, CARD_PROGS = 13641;

Bank read_card(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(CARD + 1);
    if (d.size() != CARD || std::memcmp(d.data(), "KORG", 4)) throw ParseError("not an M1 program card");
    Bank b;
    b.card = true;
    for (size_t k = 0; k < 100; k++) {
        b.progs.emplace_back(d.begin() + CARD_PROGS + k * PROG, d.begin() + CARD_PROGS + (k + 1) * PROG);
        b.combis.emplace_back(d.begin() + CARD_COMBIS + k * COMBI, d.begin() + CARD_COMBIS + (k + 1) * COMBI);
    }
    for (size_t kit = 0; kit < 4; kit++) {
        b.kits.emplace_back();
        for (size_t k = 0; k < KIT_KEYS; k++) {
            const uint8_t *e = &d[CARD_KITS + (kit * KIT_KEYS + k) * CARD_KIT_ENTRY];
            if (e[1] == 0xFF) continue;
            int sound = e[1] & 0x80 ? 44 + e[0] - 100 : e[0];
            if (sound < 0) continue;
            b.kits.back().push_back({sound, e[2], e[5], pan_of(e[3]), double(sx(e[6])), clampi(sx(e[8]), -99, 99) * 0.12});
        }
    }
    return b;
}

std::string name_of(const std::vector<uint8_t> &r) {
    std::string n;
    for (int i = 0; i < 10; i++) n += r[size_t(i)] >= 32 && r[size_t(i)] < 127 ? char(r[size_t(i)]) : ' ';
    return trim(n);
}

// A combination file often has its programs in a file beside it (Super M Synth C.SYX / Super M Synth P.SYX)
std::vector<std::vector<uint8_t>> sibling_programs(Volume &vol, const std::string &path) {
    std::string stem = path_stem(path);
    if (stem.empty() || (stem.back() != 'C' && stem.back() != 'c')) return {};
    std::string want = lower(stem.substr(0, stem.size() - 1) + "p");
    try {
        for (const DirEntry &e : vol.list(path_dir(path))) {
            if (e.dir || lower(path_stem(e.name)) != want || path_ext(e.name) != "syx") continue;
            Bank b = read_bank(vol, path_join(path_dir(path), e.name));
            if (b.progs.size() == 100) return b.progs;
        }
    } catch (const std::exception &) {}
    return {};
}

// ---- the samples: the M1's own ROM, or Korg's M1 disk ----

constexpr int ROM_RATE = 31250;
constexpr uint32_t CHIP_WORDS = 0x80000;   // a 27C4001: 512K bytes, one byte of 512K words

struct M1Sample { uint32_t start; uint16_t len, loop; };              // loop: the last n words (0 = none)
struct M1Zone { uint8_t top, orig; int8_t tune, level; uint16_t sample; };
struct M1Multi { const char *name; uint16_t first; uint8_t zones; int32_t dwgs; };   // dwgs: the wave block, -1 none
#include "korg_m1_rom.inc"

constexpr uint32_t P1_BANK = 0x400000, P1_CHIP_BYTES = 0x100000;

// The sample ROM: 4 pairs of chips, word n of pair k = ic(40+k) byte n (high) and ic(36+k) byte n (low); with the
// Plus+1's 4 chips beside them (U201.bin ... U204.bin, 512K little-endian words each), its extra sounds too
struct Rom {
    std::string dir;
    BlobPtr hi[4], lo[4], u[4];
    bool plus1 = false;
};
using RomPtr = std::shared_ptr<const Rom>;

struct Source {
    VolumePtr vol;
    std::string disk;   // the M1 disk folder, when there is no ROM
    RomPtr rom;
    bool card = false;  // a Plus+1 program card: the card bank is the Plus+1's
};

std::mutex dirs_mutex;
std::vector<std::string> extra_dirs;   // host folders to search too (the Plugin Library)
std::string cached_disk, cached_rom;   // last found, inner paths of the host volume

// the chip file names in a folder listing ("ic36.bin" ... "ic43.bin", any case); empty when one is missing
std::vector<std::string> chip_files(const std::vector<DirEntry> &list) {
    std::vector<std::string> out(8);
    for (const DirEntry &e : list)
        for (int n = 0; n < 8; n++)
            if (!e.dir && e.size == CHIP_WORDS && equals_ci(e.name, "ic" + std::to_string(36 + n) + ".bin")) out[size_t(n)] = e.name;
    for (const std::string &f : out) if (f.empty()) return {};
    return out;
}

bool is_rom(Volume &vol, const std::string &dir) {
    try { return !chip_files(vol.list(dir)).empty(); } catch (const std::exception &) { return false; }
}

// one listing per folder: find_ci on a miss re-reads every folder from the root
bool is_disk(Volume &vol, const std::string &dir) {
    if (vol.exists(path_join(dir, "00_PIANO.KMP")) && vol.exists(path_join(dir, "85_DWGS_.KMP"))) return true;
    int have = 0;
    try {
        for (const DirEntry &e : vol.list(dir))
            if (!e.dir && (equals_ci(e.name, "00_PIANO.KMP") || equals_ci(e.name, "85_DWGS_.KMP"))) have++;
    } catch (const std::exception &) {}
    return have == 2;
}

using IsDir = bool (*)(Volume &, const std::string &);

// a folder that `is`: <dir> itself or a folder up to `depth` levels below it
bool search(Volume &vol, const std::string &dir, int depth, IsDir is, std::string &found) {
    if (is(vol, dir)) { found = dir; return true; }
    if (depth <= 0) return false;
    std::vector<DirEntry> list;
    try { list = vol.list(dir); } catch (const std::exception &) { return false; }
    if (list.size() > 4000) return false;
    for (const DirEntry &e : list)
        if (e.dir && !e.name.empty() && e.name[0] != '.' && search(vol, path_join(dir, e.name), depth - 1, is, found)) return true;
    return false;
}

// The last one found; else beside the file (each folder above it and two levels under each), else the Plugin Library
// (three levels: <library>/Korg/TRITON_Content_Dump/M1 ...)
std::string find_near(Volume &vol, const std::string &file, IsDir is, std::string &cache) {
    std::vector<std::string> extra;
    {
        std::lock_guard<std::mutex> lock(dirs_mutex);
        if (!cache.empty() && vol.kind() == "Folder" && is(vol, cache)) return cache;
        extra = extra_dirs;
    }
    std::string found;
    std::string dir = path_dir(file), below;
    for (int up = 0; up < 8 && !found.size(); up++) {
        if (is(vol, dir)) { found = dir; break; }
        std::vector<DirEntry> list;
        try { list = vol.list(dir); } catch (const std::exception &) {}
        if (list.size() <= 4000)
            for (const DirEntry &e : list)
                if (e.dir && e.name != below && !e.name.empty() && e.name[0] != '.' && search(vol, path_join(dir, e.name), 1, is, found)) break;
        if (dir.empty() || !found.empty()) break;
        below = path_name(dir);
        dir = path_dir(dir);
    }
    if (found.empty() && vol.kind() == "Folder")
        for (const std::string &lib : extra) {
            std::string inner = lib;
            while (!inner.empty() && inner[0] == '/') inner.erase(0, 1);
            if (!inner.empty() && vol.is_dir(inner) && search(vol, inner, 3, is, found)) break;
        }
    if (!found.empty() && vol.kind() == "Folder") {
        std::lock_guard<std::mutex> lock(dirs_mutex);
        cache = found;
    }
    return found;
}

RomPtr open_rom(Volume &vol, const std::string &dir) {
    std::vector<DirEntry> list = vol.list(dir);
    std::vector<std::string> f = chip_files(list);
    if (f.empty()) throw ParseError("M1 ROM chips missing in " + dir);
    auto rom = std::make_shared<Rom>();
    rom->dir = dir;
    for (int k = 0; k < 4; k++) {
        rom->lo[k] = vol.open(path_join(dir, f[size_t(k)]));
        rom->hi[k] = vol.open(path_join(dir, f[size_t(k + 4)]));
    }
    std::string u[4];
    for (const DirEntry &e : list)
        for (int k = 0; k < 4; k++)
            if (!e.dir && e.size == P1_CHIP_BYTES && equals_ci(e.name, "u20" + std::to_string(k + 1) + ".bin")) u[k] = e.name;
    rom->plus1 = !u[0].empty() && !u[1].empty() && !u[2].empty() && !u[3].empty();
    if (rom->plus1)
        for (int k = 0; k < 4; k++) rom->u[k] = vol.open(path_join(dir, u[k]));
    return rom;
}

SampleRefPtr rom_sample(const RomPtr &rom, uint32_t start, int64_t frames, const std::string &name) {
    auto r = std::make_shared<SampleRef>();
    r->name = name;
    r->rate = ROM_RATE;
    r->channels = 1;
    r->frames = frames;
    r->key = "m1rom:" + rom->dir + ":" + std::to_string(start) + ":" + std::to_string(frames);
    r->decode = [rom, start, frames]() -> PcmPtr {
        auto p = std::make_shared<Pcm>();
        p->rate = ROM_RATE;
        p->channels = 1;
        p->data.resize(size_t(frames));
        bool p1 = start & P1_BANK;
        if (p1 && !rom->plus1) throw ParseError("a Plus+1 sample without the Plus+1 ROM (U201.bin ... U204.bin)");
        for (int64_t done = 0; done < frames;) {
            uint32_t a = (start & ~P1_BANK) + uint32_t(done), chip = a / CHIP_WORDS, off = a % CHIP_WORDS;
            if (chip > 3) throw ParseError("M1 sample past the end of the ROM");
            size_t n = size_t(std::min<int64_t>(frames - done, int64_t(CHIP_WORDS - off)));
            if (p1) {
                std::vector<uint8_t> b = rom->u[chip]->bytes(uint64_t(off) * 2, n * 2);
                for (size_t i = 0; i < n; i++) p->data[size_t(done) + i] = int16_t(uint16_t(b[2 * i] | b[2 * i + 1] << 8));
            } else {
                std::vector<uint8_t> h = rom->hi[chip]->bytes(off, n), l = rom->lo[chip]->bytes(off, n);
                for (size_t i = 0; i < n; i++) p->data[size_t(done) + i] = int16_t(uint16_t(h[i] << 8 | l[i]));
            }
            done += int64_t(n);
        }
        return p;
    };
    return r;
}

// the disk file whose name starts "<nn>_" with that extension
std::string disk_file(Volume &vol, const std::string &disk, int nn, const char *ext) {
    char pre[8];
    std::snprintf(pre, sizeof pre, "%02d_", nn);
    for (const DirEntry &e : vol.list(disk))   // (1154 entries: the host keeps big listings for a few seconds)
        if (!e.dir && !e.name.compare(0, 3, pre) && path_ext(e.name) == ext) return path_join(disk, e.name);
    return "";
}

// ---- programs ----

double eg_time(int v) { return 0.002 + 0.06 * (std::exp(clampi(v, 0, 99) * 0.0577) - 1.0); }   // 0 ~2 ms, 50 ~1 s, 99 ~18 s

// the VDA EG (attack time, attack level, decay time, break point, slope time, sustain, release) as the sampler's ADSR;
// returns the peak level (dB) to apply as gain
double apply_vda(Zone &z, const uint8_t *eg) {
    int at = eg[0], al = clampi(eg[1], 0, 99), dt = eg[2], bp = clampi(eg[3], 0, 99), st = eg[4], sl = clampi(eg[5], 0, 99), rt = eg[6];
    int peak = std::max(1, std::max(al, bp));
    Envelope &e = z.amp_env;
    e.set = true;
    e.attack = eg_time(at) + (bp > al ? eg_time(dt) : 0);             // a swell up to the break point: part of the attack
    e.decay = (bp < al ? eg_time(dt) : 0) + (sl != bp ? eg_time(st) : 0);
    e.sustain = sl >= peak ? 1.0 : double(sl) / peak;
    if (e.decay <= 0) e.decay = 0.002;
    e.release = eg_time(rt);
    return 20.0 * std::log10(peak / 99.0);
}

// a multisound's zones from the ROM: key ranges, original keys and tuning from the OS table
std::vector<Zone> rom_zones(const RomPtr &rom, const M1Multi &m) {
    std::vector<Zone> out;
    int lo = 0;
    for (int i = 0; i < m.zones; i++) {
        const M1Zone &mz = M1_ZONES[m.first + i];
        const M1Sample &s = M1_SAMPLES[mz.sample];
        uint32_t start = m.dwgs >= 0 ? uint32_t(m.dwgs) + s.start : s.start;   // a DWGS wave: its own block
        Zone z;
        for (const char *c = m.name; *c; c++)
            if (*c != ' ' || (!z.name.empty() && z.name.back() != ' ')) z.name += *c;   // "Organ  1" -> "Organ 1"
        z.name += " " + std::to_string(i + 1);
        z.sample = rom_sample(rom, start, s.len, z.name);
        z.key_lo = lo;
        z.key_hi = i + 1 == m.zones ? 127 : mz.top;
        lo = mz.top + 1;
        z.root = mz.orig;
        z.tune = mz.tune / 100.0;
        if (s.loop) {
            Loop l;
            l.start = std::max(0, int(s.len) - int(s.loop));   // a loop a little longer than the sample: all of it
            l.end = s.len - 1;
            z.loops.push_back(l);
        }
        out.push_back(std::move(z));
    }
    return out;
}

// from the disk: multisound n of the ROM is the disk's KMP "<n - NT before n>_*.KMP"; an NT variant plays its parent
// (the multisound before it) from the loop start
std::vector<Zone> disk_zones(const Source &src, int ms, const std::string &label, std::vector<std::string> &warnings) {
    bool nt = is_nt(ms);
    int parent = ms;
    while (is_nt(parent)) parent--;
    int disk_n = parent;
    for (int n : NT) if (n < parent) disk_n--;
    std::string kmp = disk_file(*src.vol, src.disk, disk_n, "kmp");
    if (kmp.empty()) { warnings.push_back(label + " multisound " + std::to_string(ms) + " is not on the M1 disk"); return {}; }
    const FormatReader *r = find_reader(*src.vol, kmp);
    if (!r) throw ParseError("cannot read " + kmp);
    Instrument m = r->load(src.vol, kmp, 0);
    for (auto &z : m.zones)
        if (nt && !z.loops.empty()) z.start = z.loops[0].start;   // no transient: from the loop
    for (auto &w : m.warnings) warnings.push_back(w);
    return std::move(m.zones);
}

void add_osc(Instrument &inst, const Source &src, const uint8_t *p, int o) {
    int ms = p[o ? 14 : 12];
    std::string label = "OSC" + std::to_string(o + 1);
    std::vector<Zone> zones;
    // byte 11 bit 4 / 6: OSC1 / OSC2 plays a multisound of the card bank (100 + n): the M1EX's, or on a Plus+1 card the
    // Plus+1's
    if (p[11] & (o ? 0x40 : 0x10)) {
        constexpr int n_p1 = int(sizeof P1_MULTIS / sizeof P1_MULTIS[0]);
        if (!src.card) { inst.warnings.push_back(label + " plays M1EX multisound " + std::to_string(100 + ms) + ": not available"); return; }
        if (!src.rom || !src.rom->plus1 || ms >= n_p1) {
            inst.warnings.push_back(label + " plays Plus+1 multisound " + std::to_string(100 + ms) + ": needs the Plus+1 ROM dumps "
                                    "(U201.bin ... U204.bin beside ic36.bin ... ic43.bin)");
            return;
        }
        zones = rom_zones(src.rom, P1_MULTIS[ms]);
    } else if (ms >= 100) {
        inst.warnings.push_back(label + " plays PCM card multisound " + std::to_string(ms) + ": not available");
        return;
    } else zones = src.rom ? rom_zones(src.rom, M1_MULTIS[ms]) : disk_zones(src, ms, label, inst.warnings);
    int oct = sx(p[o ? 15 : 13]);
    if (oct < -2 || oct > 2) oct = 0;
    double tune = 12.0 * oct;
    if (o) tune += clampi(sx(p[16]), -12, 12) + clampi(sx(p[17]), -50, 50) / 100.0;
    const uint8_t *vda = p + (o ? 103 : 63) + 29;
    for (auto &z : zones) {
        z.tune += tune;
        z.gain_db += apply_vda(z, vda);
        z.name = label + " " + z.name;
        inst.zones.push_back(std::move(z));
    }
}

void add_drums(Instrument &inst, const Source &src, int kit, const std::vector<std::vector<DrumHit>> &kits) {
    std::map<int, Zone> loaded;
    std::vector<int> missing;   // M1EX / Plus+1 drum sounds without their ROM
    auto sound = [&](int s) -> const Zone * {
        bool extra = s >= 44;
        if (s < 0 || s >= (src.card ? 89 : 85)) return nullptr;
        if (extra && (!src.card || !src.rom || (!src.rom->plus1 && M1_SAMPLES[P1_DRUMS[s - 44]].start & P1_BANK))) {
            if (std::find(missing.begin(), missing.end(), s) == missing.end()) missing.push_back(s);
            return nullptr;
        }
        auto it = loaded.find(s);
        if (it == loaded.end()) {
            Zone z;
            if (src.rom) {
                const M1Sample &m = M1_SAMPLES[extra ? P1_DRUMS[s - 44] : M1_DRUMS[s]];
                z.name = "Drum " + std::to_string(s + 1);
                z.sample = rom_sample(src.rom, m.start, m.len, z.name);
            } else {
                std::string ksf = disk_file(*src.vol, src.disk, s + 1, "ksf");
                const FormatReader *r = ksf.empty() ? nullptr : find_reader(*src.vol, ksf);
                if (!r) return nullptr;
                Instrument d = r->load(src.vol, ksf, 0);
                if (d.zones.empty()) return nullptr;
                z = d.zones[0];
            }
            it = loaded.emplace(s, std::move(z)).first;
        }
        return &it->second;
    };
    auto put = [&](const DrumHit &h) {
        const Zone *m = sound(h.sound);
        if (!m || h.key < 0 || h.key > 127) return;
        Zone z = *m;
        z.key_lo = z.key_hi = h.key;
        z.root = h.root >= 0 && h.root <= 127 ? h.root : h.key;
        z.loops.clear();
        z.pan = h.pan;
        z.tune += h.cents / 100.0;
        z.gain_db += h.level_db;
        z.one_shot = true;
        inst.zones.push_back(std::move(z));
    };
    if (kits.empty()) {
        // no kit in the file: the 44 drum sounds from C2 up
        inst.warnings.push_back("no drum kit in this file (no global data): the M1 drum sounds are laid out from C2");
        for (int s = 0; s < 44; s++) put({s, 36 + s, 36 + s, 0, 0, 0});
        return;
    }
    kit = clampi(kit, 0, int(kits.size()) - 1);
    if (kits[size_t(kit)].empty()) inst.warnings.push_back("drum kit " + std::to_string(kit + 1) + " is empty in this file");
    for (const DrumHit &h : kits[size_t(kit)]) put(h);
    if (!missing.empty()) {
        std::sort(missing.begin(), missing.end());
        std::string w = std::string(src.card ? "Plus+1" : "M1EX") + " drum sound";
        if (missing.size() > 1) w += "s";
        for (size_t i = 0; i < missing.size(); i++) w += (i ? ", " : " ") + std::to_string(missing[i] + 1);
        w += src.card ? ": needs the Plus+1 ROM dumps (U201.bin ... U204.bin)" : ": not available";
        inst.warnings.push_back(w);
    }
}

Instrument load_prog(const Source &src, const std::vector<uint8_t> &p, const std::vector<std::vector<DrumHit>> &global) {
    Instrument inst;
    inst.name = name_of(p);
    inst.format = "Korg M1 program";
    int mode = p[10];
    if (mode == 2) add_drums(inst, src, p[12], global);
    else {
        add_osc(inst, src, p.data(), 0);
        if (mode == 1) add_osc(inst, src, p.data(), 1);
    }
    return inst;
}

Instrument load_combi(const Source &src, const std::vector<uint8_t> &c, const std::vector<std::vector<uint8_t>> &progs,
                      const std::vector<std::vector<DrumHit>> &global) {
    Instrument inst;
    inst.name = name_of(c);
    inst.format = "Korg M1 combination";
    int mode = c[10];
    int timbres = mode == 0 ? 1 : mode <= 3 ? 2 : 8;
    for (int t = 0; t < timbres; t++) {
        const uint8_t *e = &c[36 + size_t(t) * 11];
        int prog = e[0], level = clampi(e[1], 0, 99);
        if (level == 0 || (e[10] & 0x10)) continue;
        // programs 0-99: the M1's memory (a bank file's own); 100-199: the card's (a program card's own)
        int own = src.card ? prog - 100 : prog;
        if (own < 0 || own >= int(progs.size())) {
            inst.warnings.push_back("T" + std::to_string(t + 1) + " plays " + (prog >= 100 ? "card" : "internal") + " program " +
                                    std::to_string(prog % 100) + ": not in this file");
            continue;
        }
        prog = own;
        Instrument m = load_prog(src, progs[size_t(prog)], global);
        int top = e[5] & 127, bottom = e[6] & 127, vtop = e[7] & 127, vbottom = e[8] & 127;
        if (vtop == 0) vtop = 127;
        double tune = clampi(sx(e[2]), -24, 24) + clampi(sx(e[3]), -50, 50) / 100.0;
        for (auto &z : m.zones) {
            z.key_lo = std::max(z.key_lo, std::min(top, bottom));
            z.key_hi = std::min(z.key_hi, std::max(top, bottom));
            if (z.key_lo > z.key_hi) continue;
            z.vel_lo = std::max(z.vel_lo, std::min(vtop, vbottom));
            z.vel_hi = std::min(z.vel_hi, std::max(vtop, vbottom));
            if (z.vel_lo > z.vel_hi) continue;
            z.tune += tune;
            z.gain_db += 20.0 * std::log10(level / 99.0);
            z.pan = clampd(z.pan + pan_of(e[4]), -1, 1);
            inst.zones.push_back(std::move(z));
        }
        for (auto &w : m.warnings) inst.warnings.push_back("T" + std::to_string(t + 1) + ": " + w);
    }
    if (mode >= 0 && mode <= 4 && mode != 1) inst.warnings.insert(inst.warnings.begin(), std::string(MODES[mode]) + " combination");
    return inst;
}

// ---- the reader ----

// an M1 message with data in the first 4 KB: after a short one (KIDNEPRO.SYX: a mode change, F0 42 30 19 4E 04 10 F7),
// another device's or a Mac header (ORIGPROG.SYX: 128 bytes of MacBinary)
bool probe_syx(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    for (size_t i = 0; i + 6 <= n; i++) {
        if (h[i] != 0xF0 || !is_m1(h + i, n - i)) continue;
        uint8_t fn = h[i + 4];
        if (fn == 0x40 || fn == 0x4C || fn == 0x4D || fn == 0x51 || fn == 0x50 || fn == 0x48) return true;
    }
    return false;
}

// a bank without kits: the kits of a global-only dump beside it (a file named ...GLOB... first), not another bank's
std::vector<std::vector<DrumHit>> sibling_kits(Volume &vol, const std::string &path) {
    std::vector<std::vector<DrumHit>> found;
    try {
        std::vector<DirEntry> list = vol.list(path_dir(path));
        std::stable_sort(list.begin(), list.end(), [](const DirEntry &a, const DirEntry &b) {
            return (lower(a.name).find("glob") != std::string::npos) > (lower(b.name).find("glob") != std::string::npos);
        });
        for (const DirEntry &e : list) {
            if (e.dir || path_ext(e.name) != "syx" || e.size > (size_t(1) << 20) || equals_ci(e.name, path_name(path))) continue;
            try {
                Bank b = read_bank(vol, path_join(path_dir(path), e.name));
                if (!b.kits.empty() && b.progs.empty() && b.combis.empty()) return b.kits;   // a global dump of its own
            } catch (const std::exception &) {}
        }
    } catch (const std::exception &) {}
    return found;
}

// programs as 0..99, combinations as 1000 + n (when their programs are in the file or beside it)
std::vector<PresetInfo> list_syx(VolumePtr vol, const std::string &path) {
    Bank b = read_bank(*vol, path);
    std::vector<PresetInfo> out;
    char num[32];
    for (size_t i = 0; i < b.progs.size(); i++) {
        std::snprintf(num, sizeof num, b.progs.size() > 1 ? "P%02zu " : "", i);
        out.push_back({num + name_of(b.progs[i]), int(i)});
    }
    if (!b.combis.empty() && (b.progs.size() == 100 || sibling_programs(*vol, path).size() == 100))
        for (size_t i = 0; i < b.combis.size(); i++) {
            std::snprintf(num, sizeof num, "C%02zu ", i);
            out.push_back({num + name_of(b.combis[i]), 1000 + int(i)});
        }
    if (out.empty()) {
        if (!b.combis.empty()) throw ParseError("M1 combinations without their programs (no program bank beside them)");
        if (!b.kits.empty()) throw ParseError("M1 global data (the drum kits) only: the program banks beside it play their drums with it");
        if (b.songs) throw ParseError("M1 sequencer data (songs): no sounds in this file");
        throw ParseError("no M1 programs in this file");
    }
    return out;
}

Instrument load_from(const Bank &b, VolumePtr vol, const std::string &path, int index) {
    Source src;
    src.vol = vol;
    src.card = b.card;
    std::string rom = find_near(*vol, path, is_rom, cached_rom);
    if (!rom.empty()) src.rom = open_rom(*vol, rom);
    else src.disk = find_near(*vol, path, is_disk, cached_disk);
    if (!src.rom && src.disk.empty())
        throw ParseError("M1 samples not found: put the M1 sample ROM dumps (ic36.bin ... ic43.bin) or Korg's M1 sample disk "
                         "(the folder with 00_PIANO.KMP) beside the M1 files or in the Plugin Library");
    Instrument inst;
    if (index >= 1000) {
        size_t c = size_t(index - 1000);
        if (c >= b.combis.size()) throw ParseError("no such M1 combination");
        std::vector<std::vector<uint8_t>> progs = b.progs.size() == 100 ? b.progs : sibling_programs(*vol, path);
        inst = load_combi(src, b.combis[c], progs, b.kits);
    } else {
        if (index < 0 || size_t(index) >= b.progs.size()) throw ParseError("no such M1 program");
        inst = load_prog(src, b.progs[size_t(index)], b.kits);
    }
    inst.library = "Korg M1";
    inst.category = path_stem(path);
    finish_instrument(inst);
    if (inst.zones.empty())   // say why: the first warning that is not the combination mode
        for (const std::string &w : inst.warnings)
            if (w.find("combination") == std::string::npos) throw ParseError("nothing to play: " + w);
    if (inst.zones.empty()) throw ParseError("this M1 sound has nothing to play");
    return inst;
}

Instrument load_syx(VolumePtr vol, const std::string &path, int index) {
    Bank b = read_bank(*vol, path);
    if (b.kits.empty()) b.kits = sibling_kits(*vol, path);
    return load_from(b, vol, path, index);
}

// ---- program cards ----

bool probe_card(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    return size == CARD && n >= 4 && !std::memcmp(h, "KORG", 4);
}

std::vector<PresetInfo> list_card(VolumePtr vol, const std::string &path) {
    Bank b = read_card(*vol, path);
    std::vector<PresetInfo> out;
    char num[32];
    for (size_t i = 0; i < 100; i++) {
        std::snprintf(num, sizeof num, "P%02zu ", i);
        out.push_back({num + name_of(b.progs[i]), int(i)});
    }
    for (size_t i = 0; i < 100; i++) {
        std::snprintf(num, sizeof num, "C%02zu ", i);
        out.push_back({num + name_of(b.combis[i]), 1000 + int(i)});
    }
    return out;
}

Instrument load_card(VolumePtr vol, const std::string &path, int index) { return load_from(read_card(*vol, path), vol, path, index); }

}  // namespace

void set_search_dirs(const std::vector<std::string> &dirs) {
    std::lock_guard<std::mutex> lock(dirs_mutex);
    extra_dirs = dirs;
}

void register_korg_m1() {
    register_reader({"Korg M1 SysEx", "syx", probe_syx, list_syx, load_syx});
    register_reader({"Korg M1 Plus+1 program card", "bin", probe_card, list_card, load_card});
}

}  // namespace omni

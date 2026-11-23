// Omni Sampler: Ensoniq SD-1 / VFX-SD SysEx programs (.syx), played with the SD-1's own waves, from its ROM dumps (MAME's
// sd132 set): the 12-bit wave chips u34, u35 and their low-nibble chip u36 (u14-u16 on the VFX, u57/u58/u60 on the
// VFX-SD: the same data), the SD-1's 16-bit u37 / u38, and its OS (sd1_410_hi / _lo, sd1_32_402_hi / _lo or
// sd1_21_300b_upper / _lower; or the VFX-SD's vfxsd_200 / vfxsd_133, the VFX's vfx230 / vfx210b), which holds the wave
// table. The VFX's 109 waves are the SD-1's waves 0-108, zone for zone (checked on VFX 2.10 / 2.30).
// Decoded 2026-10-16 on the SD-1 factory banks and the ROMs above:
//   - Messages F0 0F 05 00 <chan> <type> <data> F7, the data in nibbles, HIGH first (Ensoniq's SD-1 MIDI SysEx
//     Specification 3.11): type 02 one program, 03 all 60. A program is 530 bytes: six 83-byte voices, the name at
//     498 (11 characters), the patch selects at 509-512 (low 6 bits: the voices each plays; patch 1 = no button).
//     Voice: Env1 0-13, Env2 14-27, Env3 (amplitude) 28-41 (initial level, attack time, peak, decay 1, breakpoint 1,
//     decay 2, breakpoint 2, decay 3, sustain, release, ...), transpose 42 (semitones), fine 43, pitch table 44 (1 =
//     all keys at C4), keyboard scaling 59 (-128: a key zone, 60-61), volume 62 (low 7 bits), pan 65, wave 74 (0-168),
//     wave class 75 (high nibble), velocity threshold 82 (+: at least, -: at most). Byte 513's low nibble set: voices
//     5 and 6 hold a pitch table instead.
//   - The OS finds a wave's keyboard zones with `movea.l #WAVES,a0 / movea.w (a0,d0.w),a6 / adda.l #ZONES,a6`, which
//     gives both tables (SD-1 OS 3.00 / 4.02 / 4.10 alike): WAVES = 169 offsets into ZONES; a zone is 14 bytes: pitch
//     (u16), start (u24), top key, loop (u24), mode, end (u24), and a byte. Addresses count half samples; from
//     0x200000 they are in the 16-bit bank (u38 then u37, big-endian), below in the 12-bit one (u34 then u35, each
//     byte the top 8 bits of a sample, u36's low / high nibble the next 4). Mode (the OS turns it into the chip's
//     loop bits): 0 one shot, 1 / 2 loop, 3 back and forth, 5 reverse; bit 7 a transwave: its loop field is the
//     length of each of the single cycles from start (& 0x1FFFFF) to end (mode 0x80: one cycle, loop to end). The
//     chip jumps back by end - loop on reaching end, so a loop is (end - loop) / 2 samples from loop, maybe ending
//     on a half sample (see chip_loop()); the sample at end repeats the one at loop.
//   - Pitch: the zone plays its root at 29762 Hz (the 32-voice chip's rate) when root = 256 - pitch / 256 (checked
//     against the samples' own periods, within 0.3 semitones). Transwaves measure an octave lower than that while
//     programs play them untransposed alongside sampled waves, so they are taken an octave up. Zones with pitch 0
//     are markers (not played).
//   - Which zone a key plays is the OS's choice (see add_voice): by the transposed key, the pitch table for most
//     waves, and for wave 167 a drum map, one zone per key (decoded 2026-10-18).
//   - Filters, Env1 / Env2 and the LFO (2026-10-21): see pick_filter and apply_mods.
// The SQ-1 (MAME's sq1 set: OS sq1up / sq1lo, waves sq1-u25 / sq1-u26) has the same OS tables and zone records, its
// 8-bit waves one byte per sample (u25 the lower 512K samples, u26 the upper) at the same 29762 Hz (checked on its
// loops). It has no program reader here (no SQ-1 programs to hand): its ROM files open as a list of its 120 waves, each
// played as is (decoded 2026-10-21; wave 120 is a dummy spanning the whole ROM). Its waves 111-119 are mode 0x80
// single cycles (the start field holds something else, not an address), which play at the pitch formula's root with
// no octave added.
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include "../core/parse.hpp"
#include "../third_party/tinf/tinf.h"
#include "format.hpp"

namespace omni {
namespace {

constexpr size_t PROG = 530, VOICE = 83;
constexpr int RATE = 29762, N_WAVES = 169;
constexpr int DRUM_KEY = 35, DRUM_KEYS = 63;   // the drum map: keys 35-97, one zone each (lower and higher keys clamp)

int sx(uint8_t v) { return int(int8_t(v)); }

bool is_sd1(const uint8_t *h, size_t n) { return n >= 6 && h[0] == 0xF0 && h[1] == 0x0F && h[2] == 0x05 && h[3] == 0x00; }

// the programs of a file: every type 02 / 03 message's 530-byte records
std::vector<std::vector<uint8_t>> read_programs(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(size_t(1) << 20);
    std::vector<std::vector<uint8_t>> out;
    for (size_t i = 0; i < d.size();) {
        if (d[i] != 0xF0) { i++; continue; }
        size_t e = i + 1;
        while (e < d.size() && d[e] != 0xF7) e++;
        if (e < d.size() && is_sd1(&d[i], e - i) && (d[i + 5] == 0x02 || d[i + 5] == 0x03)) {
            std::vector<uint8_t> v;
            for (size_t k = i + 6; k + 1 < e; k += 2) v.push_back(uint8_t((d[k] & 15) << 4 | (d[k + 1] & 15)));
            for (size_t k = 0; k + PROG <= v.size(); k += PROG) out.emplace_back(v.begin() + long(k), v.begin() + long(k + PROG));
        }
        i = e + 1;
    }
    return out;
}

std::string name_of(const uint8_t *p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i]; i++) s += p[i] >= 32 && p[i] < 127 ? char(p[i]) : ' ';
    return trim(s);
}

// ---- the ROMs ----

struct Zone14 { int pitch; uint32_t start, loop, end; int top, mode; };

struct Rom {
    std::string dir;
    BlobPtr lo, hi, nib;           // the 12-bit bank: u34 (lower half), u35 (upper), u36 (nibbles); the SQ-1's
                                   // 8-bit u25 / u26 (no nibbles)
    BlobPtr b1lo, b1hi;            // the 16-bit bank: u38, u37 (null when absent)
    bool sq1 = false;
    std::vector<std::vector<Zone14>> waves;
    int drum_wave = -1;            // the drum map's wave number (167 on the SD-1); -1 when the OS has none
    std::vector<Zone14> drums;     // its zones for keys DRUM_KEY.. (one each)
};
using RomPtr = std::shared_ptr<const Rom>;

const char *const BASE_NAMES[3][3] = {{"u34.bin", "u35.bin", "u36.bin"}, {"u14.bin", "u15.bin", "u16.bin"}, {"u57.bin", "u58.bin", "u60.bin"}};
const char *const SQ1_NAMES[2] = {"sq1-u25.bin", "sq1-u26.bin"};
// the OS images, the high (even-byte) chip first; the SD-1's first (169 waves), then the VFX-SD's, then the VFX's (109)
const char *const OS_NAMES[7][2] = {{"sd1_410_hi.bin", "sd1_410_lo.bin"}, {"sd1_32_402_hi.bin", "sd1_32_402_lo.bin"},
                                    {"sd1_21_300b_upper.bin", "sd1_21_300b_lower.bin"},
                                    {"vfxsd_200_upper.bin", "vfxsd_200_lower.bin"}, {"vfxsd_133_upper.bin", "vfxsd_133_lower.bin"},
                                    {"vfx230-high.bin", "vfx230-low.bin"}, {"vfx210b-high.bin", "vfx210b-low.bin"}};
const char *const SQ1_OS_NAMES[2][2] = {{"sq1up.bin", "sq1lo.bin"}, {"sqr-102-upper.bin", "sqr-102-lower.bin"}};
constexpr uint64_t CHIP = 0x80000;

struct Files { std::string base[3], os[2], b1lo, b1hi; bool sq1 = false; };

Files find_files(const std::vector<DirEntry> &list) {
    Files f;
    auto has = [&](const char *name, uint64_t size) -> std::string {
        for (const DirEntry &e : list) if (!e.dir && equals_ci(e.name, name) && (!size || e.size == size)) return e.name;
        return "";
    };
    for (auto &set : BASE_NAMES) {
        std::string a = has(set[0], CHIP), b = has(set[1], CHIP), c = has(set[2], CHIP);
        if (!a.empty() && !b.empty() && !c.empty()) { f.base[0] = a; f.base[1] = b; f.base[2] = c; break; }
    }
    for (auto &set : OS_NAMES) {
        std::string h = has(set[0], 0), l = has(set[1], 0);
        if (!h.empty() && !l.empty()) { f.os[0] = h; f.os[1] = l; break; }
    }
    f.b1lo = has("u38.bin", 2 * CHIP);
    f.b1hi = has("u37.bin", 2 * CHIP);
    if (f.base[0].empty() || f.os[0].empty()) {   // an SQ-1 (or SQ-R)
        std::string a = has(SQ1_NAMES[0], CHIP), b = has(SQ1_NAMES[1], CHIP);
        for (auto &set : SQ1_OS_NAMES) {
            std::string h = has(set[0], 0), l = has(set[1], 0);
            if (!a.empty() && !b.empty() && !h.empty() && !l.empty()) {
                f = Files();
                f.base[0] = a; f.base[1] = b; f.os[0] = h; f.os[1] = l; f.sq1 = true;
                break;
            }
        }
    }
    return f;
}

bool is_rom(Volume &vol, const std::string &dir) {
    try {
        Files f = find_files(vol.list(dir));
        return !f.base[0].empty() && !f.os[0].empty();
    } catch (const std::exception &) { return false; }
}

std::mutex roms_mutex;
std::map<std::string, RomPtr> roms;   // parsed once per folder
std::string cached_rom;

RomPtr open_rom(Volume &vol, const std::string &dir) {
    {
        std::lock_guard<std::mutex> lock(roms_mutex);
        auto it = roms.find(dir);
        if (it != roms.end()) return it->second;
    }
    Files f = find_files(vol.list(dir));
    if (f.base[0].empty() || f.os[0].empty()) throw ParseError("SD-1 ROM dumps missing in " + dir);
    auto rom = std::make_shared<Rom>();
    rom->dir = dir;
    rom->lo = vol.open(path_join(dir, f.base[0]));
    rom->hi = vol.open(path_join(dir, f.base[1]));
    if (!f.sq1) rom->nib = vol.open(path_join(dir, f.base[2]));
    rom->sq1 = f.sq1;
    if (!f.b1lo.empty() && !f.b1hi.empty()) { rom->b1lo = vol.open(path_join(dir, f.b1lo)); rom->b1hi = vol.open(path_join(dir, f.b1hi)); }
    // the OS as the CPU sees it: the high chip on even bytes, the low on odd
    std::vector<uint8_t> hi = vol.open(path_join(dir, f.os[0]))->all(size_t(1) << 20), lo = vol.open(path_join(dir, f.os[1]))->all(size_t(1) << 20);
    std::vector<uint8_t> os(2 * std::max(hi.size(), lo.size()), 0xFF);
    for (size_t i = 0; i < hi.size(); i++) os[2 * i] = hi[i];
    for (size_t i = 0; i < lo.size(); i++) os[2 * i + 1] = lo[i];
    auto addr = [&](size_t at) { return uint32_t(os[at] << 24 | os[at + 1] << 16 | os[at + 2] << 8 | os[at + 3]) - 0xC00000u; };
    // `movea.w (a0,d0.w),a6 / adda.l #ZONES,a6`, with `movea.l #WAVES,a0` just before it (SD-1, VFX-SD) or a few
    // instructions earlier, then `cmp.w #2n,d0` bounding the wave number (VFX: n = 109)
    size_t waves = 0, zones = 0;
    int n_waves = N_WAVES;
    for (size_t i = 16; i + 10 <= os.size() && !waves; i += 2)
        if (os[i] == 0x3C && os[i + 1] == 0x70 && os[i + 2] == 0 && os[i + 3] == 0 && os[i + 4] == 0xDD && os[i + 5] == 0xFC && os[i + 6] == 0)
            for (size_t j = i - 6; j + 32 >= i && j > 0; j -= 2)
                if (os[j] == 0x20 && os[j + 1] == 0x7C && os[j + 2] == 0) {
                    waves = addr(j + 2);
                    zones = addr(i + 6);
                    for (size_t k = j + 6; k + 4 <= i; k += 2)
                        if (os[k] == 0xB0 && os[k + 1] == 0x7C && os[k + 2] == 0) n_waves = std::min(N_WAVES, os[k + 3] / 2);
                    break;
                }
    if (!waves || !zones || waves + 2 * size_t(n_waves) > os.size() || zones >= os.size()) throw ParseError("no wave table in the SD-1 OS (" + f.os[0] + ")");
    auto zone_at = [&](size_t at) {
        const uint8_t *r = &os[at];
        Zone14 z;
        z.pitch = r[0] << 8 | r[1];
        z.start = uint32_t(r[2] << 16 | r[3] << 8 | r[4]);
        z.top = r[5];
        z.loop = uint32_t(r[6] << 16 | r[7] << 8 | r[8]);
        z.mode = r[9];
        z.end = uint32_t(r[10] << 16 | r[11] << 8 | r[12]);
        return z;
    };
    for (int w = 0; w < n_waves; w++) {
        size_t off = size_t(os[waves + 2 * size_t(w)] << 8 | os[waves + 2 * size_t(w) + 1]), at = zones + off;
        // the SQ-1's table has no bound in the code (the wave byte & 0x7F): it ends with a dummy at offset 0, then
        // other data
        if (f.sq1 && (off % 14 || (w && !off))) break;
        std::vector<Zone14> zs;
        for (int k = 0; k < 64 && at + 14 <= os.size(); k++, at += 14) {
            zs.push_back(zone_at(at));
            if (zs.back().top >= 127) break;
        }
        rom->waves.push_back(std::move(zs));
    }
    // The drum map (SD-1 4.10 at 0xC0C802): `cmp.b #167,d0 / bne ...`, then the key less 35, clamped to 0..62, picks
    // one zone: `movea.l #DRUMS,a6 / movea.w 2(a6,d0.w),a6 / adda.l #ZONES,a6` (DRUMS[0] is the drum being edited)
    for (size_t i = 64; i + 12 <= os.size() && rom->drum_wave < 0; i += 2)
        if (os[i] == 0x2C && os[i + 1] == 0x7C && os[i + 2] == 0 && os[i + 6] == 0x3C && os[i + 7] == 0x76 && os[i + 8] == 0 &&
            os[i + 9] == 2 && os[i + 10] == 0xDD && os[i + 11] == 0xFC && addr(i + 12) == zones)
            for (size_t j = i - 2; j + 128 > i; j -= 2)
                if (os[j] == 0xB0 && os[j + 1] == 0x3C && os[j + 2] == 0 && os[j + 4] == 0x66) {   // cmp.b #wave,d0 / bne
                    size_t drums = addr(i + 2);
                    if (os[j + 3] >= rom->waves.size() || drums + 2 * (DRUM_KEYS + 1) > os.size()) break;
                    for (int k = 1; k <= DRUM_KEYS; k++) {
                        size_t at = zones + size_t(os[drums + 2 * size_t(k)] << 8 | os[drums + 2 * size_t(k) + 1]);
                        if (at + 14 > os.size()) { rom->drums.clear(); break; }
                        rom->drums.push_back(zone_at(at));
                    }
                    if (!rom->drums.empty()) rom->drum_wave = os[j + 3];
                    break;
                }
    std::lock_guard<std::mutex> lock(roms_mutex);
    roms[dir] = rom;
    return rom;
}

// `frames` samples from half-sample address `addr` (the bank from its 0x200000 bit)
SampleRefPtr rom_pcm(const RomPtr &rom, uint32_t addr, int64_t frames) {
    bool b1 = addr & 0x200000;
    uint32_t first = (addr & 0x1FFFFF) >> 1;
    auto r = std::make_shared<SampleRef>();
    r->name = (rom->sq1 ? "SQ-1 " : "SD-1 ") + std::string(b1 ? "B" : "A") + std::to_string(first);
    r->rate = RATE;
    r->channels = 1;
    r->frames = frames;
    r->key = "sd1rom:" + rom->dir + ":" + std::to_string(addr) + ":" + std::to_string(frames);
    r->decode = [rom, b1, first, frames]() -> PcmPtr {
        auto p = std::make_shared<Pcm>();
        p->rate = RATE;
        p->channels = 1;
        p->data.resize(size_t(frames));
        for (int64_t done = 0; done < frames;) {
            uint32_t w = first + uint32_t(done), half = w / uint32_t(CHIP), off = w % uint32_t(CHIP);
            if (half > 1) throw ParseError("SD-1 wave past the end of the ROM");
            size_t n = size_t(std::min<int64_t>(frames - done, int64_t(CHIP - off)));
            if (rom->sq1) {
                if (b1) throw ParseError("SQ-1 wave past the end of the ROM");
                std::vector<uint8_t> b = (half ? rom->hi : rom->lo)->bytes(off, n);
                for (size_t i = 0; i < n; i++) p->data[size_t(done) + i] = int16_t(uint16_t(b[i] << 8));
            } else if (b1) {
                const BlobPtr &chip = half ? rom->b1hi : rom->b1lo;
                std::vector<uint8_t> b = chip->bytes(uint64_t(off) * 2, n * 2);
                for (size_t i = 0; i < n; i++) p->data[size_t(done) + i] = int16_t(uint16_t(b[2 * i] << 8 | b[2 * i + 1]));
            } else {
                std::vector<uint8_t> b = (half ? rom->hi : rom->lo)->bytes(off, n), nb = rom->nib->bytes(off, n);
                for (size_t i = 0; i < n; i++) {
                    int low = half ? (nb[i] & 0xF0) : (nb[i] & 0x0F) << 4;
                    p->data[size_t(done) + i] = int16_t(uint16_t(b[i] << 8 | low));
                }
            }
            done += int64_t(n);
        }
        return p;
    };
    return r;
}

// ---- programs ----

double env_time(int v) { return 0.002 * std::pow(1.1, clampi(std::abs(v), 0, 99)); }   // 0 ~2 ms, 50 ~0.2 s, 99 ~25 s

// An envelope (Env1 at 0, Env2 at 14, Env3 at 28) as the sampler's ADSR relative to its highest level, which it returns
// (1..127): attack from the initial level to the peak, the decays on through the breakpoints to sustain
int to_env(Envelope &e, const uint8_t *v) {
    int init = clampi(v[0], 0, 127), peak = clampi(v[2], 0, 127), bp1 = clampi(v[4], 0, 127), bp2 = clampi(v[6], 0, 127),
        sus = clampi(v[8], 0, 127);
    int top = std::max(1, std::max(peak, std::max(bp1, bp2)));
    e.set = true;
    e.start_level = double(init) / top;
    e.attack = env_time(v[1]);
    e.decay = 0.002 + (bp1 != peak ? env_time(v[3]) : 0) + (bp2 != bp1 ? env_time(v[5]) : 0) + (sus != bp2 ? env_time(v[7]) : 0);
    e.sustain = double(sus) / top;
    e.release = env_time(sx(v[9]));
    return top;
}

double apply_env(Zone &z, const uint8_t *v) { return 20.0 * std::log10(to_env(z.amp_env, v + 28) / 127.0); }

// Filters (bytes 49-58). Byte 52's high nibble is filter 2's type, 0 HP/2, 1 HP/1, 2 LP/2, 3 LP/1, and filter 1 (always
// low pass) takes the poles left of the chip's four: LP/3 beside a one-pole filter 2, else LP/2 (the factory banks: HP
// types sit at cutoff 0, open, the LP ones high). Cutoff 0..127 is taken as 10 octaves from 20 Hz, the Env2 amount
// -127..127 as that whole range at the envelope's level 127, keyboard tracking 127 as 100% around C4 (all three
// approximate, not measured). The sampler has one filter per zone: two low passes become the lower one, with the poles
// of both; a high pass that does more than an open low pass is kept only when filter 1 is open.
constexpr double CUT_OCTAVES = 10.0;
double cut_hz(int c) { return 20.0 * std::pow(2.0, clampi(c, 0, 127) * CUT_OCTAVES / 127.0); }

struct Sd1Filter { FilterType type = FilterType::None; int poles = 2, cutoff = 127, kbd = 0, env = 0; };

Sd1Filter pick_filter(const uint8_t *v) {
    int mode = (v[52] >> 4) & 3;
    bool f2_lp = mode >= 2, f2_one = mode & 1;
    Sd1Filter f1{FilterType::LowPass, f2_one ? 3 : 2, v[49], sx(v[50]), sx(v[51])};
    Sd1Filter f2{f2_lp ? FilterType::LowPass : FilterType::HighPass, f2_one ? 1 : 2, v[54], sx(v[55]), sx(v[56])};
    // a filter does nothing when its cutoff stays at the end where it passes all, its envelope pushing it further out
    auto open = [](const Sd1Filter &f) {
        bool lp = f.type == FilterType::LowPass;
        int reach = std::abs(f.kbd) * 48 / 127;   // keys 12-108 around C4, in cutoff units (127 = 10 octaves)
        return lp ? f.cutoff - reach >= 115 && f.env >= 0 : f.cutoff + reach <= 8 && f.env <= 0;
    };
    bool o1 = open(f1), o2 = open(f2);
    if (o1 && o2) return {};
    if (o1) return f2;
    if (o2 || !f2_lp) return f1;
    Sd1Filter &lo = f2.cutoff < f1.cutoff ? f2 : f1;
    Sd1Filter out = lo;
    if (std::abs(f1.cutoff - f2.cutoff) <= 16) out.poles = std::min(4, f1.poles + f2.poles);
    return out;
}

// LFO rate 0..99 (about 0.1-13.5 Hz; vibrato settings, 30-45, near 5 Hz), shapes TRI, SINE, SINE/TRI, POS/SIN, SAW,
// SQUARE, delay as a fade in, restart (byte 70's high nibble) as key sync
Lfo to_lfo(const uint8_t *v) {
    static const LfoWave shapes[6] = {LfoWave::Triangle, LfoWave::Sine, LfoWave::Sine, LfoWave::Sine, LfoWave::SawUp, LfoWave::Square};
    Lfo l;
    l.set = true;
    l.wave = shapes[std::min(5, v[68] >> 4)];
    l.rate_hz = 0.1 + clampi(v[72], 0, 99) * 0.135;
    l.fade_in = v[73] ? env_time(v[73]) : 0;
    l.key_sync = (v[70] >> 4) != 0;
    return l;
}

// Filter, Env1 -> pitch (byte 45) and LFO -> pitch (46, times the LFO depth 69), 127 taken as an octave (not measured).
// The modulation sources (wheel, pressure ... at bytes 47, 52, 57, 63, 68) are not mapped: their numbering is not in
// the SysEx spec.
void apply_mods(Zone &z, const uint8_t *v) {
    Sd1Filter f = pick_filter(v);
    if (f.type != FilterType::None) {
        z.filter.type = f.type;
        z.filter.poles = f.poles;
        z.filter.key_tracking = f.kbd / 127.0;
        // the sampler tracks around the zone's root, the SD-1 around C4
        z.filter.cutoff = std::min(MAX_CUTOFF_HZ, cut_hz(f.cutoff) * std::pow(2.0, z.filter.key_tracking * (z.root - 60) / 12.0));
        if (f.env) {
            int top = to_env(z.filter.env, v + 14);
            z.filter.env_depth = clampd(f.env / 127.0 * top / 127.0 * (CUT_OCTAVES * 1200.0 / MAX_ENVELOPE_DEPTH), -1, 1);
        }
    }
    if (sx(v[45])) {
        int top = to_env(z.pitch_env, v);
        z.pitch_env_depth = sx(v[45]) / 127.0 * top / 127.0 * (1200.0 / MAX_ENVELOPE_DEPTH);
    }
    if (sx(v[46])) {
        z.pitch_lfo = to_lfo(v);   // also the wheel's vibrato
        z.pitch_lfo_depth = sx(v[46]) / 127.0 * v[69] / 127.0 * (1200.0 / MAX_ENVELOPE_DEPTH);
    }
}

// How the OS picks a voice's zone (SD-1 4.10, 0xC0C802-0xC0C8AE and 0xC0D1A2-0xC0D1E0):
//   - the drum map wave (167): the key itself, less 35, one zone per key; every key plays at C4 + transpose
//   - waves 126-168 (the drum and percussion waves): the key plus transpose; a marker zone (pitch 0) is silent
//   - the others: the key the pitch table gives (ALL-C4: 60 for every key) plus transpose
// A zone then plays at that pitch-table key plus transpose (and fine tune) against its root.
void add_voice(Instrument &inst, const RomPtr &rom, int n, const uint8_t *v) {
    std::string label = "V" + std::to_string(n + 1);
    int wave = v[74];
    if (wave >= int(rom->waves.size())) { inst.warnings.push_back(label + " plays wave " + std::to_string(wave) + ": unknown"); return; }
    int transpose = sx(v[42]);
    double tune = transpose + sx(v[43]) / 128.0;
    double gain = 20.0 * std::log10(std::max(1, v[62] & 0x7F) / 127.0);
    double pan = clampd((v[65] - 64) / 63.0, -1, 1);
    int key_lo = 0, key_hi = 127, vel_lo = 0, vel_hi = 127;
    if (sx(v[59]) == -128 && v[60] <= v[61]) { key_lo = v[60]; key_hi = v[61]; }
    int thr = sx(v[82]);
    if (thr > 0) vel_lo = std::min(127, thr);
    if (thr < 0) vel_hi = std::min(127, -thr);
    bool drum_map = wave == rom->drum_wave, by_key = wave >= 126, all_c4 = v[44] == 1;
    int played = 0;
    // one zone for the played keys lo..hi; false when the 16-bit chips are missing
    auto add_zone = [&](const Zone14 &z, int lo, int hi, bool fixed) {
        lo = std::max(lo, key_lo);
        hi = std::min(hi, key_hi);
        if (lo > hi || z.pitch == 0) return true;
        bool trans = z.mode & 0x80;
        uint32_t s = trans ? (z.mode == 0x80 ? z.loop : z.start & 0x1FFFFF) : z.start;
        if (z.end <= s) return true;
        if ((s & 0x200000) && !rom->b1lo) {
            inst.warnings.push_back(label + " plays wave " + std::to_string(wave) + ": put the SD-1's u37.bin and u38.bin beside u34-u36");
            return false;
        }
        int64_t frames = int64_t((z.end - s) >> 1) + 1;
        uint32_t frame = z.loop >> 1;   // a transwave's cycle length
        bool table = trans && z.mode != 0x80 && frame >= 16 && (frames - 1) % frame == 0 && (frames - 1) / frame >= 2;
        if (table) frames--;            // whole cycles
        Zone out;
        out.name = label + " wave " + std::to_string(wave) + " " + std::to_string(played + 1);
        out.sample = rom_pcm(rom, s, frames);
        out.key_lo = lo;
        out.key_hi = hi;
        out.vel_lo = vel_lo; out.vel_hi = vel_hi;
        // (the SQ-1's mode 0x80 single cycles measure at the root as given: 256 frames at 29762 Hz = its key 58)
        double root = 256.0 - z.pitch / 256.0 + (trans && !rom->sq1 ? 12.0 : 0.0);
        out.root = int(std::lround(root));
        if (fixed) {                    // every key at C4
            out.key_tracking = 0;
            out.tune = 60 - root + tune;
        } else out.tune = out.root - root + tune;   // the sample sounds at root
        if (table) {
            out.wt_size = int(frame);
            out.wt_count = int(frames / frame);
        } else if (trans || (z.mode & 3)) {
            uint32_t from = trans ? s : z.loop;
            Loop l = chip_loop(int64_t((from - s) >> 1), (double(z.end) - double(from)) / 2.0, frames - 1);
            if ((z.mode & 0x7F) == 3) l.type = LoopType::Alternating;
            if (l.start >= 0 && l.start < l.end) out.loops.push_back(l);
        }
        out.reverse = (z.mode & 0x7F) == 5;
        out.pan = pan;
        out.gain_db = gain + apply_env(out, v);
        apply_mods(out, v);
        inst.zones.push_back(std::move(out));
        played++;
        return true;
    };
    const std::vector<Zone14> &zs = rom->waves[size_t(wave)];
    if (drum_map) {
        for (int k = 0; k < int(rom->drums.size()); k++)
            if (!add_zone(rom->drums[size_t(k)], k ? DRUM_KEY + k : 0, k + 1 < int(rom->drums.size()) ? DRUM_KEY + k : 127, true)) return;
    } else if (all_c4 && !by_key) {
        int key = clampi(60 + transpose, 0, 127);
        for (const Zone14 &z : zs)
            if (key <= z.top || &z == &zs.back()) { add_zone(z, 0, 127, true); break; }
    } else {
        // zone keys are transposed keys: keys 85-96 transposed -28 play the zone of 57-68 (the factory TUBS / TYMPANI
        // programs reach the drum waves' key ranges that way); the outer zones go on to the ends
        int lo = 0;
        for (const Zone14 &z : zs) {
            int top = std::min(127, z.top);
            if (!add_zone(z, lo == 0 ? 0 : clampi(lo - transpose, 0, 127), top >= 127 ? 127 : clampi(top - transpose, 0, 127), all_c4)) return;
            lo = top + 1;
        }
    }
    if (!played) inst.warnings.push_back(label + " plays wave " + std::to_string(wave) + ": none of its zones is in the voice's keys");
}

Instrument load_program(const RomPtr &rom, const std::vector<uint8_t> &p) {
    Instrument inst;
    inst.name = name_of(&p[498], 11);
    inst.format = "Ensoniq SD-1 program";
    int voices = (p[513] & 0x0F) ? 4 : 6;                           // a pitch table in voices 5-6
    int mask = p[509] & 0x3F;
    if (!mask) mask = 0x3F;
    for (int n = 0; n < voices; n++)
        if (mask >> n & 1) add_voice(inst, rom, n, &p[VOICE * size_t(n)]);
    return inst;
}

// ---- the reader ----

bool probe_syx(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    for (size_t i = 0; i + 6 <= n; i++)
        if (h[i] == 0xF0 && is_sd1(h + i, n - i) && (h[i + 5] == 0x02 || h[i + 5] == 0x03)) return true;
    return false;
}

std::vector<PresetInfo> list_syx(VolumePtr vol, const std::string &path) {
    std::vector<PresetInfo> out;
    std::vector<std::vector<uint8_t>> progs = read_programs(*vol, path);
    for (size_t i = 0; i < progs.size(); i++) {
        std::string name = name_of(&progs[i][498], 11);
        char num[16];
        std::snprintf(num, sizeof num, "%02d ", int(i) + 1);
        out.push_back({num + (name.empty() ? std::string("(no name)") : name), int(i)});
    }
    return out;
}

Instrument load_syx(VolumePtr vol, const std::string &path, int index) {
    std::vector<std::vector<uint8_t>> progs = read_programs(*vol, path);
    if (index < 0 || index >= int(progs.size())) throw ParseError("no such SD-1 program");
    std::string dir = find_beside(*vol, path, is_rom, cached_rom);
    if (dir.empty())
        throw ParseError("SD-1 waves not found: put the SD-1 ROM dumps (u34.bin, u35.bin, u36.bin, u37.bin, u38.bin and "
                         "sd1_410_hi.bin / sd1_410_lo.bin; or a VFX's u14-u16.bin and vfx230-high.bin / vfx230-low.bin) "
                         "beside the .syx files or in the Plugin Library");
    Instrument inst = load_program(open_rom(*vol, dir), progs[size_t(index)]);
    if (inst.zones.empty()) {
        std::string why = inst.warnings.empty() ? "no voices" : inst.warnings[0];
        throw ParseError("nothing to play: " + inst.name + ": " + why);
    }
    finish_instrument(inst);
    return inst;
}

// ---- the SQ-1's waves, from its ROM files ----

// the SQ-1 chips by the CRC-32 of their first 4 KB (and size): sq1-u25, sq1-u26, sq1up, sq1lo
bool probe_waves(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    if ((size != CHIP && size != 0x10000) || n < 4096) return false;
    uint32_t crc = tinf_crc32(h, 4096);
    return crc == 0xF8FB5C6D || crc == 0x0B9DAB31 || crc == 0xA23FE2D7 || crc == 0x687C8766;
}

RomPtr waves_rom(Volume &vol, const std::string &path) {
    std::string dir = path_dir(path);
    Files f = find_files(vol.list(dir));
    if (!f.sq1) throw ParseError("SQ-1 ROMs incomplete: put sq1-u25.bin, sq1-u26.bin, sq1up.bin and sq1lo.bin in one folder");
    return open_rom(vol, dir);
}

// a wave's kind from its zones, for the list
std::string wave_kind(const std::vector<Zone14> &zs) {
    if (zs.empty()) return "";
    if (zs[0].mode == 0x80) return "single cycle";
    if (zs[0].mode & 0x80) return "transwave";
    if (zs.size() == 1 && (zs[0].mode & 3) == 0) return "one shot";
    return std::to_string(zs.size()) + (zs.size() == 1 ? " zone" : " zones");
}

std::vector<PresetInfo> list_waves(VolumePtr vol, const std::string &path) {
    RomPtr rom = waves_rom(*vol, path);
    std::vector<PresetInfo> out;
    for (size_t w = 0; w < rom->waves.size(); w++) {
        char num[40];
        std::snprintf(num, sizeof num, "%03d Wave %d (%s)", int(w) + 1, int(w) + 1, wave_kind(rom->waves[w]).c_str());
        out.push_back({num, int(w)});
    }
    return out;
}

Instrument load_waves(VolumePtr vol, const std::string &path, int index) {
    RomPtr rom = waves_rom(*vol, path);
    if (index < 0 || index >= int(rom->waves.size())) throw ParseError("no such SQ-1 wave");
    // a plain voice: the wave at full level, filters open, Env3 straight to full and held, a short release
    uint8_t v[VOICE] = {};
    v[74] = uint8_t(index);
    v[62] = 127;
    v[65] = 64;
    v[49] = 127;
    v[30] = v[32] = v[34] = v[36] = 127;
    v[37] = 40;
    Instrument inst;
    inst.format = "Ensoniq SQ-1 wave";
    add_voice(inst, rom, 0, v);
    char name[24];
    std::snprintf(name, sizeof name, "SQ-1 Wave %03d", index + 1);
    inst.name = name;
    for (Zone &z : inst.zones) {
        z.name = inst.name + " " + z.name.substr(z.name.rfind(' ') + 1);
        if (z.loops.empty() && !z.wt_count) z.one_shot = true;
    }
    if (inst.zones.empty()) throw ParseError("nothing to play: " + (inst.warnings.empty() ? inst.name : inst.warnings[0]));
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_ensoniq_sd1() {
    register_reader({"Ensoniq SD-1 SysEx", "syx", probe_syx, list_syx, load_syx});
    register_reader({"Ensoniq SQ-1 wave ROM", "", probe_waves, list_waves, load_waves});
}

}  // namespace omni

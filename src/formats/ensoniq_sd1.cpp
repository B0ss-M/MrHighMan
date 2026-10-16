// Omni Sampler: Ensoniq SD-1 / VFX-SD SysEx programs (.syx), played with the SD-1's own waves, from its ROM dumps (MAME's
// sd132 set): the 12-bit wave chips u34, u35 and their low-nibble chip u36 (u14-u16 on the VFX, u57/u58/u60 on the
// VFX-SD: the same data), the SD-1's 16-bit u37 / u38, and its OS (sd1_410_hi / _lo, sd1_32_402_hi / _lo or
// sd1_21_300b_upper / _lower), which holds the wave table.
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
//     length of each of the single cycles from start (& 0x1FFFFF) to end (mode 0x80: one cycle, loop to end).
//   - Pitch: the zone plays its root at 29762 Hz (the 32-voice chip's rate) when root = 256 - pitch / 256 (checked
//     against the samples' own periods, within 0.3 semitones). Transwaves measure an octave lower than that while
//     programs play them untransposed alongside sampled waves, so they are taken an octave up. Zones with pitch 0
//     are markers (not played).
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include "../core/parse.hpp"
#include "format.hpp"

namespace omni {
namespace {

constexpr size_t PROG = 530, VOICE = 83;
constexpr int RATE = 29762, N_WAVES = 169;

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
    BlobPtr lo, hi, nib;           // the 12-bit bank: u34 (lower half), u35 (upper), u36 (nibbles)
    BlobPtr b1lo, b1hi;            // the 16-bit bank: u38, u37 (null when absent)
    std::vector<std::vector<Zone14>> waves;
};
using RomPtr = std::shared_ptr<const Rom>;

const char *const BASE_NAMES[3][3] = {{"u34.bin", "u35.bin", "u36.bin"}, {"u14.bin", "u15.bin", "u16.bin"}, {"u57.bin", "u58.bin", "u60.bin"}};
const char *const OS_NAMES[3][2] = {{"sd1_410_hi.bin", "sd1_410_lo.bin"}, {"sd1_32_402_hi.bin", "sd1_32_402_lo.bin"},
                                    {"sd1_21_300b_upper.bin", "sd1_21_300b_lower.bin"}};
constexpr uint64_t CHIP = 0x80000;

struct Files { std::string base[3], os[2], b1lo, b1hi; };

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
    rom->nib = vol.open(path_join(dir, f.base[2]));
    if (!f.b1lo.empty() && !f.b1hi.empty()) { rom->b1lo = vol.open(path_join(dir, f.b1lo)); rom->b1hi = vol.open(path_join(dir, f.b1hi)); }
    // the OS as the CPU sees it: the high chip on even bytes, the low on odd
    std::vector<uint8_t> hi = vol.open(path_join(dir, f.os[0]))->all(size_t(1) << 20), lo = vol.open(path_join(dir, f.os[1]))->all(size_t(1) << 20);
    std::vector<uint8_t> os(2 * std::max(hi.size(), lo.size()), 0xFF);
    for (size_t i = 0; i < hi.size(); i++) os[2 * i] = hi[i];
    for (size_t i = 0; i < lo.size(); i++) os[2 * i + 1] = lo[i];
    auto addr = [&](size_t at) { return uint32_t(os[at] << 24 | os[at + 1] << 16 | os[at + 2] << 8 | os[at + 3]) - 0xC00000u; };
    size_t waves = 0, zones = 0;
    for (size_t i = 0; i + 16 <= os.size(); i += 2)
        if (os[i] == 0x20 && os[i + 1] == 0x7C && os[i + 6] == 0x3C && os[i + 7] == 0x70 && os[i + 8] == 0 && os[i + 9] == 0 &&
            os[i + 10] == 0xDD && os[i + 11] == 0xFC) { waves = addr(i + 2); zones = addr(i + 12); break; }
    if (!waves || !zones || waves + 2 * N_WAVES > os.size() || zones >= os.size()) throw ParseError("no wave table in the SD-1 OS (" + f.os[0] + ")");
    for (int w = 0; w < N_WAVES; w++) {
        size_t at = zones + size_t(os[waves + 2 * size_t(w)] << 8 | os[waves + 2 * size_t(w) + 1]);
        std::vector<Zone14> zs;
        for (int k = 0; k < 64 && at + 14 <= os.size(); k++, at += 14) {
            const uint8_t *r = &os[at];
            Zone14 z;
            z.pitch = r[0] << 8 | r[1];
            z.start = uint32_t(r[2] << 16 | r[3] << 8 | r[4]);
            z.top = r[5];
            z.loop = uint32_t(r[6] << 16 | r[7] << 8 | r[8]);
            z.mode = r[9];
            z.end = uint32_t(r[10] << 16 | r[11] << 8 | r[12]);
            zs.push_back(z);
            if (z.top >= 127) break;
        }
        rom->waves.push_back(std::move(zs));
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
    r->name = "SD-1 " + std::string(b1 ? "B" : "A") + std::to_string(first);
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
            if (b1) {
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

// Env3 as the sampler's ADSR: attack from the initial level to the peak, the decays on through the breakpoints to sustain
double apply_env(Zone &z, const uint8_t *v) {
    int init = clampi(v[28], 0, 127), peak = clampi(v[30], 0, 127), bp1 = clampi(v[32], 0, 127), bp2 = clampi(v[34], 0, 127),
        sus = clampi(v[36], 0, 127);
    int top = std::max(1, std::max(peak, std::max(bp1, bp2)));
    Envelope &e = z.amp_env;
    e.set = true;
    e.start_level = double(init) / top;
    e.attack = env_time(v[29]);
    e.decay = 0.002 + (bp1 != peak ? env_time(v[31]) : 0) + (bp2 != bp1 ? env_time(v[33]) : 0) + (sus != bp2 ? env_time(v[35]) : 0);
    e.sustain = double(sus) / top;
    e.release = env_time(sx(v[37]));
    return 20.0 * std::log10(top / 127.0);
}

void add_voice(Instrument &inst, const RomPtr &rom, int n, const uint8_t *v) {
    std::string label = "V" + std::to_string(n + 1);
    int wave = v[74];
    if (wave >= int(rom->waves.size())) { inst.warnings.push_back(label + " plays wave " + std::to_string(wave) + ": unknown"); return; }
    double tune = sx(v[42]) + sx(v[43]) / 128.0;
    double gain = 20.0 * std::log10(std::max(1, v[62] & 0x7F) / 127.0);
    double pan = clampd((v[65] - 64) / 63.0, -1, 1);
    int key_lo = 0, key_hi = 127, vel_lo = 0, vel_hi = 127;
    if (sx(v[59]) == -128 && v[60] <= v[61]) { key_lo = v[60]; key_hi = v[61]; }
    int thr = sx(v[82]);
    if (thr > 0) vel_lo = std::min(127, thr);
    if (thr < 0) vel_hi = std::min(127, -thr);
    int lo = 0, played = 0;
    for (const Zone14 &z : rom->waves[size_t(wave)]) {
        int top = std::min(127, z.top);
        if (z.pitch == 0) continue;                                  // a marker: the next zone covers its keys
        bool trans = z.mode & 0x80;
        uint32_t s = trans ? (z.mode == 0x80 ? z.loop : z.start & 0x1FFFFF) : z.start;
        if (z.end <= s) { lo = top + 1; continue; }
        if ((s & 0x200000) && !rom->b1lo) {
            inst.warnings.push_back(label + " plays wave " + std::to_string(wave) + ": put the SD-1's u37.bin and u38.bin beside u34-u36");
            return;
        }
        int64_t frames = int64_t((z.end - s) >> 1) + 1;
        uint32_t frame = z.loop >> 1;   // a transwave's cycle length
        bool table = trans && z.mode != 0x80 && frame >= 16 && (frames - 1) % frame == 0 && (frames - 1) / frame >= 2;
        if (table) frames--;            // whole cycles
        Zone out;
        out.name = label + " wave " + std::to_string(wave) + " " + std::to_string(played + 1);
        out.sample = rom_pcm(rom, s, frames);
        out.key_lo = std::max(lo, key_lo);
        out.key_hi = std::min(top, key_hi);
        lo = top + 1;
        if (out.key_lo > out.key_hi) continue;
        out.vel_lo = vel_lo; out.vel_hi = vel_hi;
        double root = 256.0 - z.pitch / 256.0 + (trans ? 12.0 : 0.0);
        out.root = int(std::lround(root));
        out.tune = root - out.root + tune;
        if (v[44] == 1) out.key_tracking = 0;                       // the ALL-C4 pitch table
        if (table) {
            out.wt_size = int(frame);
            out.wt_count = int(frames / frame);
        } else if (trans || (z.mode & 3)) {
            Loop l;
            l.start = trans ? 0 : int64_t((z.loop - s) >> 1);
            l.end = frames - 1;
            if ((z.mode & 0x7F) == 3) l.type = LoopType::Alternating;
            if (l.start >= 0 && l.start < l.end) out.loops.push_back(l);
        }
        out.reverse = (z.mode & 0x7F) == 5;
        out.pan = pan;
        out.gain_db = gain + apply_env(out, v);
        inst.zones.push_back(std::move(out));
        played++;
    }
    if (!played) inst.warnings.push_back(label + " plays wave " + std::to_string(wave) + ": a drum map, not read yet");
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
                         "sd1_410_hi.bin / sd1_410_lo.bin) beside the .syx files or in the Plugin Library");
    Instrument inst = load_program(open_rom(*vol, dir), progs[size_t(index)]);
    if (inst.zones.empty()) {
        std::string why = inst.warnings.empty() ? "no voices" : inst.warnings[0];
        throw ParseError("nothing to play: " + inst.name + ": " + why);
    }
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_ensoniq_sd1() {
    register_reader({"Ensoniq SD-1 SysEx", "syx", probe_syx, list_syx, load_syx});
}

}  // namespace omni

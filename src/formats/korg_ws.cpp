// Omni Sampler: Korg Wavestation / EX / A/D / SR SysEx banks (.syx): performances and patches, played with the waves of
// the Wavestation EX expansion ROM, from its four EPROM dumps (ws3p7.bin, ws3p8.bin, ws3p9.bin, ws4p0.bin, 512 KB each;
// MAME's korgwsex set). The Wavestation's own 365 waves (the SR's wave chips 814-817) are not read yet: a sound that
// uses them says so.
// Decoded 2026-10-15, on the Wavestation Patch Archive's banks (factory, SR ROM, A/D, EX) and the EX chips:
//   - Messages F0 42 3n 28 <fn> <bank> <data> <checksum> F7, the data in nibbles (low first). 4D: 50 performances x
//     181 bytes, 4C: 35 patches x 426, 54: the wave sequences (32 x 16-byte headers, 501 x 16-byte steps, 32 x 8-byte
//     names). Bank 0 RAM1, 1 RAM2, 4 RAM3 (the A/D's and SR's expansion RAM); a performance may play another bank's
//     patches, found in the same file or one beside it. 50 (no bank byte; MSP's sound disks): all data, 67301 bytes:
//     1813 global, then 100 performances (RAM1, RAM2), 70 patches, the two wave sequence blocks.
//   - The layouts are Korg's (from its developer notes, as quoted in Chris Arndt's korgwslib). Performance: name[16],
//     effects[21], 8 parts x 18: bank (0 RAM1, 1 RAM2, 2 ROM, 3 card; part mode bit 6: RAM3), patch, level, output,
//     part mode, low / high key, low / high velocity, transpose, detune (cents), ... Patch: name[16], the mix
//     envelope (point 0 at X 76, Y 81), number of waves 86 (0: 1, 1: 2, 2: 4), hard sync, Bank_Exp 88 (bit n: wave n's
//     sequence is in RAM3), then 4 x 84 bytes, one per oscillator: coarse, fine, wave bank, wave number (u16 BE), ...
//     amp EG rates 34-37 and levels 38-41, level 73, wave number expansion 83. A wave number under 32 is a wave
//     sequence of that bank; else the wave is number + expansion - 32: 0-364 the Wavestation's, 365-483 the EX's.
//   - Wave sequence step: next / previous / loop links, wave (u16 BE, bit 15 a flag; - 32 as in patches), coarse
//     (48-96: an EX wave, coarse - 72), fine, crossfade, duration, level. Played here as its first step's wave.
//   - The EX ROM describes itself. In ws3p9 / ws4p0 interleaved (ws3p9 the even bytes): from 0xFDB40, 118 waves of 14
//     bytes: first zone (u16 LE), zone count (u16 BE), name[10] (the numbers come BEFORE the name, as in the M1 OS: so
//     Drum Kit is the 44-key map and GrandPiano 12 zones). The ROM's header (0x14) holds the count: 119 waves, though
//     the SR's own copy of the table stops at 118. A type byte of 1 (VS126-VS155) marks a single-cycle wave: the 30
//     of them come first in the audio, 2427 samples each, the same cycle 9 times at less and less bandwidth: 514, 364,
//     514, 364, 258, 183, 130, 66 and 34 samples, the last 2 repeating the first (Rainer Buchty's notes, buchty.net/
//     korg/ws-rom.html); played here as six key ranges. At 0xFE1C2, 10-byte zones: sample (u16 LE), top key, 5, tune (cents),
//     original key, 4 bytes. At 0xFEF32, 299 14-byte samples: u16, then start, loop start (flag 4: loops) and end, each
//     a 24-bit LE byte address + a flag byte. The audio is 8-bit signed at 31250 Hz (the loops hold whole cycles of
//     their zone's pitch at 31.25-32 kHz), its 2 MB laid out ws3p7, ws3p8, ws4p0, ws3p9 one after another.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include "../core/parse.hpp"
#include "format.hpp"

namespace omni {
namespace {

constexpr size_t PERF = 181, PATCH = 426, N_PERF = 50, N_PATCH = 35, SEQ_STEPS = 512, SEQ_NAMES = 512 + 501 * 16;
constexpr int RAM3 = 4, ROM_RATE = 31250, N_BASE = 365;

int sx(uint8_t v) { return int(int8_t(v)); }
int be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

bool is_ws(const uint8_t *h, size_t n) { return n >= 6 && h[0] == 0xF0 && h[1] == 0x42 && (h[2] & 0xF0) == 0x30 && h[3] == 0x28; }

struct Bank {
    int bank = -1;                                     // the patches' bank (0 RAM1, 1 RAM2, 4 RAM3)
    std::vector<std::vector<uint8_t>> perfs, patches;
    std::vector<uint8_t> seqs;                         // the wave sequence block; empty: none in the file
};

constexpr size_t SEQ_BLOCK = SEQ_NAMES + 32 * 8, ALL_GLOBAL = 1813;

// A file's banks, in order: one per bank number it holds
std::vector<Bank> read_banks(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(size_t(1) << 20);
    std::vector<Bank> out;
    Bank b;
    for (size_t i = 0; i < d.size();) {
        if (d[i] != 0xF0) { i++; continue; }
        size_t e = i + 1;
        while (e < d.size() && d[e] != 0xF7) e++;
        if (e < d.size() && e - i > 8 && is_ws(&d[i], e - i)) {
            uint8_t fn = d[i + 4];
            if (fn == 0x50) {   // all data: RAM1 and RAM2 in one message, no bank byte
                std::vector<uint8_t> v;
                for (size_t k = i + 5; k + 1 < e - 1; k += 2) v.push_back(uint8_t((d[k] & 15) | (d[k + 1] & 15) << 4));
                if (v.size() >= ALL_GLOBAL + 2 * (N_PERF * PERF + N_PATCH * PATCH + SEQ_BLOCK)) {
                    const uint8_t *perf = &v[ALL_GLOBAL], *patch = perf + 2 * N_PERF * PERF, *seq = patch + 2 * N_PATCH * PATCH;
                    for (int r = 0; r < 2; r++) {
                        Bank a;
                        a.bank = r;
                        for (size_t k = 0; k < N_PERF; k++) a.perfs.emplace_back(perf + (r * N_PERF + k) * PERF, perf + (r * N_PERF + k + 1) * PERF);
                        for (size_t k = 0; k < N_PATCH; k++) a.patches.emplace_back(patch + (r * N_PATCH + k) * PATCH, patch + (r * N_PATCH + k + 1) * PATCH);
                        a.seqs.assign(seq + r * SEQ_BLOCK, seq + (r + 1) * SEQ_BLOCK);
                        out.push_back(std::move(a));
                    }
                }
                i = e + 1;
                continue;
            }
            // a message for another bank (RAM1 then RAM2 in one file) starts a new one
            if ((fn == 0x4C || fn == 0x4D || fn == 0x54) && b.bank >= 0 && d[i + 5] != b.bank) {
                out.push_back(std::move(b));
                b = Bank{};
            }
            std::vector<uint8_t> v;
            for (size_t k = i + 6; k + 1 < e - 1; k += 2) v.push_back(uint8_t((d[k] & 15) | (d[k + 1] & 15) << 4));   // (the last byte: the checksum)
            if (fn == 0x4C && v.size() >= N_PATCH * PATCH) {
                b.bank = d[i + 5];
                b.patches.clear();
                for (size_t k = 0; k < N_PATCH; k++) b.patches.emplace_back(v.begin() + k * PATCH, v.begin() + (k + 1) * PATCH);
            } else if (fn == 0x4D && v.size() >= N_PERF * PERF) {
                b.bank = d[i + 5];
                b.perfs.clear();
                for (size_t k = 0; k < N_PERF; k++) b.perfs.emplace_back(v.begin() + k * PERF, v.begin() + (k + 1) * PERF);
            } else if (fn == 0x54 && v.size() >= SEQ_BLOCK) {
                b.bank = d[i + 5];
                b.seqs = v;
            }
        }
        i = e + 1;
    }
    if (b.bank >= 0) out.push_back(std::move(b));
    return out;
}

std::string name_of(const uint8_t *p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i]; i++) s += p[i] >= 32 && p[i] < 127 ? char(p[i]) : ' ';
    return trim(s);
}

// The bank `want` (0 RAM1, 1 RAM2, 4 RAM3): this file's patches and wave sequences, the parts it lacks from the one
// file beside it that holds them (factory RAM1 + RAM2; a card split into "perf", "patch" and "seq" files)
const Bank *bank_for(Volume &vol, const std::string &path, const std::vector<Bank> &own, int want, std::map<int, Bank> &others) {
    auto it = others.find(want);
    if (it != others.end()) return it->second.bank == want ? &it->second : nullptr;
    Bank &m = others[want];
    for (const Bank &b : own)
        if (b.bank == want) {
            m.bank = want;
            if (m.patches.empty()) m.patches = b.patches;
            if (m.seqs.empty()) m.seqs = b.seqs;
        }
    if (!m.patches.empty() && !m.seqs.empty()) return &m;
    std::vector<std::vector<uint8_t>> patches;
    std::vector<uint8_t> seqs;
    int n_patches = 0, n_seqs = 0;
    try {
        for (const DirEntry &e : vol.list(path_dir(path))) {
            if (e.dir || path_ext(e.name) != "syx" || e.size > (size_t(1) << 20) || equals_ci(e.name, path_name(path))) continue;
            try {
                for (Bank &b : read_banks(vol, path_join(path_dir(path), e.name))) {
                    if (b.bank != want) continue;
                    if (!b.patches.empty() && !n_patches++) patches = std::move(b.patches);
                    if (!b.seqs.empty() && !n_seqs++) seqs = std::move(b.seqs);
                }
            } catch (const std::exception &) {}
        }
    } catch (const std::exception &) {}
    // only when exactly one file has it: in a folder of banks all made for RAM1, which one is unknown
    if (m.patches.empty() && n_patches == 1) { m.patches = std::move(patches); m.bank = want; }
    if (m.seqs.empty() && n_seqs == 1) { m.seqs = std::move(seqs); m.bank = want; }
    return m.bank == want ? &m : nullptr;
}

// ---- the EX ROM ----

constexpr uint32_t CHIP = 0x80000, TABLE_WAVES = 0xFDB44, TABLE_ZONES = 0xFE1C2, TABLE_SAMPLES = 0xFEF32;
constexpr int EX_WAVES = 118, EX_SAMPLES = 299, VS_SET = 2427;
const char *const CHIPS[4] = {"ws3p7.bin", "ws3p8.bin", "ws4p0.bin", "ws3p9.bin"};   // in address order

struct ExWave { std::string name; int first, count; bool zoned; int vs; };   // vs: its single-cycle set, -1 none
struct ExZone { int sample, top, tune, orig; };
struct ExSample { uint32_t start, loop, end; bool looped; };

struct Rom {
    std::string dir;
    BlobPtr chip[4];
    std::vector<ExWave> waves;
    std::vector<ExZone> zones;
    std::vector<ExSample> samples;
};
using RomPtr = std::shared_ptr<const Rom>;

std::vector<std::string> chip_files(const std::vector<DirEntry> &list) {
    std::vector<std::string> out(4);
    for (const DirEntry &e : list)
        for (int k = 0; k < 4; k++)
            if (!e.dir && e.size == CHIP && equals_ci(e.name, CHIPS[k])) out[size_t(k)] = e.name;
    for (const std::string &f : out) if (f.empty()) return {};
    return out;
}

bool is_rom(Volume &vol, const std::string &dir) {
    try { return !chip_files(vol.list(dir)).empty(); } catch (const std::exception &) { return false; }
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
    std::vector<std::string> f = chip_files(vol.list(dir));
    if (f.empty()) throw ParseError("Wavestation EX ROM dumps missing in " + dir);
    auto rom = std::make_shared<Rom>();
    rom->dir = dir;
    for (int k = 0; k < 4; k++) rom->chip[k] = vol.open(path_join(dir, f[size_t(k)]));
    // the tables: the last 16 KB of ws3p9 / ws4p0, interleaved
    constexpr uint32_t from = 0xFC000;
    std::vector<uint8_t> ev = rom->chip[3]->bytes(from / 2, (0x100000 - from) / 2), od = rom->chip[2]->bytes(from / 2, (0x100000 - from) / 2);
    std::vector<uint8_t> t(0x100000 - from);
    for (size_t i = 0; i < ev.size(); i++) { t[2 * i] = ev[i]; t[2 * i + 1] = od[i]; }
    auto at = [&](uint32_t a) { return &t[a - from]; };
    if (std::memcmp(at(TABLE_WAVES), "MagicOrgan", 10)) throw ParseError("not the Wavestation EX ROM (no wave table in ws3p9.bin / ws4p0.bin)");
    for (uint32_t a = TABLE_ZONES; a + 10 <= TABLE_SAMPLES; a += 10) {
        const uint8_t *z = at(a);
        rom->zones.push_back({z[0] | z[1] << 8, z[2], sx(z[4]), z[5]});
    }
    // the wave count, from the header (at 0 of the same interleave): "KORG", type 0x0F, ..., the wave table's u16 count at 0x14
    std::vector<uint8_t> hev = rom->chip[3]->bytes(0, 12), hod = rom->chip[2]->bytes(0, 12);
    int n_waves = EX_WAVES;
    if (hev[0] == 'K' && hod[0] == 'O' && hev[1] == 'R' && hod[1] == 'G') n_waves = clampi(hev[10] | hod[10] << 8, EX_WAVES, EX_WAVES + 8);
    for (int k = 0, vs = 0; k < n_waves; k++) {
        const uint8_t *e = at(TABLE_WAVES + 14 * uint32_t(k));
        int first = e[-4] | e[-3] << 8, type = e[-2], count = e[-1];
        bool zoned = type == 0 && count >= 1 && first + count <= int(rom->zones.size());
        rom->waves.push_back({name_of(e, 10), first, count, zoned, type == 1 ? vs++ : -1});
    }
    for (int k = 0; k < EX_SAMPLES; k++) {
        const uint8_t *s = at(TABLE_SAMPLES + 14 * uint32_t(k));
        auto a24 = [](const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16); };
        rom->samples.push_back({a24(s + 2), a24(s + 6), a24(s + 10), (s[9] & 4) != 0});
    }
    std::lock_guard<std::mutex> lock(roms_mutex);
    roms[dir] = rom;
    return rom;
}

// `frames` samples of the ROM's audio from byte address `start`
SampleRefPtr rom_pcm(const RomPtr &rom, uint32_t start, int64_t frames, const std::string &name) {
    auto r = std::make_shared<SampleRef>();
    r->name = name;
    r->rate = ROM_RATE;
    r->channels = 1;
    r->frames = frames;
    r->key = "wsrom:" + rom->dir + ":" + std::to_string(start) + ":" + std::to_string(frames);
    r->decode = [rom, start, frames]() -> PcmPtr {
        auto p = std::make_shared<Pcm>();
        p->rate = ROM_RATE;
        p->channels = 1;
        p->data.resize(size_t(frames));
        for (int64_t done = 0; done < frames;) {
            uint32_t a = start + uint32_t(done), chip = a / CHIP, off = a % CHIP;
            if (chip > 3) throw ParseError("Wavestation sample past the end of the ROM");
            size_t n = size_t(std::min<int64_t>(frames - done, int64_t(CHIP - off)));
            std::vector<uint8_t> b = rom->chip[chip]->bytes(off, n);
            for (size_t i = 0; i < n; i++) p->data[size_t(done) + i] = int16_t(int8_t(b[i]) * 256);
            done += int64_t(n);
        }
        return p;
    };
    return r;
}

SampleRefPtr rom_sample(const RomPtr &rom, int n) {
    const ExSample &s = rom->samples[size_t(n)];
    return rom_pcm(rom, s.start, int64_t(s.end) - s.start + 1, "EX " + std::to_string(n));
}

// ---- patches ----

double eg_time(int rate) { return 0.002 + 0.06 * (std::exp((99 - clampi(rate, 0, 99)) * 0.0577) - 1.0); }   // 99 fast, 0 ~18 s

// the amp EG: start level 0, rate 1 to level 1, rate 2 to level 2, rate 3 to level 3 (sustain), rate 4 (release); as the
// sampler's ADSR: the attack up to the highest of levels 0-2 (none when the EG starts there), the decay on to level 3
double apply_aeg(Zone &z, const uint8_t *o) {
    int r[4] = {o[34], o[35], o[36], o[37]};
    int l[4] = {clampi(o[38], 0, 99), clampi(o[39], 0, 99), clampi(o[40], 0, 99), clampi(o[41], 0, 99)};
    int top = 0;
    for (int i = 1; i < 3; i++) if (l[i] > l[top]) top = i;
    int peak = std::max(1, l[top]);
    Envelope &e = z.amp_env;
    e.set = true;
    e.start_level = double(l[0]) / peak;
    e.attack = 0.002;
    for (int i = 0; i < top; i++) e.attack += eg_time(r[i]);
    e.decay = 0.002;
    for (int i = top; i < 3; i++) if (l[i + 1] != l[i]) e.decay += eg_time(r[i]);
    e.sustain = double(l[3]) / peak;
    e.release = eg_time(r[3]);
    return 20.0 * std::log10(peak / 99.0);
}

struct Ctx {
    VolumePtr vol;
    std::string path;
    const std::vector<Bank> *own;
    std::map<int, Bank> others;
    RomPtr rom;
    const Bank *bank(int want) { return bank_for(*vol, path, *own, want, others); }
};

void add_wave(Instrument &inst, Ctx &c, const std::string &label, int wave, double tune, double gain, const uint8_t *osc) {
    if (wave < N_BASE) {
        inst.warnings.push_back(label + " plays Wavestation wave " + std::to_string(wave) + ": its ROM is not available");
        return;
    }
    int ex = wave - N_BASE;
    if (ex >= int(c.rom->waves.size())) { inst.warnings.push_back(label + " plays wave " + std::to_string(wave) + ": unknown"); return; }
    const ExWave &w = c.rom->waves[size_t(ex)];
    if (w.vs >= 0) {
        // a single cycle: the cycle with the most harmonics up to the note where its top one (40 ... 4) reaches ~20 kHz
        // (sounding notes: the keys move by the oscillator's transpose)
        static const struct { int at, len, top; } levels[6] = {{0, 512, 71}, {1756, 256, 75}, {2014, 181, 81}, {2197, 128, 87}, {2327, 64, 99}, {2393, 32, 999}};
        int lo = 0, shift = int(std::lround(tune));
        for (const auto &v : levels) {
            if (lo > 127) break;
            Zone out;
            out.name = label + " " + w.name + " " + std::to_string(v.len);
            out.sample = rom_pcm(c.rom, uint32_t(w.vs * VS_SET + v.at), v.len, "EX " + w.name + " " + std::to_string(v.len));
            out.key_lo = lo;
            out.key_hi = clampi(v.top - shift, lo, 127);
            lo = out.key_hi + 1;
            double pitch = 69 + 12 * std::log2(double(ROM_RATE) / v.len / 440.0);   // the cycle played as is
            out.root = int(std::lround(pitch));
            out.tune = pitch - out.root + tune;
            Loop l;
            l.end = v.len - 1;
            out.loops.push_back(l);
            out.gain_db = gain + apply_aeg(out, osc);
            inst.zones.push_back(std::move(out));
        }
        return;
    }
    if (!w.zoned) { inst.warnings.push_back(label + " plays EX wave " + w.name + ": not read yet"); return; }
    int lo = 0;
    for (int i = 0; i < w.count; i++) {
        const ExZone &z = c.rom->zones[size_t(w.first + i)];
        if (z.sample < 0 || z.sample >= int(c.rom->samples.size())) continue;
        const ExSample &s = c.rom->samples[size_t(z.sample)];
        Zone out;
        out.name = label + " " + w.name + " " + std::to_string(i + 1);
        out.sample = rom_sample(c.rom, z.sample);
        out.key_lo = lo;
        out.key_hi = i + 1 == w.count ? 127 : z.top;
        lo = z.top + 1;
        out.root = z.orig;
        out.tune = z.tune / 100.0 + tune;
        if (s.looped && s.loop >= s.start && s.loop <= s.end) {
            Loop l;
            l.start = s.loop - s.start;
            l.end = s.end - s.start;
            out.loops.push_back(l);
        }
        out.gain_db = gain + apply_aeg(out, osc);
        if (out.key_lo <= out.key_hi) inst.zones.push_back(std::move(out));
    }
}

Instrument load_patch(Ctx &c, const Bank &b, int n) {
    const std::vector<uint8_t> &p = b.patches[size_t(n)];
    Instrument inst;
    inst.name = name_of(p.data(), 16);
    inst.format = "Korg Wavestation patch";
    int oscs = p[86] == 0 ? 1 : p[86] == 1 ? 2 : 4;
    // the joystick's start point (mix envelope point 0): X between A and C (B for 2 waves), Y between B and D
    double x = clampd(p[76] / 254.0, 0, 1), y = clampd((sx(p[81]) + 127) / 254.0, 0, 1);
    double weight[4] = {1, 0, 0, 0};
    if (oscs == 2) { weight[0] = 1 - x; weight[1] = x; }
    if (oscs == 4) { weight[0] = (1 - x) / 2; weight[2] = x / 2; weight[1] = (1 - y) / 2; weight[3] = y / 2; }
    for (int o = 0; o < oscs; o++) {
        const uint8_t *osc = &p[90 + 84 * size_t(o)];
        std::string label(1, char('A' + o));
        if (weight[o] < 0.02) continue;   // mixed out at the start point
        double gain = 20.0 * std::log10(weight[o]) + 20.0 * std::log10(std::max(1, clampi(osc[73], 0, 99)) / 99.0);
        double tune = sx(osc[0]) + sx(osc[1]) / 100.0;
        int num = be16(osc + 3);
        if (num >= 32) { add_wave(inst, c, label, num + sx(osc[83]) - 32, tune, gain, osc); continue; }
        // a wave sequence: its first step
        int want = (p[88] >> o & 1) ? RAM3 : osc[2];
        const Bank *sb = want == 0 || want == 1 || want == RAM3 ? c.bank(want) : nullptr;
        if (!sb || sb->seqs.empty()) {
            inst.warnings.push_back(label + " plays a wave sequence of bank " + std::string(want == 2 ? "ROM" : want == 3 ? "card" : want == RAM3 ? "RAM3" : want ? "RAM2" : "RAM1") + ": not in this folder");
            continue;
        }
        const std::vector<uint8_t> &s = sb->seqs;
        int step = be16(&s[16 * size_t(num)]);
        if (step <= 0 || step >= 501) { inst.warnings.push_back(label + " plays an empty wave sequence"); continue; }
        const uint8_t *st = &s[SEQ_STEPS + 16 * size_t(step)];
        int wave = (be16(st + 6) & 0x7FFF) - 32, coarse = sx(st[8]);
        if (coarse >= 48 && coarse <= 96) { coarse -= 72; wave += N_BASE; }
        inst.warnings.push_back(label + " plays wave sequence " + name_of(&s[SEQ_NAMES + 8 * size_t(num)], 8) + ": its first wave only");
        add_wave(inst, c, label, wave, tune + coarse + sx(st[9]) / 100.0, gain, osc);
    }
    return inst;
}

Instrument load_perf(Ctx &c, const Bank &b, int n) {
    const std::vector<uint8_t> &f = b.perfs[size_t(n)];
    Instrument inst;
    inst.name = name_of(f.data(), 16);
    inst.format = "Korg Wavestation performance";
    for (int t = 0; t < 8; t++) {
        const uint8_t *q = &f[37 + 18 * size_t(t)];
        int bank = sx(q[0]), patch = sx(q[1]), level = clampi(q[2], 0, 99);
        if (patch < 0 || patch >= int(N_PATCH) || level == 0 || bank < 0) continue;
        int want = (q[4] >> 6 & 1) ? RAM3 : bank;
        std::string part = "P" + std::to_string(t + 1);
        const Bank *pb = want == 0 || want == 1 || want == RAM3 ? c.bank(want) : nullptr;
        if (!pb || pb->patches.empty()) {
            inst.warnings.push_back(part + " plays " + std::string(want == 2 ? "ROM" : want == 3 ? "card" : want == RAM3 ? "RAM3" : want ? "RAM2" : "RAM1") +
                                    " patch " + std::to_string(patch) + ": not in this folder");
            continue;
        }
        Instrument m = load_patch(c, *pb, patch);
        int lo = q[5] & 127, hi = q[6] & 127, vlo = q[7] & 127, vhi = q[8] & 127;
        if (vhi == 0) vhi = 127;
        double tune = clampi(sx(q[9]), -48, 48) + clampi(sx(q[10]), -99, 99) / 100.0;
        for (auto &z : m.zones) {
            z.key_lo = std::max(z.key_lo, std::min(lo, hi));
            z.key_hi = std::min(z.key_hi, std::max(lo, hi));
            z.vel_lo = std::max(z.vel_lo, std::min(vlo, vhi));
            z.vel_hi = std::min(z.vel_hi, std::max(vlo, vhi));
            if (z.key_lo > z.key_hi || z.vel_lo > z.vel_hi) continue;
            z.tune += tune;
            z.gain_db += 20.0 * std::log10(level / 99.0);
            inst.zones.push_back(std::move(z));
        }
        for (auto &w : m.warnings) inst.warnings.push_back(part + " " + m.name + ": " + w);
    }
    return inst;
}

// ---- the reader ----

bool probe_syx(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    for (size_t i = 0; i + 6 <= n; i++)
        if (h[i] == 0xF0 && is_ws(h + i, n - i) && (h[i + 4] == 0x4C || h[i + 4] == 0x4D || h[i + 4] == 0x54 || h[i + 4] == 0x50)) return true;
    return false;
}

// performances as 0, 1 ..., patches as 1000 + n, numbered on through the file's banks (Pf50 is RAM2's first)
std::vector<PresetInfo> list_syx(VolumePtr vol, const std::string &path) {
    std::vector<Bank> banks = read_banks(*vol, path);
    std::vector<PresetInfo> out;
    char num[32];
    int n = 0;
    for (const Bank &b : banks)
        for (const auto &f : b.perfs) {
            std::snprintf(num, sizeof num, "Pf%02d ", n);
            out.push_back({num + name_of(f.data(), 16), n++});
        }
    n = 0;
    for (const Bank &b : banks)
        for (const auto &p : b.patches) {
            std::snprintf(num, sizeof num, "Pt%02d ", n);
            out.push_back({num + name_of(p.data(), 16), 1000 + n++});
        }
    if (out.empty()) {
        bool seqs = false;
        for (const Bank &b : banks) seqs |= !b.seqs.empty();
        throw ParseError(seqs ? "Wavestation wave sequences only: no sounds in this file" : "no Wavestation performances or patches in this file");
    }
    return out;
}

Instrument load_syx(VolumePtr vol, const std::string &path, int index) {
    std::vector<Bank> banks = read_banks(*vol, path);
    Ctx c;
    c.vol = vol;
    c.path = path;
    c.own = &banks;
    std::string dir = find_beside(*vol, path, is_rom, cached_rom);
    if (dir.empty())
        throw ParseError("Wavestation waves not found: put the Wavestation EX ROM dumps (ws3p7.bin, ws3p8.bin, ws3p9.bin, "
                         "ws4p0.bin) beside the .syx files or in the Plugin Library");
    c.rom = open_rom(*vol, dir);
    Instrument inst;
    bool patch = index >= 1000;
    int k = patch ? index - 1000 : index;
    const Bank *in = nullptr;
    for (const Bank &b : banks) {
        int count = int(patch ? b.patches.size() : b.perfs.size());
        if (k >= 0 && k < count) { in = &b; break; }
        k -= count;
    }
    if (!in) throw ParseError(patch ? "no such Wavestation patch" : "no such Wavestation performance");
    inst = patch ? load_patch(c, *in, k) : load_perf(c, *in, k);
    inst.library = "Korg Wavestation";
    inst.category = path_stem(path);
    finish_instrument(inst);
    if (inst.zones.empty()) {   // say why: a missing ROM / bank first, not the wave sequence note
        for (const char *why : {"not available", "not in this folder", "not read yet"})
            for (const std::string &w : inst.warnings)
                if (w.find(why) != std::string::npos) throw ParseError("nothing to play: " + w);
        throw ParseError(inst.warnings.empty() ? "an empty Wavestation slot: no parts or waves set" : "nothing to play: " + inst.warnings[0]);
    }
    return inst;
}

}  // namespace

void register_korg_ws() {
    register_reader({"Korg Wavestation SysEx", "syx", probe_syx, list_syx, load_syx});
}

}  // namespace omni

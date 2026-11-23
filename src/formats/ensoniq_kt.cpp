// Omni Sampler: the Ensoniq KT-76's 223 waves, played from its ROM dumps (MAME's kt76 set: OS kt76_162_hi / _lo, wave
// ROMs 1351000401_rom0.u103 and 1351000402_rom1.u102; the third, 1351000403_rom2.u104, is not used by any wave).
// Each wave opens as a multisample, under its name from the OS.
// Decoded 2026-10-21 from OS 1.62 and the ROMs above:
//   - OS image: 32-bit words, the hi chip's 16-bit word then the lo chip's, each byte-swapped (68EC020, at 0).
//   - Waves are picked by class and number: `movea.l #CLASSES,a6 / movea.l (a6,d0.w),a6 / move.w (a6,d1.w),d0 /
//     smi / and.w #0x3FFF,d0 / movea.l #ZONES,a6 / adda.w d0,a6`, then `cmp.b (a6),d2 / bls / adda.w #16,a6` walks the
//     zones. CLASSES: 16 pointers into one list of u16 entries (the counts per class follow the pointers); all 225
//     entries in order match the 16-character names that end with WAVE-LIST and DRUM-MAP (bit 14 set: not waves).
//     Entry bit 15 picks the wave ROM: 0 rom0, 1 rom1 (the zones then cover 99.8% / 98.4% of the two chips).
//   - Zone, 16 bytes: top key, control (bits 0-1: 0 one shot, 1 / 2 loop, 3 back and forth; bit 6 compressed samples;
//     bit 7 the transwave / cross wave family, looped), pitch u16, start u32, loop u32, end u32; addresses are sample
//     numbers x 16 (the low 4 bits a fraction: some loop ends fall between samples, CLARINET's 83.5625 samples long).
//     The chip jumps back by end - loop on reaching end, so the sample at end (a copy of the one at loop on 239 of the
//     245 loops that tell) is never played: the loop is (end - loop) / 16 samples from loop (see chip_loop()).
//   - Samples: one byte each. With control bit 6 (all but 5 zones), the OTTO chip's compressed format: 3-bit exponent,
//     sign (set = positive), 4-bit mantissa with an implied leading one (see ulaw()); without it, signed 8-bit linear
//     (1K TEST SINE is one clean cycle that way).
//   - Pitch: root = 256 - pitch / 256 at 29762 Hz, like the SD-1 (exact on the single-cycle waveforms).
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include "../third_party/tinf/tinf.h"
#include "format.hpp"

namespace omni {
namespace {

constexpr int RATE = 29762;
constexpr uint64_t WAVE_CHIP = 0x200000;

enum Part { OS_HI, OS_LO, ROM0, ROM1, ROM2, N_PARTS };
struct PartInfo { uint32_t head_crc; uint64_t size; const char *name; };
const PartInfo PARTS[N_PARTS] = {
    {0x6FF7F02D, 0x40000, "kt76_162_hi.bin"},       // de16d236
    {0xFBB135DD, 0x20000, "kt76_162_lo.bin"},       // 1a1ab910
    {0x107B3072, WAVE_CHIP, "1351000401_rom0.u103"},  // 425047af
    {0x563C6093, WAVE_CHIP, "1351000402_rom1.u102"},  // 64459185
    {0x866912CE, WAVE_CHIP, "1351000403_rom2.u104"},  // c2aacc5d
};

int part_of(const uint8_t *h, size_t n, uint64_t size) {
    if (n < 4096) return -1;
    uint32_t crc = tinf_crc32(h, 4096);
    for (int k = 0; k < N_PARTS; k++)
        if (PARTS[k].size == size && PARTS[k].head_crc == crc) return k;
    return -1;
}

// a compressed sample byte as 16 bits
// (the chip reads the byte as the top of a 16-bit word: sign and mantissa move up under the exponent, half a step added;
// with an exponent the inverted sign becomes the leading one)
int16_t ulaw(uint8_t b) {
    int exp = b >> 5;
    uint16_t m = uint16_t((b << 8 | 0x80) << 3);
    if (!exp) return int16_t(int16_t(m) >> 7);
    m = uint16_t(m >> 1 | (~m & 0x8000));
    return int16_t(int16_t(m) >> (7 - exp));
}

struct KtZone { int top, ctl, pitch; uint32_t start, loop, end; double loop_len; };
struct KtWave { std::string name; int chip; std::vector<KtZone> zones; };

struct Rom {
    std::string dir;
    BlobPtr chip[2];
    std::vector<KtWave> waves;
};
using RomPtr = std::shared_ptr<const Rom>;

std::mutex cache_mutex;
std::map<std::string, RomPtr> roms;

uint32_t be32(const std::vector<uint8_t> &d, size_t at) { return uint32_t(d[at] << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3]); }

RomPtr open_rom(Volume &vol, const std::string &dir) {
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = roms.find(dir);
        if (it != roms.end()) return it->second;
    }
    std::string path[N_PARTS];
    for (const DirEntry &e : vol.list(dir)) {
        if (e.dir) continue;
        bool sized = false;
        for (const PartInfo &p : PARTS) sized |= p.size == e.size;
        if (!sized) continue;
        try {
            std::vector<uint8_t> h = vol.open(path_join(dir, e.name))->head(4096);
            int k = part_of(h.data(), h.size(), e.size);
            if (k >= 0 && path[k].empty()) path[k] = path_join(dir, e.name);
        } catch (const std::exception &) {}
    }
    for (int k : {OS_HI, OS_LO, ROM0, ROM1})
        if (path[k].empty()) throw ParseError(std::string("KT-76 ROM missing: put ") + PARTS[k].name + " with the others in one folder");
    auto rom = std::make_shared<Rom>();
    rom->dir = dir;
    rom->chip[0] = vol.open(path[ROM0]);
    rom->chip[1] = vol.open(path[ROM1]);
    std::vector<uint8_t> hi = vol.open(path[OS_HI])->all(PARTS[OS_HI].size), lo = vol.open(path[OS_LO])->all(PARTS[OS_LO].size);
    std::vector<uint8_t> os(2 * hi.size(), 0xFF);
    for (size_t i = 0; i + 1 < hi.size(); i += 2) { os[2 * i] = hi[i + 1]; os[2 * i + 1] = hi[i]; }
    for (size_t i = 0; i + 1 < lo.size(); i += 2) { os[2 * i + 2] = lo[i + 1]; os[2 * i + 3] = lo[i]; }
    // `and.w #0x3FFF,d0 / movea.l #ZONES,a6`, and before it `movea.l #CLASSES,a6 / movea.l (a6,d0.w),a6` and
    // `movea.l #COUNTS,a0 / cmp.b (a0,d0.w),d1`
    size_t zones = 0, classes = 0, counts = 0;
    for (size_t i = 64; i + 10 <= os.size() && !zones; i += 2)
        if (os[i] == 0xC0 && os[i + 1] == 0x7C && os[i + 2] == 0x3F && os[i + 3] == 0xFF && os[i + 4] == 0x2C && os[i + 5] == 0x7C) {
            zones = be32(os, i + 6);
            for (size_t j = i - 2; j + 64 > i && j >= 6; j -= 2) {
                if (!classes && os[j] == 0x2C && os[j + 1] == 0x7C && os[j + 6] == 0x2C && os[j + 7] == 0x76) classes = be32(os, j + 2);
                if (!counts && os[j] == 0x20 && os[j + 1] == 0x7C && os[j + 6] == 0xB2 && os[j + 7] == 0x30) counts = be32(os, j + 2);
            }
        }
    if (!zones || !classes || counts <= classes || (counts - classes) % 4 || zones >= os.size() || counts + 64 > os.size())
        throw ParseError("no wave table in the KT-76 OS");
    size_t n_classes = (counts - classes) / 4, first = be32(os, classes), total = 0;
    for (size_t c = 0; c < n_classes; c++) total += os[counts + c];
    if (first + 2 * total > os.size()) throw ParseError("KT-76 wave list out of range");
    // the names: 16 characters each, the list ending with DRUM-MAP
    static const char last[] = "DRUM-MAP        ";
    size_t names = 0;
    for (size_t i = 16 * total; i + 16 <= os.size() && !names; i++)
        if (!std::memcmp(&os[i], last, 16)) names = i + 16 - 16 * total;
    for (size_t w = 0; w < total; w++) {
        uint16_t e = uint16_t(os[first + 2 * w] << 8 | os[first + 2 * w + 1]);
        if (e & 0x4000) continue;                      // the wave list and the drum map
        KtWave wave;
        wave.chip = e >> 15;
        if (names) {
            std::string s;
            for (size_t k = 0; k < 16; k++) { char ch = char(os[names + 16 * w + k]); s += ch >= 32 && ch < 127 ? ch : ' '; }
            wave.name = trim(s);
        }
        if (wave.name.empty()) wave.name = "Wave " + std::to_string(w + 1);
        size_t at = zones + (e & 0x3FFF);
        for (int k = 0; k < 64 && at + 16 <= os.size(); k++, at += 16) {
            KtZone z;
            z.top = os[at];
            z.ctl = os[at + 1];
            z.pitch = os[at + 2] << 8 | os[at + 3];
            z.start = be32(os, at + 4) >> 4;
            z.loop = be32(os, at + 8) >> 4;
            z.end = be32(os, at + 12) >> 4;
            z.loop_len = (double(be32(os, at + 12)) - double(be32(os, at + 8))) / 16.0;
            wave.zones.push_back(z);
            if (z.top >= 127) break;
        }
        rom->waves.push_back(std::move(wave));
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    roms[dir] = rom;
    return rom;
}

SampleRefPtr rom_pcm(const RomPtr &rom, int chip, bool compressed, uint32_t start, int64_t frames, const std::string &name) {
    auto r = std::make_shared<SampleRef>();
    r->name = name;
    r->rate = RATE;
    r->channels = 1;
    r->frames = frames;
    r->key = "kt76:" + rom->dir + ":" + std::to_string(chip) + (compressed ? "c:" : "l:") + std::to_string(start) + ":" + std::to_string(frames);
    r->decode = [rom, chip, compressed, start, frames]() -> PcmPtr {
        if (uint64_t(start) + uint64_t(frames) > WAVE_CHIP) throw ParseError("KT-76 wave past the end of its ROM");
        std::vector<uint8_t> b = rom->chip[chip]->bytes(start, size_t(frames));
        auto p = std::make_shared<Pcm>();
        p->rate = RATE;
        p->channels = 1;
        p->data.resize(size_t(frames));
        for (size_t i = 0; i < b.size(); i++) p->data[i] = compressed ? ulaw(b[i]) : int16_t(int8_t(b[i]) * 256);
        return p;
    };
    return r;
}

// ---- the reader ----

bool probe_rom(const std::string &, const uint8_t *h, size_t n, uint64_t size) { return part_of(h, n, size) >= 0; }

std::vector<PresetInfo> list_rom(VolumePtr vol, const std::string &path) {
    RomPtr rom = open_rom(*vol, path_dir(path));
    std::vector<PresetInfo> out;
    for (size_t w = 0; w < rom->waves.size(); w++) {
        char num[16];
        std::snprintf(num, sizeof num, "%03d ", int(w) + 1);
        out.push_back({num + rom->waves[w].name, int(w)});
    }
    return out;
}

Instrument load_rom(VolumePtr vol, const std::string &path, int index) {
    RomPtr rom = open_rom(*vol, path_dir(path));
    if (index < 0 || index >= int(rom->waves.size())) throw ParseError("no such KT-76 wave");
    const KtWave &w = rom->waves[size_t(index)];
    Instrument inst;
    inst.name = w.name;
    inst.format = "Ensoniq KT-76 wave";
    int lo = 0;
    for (const KtZone &z : w.zones) {
        int top = std::min(127, z.top);
        if (top < lo) continue;
        if (z.end <= z.start || z.end >= WAVE_CHIP) { inst.warnings.push_back("zone to key " + std::to_string(top) + ": bad addresses"); lo = top + 1; continue; }
        Zone out;
        out.name = w.name + " " + std::to_string(inst.zones.size() + 1);
        int64_t frames = int64_t(z.end - z.start) + 1;
        out.sample = rom_pcm(rom, w.chip, (z.ctl & 0x40) != 0, z.start, frames, out.name);
        out.key_lo = lo;
        out.key_hi = top;
        double root = 256.0 - z.pitch / 256.0;
        out.root = int(std::lround(root));
        out.tune = out.root - root;   // the sample sounds at root: play it that much lower
        int mode = z.ctl & 3;
        if ((mode || (z.ctl & 0x80)) && z.loop >= z.start && z.loop + 1 < z.end) {
            Loop l = chip_loop(int64_t(z.loop - z.start), z.loop_len, frames - 1);
            if (mode == 3) l.type = LoopType::Alternating;
            out.loops.push_back(l);
        } else out.one_shot = true;
        inst.zones.push_back(std::move(out));
        lo = top + 1;
    }
    if (inst.zones.empty()) throw ParseError("nothing to play: " + w.name);
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_ensoniq_kt() {
    register_reader({"Ensoniq KT-76 ROM", "", probe_rom, list_rom, load_rom});
}

}  // namespace omni

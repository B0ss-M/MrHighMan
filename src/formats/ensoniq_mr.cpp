// Omni Sampler: the Ensoniq MR Rack / MR61 / MR76 waves, played from the wave ROM dumps (MAME's mrrack set:
// 1351000901_h-rom0.u5, 1351000902_h-rom1.u23, 1351000903_h-rom2.u24). Each ROM describes itself, so each opens on
// its own, with no OS, and lists its waves under their names; a wave opens as a multisample.
// Decoded 2026-10-21 from the ROMs above:
//   - The header and tables sit in the odd bytes (one byte per 16-bit word, even bytes 0): 0x1065 + n, 0, 0x40, 0,
//     "SNDR", name length 14, entry size 22, 0x10, 0x0050, first table byte u16, table end byte u16, wave count u16,
//     zone count u16, the ROM's name (16, "Hornet ROM A"); then from word 42 the waves, 22 bytes each: table byte u16,
//     a u32 (a checksum?), name (14).
//   - Table bytes count ROM bytes (word = byte / 2). A wave's zones run from its own to the next wave's (or the table
//     end), the last one up to key 127. Zone, 16 bytes: top key, control, pitch u16, start u32, loop u32, end u32;
//     addresses are sample numbers x 16, the low 4 bits a fraction. The sample at end repeats the one at loop (loop
//     length (end - loop) / 16, which may end between samples: see chip_loop()).
//   - Control: 0 one shot (its loop field is ignored), 3 back and forth, other values loop forward (bit 2: the cross wave family, whose loop may
//     begin before the start).
//   - Samples: 16-bit little endian.
//   - Pitch: root = 256 - pitch / 256 at 29762 Hz, like the KT-76 (within 0.012 st on 467 single-cycle loops).
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include "format.hpp"

namespace omni {
namespace {

constexpr int RATE = 29762;
constexpr size_t DIR_WORD = 42, ENTRY = 22, NAME = 14;

struct MrZone { int top, ctl, pitch; uint32_t start, loop, end; double loop_len; };
struct MrWave { std::string name; std::vector<MrZone> zones; };

struct Rom {
    std::string path;
    BlobPtr blob;
    std::vector<MrWave> waves;
};
using RomPtr = std::shared_ptr<const Rom>;

std::mutex cache_mutex;
std::map<std::string, RomPtr> roms;

bool is_header(const uint8_t *h, size_t n, uint64_t size) {
    if (n < 2 * (DIR_WORD + ENTRY) || size < 0x100000 || size % 0x100000) return false;
    static const char tag[] = "SNDR";
    for (int k = 0; k < 4; k++)
        if (h[0x10 + 2 * k] || h[0x11 + 2 * k] != uint8_t(tag[k])) return false;
    return h[0x19] == NAME && h[0x1B] == ENTRY;
}

RomPtr open_rom(Volume &vol, const std::string &path) {
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = roms.find(path);
        if (it != roms.end()) return it->second;
    }
    auto rom = std::make_shared<Rom>();
    rom->path = path;
    rom->blob = vol.open(path);
    uint64_t size = rom->blob->size();
    std::vector<uint8_t> h = rom->blob->head(2 * (DIR_WORD + ENTRY));
    if (!is_header(h.data(), h.size(), size)) throw ParseError("not an Ensoniq MR wave ROM");
    auto b = [&](size_t i) { return h[2 * i + 1]; };   // header byte i
    size_t table_end = size_t(b(20) << 8 | b(21)) / 2, count = size_t(b(22) << 8 | b(23));
    if (table_end < DIR_WORD + count * ENTRY || 2 * table_end > size) throw ParseError("MR wave table out of range");
    std::vector<uint8_t> raw = rom->blob->bytes(0, 2 * table_end);
    std::vector<uint8_t> t(table_end);
    for (size_t i = 0; i < table_end; i++) t[i] = raw[2 * i + 1];
    auto u16 = [&](size_t i) { return uint32_t(t[i] << 8 | t[i + 1]); };
    auto u32 = [&](size_t i) { return u16(i) << 16 | u16(i + 2); };
    std::vector<size_t> first(count + 1, table_end);
    for (size_t w = 0; w < count; w++) first[w] = u16(DIR_WORD + w * ENTRY) / 2;
    uint64_t samples = size / 2;
    for (size_t w = 0; w < count; w++) {
        MrWave wave;
        std::string s;
        for (size_t k = 0; k < NAME; k++) {
            char ch = char(t[DIR_WORD + w * ENTRY + 6 + k]);
            if (!ch) break;
            s += ch >= 32 && ch < 127 ? ch : ' ';
        }
        wave.name = trim(s);
        if (wave.name.empty()) wave.name = "Wave " + std::to_string(w + 1);
        size_t end = std::min(first[w + 1] > first[w] ? first[w + 1] : table_end, table_end);
        for (size_t at = first[w]; at + 16 <= end; at += 16) {
            MrZone z{t[at], t[at + 1], int(u16(at + 2)), u32(at + 4) >> 4, u32(at + 8) >> 4, u32(at + 12) >> 4,
                     (double(u32(at + 12)) - double(u32(at + 8))) / 16.0};
            if (!z.ctl) z.loop = z.start;   // one shots never loop; "Spoons" (ROM B) has a stray bit 27 there
            if (z.end >= samples || z.start >= samples || z.loop >= samples) break;
            wave.zones.push_back(z);
            if (z.top >= 127) break;
        }
        rom->waves.push_back(std::move(wave));
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    roms[path] = rom;
    return rom;
}

// ---- the reader ----

bool probe_rom(const std::string &, const uint8_t *h, size_t n, uint64_t size) { return is_header(h, n, size); }

std::vector<PresetInfo> list_rom(VolumePtr vol, const std::string &path) {
    RomPtr rom = open_rom(*vol, path);
    std::vector<PresetInfo> out;
    for (size_t w = 0; w < rom->waves.size(); w++) {
        char num[16];
        std::snprintf(num, sizeof num, "%03d ", int(w) + 1);
        out.push_back({num + rom->waves[w].name, int(w)});
    }
    return out;
}

Instrument load_rom(VolumePtr vol, const std::string &path, int index) {
    RomPtr rom = open_rom(*vol, path);
    if (index < 0 || index >= int(rom->waves.size())) throw ParseError("no such MR wave");
    const MrWave &w = rom->waves[size_t(index)];
    Instrument inst;
    inst.name = w.name;
    inst.format = "Ensoniq MR wave";
    int lo = 0;
    for (const MrZone &z : w.zones) {
        int top = std::min(127, z.top);
        if (top < lo) continue;
        uint32_t first = std::min(z.start, z.loop);
        if (z.end <= first) { inst.warnings.push_back("zone to key " + std::to_string(top) + ": bad addresses"); lo = top + 1; continue; }
        Zone out;
        out.name = w.name + " " + std::to_string(inst.zones.size() + 1);
        int64_t frames = int64_t(z.end - first) + 1;
        out.sample = raw_sample_ref(rom->blob, 2 * uint64_t(first), frames, 1, Enc::S16LE, RATE, out.name);
        out.start = int64_t(z.start - first);
        out.key_lo = lo;
        out.key_hi = top;
        double root = 256.0 - z.pitch / 256.0;
        out.root = int(std::lround(root));
        out.tune = out.root - root;   // the sample sounds at root: play it that much lower
        if (z.ctl && z.end > z.loop + 1) {
            Loop l = chip_loop(int64_t(z.loop - first), z.loop_len, frames - 1);
            if (z.ctl == 3) l.type = LoopType::Alternating;
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

void register_ensoniq_mr() {
    register_reader({"Ensoniq MR wave ROM", "", probe_rom, list_rom, load_rom});
}

}  // namespace omni

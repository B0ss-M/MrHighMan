// Omni Sampler: E-mu Emulator (1981) disks (.emufd / .ei raw images, .hfe through the floppy container). A disk
// holds the OS, a lower and an upper bank (24 keys each, up to 8 zones) and the sequencer; one disk is one
// instrument. Audio is companded 8-bit (AM6072) at 27778 Hz. Translated from ConvertWithMoss's Emulator1Detector /
// Emulator1Constants / EmuCompanding (LGPL-3.0).
#include <cmath>
#include "format.hpp"

namespace omni {

int16_t emu_expand(uint8_t value) {   // AM6072 companding DAC, shared by the Emulator I/II readers
    int chord = value >> 4 & 7, step = value & 15;
    int magnitude = (((step << 1) + 33) << chord) - 33;
    return int16_t(value & 0x80 ? -magnitude * 4 : magnitude * 4);
}

namespace {

constexpr int TRACK = 3584, NUM_TRACKS = 35, IMAGE_SIZE = NUM_TRACKS * TRACK;
constexpr int LOWER = 2 * TRACK, UPPER = 18 * TRACK, BANK_ADDR = 0x2000, HEADER = 0x100, MEM_END = 0x10000;
constexpr int RECORD = 16, FIRST_ZONE = 0x10, MAX_ZONES = 8, TABLE_ENTRIES = 25, RATE = 27778;
constexpr int LOWEST_KEY = 36, KEYS_HALF = 24, NUM_KEYS = 49;
const double FILTER_CUTOFF[8] = {-1, 19300, 18200, 14500, 9000, 5000, 2200, 800};

int rd(const std::vector<uint8_t> &d, size_t o) { return o + 1 < d.size() ? le16(&d[o]) : 0; }
double pitch_cents(int value) {
    int period = 3072 - value;
    return period <= 0 ? 0 : 1200.0 * std::log2(double(3072 - 2656) / period);
}

bool make_zone(BlobPtr blob, const std::vector<uint8_t> &img, int bank, int rec, const std::string &name, int lo, int hi, Zone &z) {
    size_t r = size_t(bank + rec);
    int start = rd(img, r + 4);
    int loop_start = rd(img, r + 6) + 1, loop_len = rd(img, r + 10) + 1, release = rd(img, r + 14);
    if (start < BANK_ADDR + RECORD || start >= MEM_END) return false;
    int loop_end = loop_start + loop_len;
    int frames = std::min(loop_end + release - 3, MEM_END - start);
    int audio = bank + start - BANK_ADDR;
    frames = std::min(frames, int(img.size()) - audio);
    if (frames <= 0) return false;
    auto ref = std::make_shared<SampleRef>();
    ref->name = name; ref->rate = RATE; ref->channels = 1; ref->frames = frames;
    ref->key = "e1:" + std::to_string(reinterpret_cast<uintptr_t>(blob.get())) + ":" + std::to_string(audio);
    ref->decode = [blob, audio, frames]() {
        std::vector<uint8_t> raw = blob->bytes(uint64_t(audio), size_t(frames));
        auto p = std::make_shared<Pcm>();
        p->rate = RATE; p->channels = 1; p->data.resize(size_t(frames));
        for (int i = 0; i < frames; i++) p->data[size_t(i)] = emu_expand(raw[size_t(i)]);
        return PcmPtr(p);
    };
    z = Zone();
    z.name = name;
    z.sample = ref;
    z.key_lo = lo; z.key_hi = hi;
    if (loop_len > 2 && loop_end <= frames) { Loop l; l.start = loop_start; l.end = loop_end; z.loops.push_back(l); }
    int fs = img[r + 1] >> 5;
    if (fs > 0) { z.filter.type = FilterType::LowPass; z.filter.poles = 2; z.filter.cutoff = std::min(FILTER_CUTOFF[fs], MAX_CUTOFF_HZ); }
    return true;
}

void pitch_table(const std::vector<uint8_t> &img, size_t table, Zone &z) {
    int root = -1;
    double root_cents = 0, prev = 0;
    bool tracks = true;
    for (int key = z.key_lo; key <= z.key_hi; key++) {
        int idx = key - LOWEST_KEY;
        if (idx < 0 || idx >= TABLE_ENTRIES) break;
        double c = pitch_cents(rd(img, table + size_t(idx) * 2) & 0x1FFF);
        if (root < 0 || std::fabs(c) < std::fabs(root_cents)) { root = key; root_cents = c; }
        if (key > z.key_lo && std::fabs(c - prev) < 50) tracks = false;
        prev = c;
    }
    if (root < 0) return;
    z.root = root;
    z.tune = clampd(root_cents / 100.0, -0.5, 0.5);
    if (!tracks && z.key_hi > z.key_lo) z.key_tracking = 0;
}

void parse_bank(BlobPtr blob, const std::vector<uint8_t> &img, int bank, int lowest, int nkeys, int def_root, const char *label, Instrument &inst) {
    if (size_t(bank + HEADER) > img.size()) return;
    Zone z;
    if (!(img[size_t(bank)] & 0x10)) {   // a single-sample bank (before the multi-sampling software)
        if (make_zone(blob, img, bank, 0, label, lowest, lowest + nkeys - 1, z)) { z.root = def_root; inst.zones.push_back(z); }
        return;
    }
    std::vector<int> recs;
    for (int rec = FIRST_ZONE; int(recs.size()) < MAX_ZONES; rec += RECORD) {
        int ta = rd(img, size_t(bank + rec + 2)), ss = rd(img, size_t(bank + rec + 4));
        if (ta < BANK_ADDR + FIRST_ZONE || ta >= BANK_ADDR + HEADER || ss < BANK_ADDR + RECORD || ss >= MEM_END) break;
        recs.push_back(rec);
    }
    if (recs.empty()) return;
    int n = int(recs.size()), width = KEYS_HALF / n;
    int table = rd(img, size_t(bank + recs[0] + 2)) - BANK_ADDR;
    bool has_table = table >= FIRST_ZONE && table + TABLE_ENTRIES * 2 <= HEADER;
    for (int i = 0; i < n; i++) {
        int lo = lowest + i * width, hi = i == n - 1 ? lowest + nkeys - 1 : lo + width - 1;
        if (!make_zone(blob, img, bank, recs[size_t(i)], std::string(label) + " " + std::to_string(i + 1), lo, hi, z)) continue;
        if (has_table) pitch_table(img, size_t(bank + table), z);
        else z.root = def_root;
        inst.zones.push_back(z);
    }
}

std::vector<PresetInfo> list_single(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

Instrument load_e1(VolumePtr vol, const std::string &path, int) {
    BlobPtr blob = vol->open(path);
    if (blob->size() != uint64_t(IMAGE_SIZE)) throw ParseError("not an Emulator disk image");
    std::vector<uint8_t> img = blob->all();
    Instrument inst;
    inst.format = "E-mu Emulator disk";
    inst.name = path_stem(path);
    parse_bank(blob, img, LOWER, LOWEST_KEY, KEYS_HALF, 48, "Lower", inst);
    parse_bank(blob, img, UPPER, LOWEST_KEY + KEYS_HALF, NUM_KEYS - KEYS_HALF, 72, "Upper", inst);
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("no samples on this Emulator disk");
    return inst;
}

bool probe_e1(const std::string &name, const uint8_t *, size_t, uint64_t size) { return size == uint64_t(IMAGE_SIZE); }

}  // namespace

void register_emu_e1() {
    register_reader({"E-mu Emulator disk", "ei emufd img", probe_e1, list_single, load_e1});
}

}  // namespace omni

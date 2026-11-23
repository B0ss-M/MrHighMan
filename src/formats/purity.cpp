// Native LUXONIX Purity waveform maps and their companion PCM banks.
#include "format.hpp"
#include <cmath>
#include <cstring>
#include <map>

namespace omni {
namespace {

constexpr char map_magic[] = "LUXONIX -Purity    -Wavetables";
constexpr char bank_magic[] = "LUXONIX -Purity    -Samples";

bool signature(const uint8_t *data, size_t size, const char *magic) {
    return size >= 48 && !std::memcmp(data, magic, std::strlen(magic)) &&
           data[37] == 0x1a && le32(data + 40) == 1;
}

bool probe(const std::string &, const uint8_t *data, size_t size, uint64_t) {
    return signature(data, size, map_magic);
}

// User banks contain variable-length records. Only the directory is mapped.
constexpr char user_magic[] = "LUXONIX -Purity    -User";

bool probe_user(const std::string &, const uint8_t *data, size_t size, uint64_t) {
    return signature(data, size, user_magic);
}

std::vector<PresetInfo> list_user(VolumePtr volume, const std::string &path) {
    const auto data = volume->open(path)->all(4u << 20);
    if (!signature(data.data(), data.size(), user_magic))
        throw ParseError("Purity: invalid user bank header");
    const uint32_t count = le32(data.data() + 44);
    size_t pos = 48;
    std::vector<PresetInfo> result;
    for (uint32_t slot = 0; slot < count; ++slot) {
        if (data.size() - pos < 32)
            throw ParseError("Purity: truncated user preset header");
        const auto *record = data.data() + pos;
        const uint32_t payload = le32(record + 28);
        if (payload > data.size() - pos - 32)
            throw ParseError("Purity: truncated user preset payload");
        size_t length = 0;
        while (length < 24 && record[length]) ++length;
        const std::string name(record, record + length);
        if (!name.empty() && name != "<empty>")
            result.push_back({name, int(slot)});
        pos += 32 + size_t(payload);
    }
    if (pos != data.size()) throw ParseError("Purity: unexpected user bank data");
    return result;
}

Instrument load_user(VolumePtr, const std::string &, int) {
    throw ParseError("Purity: user preset playback requires verified waveform and envelope mappings");
}

struct Region {
    int bank, sample, root, low, high;
};
struct Mapping {
    std::string name;
    std::vector<Region> zones;
};

std::vector<Mapping> read_maps(Volume &volume, const std::string &path) {
    const auto data = volume.open(path)->all(4u << 20);
    if (data.size() < 52 || !signature(data.data(), data.size(), map_magic))
        throw ParseError("Purity: invalid waveform header");
    const uint32_t count = le32(data.data() + 48);
    size_t pos = 52;
    std::vector<Mapping> maps;
    for (uint32_t i = 0; i < count; ++i) {
        if (data.size() - pos < 27 || data[pos + 24] > 1)
            throw ParseError("Purity: unsupported waveform record");
        const auto *name = data.data() + pos;
        size_t len = 0;
        while (len < 24 && name[len]) ++len;
        const unsigned zones = le16(name + 25);
        pos += 27;
        if (!len || !zones || zones > (data.size() - pos) / 8)
            throw ParseError("Purity: invalid waveform zones");
        Mapping map{std::string(name, name + len), {}};
        int low = 0;
        for (unsigned j = 0; j < zones; ++j, pos += 8) {
            const auto *r = data.data() + pos;
            const int root = le16(r + 4), high = le16(r + 6);
            if (root > 127 || high > 127 || high < low)
                throw ParseError("Purity: invalid key range");
            map.zones.push_back({le16(r), le16(r + 2), root, low, high});
            low = high + 1;
        }
        maps.push_back(std::move(map));
    }
    if (pos != data.size()) throw ParseError("Purity: unexpected waveform data");
    return maps;
}

struct Sample {
    uint64_t offset;
    uint32_t frames;
    int rate;
    uint32_t loop_start;   // 0 = one-shot; else the loop runs from here to the last frame
};

struct Bank {
    BlobPtr blob;
    std::vector<Sample> samples;
};

Bank open_bank(Volume &volume, const std::string &dir, int number) {
    if (number > 999) throw ParseError("Purity: unsupported sample bank number");
    std::string prefix = std::to_string(number);
    prefix.insert(0, 3 - prefix.size(), '0');
    prefix += ' ';
    std::string path;
    for (const auto &entry : volume.list(dir)) {
        if (entry.dir || entry.name.compare(0, prefix.size(), prefix) ||
            path_ext(entry.name) != "smp") continue;
        if (!path.empty()) throw ParseError("Purity: ambiguous sample bank " + prefix);
        path = path_join(dir, entry.name);
    }
    if (path.empty()) throw ParseError("Purity: missing sample bank " + prefix);
    Bank bank{volume.open(path), {}};
    const auto head = bank.blob->bytes(0, 48);
    if (!signature(head.data(), head.size(), bank_magic))
        throw ParseError("Purity: invalid sample bank header");
    const uint32_t count = le32(head.data() + 44);
    uint64_t pos = 48;
    for (uint32_t i = 0; i < count; ++i) {
        if (bank.blob->size() - pos < 12) throw ParseError("Purity: truncated sample header");
        const auto record = bank.blob->bytes(pos, 12);
        const uint32_t rate = le32(record.data()), loop = le32(record.data() + 4), bytes = le32(record.data() + 8);
        pos += 12;
        if (!rate || rate > 384000 || !bytes || bytes % 2 || bytes > bank.blob->size() - pos)
            throw ParseError("Purity: invalid PCM record");
        bank.samples.push_back({pos, bytes / 2, int(rate), loop < bytes / 2 - 1 ? loop : 0});
        pos += bytes;
    }
    if (pos != bank.blob->size()) throw ParseError("Purity: unexpected sample bank data");
    return bank;
}

std::vector<PresetInfo> list(VolumePtr volume, const std::string &path) {
    const auto maps = read_maps(*volume, path);
    std::vector<PresetInfo> result;
    for (const auto &map : maps)
        result.push_back({map.name, int(result.size())});
    return result;
}

Instrument load(VolumePtr volume, const std::string &path, int index) {
    const auto maps = read_maps(*volume, path);
    if (index < 0 || size_t(index) >= maps.size()) throw ParseError("Purity: invalid waveform index");
    const auto &map = maps[size_t(index)];
    const auto dir = path_dir(path);
    std::map<int, Bank> banks;
    Instrument inst;
    inst.name = map.name;
    inst.format = "LUXONIX Purity waveform";
    for (const auto &r : map.zones) {
        auto bank = banks.find(r.bank);
        if (bank == banks.end())
            bank = banks.emplace(r.bank, open_bank(*volume, dir, r.bank)).first;
        const auto &samples = bank->second.samples;
        if (size_t(r.sample) >= samples.size()) throw ParseError("Purity: sample ID outside bank");
        const auto &s = samples[size_t(r.sample)];
        Zone zone;
        zone.name = map.name + " " + std::to_string(r.sample);
        zone.root = r.root;
        zone.key_lo = r.low;
        zone.key_hi = r.high;
        zone.sample = raw_sample_ref(bank->second.blob, s.offset, s.frames, 1,
                                     Enc::S16LE, s.rate, zone.name);
        if (s.loop_start) {   // sustained waves (strings, organs, pads) loop to their end; drums play once
            Loop loop;
            loop.start = s.loop_start;
            loop.end = int64_t(s.frames) - 1;
            zone.loops.push_back(loop);
        }
        inst.zones.push_back(std::move(zone));
    }
    inst.warnings.push_back("Purity: waveform map (the .pth banks hold the presets made of these)");
    finish_instrument(inst);
    return inst;
}

// ---------------------------------------------------------------------------------------------------------------
// Each patch contains named parts, followed by a 40-byte performance trailer.
// Part headers carry the number of little-endian float words in that part.
constexpr char patch_magic[] = "LUXONIX -Purity    -Patches";
constexpr unsigned melody_words = 727, drum_words = 1789;

bool probe_pth(const std::string &, const uint8_t *data, size_t size, uint64_t) {
    return signature(data, size, patch_magic);
}

struct Part { size_t controls; bool drum; };
struct Patch { std::string name; std::vector<Part> parts; };

std::vector<Patch> read_patches(const std::vector<uint8_t>& d) {
    if (!signature(d.data(), d.size(), patch_magic)) throw ParseError("Purity: invalid patch header");
    std::vector<Patch> patches;
    size_t pos = 48;
    for (uint32_t i = 0; i < le32(d.data() + 44); ++i) {
        if (d.size() - pos < 32) throw ParseError("Purity: truncated patch header");
        const auto* header = d.data() + pos;
        const size_t layers = le16(header + 26), length = le32(header + 28);
        pos += 32;
        if (!layers || layers > 16 || length < 40 || length > d.size() - pos)
            throw ParseError("Purity: invalid patch bounds");
        const size_t trailer = pos + length - 40;
        size_t name_len = 0;
        while (name_len < 24 && header[name_len]) ++name_len;
        Patch patch{std::string(header, header + name_len), {}};
        for (size_t j = 0; j < layers; ++j) {
            if (trailer - pos < 28) throw ParseError("Purity: truncated part header");
            const uint32_t words = le32(d.data() + pos + 24);
            if (words != melody_words && words != drum_words) throw ParseError("Purity: unknown part type");
            if (words > (trailer - pos - 28) / 4) throw ParseError("Purity: truncated part");
            patch.parts.push_back({pos + 40, words == drum_words});
            pos += 28 + 4 * words;
        }
        if (pos != trailer) throw ParseError("Purity: invalid performance trailer");
        pos += 40;
        patches.push_back(std::move(patch));
    }
    if (pos != d.size()) throw ParseError("Purity: trailing patch bank data");
    return patches;
}

std::vector<PresetInfo> list_pth(VolumePtr volume, const std::string &path) {
    const auto data = volume->open(path)->all(16u << 20);
    std::vector<PresetInfo> result;
    const auto patches = read_patches(data);
    for (size_t i = 0; i < patches.size(); ++i) result.push_back({patches[i].name, int(i)});
    return result;
}

// the waveform maps of bank `number` (the "NNN name.wvt" beside the patches)
const std::vector<Mapping> *maps_of(Volume &volume, const std::string &dir, int number, std::map<int, std::vector<Mapping>> &cache) {
    auto it = cache.find(number);
    if (it != cache.end()) return &it->second;
    std::string prefix = std::to_string(number);
    prefix.insert(0, 3 - std::min<size_t>(3, prefix.size()), '0');
    prefix += ' ';
    for (const auto &e : volume.list(dir))
        if (!e.dir && !e.name.compare(0, prefix.size(), prefix) && path_ext(e.name) == "wvt")
            return &cache.emplace(number, read_maps(volume, path_join(dir, e.name))).first->second;
    cache.emplace(number, std::vector<Mapping>());
    return nullptr;
}

float f32(const uint8_t *p) { float v; uint32_t u = le32(p); std::memcpy(&v, &u, 4); return std::isfinite(v) ? v : 0.0f; }

Instrument load_pth(VolumePtr volume, const std::string &path, int index) {
    const auto data = volume->open(path)->all(16u << 20);
    const auto patches = read_patches(data);
    if (index < 0 || size_t(index) >= patches.size()) throw ParseError("Purity: invalid patch index");
    const Patch &patch = patches[size_t(index)];
    const auto dir = path_dir(path);
    std::map<int, std::vector<Mapping>> wave_cache;
    std::map<int, Bank> banks;
    Instrument inst;
    inst.name = patch.name;
    inst.format = "LUXONIX Purity patch";
    int missing = 0;
    for (const Part& part : patch.parts) {
        const uint8_t *p = data.data() + part.controls;
        auto fv = [&](int off) { return double(std::max(0.0f, std::min(1.0f, f32(p + off)))); };
        if (part.drum) {
            // The Standard Kit confirms slot 11 = MIDI 35, slot 12 = MIDI 36,
            // slot 14 = MIDI 38 and slot 18 = MIDI 42. Each pad has 17 floats.
            for (int pad = 0; pad < 65; ++pad) {
                const int base = 2712 + pad * 68;
                const int id = int(std::lround(fv(base) * 16384));
                if (!id) continue;
                const auto* maps = maps_of(*volume, dir, id / 128, wave_cache);
                const int local = id % 128;
                if (!maps || local < 1 || size_t(local) > maps->size()) { ++missing; continue; }
                const auto& map = (*maps)[local - 1];
                if (map.zones.size() != 1) {
                    ++missing;
                    continue;
                }
                const auto& r = map.zones.front();
                auto bank = banks.find(r.bank);
                if (bank == banks.end()) bank = banks.emplace(r.bank, open_bank(*volume, dir, r.bank)).first;
                if (size_t(r.sample) >= bank->second.samples.size()) throw ParseError("Purity: invalid pad sample ID");
                const auto& sample = bank->second.samples[r.sample];
                Zone zone;
                zone.name = map.name;
                zone.root = zone.key_lo = zone.key_hi = 24 + pad;
                zone.one_shot = true;
                zone.pan = clampd(2 * (fv(4) + fv(base + 32) - 1), -1, 1);
                zone.tune = std::round((fv(base + 8) - 0.5) * 96) + 2 * (fv(base + 12) - 0.5);
                const double gain = fv(0) * fv(base + 28);
                if (gain == 0) continue;
                zone.gain_db = std::max(-60.0, 40 * std::log10(gain / 0.5));
                zone.sample = raw_sample_ref(bank->second.blob, sample.offset, sample.frames, 1,
                                             Enc::S16LE, sample.rate, zone.name);
                inst.zones.push_back(std::move(zone));
            }
            continue;
        }
        const int klo = int(std::lround(fv(2712) * 127)), khi = int(std::lround(fv(2716) * 127));
        const int vlo = int(std::lround(fv(2720) * 127)), vhi = int(std::lround(fv(2724) * 127));
        if (klo > khi || vlo > vhi) continue;
        const double vol = fv(0);
        const double layer_db = vol > 0.001 ? std::max(-60.0, 40.0 * std::log10(vol / 0.70710678)) : -60.0;
        const double pan = (fv(4) - 0.5) * 2;
        const double transpose = std::lround((fv(12) - 0.5) * 96);
        Envelope env;
        env.set = true;
        auto t2 = [](double v, double lo, double hi) { return std::max(lo, v * v * hi); };   // (Purity's knobs: finer at the short end)
        env.attack = t2(fv(2848), 0.0, 10.0);
        env.decay = t2(fv(2852), 0.01, 30.0);
        env.sustain = fv(2856);
        env.release = t2(fv(2860), 0.01, 20.0);
        const uint32_t osc[2] = {uint32_t(std::lround(fv(2768) * 16384)), uint32_t(std::lround(fv(2772) * 16384))};
        const double mix = fv(2792);
        const bool both = osc[0] && osc[1];
        for (int o = 0; o < 2; ++o) {
            if (!osc[o]) continue;
            const int bank_no = int(osc[o] / 128), map_no = int(osc[o] % 128);
            const auto *maps = maps_of(*volume, dir, bank_no, wave_cache);
            if (!maps || map_no < 1 || size_t(map_no) > maps->size()) { ++missing; continue; }
            const Mapping &map = (*maps)[size_t(map_no - 1)];
            const double semi = std::lround((fv(o ? 2780 : 2776) - 0.5) * 96), fine = (fv(o ? 2788 : 2784) - 0.5) * 2;
            const double osc_db = both ? 20.0 * std::log10(std::max(0.001, o ? mix : 1 - mix) * 2) : 0.0;
            for (const auto &r : map.zones) {
                const int lo = std::max(r.low, klo), hi = std::min(r.high, khi);
                if (lo > hi) continue;
                auto bank = banks.find(r.bank);
                if (bank == banks.end()) bank = banks.emplace(r.bank, open_bank(*volume, dir, r.bank)).first;
                const auto &samples = bank->second.samples;
                if (size_t(r.sample) >= samples.size()) continue;
                const auto &s = samples[size_t(r.sample)];
                Zone zone;
                zone.name = map.name + " " + std::to_string(r.sample);
                zone.root = r.root;
                zone.key_lo = lo;
                zone.key_hi = hi;
                zone.vel_lo = vlo;
                zone.vel_hi = vhi;
                zone.tune = transpose + semi + fine;
                zone.gain_db = std::max(-60.0, layer_db + osc_db);
                zone.pan = std::max(-1.0, std::min(1.0, pan));
                zone.amp_env = env;
                zone.sample = raw_sample_ref(bank->second.blob, s.offset, s.frames, 1, Enc::S16LE, s.rate, zone.name);
                if (s.loop_start) {
                    Loop loop;
                    loop.start = s.loop_start;
                    loop.end = int64_t(s.frames) - 1;
                    zone.loops.push_back(loop);
                }
                inst.zones.push_back(std::move(zone));
            }
        }
    }
    if (inst.zones.empty()) throw ParseError(missing ? "Purity: this patch's waveforms are not beside it" : "Purity: patch has no zones");
    if (missing) inst.warnings.push_back("Purity: " + std::to_string(missing) + " unresolved waveform reference(s)");
    inst.warnings.push_back("Purity: sequencer, arpeggiator, effects, filter and modulation are not rendered");
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_purity() {
    register_reader({"LUXONIX Purity waveform", "wvt", probe, list, load});
    register_reader({"LUXONIX Purity user bank", "usr", probe_user, list_user, load_user});
    register_reader({"LUXONIX Purity patch", "pth", probe_pth, list_pth, load_pth});
}
}  // namespace omni

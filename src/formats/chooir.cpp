// Native reader for the split UJAM SE-CHOOIR 1.1.0 sample library.
#include "format.hpp"
#include "../core/parse.hpp"
#include <cmath>
#include <cstring>
#include <map>

namespace omni {
namespace {

constexpr size_t record_size = 304;
const std::string suffix = "_part1.blob";

struct Sample {
    uint64_t offset, length;
    uint32_t frames;
    int rate, channels, bits;
};

uint64_t u64(const uint8_t *data) {
    return uint64_t(le32(data)) | (uint64_t(le32(data + 4)) << 32);
}

uint64_t field(const Json &object, const char *name, uint64_t limit, uint64_t fallback = 0) {
    if (!object.has(name)) return fallback;
    const Json &value = object[name];
    if (value.type != Json::Number || !std::isfinite(value.num) || value.num < 0 ||
        value.num > double(limit) || std::floor(value.num) != value.num)
        throw ParseError(std::string("CHOOIR: invalid ") + name);
    return uint64_t(value.num);
}

bool probe(const std::string &name, const uint8_t *head, size_t size, uint64_t) {
    return size >= 8 && le32(head) == 0x62b14567 && name.size() >= suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

struct Choir {
    Json instrument;
    std::map<std::string, Sample> samples;
    std::string uuid;
    uint64_t audio_end = 40;

    explicit Choir(const Blob &part) {
        auto prefix = part.bytes(0, 8);
        auto length = le32(prefix.data() + 4);
        if (le32(prefix.data()) != 0x62b14567 || !length || length > 65536 ||
            8 + uint64_t(length) > part.size())
            throw ParseError("CHOOIR: invalid Part 1 header");
        uint64_t base = 8 + length;
        auto bytes = part.bytes(8, length);
        auto header = parse_json(std::string(bytes.begin(), bytes.end()));
        uuid = header["uuid"].as_str();
        auto &version = header["version"];
        auto &entries = header["instruments"];
        if (uuid.size() != 36 || version["major"].as_int() != 1 ||
            version["minor"].as_int() != 1 || version["patch"].as_int(-1) != 0 ||
            entries.type != Json::Array || entries.arr.size() != 1 ||
            entries[0]["name"].as_str() != "SE-CHOOIR")
            throw ParseError("CHOOIR: unsupported library");
        uint64_t split = part.size() - base;
        auto section = [&](const Json &entry) {
            uint64_t offset = field(entry, "at_byte", 64u << 20);
            uint64_t size = field(entry, "size_bytes", 64u << 20);
            if (!size || offset > split || size > split - offset)
                throw ParseError("CHOOIR: invalid metadata bounds");
            return part.bytes(base + offset, size_t(size));
        };
        auto index = section(header["sample_index"]);
        if (index.size() % record_size)
            throw ParseError("CHOOIR: incomplete sample record");
        for (size_t pos = 0; pos < index.size(); pos += record_size) {
            const uint8_t *r = index.data() + pos;
            const auto *end = static_cast<const uint8_t *>(std::memchr(r + 52, 0, record_size - 52));
            if (!end || end == r + 52 || u64(r) != split || u64(r + 8) < split)
                throw ParseError("CHOOIR: invalid sample index");
            uint32_t rate_bits = le32(r + 44);
            float rate;
            std::memcpy(&rate, &rate_bits, sizeof rate);
            if (!std::isfinite(rate) || rate < 1 || rate > 384000 || std::floor(rate) != rate)
                throw ParseError("CHOOIR: invalid sample rate");
            Sample sample{u64(r + 8) - split, u64(r + 28), le32(r + 40), int(rate), r[48], r[50]};
            if (sample.offset < audio_end || !sample.frames || sample.channels < 1 || sample.channels > 8 ||
                (sample.bits != 16 && sample.bits != 24 && sample.bits != 32) ||
                sample.length != uint64_t(sample.frames) * sample.channels * (sample.bits / 8) ||
                sample.offset > UINT64_MAX - sample.length)
                throw ParseError("CHOOIR: invalid sample range");
            audio_end = sample.offset + sample.length;
            if (!samples.emplace(std::string(r + 52, end), sample).second)
                throw ParseError("CHOOIR: duplicate sample name");
        }
        auto data = section(entries[0]);
        instrument = parse_json(std::string(data.begin(), data.end()));
        if (instrument["Mappings"].type != Json::Array || instrument["Mappings"].arr.size() > 4096)
            throw ParseError("CHOOIR: invalid mappings");
    }
};

std::vector<PresetInfo> list(VolumePtr volume, const std::string &path) {
    Choir choir(*volume->open(path));
    std::vector<PresetInfo> result;
    for (const auto &mapping : choir.instrument["Mappings"].arr)
        result.push_back({mapping["Name"].as_str(), int(result.size())});
    return result;
}

Instrument load(VolumePtr volume, const std::string &path, int index) {
    Choir choir(*volume->open(path));
    const auto &maps = choir.instrument["Mappings"].arr;
    if (index < 0 || size_t(index) >= maps.size() || path.size() < suffix.size())
        throw ParseError("CHOOIR: invalid mapping index");
    auto part2 = volume->open(path.substr(0, path.size() - suffix.size()) + "_part2.blob");
    if (part2->size() != choir.audio_end)
        throw ParseError("CHOOIR: Part 2 size does not match index");
    auto header = part2->bytes(0, 40);
    if (std::string(header.begin(), header.end()) != choir.uuid + std::string(4, '\0'))
        throw ParseError("CHOOIR: Part 2 UUID does not match");

    const Json &mapping = maps[size_t(index)];
    Instrument instrument;
    instrument.name = mapping["Name"].as_str();
    instrument.format = "UJAM SE-CHOOIR";
    std::map<std::string, SampleRefPtr> refs;
    size_t skipped = 0;
    for (const auto &spec : mapping["Zones"].arr) {
        auto name = spec["Sample"].as_str();
        auto found = choir.samples.find(name);
        if (found == choir.samples.end() || found->second.bits == 32 || found->second.channels > 2) {
            ++skipped;
            continue;
        }
        const Sample &s = found->second;
        Zone zone;
        zone.name = name;
        zone.root = int(field(spec, "Root Key", 127, 60));
        zone.key_lo = int(field(spec, "Low Key", 127));
        zone.key_hi = int(field(spec, "High Key", 127, 127));
        zone.vel_lo = int(field(spec, "Low Vel", 127));
        zone.vel_hi = int(field(spec, "High Vel", 127, 127));
        zone.start = field(spec, "Start", s.frames);
        zone.stop = field(spec, "End", s.frames, s.frames);
        if (zone.start >= zone.stop || zone.key_lo > zone.key_hi || zone.vel_lo > zone.vel_hi) {
            ++skipped;
            continue;
        }
        if (spec["Loop"].as_str() == "On") {
            auto start = field(spec, "Loop Start", s.frames);
            auto end = field(spec, "Loop End", s.frames);
            if (start >= uint64_t(zone.start) && end > start + 1 && end <= uint64_t(zone.stop)) {
                Loop loop;
                loop.start = start;
                loop.end = end - 1;
                loop.until_release = true;
                zone.loops.push_back(loop);
            }
        }
        auto &ref = refs[name];
        if (!ref)
            ref = raw_sample_ref(part2, s.offset, s.frames, s.channels,
                                 s.bits == 16 ? Enc::S16LE : Enc::S24LE, s.rate, name);
        zone.sample = ref;
        instrument.zones.push_back(std::move(zone));
    }
    if (instrument.zones.empty())
        throw ParseError("CHOOIR: mapping has no supported mono/stereo PCM zones");
    if (skipped) instrument.warnings.push_back("CHOOIR: skipped " + std::to_string(skipped) + " zones");
    instrument.warnings.push_back("CHOOIR: dry mapping; phrase and effects processing unavailable");
    finish_instrument(instrument);
    return instrument;
}

}  // namespace

void register_chooir() {
    register_reader({"UJAM SE-CHOOIR", "blob", probe, list, load});
}
}  // namespace omni

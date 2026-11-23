// Read verified raw PCM regions described by a sidecar, using Omni's normal sample loader.
#include <atomic>
#include <cmath>
#include "../core/parse.hpp"
#include "format.hpp"

namespace omni {
namespace {

constexpr uint64_t MAX_OFFSET = (uint64_t(1) << 53) - 1;
constexpr uint64_t MAX_SAMPLE_BYTES = 64 * 1024 * 1024;
std::atomic<unsigned> next_image_id{0};

uint64_t number(const Json &obj, const char *field, uint64_t low, uint64_t high) {
    const Json &value = obj[field];
    if (value.type != Json::Number || !std::isfinite(value.num) ||
        value.num < double(low) || value.num > double(high) || std::floor(value.num) != value.num)
        throw ParseError(std::string("Omni ROM map: invalid ") + field);
    return uint64_t(value.num);
}

uint64_t optional_number(const Json &obj, const char *field, uint64_t high, uint64_t fallback) {
    return obj.has(field) ? number(obj, field, 0, high) : fallback;
}

Json read_map(Volume &vol, const std::string &path) {
    auto data = vol.open(path)->all(1024 * 1024);
    Json map = parse_json(std::string(data.begin(), data.end()));
    number(map, "version", 1, 1);
    const Json &presets = map["presets"];
    if (presets.type != Json::Array || presets.arr.empty() || presets.arr.size() > 1024)
        throw ParseError("Omni ROM map: expected 1 to 1024 presets");
    return map;
}

std::vector<PresetInfo> list_rom_map(VolumePtr vol, const std::string &path) {
    Json map = read_map(*vol, path);
    std::vector<PresetInfo> result;
    for (const Json &preset : map["presets"].arr) {
        std::string name = preset["name"].as_str();
        if (name.empty()) throw ParseError("Omni ROM map: missing preset name");
        result.push_back({name, int(result.size())});
    }
    return result;
}

Instrument load_rom_map(VolumePtr vol, const std::string &path, int index) {
    Json map = read_map(*vol, path);
    const auto &presets = map["presets"].arr;
    if (index < 0 || size_t(index) >= presets.size())
        throw ParseError("Omni ROM map: invalid preset index");

    std::string image = map["image"].as_str();
    if (image.empty() || image == "." || image == ".." ||
        image.find_first_of("/\\:") != std::string::npos || image.find('\0') != std::string::npos)
        throw ParseError("Omni ROM map: image must be a filename beside the map");
    std::string image_path = path_join(path_dir(path), image);
    BlobPtr blob = vol->open(image_path);
    // A fresh load may read a replaced image at the same path.
    const std::string image_id = std::to_string(next_image_id.fetch_add(1, std::memory_order_relaxed));

    const Json &preset = presets[size_t(index)];
    const Json &zones = preset["zones"];
    if (zones.type != Json::Array || zones.arr.empty() || zones.arr.size() > 1024)
        throw ParseError("Omni ROM map: expected 1 to 1024 zones");

    Instrument inst;
    inst.name = preset["name"].as_str();
    if (inst.name.empty()) throw ParseError("Omni ROM map: missing preset name");
    inst.format = "Omni ROM map";
    for (const Json &spec : zones.arr) {
        std::string codec = spec["codec"].as_str();
        bool pcm8 = codec == "s8";
        bool little = codec == "s16le";
        if (!pcm8 && !little && codec != "s16be")
            throw ParseError("Omni ROM map: unsupported codec " + codec);

        uint64_t offset = number(spec, "offset", 0, MAX_OFFSET);
        uint64_t frames = number(spec, "frames", 1, MAX_SAMPLE_BYTES);
        int rate = int(number(spec, "rate", 1, 384000));
        int channels = int(number(spec, "channels", 1, 2));
        uint64_t words = frames * unsigned(channels);
        uint64_t length = words * (pcm8 ? 1 : 2);
        if (length > MAX_SAMPLE_BYTES || offset > blob->size() || length > blob->size() - offset)
            throw ParseError("Omni ROM map: sample range is too large or outside the image");

        Zone zone;
        zone.name = spec["name"].as_str(inst.name);
        zone.key_lo = int(optional_number(spec, "key_lo", 127, 0));
        zone.key_hi = int(optional_number(spec, "key_hi", 127, 127));
        zone.root = int(optional_number(spec, "root", 127, 60));
        zone.vel_lo = int(optional_number(spec, "vel_lo", 127, 0));
        zone.vel_hi = int(optional_number(spec, "vel_hi", 127, 127));
        if (zone.key_lo > zone.key_hi || zone.vel_lo > zone.vel_hi)
            throw ParseError("Omni ROM map: inverted zone range");
        if (spec.has("loop_start") || spec.has("loop_end")) {
            Loop loop;
            loop.start = number(spec, "loop_start", 0, frames - 1);
            loop.end = number(spec, "loop_end", uint64_t(loop.start) + 1, frames - 1);
            zone.loops.push_back(loop);
        }

        auto sample = std::make_shared<SampleRef>();
        sample->key = "omnirom:" + image_id + ":" +
            std::to_string(offset) + ":" +
            std::to_string(frames) + ":" + std::to_string(channels) + ":" +
            std::to_string(rate) + ":" + codec;
        sample->name = zone.name;
        sample->rate = rate;
        sample->channels = channels;
        sample->frames = frames;
        sample->decode = [blob, offset, length, words, rate, channels, pcm8, little]() -> PcmPtr {
            auto pcm = std::make_shared<Pcm>();
            pcm->rate = rate;
            pcm->channels = channels;
            auto data = blob->bytes(offset, size_t(length));
            pcm->data.resize(size_t(words));
            for (size_t i = 0; i < pcm->data.size(); ++i) {
                if (pcm8) {
                    int value = data[i];
                    pcm->data[i] = int16_t((value < 128 ? value : value - 256) * 256);
                } else {
                    unsigned value = little ? le16(data.data() + 2 * i) : be16(data.data() + 2 * i);
                    pcm->data[i] = int16_t(value < 32768 ? int(value) : int(value) - 65536);
                }
            }
            return pcm;
        };
        zone.sample = sample;
        inst.zones.push_back(std::move(zone));
    }
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_rom_map() {
    register_reader({"Omni ROM map", "omnirom", nullptr, list_rom_map, load_rom_map});
}

}  // namespace omni

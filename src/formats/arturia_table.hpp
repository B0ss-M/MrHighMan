#pragma once
#include "format.hpp"
#include "../core/parse.hpp"
#include "../engine/synth.hpp"
#include <algorithm>

namespace omni {
inline std::string arturia_table_path(Volume &vol, const std::string &preset, std::string ref) {
    std::replace(ref.begin(), ref.end(), '\\', '/');
    if (lower(path_ext(ref)) != "wav") throw ParseError("Missing Arturia wavetable WAV reference");
    while (!ref.empty() && ref.front() == '/') ref.erase(0, 1);
    std::string found;
    // 2.4.23: the full Arturia path and its shorter tails ("SFZ/Cello/X.sfz", "Cello/X.sfz") at each parent folder,
    // so libraries copied without the Factory folder still resolve
    std::vector<std::string> tails{ref};
    for (size_t p = ref.find('/'); p != std::string::npos; p = ref.find('/', p + 1))
        if (p + 1 < ref.size() && ref.find('/', p + 1) != std::string::npos) tails.push_back(ref.substr(p + 1));
    for (auto dir = path_dir(preset);; dir = path_dir(dir)) {
        for (const auto &t : tails)
            if (find_ci(vol, path_resolve(dir, t), found)) return found;
        if (dir.empty() || path_dir(dir) == dir) break;
    }
    if (find_ci(vol, path_join(path_dir(preset), path_name(ref)), found)) return found;
    throw ParseError("Missing Arturia wavetable: " + ref);
}

// Reopen through the preset's volume so project restore and mounted paths use the same lookup.
inline WavetablePtr arturia_table(Vfs &fs, const std::string &preset) {
    auto loc = fs.resolve(preset);
    auto bytes = loc.volume->open(loc.inner)->all(4u << 20);
    auto xml = parse_xml(std::string(bytes.begin(), bytes.end()));
    if (!xml) throw ParseError("Invalid Arturia preset");
    for (auto param : xml->all("param")) {
        if (param->attr("key") != "Engine_WavetableFilePath") continue;
        auto path = arturia_table_path(*loc.volume, loc.inner, param->attr("value"));
        auto file = loc.volume->open(path);
        auto info = probe_audio(*file);
        if (info.channels != 1 || info.frames < WT_SIZE || info.frames % WT_SIZE ||
            info.frames > WT_SIZE * WT_MAX_FRAMES) throw ParseError("Unsupported Arturia wavetable geometry");
        auto pcm = decode_audio(*file);
        if (pcm->frames() != info.frames) throw ParseError("Incomplete Arturia wavetable");
        std::vector<float> cycles(pcm->data.size());
        std::transform(pcm->data.begin(), pcm->data.end(), cycles.begin(),
                       [](int16_t sample) { return sample / 32768.f; });
        return build_wavetable(path_stem(path), cycles, int(info.frames / WT_SIZE));
    }
    throw ParseError("Missing Arturia wavetable parameter");
}
} // namespace omni

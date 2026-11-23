// Omni Sampler 2.4.33: a Serum 2 preset's SAMPLE / MULTISAMPLE oscillator as the SYNTH's sample oscillator
// ("serumsmp:<osc>:<preset path>", plugin.cpp). Checked on Serum 2's factory library (the Live III's Serum 2 Presets):
//   Oscillator<n>.plainParams.kParamType "kOsc_Sample": SampleOsc<n>.samplePathRelative ("Factory Non-Tonal/Drum/...flac",
//     under Serum's "Samples" folder), baseNote (absent = 60), plainParams kParamLoopMode (kForward / kPingPong /
//     kTailed; absent = no loop), kParamLoopStart / kParamLoopEnd (percent of the sample), kParamStart (percent)
//   "kOsc_MultiSample": MultiSampleOsc<n>.sfzPathRelative ("Factory/Plucked/Oud.sfz", under "Multisamples") and
//     embedded_sfz (libSFZ text: <group> / <region> lines; sample= relative to the .sfz's folder, last on the line)
// The files are looked for from the preset's folder upwards: <dir>/Samples|Multisamples/<path>, then <dir>/<path>.
// Everything is decoded here (the synth worker, not the audio thread), at most SERUM_SAMPLE_MAX bytes per oscillator.
// 2.4.35 (user: Piano / Flute presets silent on the Live III):
//   - the library folder is found ONCE per oscillator, then one exists() per region (2.4.34 walked every folder from "/"
//     for each region and each folder level: 2 of the 2.9 s a 110-region piano took on x86, far more on the MPC)
//   - decoded files are shared (a weak cache by host path + size): the presets that use the same piano decode it once
//   - `cancelled` (the worker: another sound picked meanwhile) stops a long load between files
//   - kOsc_Granular / kOsc_Spectral: their source sample (GranularOsc<n> / SpectralOsc<n>.samplePathRelative, under
//     "Samples") played as a plain sample [G: no grains, no resynthesis]
//   - the cap: 192 MB on a 32-bit MPC (Live II), 512 MB on a 64-bit one (Live III)
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include "../core/audio.hpp"
#include "../core/vfs.hpp"
#include "../engine/synth.hpp"
#include "serum_table.hpp"

namespace omni {

constexpr uint64_t SERUM_SAMPLE_MAX = (UINTPTR_MAX > 0xffffffffu ? uint64_t(512) : uint64_t(192)) << 20;   // decoded bytes per oscillator

// "a/b/../c" -> "a/c"; "" when it climbs above its start (a path out of Serum's library is refused)
inline std::string serum_norm_path(const std::string &p) {
    std::vector<std::string> out;
    std::string part;
    std::istringstream in(p);
    while (std::getline(in, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") { if (out.empty()) return ""; out.pop_back(); continue; }
        out.push_back(part);
    }
    std::string r;
    for (const std::string &x : out) r += (r.empty() ? "" : "/") + x;
    return r;
}

// a library file of Serum's: "<root>/<rel>" in the preset's folder or any folder above it, else "<rel>" there. 2.4.33:
// a drum kit's multisample names "../../../Samples/..." from its .sfz's folder: the ".." steps are resolved inside
// "<root>/<rel>" (Multisamples/Factory/Drums/../../../Samples/x = Samples/x)
inline std::string serum_library_file(Volume &vol, const std::string &preset, const std::string &root, std::string rel) {
    std::replace(rel.begin(), rel.end(), '\\', '/');
    while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
    const std::string joined = serum_norm_path(root + "/" + rel), plain = serum_norm_path(rel);
    if (rel.empty() || joined.empty()) throw ParseError("Serum 2: no sample file named");
    std::string found;
    for (std::string dir = path_dir(preset);; dir = path_dir(dir)) {
        for (const std::string &r : {joined, plain})
            if (!r.empty() && find_ci(vol, path_resolve(dir, r), found)) return found;
        if (dir.empty() || path_dir(dir) == dir) break;
    }
    throw ParseError("Serum 2: sample not found: " + rel + " (copy Serum's " + root + " folder above the presets)");
}

// 2.4.35: the folder "<root>" (Serum's Samples / Multisamples) beside the preset or in a folder above it, looked for once
// per oscillator; "" when there is none
inline std::string serum_library_root(Volume &vol, const std::string &preset, const std::string &root) {
    std::string found;
    for (std::string dir = path_dir(preset);; dir = path_dir(dir)) {
        if (find_ci(vol, path_resolve(dir, root), found) && vol.is_dir(found)) return found;
        if (dir.empty() || path_dir(dir) == dir) break;
    }
    return "";
}

// 2.4.35: a library file under `lib` (serum_library_root's folder): the exact spelling with one exists(), then one
// case-insensitive walk, then the full search of serum_library_file
inline std::string serum_library_file_in(Volume &vol, const std::string &preset, const std::string &root, const std::string &lib,
                                         std::string rel) {
    std::replace(rel.begin(), rel.end(), '\\', '/');
    while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
    if (!lib.empty() && !rel.empty()) {
        const std::string joined = serum_norm_path(root + "/" + rel);   // ".." resolved inside the library
        if (!joined.empty()) {
            const std::string cand = path_resolve(path_dir(lib), joined);
            std::string found;
            if (vol.exists(cand)) return cand;
            if (find_ci(vol, cand, found)) return found;
        }
    }
    return serum_library_file(vol, preset, root, rel);
}

// 2.4.35: decoded library files, shared while anything plays them (host folders only: their paths are unique)
struct SerumPcmEntry { std::weak_ptr<const Pcm> pcm; AudioInfo info; };
inline std::mutex &serum_pcm_mutex() { static std::mutex m; return m; }
inline std::map<std::string, SerumPcmEntry> &serum_pcm_cache() { static std::map<std::string, SerumPcmEntry> c; return c; }

// libSFZ's text: one region per <region> line, <group> opcodes inherited; sample= runs to the end of the line
struct SerumSfzRegion {
    std::string sample;
    int lokey = 0, hikey = 127, lovel = 0, hivel = 127, key_center = 60;
    double tune_cents = 0, transpose = 0;
    int64_t offset = 0, end = -1, loop_start = -1, loop_end = -1;
    std::string loop_mode;
};
inline int serum_sfz_note(const std::string &v, int fallback) {
    if (v.empty()) return fallback;
    if (std::isdigit(static_cast<unsigned char>(v[0])) || v[0] == '-') return std::atoi(v.c_str());
    static const int BASE[7] = {9, 11, 0, 2, 4, 5, 7};   // a..g
    const char c = char(std::tolower(static_cast<unsigned char>(v[0])));
    if (c < 'a' || c > 'g') return fallback;
    size_t i = 1;
    int n = BASE[c - 'a'];
    if (i < v.size() && v[i] == '#') { n++; i++; } else if (i < v.size() && v[i] == 'b') { n--; i++; }
    return n + (std::atoi(v.c_str() + i) + 1) * 12;   // c4 = 60
}
inline std::vector<SerumSfzRegion> serum_sfz_regions(const std::string &text) {
    std::vector<SerumSfzRegion> out;
    std::map<std::string, std::string> group;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        // a comment: "//" ... but libSFZ writes paths with "//" ("Violin Long Samples//XFVln ....flac"): after sample= it
        // is part of the path, not a comment
        const size_t smp = line.find("sample=");
        const size_t cmt = line.find("//");
        if (cmt != std::string::npos && (smp == std::string::npos || cmt < smp)) line.erase(cmt);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t at = line.find('<');
        if (at == std::string::npos) continue;
        const size_t close = line.find('>', at);
        if (close == std::string::npos) continue;
        const std::string head = line.substr(at + 1, close - at - 1);
        std::map<std::string, std::string> ops;
        std::string rest = line.substr(close + 1);
        // opcodes: name=value pairs; sample= takes the rest of the line (its name may hold spaces)
        size_t p = 0;
        while (p < rest.size()) {
            while (p < rest.size() && rest[p] == ' ') p++;
            const size_t eq = rest.find('=', p);
            if (eq == std::string::npos) break;
            const std::string name = rest.substr(p, eq - p);
            if (name == "sample") { ops[name] = rest.substr(eq + 1); break; }
            size_t sp = rest.find(' ', eq + 1);
            if (sp == std::string::npos) sp = rest.size();
            ops[name] = rest.substr(eq + 1, sp - eq - 1);
            p = sp;
        }
        if (head == "group" || head == "global" || head == "master") { group = ops; continue; }
        if (head != "region") continue;
        std::map<std::string, std::string> all = group;
        for (auto &kv : ops) all[kv.first] = kv.second;
        auto get = [&](const char *k) { auto it = all.find(k); return it == all.end() ? std::string() : it->second; };
        SerumSfzRegion r;
        r.sample = get("sample");
        if (r.sample.empty()) continue;
        const std::string key = get("key");
        r.lokey = serum_sfz_note(get("lokey"), serum_sfz_note(key, 0));
        r.hikey = serum_sfz_note(get("hikey"), serum_sfz_note(key, 127));
        r.key_center = serum_sfz_note(get("pitch_keycenter"), serum_sfz_note(key, 60));
        r.lovel = get("lovel").empty() ? 0 : std::atoi(get("lovel").c_str());
        r.hivel = get("hivel").empty() ? 127 : std::atoi(get("hivel").c_str());
        r.tune_cents = std::atof(get("tune").c_str());
        r.transpose = std::atof(get("transpose").c_str());
        r.offset = std::atoll(get("offset").c_str());
        if (!get("end").empty()) r.end = std::atoll(get("end").c_str());
        if (!get("loop_start").empty()) r.loop_start = std::atoll(get("loop_start").c_str());
        if (!get("loop_end").empty()) r.loop_end = std::atoll(get("loop_end").c_str());
        r.loop_mode = get("loop_mode");
        out.push_back(r);
    }
    return out;
}

// 2.4.35: the oscillator types that play one sample file, and the module holding it
inline bool serum_plays_sample(const SerumItem &type) {
    return type.equals("kOsc_Sample") || type.equals("kOsc_Granular") || type.equals("kOsc_Spectral");
}
inline SerumItem serum_sample_module(const SerumItem &osc, const SerumItem &type, const std::string &id) {
    if (type.equals("kOsc_Granular")) return osc.field("GranularOsc" + id);
    if (type.equals("kOsc_Spectral")) return osc.field("SpectralOsc" + id);
    return osc.field("SampleOsc" + id);
}

// oscillator `osc` (0..2; 3 = the NOISE oscillator's sample, 2.4.35) of a Serum 2 preset as a SYNTH sample (a single
// sample, or a multisample's zones).
// cancelled (2.4.35): asked between files; true stops the load (ParseError)
inline SynthSamplePtr serum_sample(Vfs &fs, const std::string &preset, int osc, const std::function<bool()> &cancelled = {}) {
    if (osc < 0 || osc > 3) throw ParseError("Serum 2: invalid oscillator");
    Location loc = fs.resolve(preset);
    Volume &vol = *loc.volume;
    const bool host = vol.kind() == "Folder";
    const std::vector<uint8_t> decoded = serum_unpack(*vol.open(loc.inner));
    const SerumItem root = serum_root(decoded);
    const std::string id = std::to_string(osc);
    const SerumItem o = root.field("Oscillator" + id), params = o.field("plainParams");
    const SerumItem type = params.field("kParamType");
    auto ss = std::make_shared<SynthSample>();
    uint64_t bytes = 0;
    auto load = [&](const std::string &file, AudioInfo &ai) {
        if (cancelled && cancelled()) throw ParseError("Serum 2: load cancelled");
        BlobPtr blob = vol.open(file);
        const std::string key = host ? file + "|" + std::to_string(blob->size()) : std::string();
        PcmPtr pcm;
        if (host) {
            std::lock_guard<std::mutex> lock(serum_pcm_mutex());
            auto it = serum_pcm_cache().find(key);
            if (it != serum_pcm_cache().end()) { pcm = it->second.pcm.lock(); if (pcm) ai = it->second.info; }
        }
        if (!pcm) {
            pcm = decode_audio(*blob, &ai);
            if (host && pcm) {
                std::lock_guard<std::mutex> lock(serum_pcm_mutex());
                auto &c = serum_pcm_cache();
                for (auto it = c.begin(); it != c.end();) it = it->second.pcm.expired() ? c.erase(it) : std::next(it);
                c[key] = SerumPcmEntry{pcm, ai};
            }
        }
        if (!pcm || pcm->frames() < 2) throw ParseError("Serum 2: empty sample: " + path_name(file));
        bytes += uint64_t(pcm->data.size()) * sizeof(pcm->data[0]);
        if (bytes > SERUM_SAMPLE_MAX) throw ParseError("Serum 2: multisample over " + std::to_string(SERUM_SAMPLE_MAX >> 20) + " MB");
        return pcm;
    };
    if (osc == 3) {   // 2.4.35: the NOISE oscillator's sample (Samples/Factory Non-Tonal/Noises/<rel>), looped unless one-shot
        const SerumItem n = o.field("NoiseOsc3");
        const std::string rel = n.field("relativePathToNoiseSample").text();
        if (rel.empty()) throw ParseError("Serum 2: the noise oscillator plays no sample");
        const std::string file = serum_library_file(vol, loc.inner, "Samples", "Factory Non-Tonal/Noises/" + rel);
        AudioInfo ai;
        ss->pcm = load(file, ai);
        ss->name = "Serum / " + path_stem(file);
        ss->root = 60;   // [G] Serum's noise does not follow the keys by default; Omni's sample oscillator does
        if (n.field("plainParams").number("kParamOneShot", 0) < 0.5) { ss->loop_start = 0; ss->loop_end = ss->pcm->frames(); }
        return ss;
    }
    if (serum_plays_sample(type)) {
        const SerumItem s = serum_sample_module(o, type, id);
        const std::string file = serum_library_file(vol, loc.inner, "Samples", s.field("samplePathRelative").text());
        AudioInfo ai;
        ss->pcm = load(file, ai);
        ss->name = "Serum / " + path_stem(file);
        ss->root = int(std::lround(s.number("baseNote", 60)));   // [G] absent = C3 (60)
        const int64_t n = ss->pcm->frames();
        const SerumItem mode = params.field("kParamLoopMode");
        const bool grains = !type.equals("kOsc_Sample");
        if ((mode.p && (mode.equals("kForward") || mode.equals("kPingPong") || mode.equals("kTailed"))) || (grains && !mode.p)) {
            // 2.4.35 [G]: a granular / spectral oscillator without a loop of its own holds its sound: the file loops
            const double a = clampd(params.number("kParamLoopStart", grains && !mode.p ? params.number("kParamStart", 0) : 0), 0, 100) / 100,
                         b = clampd(params.number("kParamLoopEnd", 100), 0, 100) / 100;
            ss->loop_start = std::min(n - 2, int64_t(a * double(n)));
            ss->loop_end = std::max(ss->loop_start + 2, int64_t(b * double(n)));
            if (ss->loop_end > n) ss->loop_end = n;
            ss->loop_type = mode.equals("kPingPong") ? LoopType::Alternating : LoopType::Forward;
        }
        return ss;
    }
    if (type.equals("kOsc_MultiSample")) {
        const SerumItem m = o.field("MultiSampleOsc" + id);
        const std::string sfz_rel = m.field("sfzPathRelative").text();
        std::string text = m.field("embedded_sfz").text();
        if (text.empty()) {   // no embedded map: the .sfz itself
            const std::string sfz = serum_library_file(vol, loc.inner, "Multisamples", sfz_rel);
            const std::vector<uint8_t> b = vol.open(sfz)->all(size_t(4) << 20);
            text.assign(b.begin(), b.end());
        }
        std::string base = path_dir(sfz_rel);
        std::replace(base.begin(), base.end(), '\\', '/');
        const std::vector<SerumSfzRegion> regions = serum_sfz_regions(text);
        if (regions.empty()) throw ParseError("Serum 2: multisample has no regions: " + sfz_rel);
        const std::string lib = serum_library_root(vol, loc.inner, "Multisamples");   // 2.4.35: once
        std::map<std::string, std::pair<PcmPtr, AudioInfo>> cache;   // a sample used by several regions: decoded once
        for (const SerumSfzRegion &r : regions) {
            std::string rel = r.sample;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            while (rel.find("//") != std::string::npos) rel.replace(rel.find("//"), 2, "/");
            auto it = cache.find(rel);
            if (it == cache.end()) {
                const std::string file = serum_library_file_in(vol, loc.inner, "Multisamples", lib, base.empty() ? rel : base + "/" + rel);
                AudioInfo ai;
                PcmPtr pcm = load(file, ai);
                it = cache.emplace(rel, std::make_pair(pcm, ai)).first;
            }
            SynthSample::Zone z;
            z.pcm = it->second.first;
            const AudioInfo &ai = it->second.second;
            const int64_t n = z.pcm->frames();
            z.lokey = clampi(r.lokey, 0, 127); z.hikey = clampi(r.hikey, z.lokey, 127);
            z.lovel = clampi(r.lovel, 0, 127); z.hivel = clampi(r.hivel, z.lovel, 127);
            z.root = r.key_center - r.transpose - r.tune_cents / 100.0;
            z.start = std::max<int64_t>(0, std::min<int64_t>(r.offset, n - 1));
            z.end = r.end >= 0 ? std::min<int64_t>(r.end + 1, n) : n;   // SFZ end is the last frame
            const bool no_loop = r.loop_mode == "no_loop" || r.loop_mode == "one_shot";
            if (!no_loop && r.loop_start >= 0 && r.loop_end > r.loop_start) {
                z.loop_start = r.loop_start; z.loop_end = std::min<int64_t>(r.loop_end + 1, z.end);
            } else if (!no_loop && !r.loop_mode.empty() && !ai.loops.empty()) {
                z.loop_start = ai.loops[0].start; z.loop_end = std::min<int64_t>(ai.loops[0].end + 1, z.end);
            }
            ss->zones.push_back(z);
        }
        ss->name = "Serum / " + path_stem(sfz_rel);
        return ss;
    }
    throw ParseError("Serum 2: oscillator " + id + " is not a sample or multisample");
}

}  // namespace omni

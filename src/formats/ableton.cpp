// Omni Sampler: Ableton Live Sampler / Simpler presets (.adv) and racks (.adg). Layout from ConvertWithMoss's
// format/ableton (AbletonDetector, AbletonTag and its ADV templates; LGPL-3.0). Not yet checked here against real
// files from Live: no .adv/.adg is in the workspace (only the synthetic test file this reader was written with,
// testdata/ableton/).
//
// Both are gzip-compressed XML (plain XML is accepted too), root <Ableton>. An .adv holds one device, <MultiSampler>
// (Sampler) or <OriginalSimpler> (Simpler); an .adg holds a <GroupDevicePreset> rack whose chains hold any number of
// them. Each device found (in document order) is one preset. Parameters are child elements with a Value attribute
// (<RootKey Value="60"/>), or with a <Manual Value=".."/> child for automatable ones.
// Per zone, Player/MultiSampleMap/SampleParts/MultiSamplePart: KeyRange / VelocityRange {Min, Max, CrossfadeMin,
// CrossfadeMax}, SelectorRange, RootKey, Detune (cents), TuneScale (0..100 = key tracking), Panorama (-1..1),
// Volume (linear), SampleStart / SampleEnd (frames), SustainLoop {Start, End, Mode 0 off / 1 forward / 2 back and
// forth, Crossfade, Detune}, ReleaseLoop Mode (< 3: the sustain loop ends at release), SampleRef/FileRef
// {RelativePath, Path, Name, RelativePathElement Dir=.. (Live 8/9 files), SearchHint/PathHint}.
// Device: MultiSampleMap RoundRobin / RoundRobinMode (0 forward, 1 backward, 3 random); VolumeAndPan {Volume (dB),
// Panorama, VolumeVelScale, Envelope {AttackTime, DecayTime, ReleaseTime (ms), AttackLevel, SustainLevel,
// ReleaseLevel, *Slope}}; Filter {IsOn, Slot/Value/SimplerFilter {Type 0 LP / 1 HP / 2 BP / 3 notch, Freq (Hz),
// Res (0..1.25), Slope (true = 24 dB), Envelope {IsOn, Amount (semitones), times as above}, ModByVelocity,
// ModByPitch}}; Globals/PlaybackMode 1 = one-shot (with VolumeAndPan/OneShotEnvelope/SustainMode 0 = trigger).
// Samples are looked up nearest first: RelativePath from the preset's folder and each folder above it (a Live pack
// or User Library root), the absolute Path on this machine, then the file name beside the preset, in its "Samples"
// folder or where the previous sample was found.
// Not read: the rack's chain key / velocity zones, chain volumes and macros, the pitch (aux) envelope, LFOs,
// transpose, warping and slicing, effects; the Core Library and Live Packs (.alp) are not searched; Live Sets (.als);
// the old binary preset format (Live 5-7) is refused.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include "../core/parse.hpp"
#include "format.hpp"
extern "C" {
#include "../third_party/tinf/tinf.h"
}

namespace omni {
namespace {

constexpr size_t MAX_FILE = size_t(32) << 20, MAX_XML = size_t(40) << 20, MAX_ZONES = 4096, MAX_DEVICES = 1024;

std::string read_text(VolumePtr vol, const std::string &path) {
    std::vector<uint8_t> d = vol->open(path)->all(MAX_FILE);
    if (d.size() >= 2 && d[0] == 0x1F && d[1] == 0x8B) {
        if (d.size() < 18) throw ParseError("Ableton: truncated file");
        uint32_t isize = le32(&d[d.size() - 4]);   // the gzip trailer
        if (isize == 0 || isize > MAX_XML) throw ParseError("Ableton: implausible uncompressed size");
        // exactly isize bytes, never more: inflate_gzip grows its buffer x4 when the data is longer than the trailer
        // says (up to 2.5 GB from a damaged or crafted 32 MB file)
        std::vector<uint8_t> out(isize);
        unsigned int len = isize;
        tinf_init();
        if (tinf_gzip_uncompress(out.data(), &len, d.data(), unsigned(d.size())) != TINF_OK) throw ParseError("Ableton: damaged gzip data");
        out.resize(len);
        d.swap(out);
    }
    if (d.size() >= 4 && le32(d.data()) == 0x78561EABu) throw ParseError("Ableton: old binary preset format (Live 5-7) is not readable");
    std::string s(d.begin(), d.end());
    size_t p = s.find('<');
    if (p == std::string::npos || p > 64 || s.find("<Ableton", p) == std::string::npos) throw ParseError("not an Ableton preset");
    return s;
}

bool tag_at(const std::string &s, size_t p, const char *name) {   // "<name" followed by '>', '/' or space
    size_t n = std::strlen(name);
    if (s.compare(p, n, name)) return false;
    char c = p + n < s.size() ? s[p + n] : 0;
    return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// The devices in document order, by a scan of the text (cheap: list() does not build the DOM)
size_t count_devices(const std::string &s) {
    size_t n = 0;
    for (size_t p = s.find('<'); p != std::string::npos; p = s.find('<', p + 1))
        if (tag_at(s, p, "<MultiSampler") || tag_at(s, p, "<OriginalSimpler")) n++;
    return n;
}

void collect_devices(const XmlNode *n, std::vector<const XmlNode *> &out) {
    for (const auto &c : n->children) {
        if (c->name == "MultiSampler" || c->name == "OriginalSimpler") {
            if (out.size() < MAX_DEVICES) out.push_back(c.get());
        } else collect_devices(c.get(), out);
    }
}

std::vector<PresetInfo> list_ableton(VolumePtr vol, const std::string &path) {
    size_t n = count_devices(read_text(vol, path));
    if (!n) throw ParseError("Ableton: no Sampler or Simpler in this preset");
    n = std::min(n, MAX_DEVICES);
    std::vector<PresetInfo> out;
    for (size_t i = 0; i < n; i++) out.push_back({n == 1 ? path_stem(path) : path_stem(path) + " " + std::to_string(i + 1), int(i)});
    return out;
}

// ---- values ----

std::string val(const XmlNode *n, const char *name) {
    const XmlNode *c = n ? n->child(name) : nullptr;
    return c ? c->attr("Value") : std::string();
}
double num(const std::string &s, double def) {
    if (s.empty()) return def;
    char *e = nullptr;
    double v = std::strtod(s.c_str(), &e);
    return e != s.c_str() && std::isfinite(v) ? clampd(v, -1e9, 1e9) : def;   // the int() casts stay defined
}
double val_num(const XmlNode *n, const char *name, double def) { return num(val(n, name), def); }
// <Name><Manual Value=".."/></Name>
std::string manual(const XmlNode *n, const char *name) { return val(n ? n->child(name) : nullptr, "Manual"); }
double manual_num(const XmlNode *n, const char *name, double def) { return num(manual(n, name), def); }
bool manual_on(const XmlNode *n, const char *name) { return manual(n, name) == "true"; }
const XmlNode *path(const XmlNode *n, std::initializer_list<const char *> names) {
    for (const char *s : names) {
        if (!n) return nullptr;
        n = n->child(s);
    }
    return n;
}

void read_env(const XmlNode *e, Envelope &env, double sustain_def) {
    env.set = true;
    env.attack = clampd(manual_num(e, "AttackTime", 0) / 1000.0, 0, 60);
    env.decay = clampd(manual_num(e, "DecayTime", 0) / 1000.0, 0, 60);
    env.release = clampd(manual_num(e, "ReleaseTime", 0) / 1000.0, 0, 60);
    env.start_level = clampd(manual_num(e, "AttackLevel", 0), 0, 1);
    env.sustain = clampd(manual_num(e, "SustainLevel", sustain_def), 0, 1);
    env.end_level = clampd(manual_num(e, "ReleaseLevel", 0), 0, 1);
    env.attack_slope = clampd(-manual_num(e, "AttackSlope", 0), -1, 1);
    env.decay_slope = clampd(-manual_num(e, "DecaySlope", 0), -1, 1);
    env.release_slope = clampd(-manual_num(e, "ReleaseSlope", 0), -1, 1);
}

// ---- sample lookup, nearest first ----

struct Finder {
    VolumePtr vol;
    std::string dir, stem;
    std::string base, folder;                       // where the last sample was found: by relative path / by name
    std::map<std::string, std::string> memo;        // reference -> found path ("" = missing)

    bool look(const std::string &p, std::string &found) {
        if (p.empty()) return false;
        try { return find_ci(*vol, p, found); } catch (const std::exception &) { return false; }
    }
    std::string find(const XmlNode *ref) {
        std::string rel = val(ref, "RelativePath");
        std::string name = val(ref, "Name");
        if (rel.empty() && !name.empty())
            if (const XmlNode *rp = ref->child("RelativePath")) {   // Live 8/9: one element per folder
                for (const XmlNode *e : rp->all("RelativePathElement")) {
                    std::string d = e->attr("Dir");
                    rel += (d.empty() ? std::string("..") : d) + "/";
                }
                rel += name;
            }
        std::string abs = val(ref, "Path");
        if (abs.empty() && !name.empty())
            if (const XmlNode *ph = path(ref, {"SearchHint", "PathHint"})) {
                for (const XmlNode *e : ph->all("RelativePathElement")) abs += "/" + e->attr("Dir");
                if (!abs.empty()) abs += "/" + name;
            }
        std::replace(rel.begin(), rel.end(), '\\', '/');
        std::replace(abs.begin(), abs.end(), '\\', '/');
        std::string key = rel + "|" + abs;
        auto m = memo.find(key);
        if (m != memo.end()) return m->second;
        std::string found;
        if (!rel.empty()) {
            if (!base.empty() && look(path_resolve(base, rel), found)) return memo[key] = found;
            std::string b = dir;
            for (int up = 0; up < 8; up++) {
                if ((base.empty() || b != base) && look(path_resolve(b, rel), found)) { base = b; return memo[key] = found; }
                if (b.empty()) break;
                b = path_dir(b);
            }
        }
        if (!abs.empty() && abs[0] == '/' && vol->kind() == "Folder" && look(abs.substr(1), found)) return memo[key] = found;
        std::string file = path_name(!rel.empty() ? rel : abs);
        if (!file.empty() && file != "." && file != "..") {
            for (const std::string &d : {folder, dir, path_join(dir, "Samples"), path_join(dir, stem)}) {
                if ((d.empty() && !dir.empty()) || !look(path_join(d, file), found)) continue;
                folder = path_dir(found);
                return memo[key] = found;
            }
        }
        return memo[key] = "";
    }
};

Instrument load_ableton(VolumePtr vol, const std::string &file, int index) {
    std::unique_ptr<XmlNode> root = parse_xml(read_text(vol, file));
    if (!root || root->name != "Ableton") throw ParseError("not an Ableton preset");
    std::vector<const XmlNode *> devices;
    collect_devices(root.get(), devices);
    if (devices.empty()) throw ParseError("Ableton: no Sampler or Simpler in this preset");
    if (index < 0 || size_t(index) >= devices.size()) throw ParseError("Ableton: no such device");
    const XmlNode *dev = devices[size_t(index)];

    Instrument inst;
    inst.format = dev->name == "OriginalSimpler" ? "Ableton Simpler" : "Ableton Sampler";
    inst.name = devices.size() == 1 ? path_stem(file) : path_stem(file) + " " + std::to_string(index + 1);
    const XmlNode *map = path(dev, {"Player", "MultiSampleMap"});
    const XmlNode *parts = map ? map->child("SampleParts") : nullptr;
    if (!parts) throw ParseError("Ableton: the device has no sample map");
    std::vector<const XmlNode *> zones = parts->all("MultiSamplePart");
    if (zones.empty()) throw ParseError("Ableton: the device has no samples");
    if (zones.size() > MAX_ZONES) throw ParseError("Ableton: too many zones");

    Finder finder{vol, path_dir(file), path_stem(file), "", "", {}};
    std::map<std::string, SampleRefPtr> refs;
    struct Sel { int lo, hi, xlo, xhi; };
    std::vector<Sel> sel;
    size_t missing = 0;
    std::string first_missing;
    for (const XmlNode *part : zones) {
        const XmlNode *ref = path(part, {"SampleRef", "FileRef"});
        if (!ref) { missing++; continue; }
        std::string p = finder.find(ref);
        if (p.empty()) {
            if (!missing++) {
                first_missing = val(ref, "RelativePath");
                if (first_missing.empty()) first_missing = val(ref, "Path");
                if (first_missing.empty()) first_missing = val(ref, "Name");
            }
            continue;
        }
        Zone z;
        auto it = refs.find(p);
        z.sample = it != refs.end() ? it->second : (refs[p] = file_sample_ref(vol, p));
        z.name = path_stem(val(part, "Name"));
        if (z.name.empty()) z.name = path_stem(p);
        const XmlNode *kr = part->child("KeyRange"), *vr = part->child("VelocityRange");
        z.key_lo = clampi(int(val_num(kr, "Min", 0)), 0, 127);
        z.key_hi = clampi(int(val_num(kr, "Max", 127)), 0, 127);
        z.key_xfade_lo = clampi(std::abs(int(val_num(kr, "CrossfadeMin", z.key_lo)) - z.key_lo), 0, 127);
        z.key_xfade_hi = clampi(std::abs(int(val_num(kr, "CrossfadeMax", z.key_hi)) - z.key_hi), 0, 127);
        z.vel_lo = clampi(int(val_num(vr, "Min", 1)), 0, 127);
        z.vel_hi = clampi(int(val_num(vr, "Max", 127)), 0, 127);
        z.vel_xfade_lo = clampi(std::abs(int(val_num(vr, "CrossfadeMin", z.vel_lo)) - z.vel_lo), 0, 127);
        z.vel_xfade_hi = clampi(std::abs(int(val_num(vr, "CrossfadeMax", z.vel_hi)) - z.vel_hi), 0, 127);
        z.root = clampi(int(val_num(part, "RootKey", 60)), 0, 127);
        z.tune = clampd(val_num(part, "Detune", 0), -4800, 4800) / 100.0;
        z.key_tracking = clampd(val_num(part, "TuneScale", 100) / 100.0, 0, 1);
        z.pan = clampd(val_num(part, "Panorama", 0), -1, 1);
        double vol_lin = val_num(part, "Volume", 1);
        z.gain_db = vol_lin <= 1e-6 ? -96 : clampd(20 * std::log10(vol_lin), -96, 24);
        int64_t start = int64_t(clampd(val_num(part, "SampleStart", 0), 0, 4e9));
        int64_t end = int64_t(clampd(val_num(part, "SampleEnd", -1), -1, 4e9));
        z.start = start;
        if (end > start) z.stop = end;
        z.reverse = manual(part, "Reverse") == "true";
        if (const XmlNode *sl = part->child("SustainLoop")) {
            int mode = int(val_num(sl, "Mode", 0));
            if (mode > 0) {
                Loop l;
                l.type = mode == 1 ? LoopType::Forward : LoopType::Alternating;
                l.start = int64_t(clampd(val_num(sl, "Start", 0), 0, 4e9));
                l.end = int64_t(clampd(val_num(sl, "End", double(z.stop)), -1, 4e9));
                if (z.stop > 0) l.end = std::min(l.end, z.stop - 1);
                l.crossfade = int64_t(clampd(val_num(sl, "Crossfade", 0), 0, 4e9));
                l.tune = clampd(val_num(sl, "Detune", 0), -4800, 4800) / 100.0;
                const XmlNode *rl = part->child("ReleaseLoop");
                l.until_release = rl && int(val_num(rl, "Mode", 0)) < 3;
                if (l.end > l.start) z.loops.push_back(l);
            }
        }
        const XmlNode *sr = part->child("SelectorRange");
        int slo = int(val_num(sr, "Min", 0)), shi = int(val_num(sr, "Max", 127));
        sel.push_back({slo, shi, int(val_num(sr, "CrossfadeMin", slo)), int(val_num(sr, "CrossfadeMax", shi))});
        inst.zones.push_back(std::move(z));
    }

    // round robin: zones on the same key / velocity range take turns
    std::map<std::string, std::vector<size_t>> clusters;
    std::vector<std::string> order;
    for (size_t i = 0; i < inst.zones.size(); i++) {
        const Zone &z = inst.zones[i];
        std::string k = std::to_string(z.key_lo) + "," + std::to_string(z.key_hi) + "," + std::to_string(z.vel_lo) + "," + std::to_string(z.vel_hi);
        if (!clusters.count(k)) order.push_back(k);
        clusters[k].push_back(i);
    }
    if (val(map, "RoundRobin") == "true") {
        int mode = int(val_num(map, "RoundRobinMode", 0));
        for (const std::string &k : order) {
            const std::vector<size_t> &c = clusters[k];
            int n = int(c.size());
            if (n < 2) continue;
            for (int i = 0; i < n; i++) {
                Zone &z = inst.zones[c[size_t(i)]];
                if (mode == 3) {
                    z.play_logic = PlayLogic::Random;
                    z.rand_lo = double(i) / n;
                    z.rand_hi = double(i + 1) / n;
                } else {
                    z.play_logic = PlayLogic::RoundRobin;
                    z.seq_position = mode == 1 ? n - i : i + 1;
                    z.seq_length = n;
                }
            }
        }
    } else {
        // the chain selector used as a round robin: distinct, non-overlapping selector ranges without fades
        for (const std::string &k : order) {
            std::vector<size_t> c = clusters[k];
            if (c.size() < 2) continue;
            bool ok = true;
            for (size_t i : c) ok = ok && sel[i].xlo == sel[i].lo && sel[i].xhi == sel[i].hi;
            if (!ok) continue;
            std::stable_sort(c.begin(), c.end(), [&](size_t a, size_t b) { return sel[a].lo < sel[b].lo; });
            for (size_t i = 1; i < c.size() && ok; i++) ok = sel[c[i]].lo > sel[c[i - 1]].hi;
            if (!ok) continue;
            int n = int(c.size());
            for (int i = 0; i < n; i++) {
                Zone &z = inst.zones[c[size_t(i)]];
                z.play_logic = PlayLogic::RoundRobin;
                z.seq_position = i + 1;
                z.seq_length = n;
            }
        }
    }

    // device-wide settings
    const XmlNode *vp = dev->child("VolumeAndPan");
    inst.gain_db = clampd(manual_num(vp, "Volume", 0), -36, 36);
    double dev_pan = clampd(manual_num(vp, "Panorama", 0), -1, 1);
    double vel_depth = clampd(manual_num(vp, "VolumeVelScale", 1), 0, 1);
    bool one_shot = false;
    if (int(val_num(dev->child("Globals"), "PlaybackMode", 0)) == 1) {
        const XmlNode *os = vp ? vp->child("OneShotEnvelope") : nullptr;
        one_shot = !os || !os->child("SustainMode") || int(manual_num(os, "SustainMode", 0)) == 0;
    }
    Envelope amp;
    const XmlNode *ae = vp ? vp->child("Envelope") : nullptr;
    if (ae && !one_shot) read_env(ae, amp, 1);
    Filter flt;
    const XmlNode *fl = dev->child("Filter");
    if (fl && manual_on(fl, "IsOn")) {
        if (const XmlNode *sf = path(fl, {"Slot", "Value", "SimplerFilter"})) {
            static const FilterType types[4] = {FilterType::LowPass, FilterType::HighPass, FilterType::BandPass, FilterType::BandReject};
            int t = int(manual_num(sf, "Type", 0));
            flt.type = t >= 0 && t < 4 ? types[t] : FilterType::LowPass;
            flt.poles = manual_on(sf, "Slope") ? 4 : 2;
            flt.cutoff = clampd(manual_num(sf, "Freq", MAX_CUTOFF_HZ), 20, MAX_CUTOFF_HZ);
            flt.resonance = clampd(manual_num(sf, "Res", 0) / 1.25, 0, 1);
            flt.vel_depth = clampd(manual_num(sf, "ModByVelocity", 0), -1, 1);
            flt.key_tracking = clampd(manual_num(sf, "ModByPitch", 0), -1, 1);
            const XmlNode *fe = sf->child("Envelope");
            if (fe && manual_on(fe, "IsOn")) {
                read_env(fe, flt.env, 1);
                flt.env_depth = clampd(manual_num(fe, "Amount", 0) * 100.0 / MAX_ENVELOPE_DEPTH, -1, 1);
            }
        }
    }
    for (Zone &z : inst.zones) {
        z.pan = clampd(z.pan + dev_pan, -1, 1);
        z.amp_vel_depth = vel_depth;
        z.one_shot = one_shot;
        if (amp.set) z.amp_env = amp;
        z.filter = flt;
    }

    if (missing) inst.warnings.push_back("Ableton: " + std::to_string(missing) + " sample(s) not found, e.g. " + first_missing);
    if (path(dev, {"AuxEnv", "IsOn"}) && manual_on(dev->child("AuxEnv"), "IsOn")) inst.warnings.push_back("Ableton: the aux (pitch) envelope is not read");
    if (root->find("GroupDevicePreset")) inst.warnings.push_back("Ableton: the rack's chain zones, volumes and macros are not read");
    inst.warnings.push_back("Ableton: LFOs, transpose, warping and effects are not read");
    if (inst.zones.empty())
        throw ParseError("Ableton: none of the " + std::to_string(missing) + " samples found, e.g. " + first_missing);
    finish_instrument(inst);
    return inst;
}

bool probe_ableton(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    if (n < 2) return false;
    if (h[0] == 0x1F && h[1] == 0x8B) return size >= 18;   // gzip: the content is checked when listing
    std::string head(reinterpret_cast<const char *>(h), std::min<size_t>(n, 512));
    return head.find("<Ableton") != std::string::npos;
}

}  // namespace

void register_ableton() {
    register_reader({"Ableton Sampler/Simpler", "adv adg", probe_ableton, list_ableton, load_ableton});
}

}  // namespace omni

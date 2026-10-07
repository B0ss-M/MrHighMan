// Omni Sampler: its own instrument format, written when a preset is extracted from a disk image (or on EXTRACT):
// <name>.omni (JSON: every field of the instrument model, lossless) next to "<name> Samples/" (16-bit WAVs, as the
// engine plays them). A project that used a preset from a disk image then loads just this, not the image.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sys/stat.h>
#include "../core/audio.hpp"
#include "../core/parse.hpp"
#include "format.hpp"
#include "omni_native.hpp"

namespace omni {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// JSON writing

std::string jstr(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}
std::string jnum(double v) {
    if (!std::isfinite(v)) v = 0;
    char b[40];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}

struct Obj {
    std::string s = "{";
    Obj &add(const char *k, const std::string &raw) {
        if (s.size() > 1) s += ",";
        s += jstr(k) + ":" + raw;
        return *this;
    }
    Obj &num(const char *k, double v) { return add(k, jnum(v)); }
    Obj &str(const char *k, const std::string &v) { return add(k, jstr(v)); }
    Obj &flag(const char *k, bool v) { return add(k, v ? "true" : "false"); }
    std::string done() { return s + "}"; }
};

std::string env_json(const Envelope &e) {
    if (!e.set) return "null";
    return Obj().num("delay", e.delay).num("start", e.start_level).num("attack", e.attack).num("hold", e.hold).num("decay", e.decay)
        .num("sustain", e.sustain).num("release", e.release).num("end", e.end_level).num("attack_slope", e.attack_slope)
        .num("decay_slope", e.decay_slope).num("release_slope", e.release_slope).num("time_key", e.time_key_tracking)
        .num("time_vel", e.time_vel_tracking).done();
}
std::string lfo_json(const Lfo &l) {
    if (!l.set) return "null";
    return Obj().num("wave", int(l.wave)).num("rate", l.rate_hz).num("delay", l.delay).num("fade_in", l.fade_in)
        .num("phase", l.start_phase).flag("key_sync", l.key_sync).done();
}
std::string filter_json(const Filter &f) {
    if (f.type == FilterType::None) return "null";
    return Obj().num("type", int(f.type)).num("poles", f.poles).num("cutoff", f.cutoff).num("resonance", f.resonance)
        .num("key_tracking", f.key_tracking).num("env_depth", f.env_depth).add("env", env_json(f.env)).num("vel_depth", f.vel_depth)
        .num("lfo_depth", f.lfo_depth).add("lfo", lfo_json(f.lfo)).num("modwheel_depth", f.modwheel_depth).done();
}

// ---------------------------------------------------------------------------------------------------------------
// JSON reading

Envelope env_from(const Json &j) {
    Envelope e;
    if (j.type != Json::Object) return e;
    e.set = true;
    e.delay = j["delay"].as_num(0); e.start_level = j["start"].as_num(0); e.attack = j["attack"].as_num(0); e.hold = j["hold"].as_num(0);
    e.decay = j["decay"].as_num(0); e.sustain = j["sustain"].as_num(1); e.release = j["release"].as_num(0); e.end_level = j["end"].as_num(0);
    e.attack_slope = j["attack_slope"].as_num(0); e.decay_slope = j["decay_slope"].as_num(0); e.release_slope = j["release_slope"].as_num(0);
    e.time_key_tracking = j["time_key"].as_num(0); e.time_vel_tracking = j["time_vel"].as_num(0);
    return e;
}
Lfo lfo_from(const Json &j) {
    Lfo l;
    if (j.type != Json::Object) return l;
    l.set = true;
    l.wave = LfoWave(std::max(0, std::min(5, j["wave"].as_int(1))));
    l.rate_hz = j["rate"].as_num(5); l.delay = j["delay"].as_num(0); l.fade_in = j["fade_in"].as_num(0);
    l.start_phase = j["phase"].as_num(0); l.key_sync = j["key_sync"].as_bool(true);
    return l;
}
Filter filter_from(const Json &j) {
    Filter f;
    if (j.type != Json::Object) return f;
    f.type = FilterType(std::max(0, std::min(4, j["type"].as_int(0))));
    f.poles = j["poles"].as_int(2); f.cutoff = j["cutoff"].as_num(MAX_CUTOFF_HZ); f.resonance = j["resonance"].as_num(0);
    f.key_tracking = j["key_tracking"].as_num(0); f.env_depth = j["env_depth"].as_num(0); f.env = env_from(j["env"]);
    f.vel_depth = j["vel_depth"].as_num(0); f.lfo_depth = j["lfo_depth"].as_num(0); f.lfo = lfo_from(j["lfo"]);
    f.modwheel_depth = j["modwheel_depth"].as_num(0);
    return f;
}

// ---------------------------------------------------------------------------------------------------------------
// files

std::string file_safe(std::string s) {
    for (char &c : s) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    s = trim(s);
    while (!s.empty() && s.back() == '.') s.pop_back();
    if (s.size() > 80) s.resize(80);
    return s.empty() ? "Instrument" : s;
}

void mkdirs(const std::string &dir) {
    std::string cur;
    for (auto &part : path_split(dir)) {
        cur += "/" + part;
        mkdir(cur.c_str(), 0755);
    }
}

bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

void write_file(const std::string &path, const std::string &data) {
    std::string tmp = path + ".new";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) throw ParseError("cannot write " + path);
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); throw ParseError("cannot write " + path); }
}

std::string wav_bytes(const Pcm &p) {
    auto u32 = [](std::string &s, uint32_t v) { for (int i = 0; i < 4; i++) s += char(v >> (8 * i) & 0xFF); };
    auto u16 = [](std::string &s, uint16_t v) { s += char(v & 0xFF); s += char(v >> 8); };
    uint32_t data = uint32_t(p.data.size() * 2);
    std::string s = "RIFF";
    u32(s, 36 + data);
    s += "WAVEfmt ";
    u32(s, 16); u16(s, 1); u16(s, uint16_t(p.channels)); u32(s, uint32_t(p.rate));
    u32(s, uint32_t(p.rate * p.channels * 2)); u16(s, uint16_t(p.channels * 2)); u16(s, 16);
    s += "data";
    u32(s, data);
    size_t at = s.size();
    s.resize(at + data);
    for (size_t i = 0; i < p.data.size(); i++) { uint16_t v = uint16_t(p.data[i]); s[at + 2 * i] = char(v & 0xFF); s[at + 2 * i + 1] = char(v >> 8); }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// the reader

Json read_doc(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(64 << 20);
    Json j = parse_json(std::string(d.begin(), d.end()));
    if (j["omni"].as_int(0) < 1 || j["omni"].as_int(0) > OMNI_VERSION) throw ParseError("not an Omni Sampler instrument");
    return j;
}

std::vector<PresetInfo> list_omni(VolumePtr vol, const std::string &path) {
    Json j = read_doc(*vol, path);
    return {PresetInfo{j["name"].as_str(path_stem(path)), 0}};
}

Instrument load_omni(VolumePtr vol, const std::string &path, int) {
    Json j = read_doc(*vol, path);
    Instrument inst;
    inst.name = j["name"].as_str(path_stem(path));
    std::string fmt = j["format"].as_str("");
    inst.format = fmt.empty() ? "Omni instrument" : fmt + " (extracted)";
    inst.polyphony = j["polyphony"].as_int(0);
    inst.gain_db = j["gain_db"].as_num(0);
    inst.groups.clear();
    for (auto &g : j["groups"].arr) inst.groups.push_back(Group{g["name"].as_str(""), Trigger(std::max(0, std::min(3, g["trigger"].as_int(0))))});
    if (inst.groups.empty()) inst.groups.push_back(Group{});
    std::vector<SampleRefPtr> refs;
    std::string dir = path_dir(path);
    int missing = 0;
    for (auto &s : j["samples"].arr) {
        std::string rel = s.as_str(""), found;
        if (!rel.empty() && find_ci(*vol, path_resolve(dir, rel), found)) refs.push_back(file_sample_ref(vol, found));
        else { refs.push_back(nullptr); missing++; }
    }
    for (auto &zj : j["zones"].arr) {
        int si = zj["sample"].as_int(-1);
        if (si < 0 || si >= int(refs.size()) || !refs[size_t(si)]) continue;
        Zone z;
        z.name = zj["name"].as_str("");
        z.sample = refs[size_t(si)];
        z.key_lo = zj["key_lo"].as_int(0); z.key_hi = zj["key_hi"].as_int(127); z.root = zj["root"].as_int(60);
        z.key_xfade_lo = zj["key_xfade_lo"].as_int(0); z.key_xfade_hi = zj["key_xfade_hi"].as_int(0);
        z.vel_lo = zj["vel_lo"].as_int(0); z.vel_hi = zj["vel_hi"].as_int(127);
        z.vel_xfade_lo = zj["vel_xfade_lo"].as_int(0); z.vel_xfade_hi = zj["vel_xfade_hi"].as_int(0);
        z.group = std::max(0, std::min(int(inst.groups.size()) - 1, zj["group"].as_int(0)));
        z.gain_db = zj["gain_db"].as_num(0); z.pan = zj["pan"].as_num(0); z.tune = zj["tune"].as_num(0);
        z.key_tracking = zj["key_tracking"].as_num(1); z.amp_key_tracking = zj["amp_key_tracking"].as_num(0);
        z.amp_vel_depth = zj["amp_vel_depth"].as_num(1); z.amp_vel_curve = zj["amp_vel_curve"].as_num(0);
        z.reverse = zj["reverse"].as_bool(false); z.one_shot = zj["one_shot"].as_bool(false);
        z.exclusive_group = zj["exclusive_group"].as_int(0); z.off_by = zj["off_by"].as_int(-1);
        z.trigger = Trigger(std::max(0, std::min(3, zj["trigger"].as_int(0))));
        z.play_logic = PlayLogic(std::max(0, std::min(2, zj["play_logic"].as_int(0))));
        z.seq_position = zj["seq_position"].as_int(1); z.seq_length = zj["seq_length"].as_int(1);
        z.rand_lo = zj["rand_lo"].as_num(0); z.rand_hi = zj["rand_hi"].as_num(1);
        z.start = int64_t(zj["start"].as_num(0)); z.stop = int64_t(zj["stop"].as_num(-1));
        for (auto &l : zj["loops"].arr) {
            Loop lp;
            lp.type = LoopType(std::max(0, std::min(2, l["type"].as_int(0))));
            lp.start = int64_t(l["start"].as_num(0)); lp.end = int64_t(l["end"].as_num(0));
            lp.until_release = l["until_release"].as_bool(false); lp.crossfade = int64_t(l["crossfade"].as_num(0)); lp.tune = l["tune"].as_num(0);
            z.loops.push_back(lp);
        }
        z.amp_env = env_from(zj["amp_env"]);
        z.amp_lfo_depth = zj["amp_lfo_depth"].as_num(0); z.amp_lfo = lfo_from(zj["amp_lfo"]);
        z.pitch_env_depth = zj["pitch_env_depth"].as_num(0); z.pitch_env = env_from(zj["pitch_env"]);
        z.pitch_lfo_depth = zj["pitch_lfo_depth"].as_num(0); z.pitch_lfo = lfo_from(zj["pitch_lfo"]);
        z.bend_up = zj["bend_up"].as_int(200); z.bend_down = zj["bend_down"].as_int(-200);
        z.filter = filter_from(zj["filter"]);
        z.midi_channel = zj["midi_channel"].as_int(-1);
        inst.zones.push_back(z);
    }
    if (missing) inst.warnings.push_back(std::to_string(missing) + " sample files missing next to the instrument");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("this Omni instrument's samples are missing");
    return inst;
}

bool probe_omni(const std::string &name, const uint8_t *h, size_t n, uint64_t) {
    std::string s(reinterpret_cast<const char *>(h), std::min<size_t>(n, 32));
    return ends_with_ci(name, ".omni") && s.find("\"omni\"") != std::string::npos;
}

}  // namespace

std::string omni_source_of(const std::string &host_path) {
    FILE *f = std::fopen(host_path.c_str(), "rb");
    if (!f) return "";
    char buf[4096];
    size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    // the header keys come first (written that way below): parse only them
    const char *p = std::strstr(buf, "\"source\":");
    if (!p) return "";
    const char *q = std::strstr(p, ",\"source_preset\":");
    const char *e = q ? std::strchr(q + 17, ',') : nullptr;
    if (!q || !e) return "";
    try {
        Json j = parse_json("{" + std::string(p, size_t(e - p)) + "}");
        return j["source"].as_str("") + "\n" + std::to_string(j["source_preset"].as_int(0));
    } catch (const ParseError &) { return ""; }
}

bool omni_stale(const std::string &host_path) {
    FILE *f = std::fopen(host_path.c_str(), "rb");
    if (!f) return false;
    char buf[64];
    size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    int version = 0;
    if (std::sscanf(buf, "{\"omni\":%d", &version) != 1 || version >= OMNI_VERSION) return false;
    // version 1 wrote Roland S-500 loops one frame long and without their loop tune
    std::string src = omni_source_of(host_path);
    size_t nl = src.find('\n');
    return nl != std::string::npos && ends_with_ci(src.substr(0, nl), ".r5p");
}

Settings_list omni_settings_of(const std::string &host_path) {
    Settings_list out;
    FILE *f = std::fopen(host_path.c_str(), "rb");
    if (!f) return out;
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (64u << 20)) text.append(buf, n);
    std::fclose(f);
    try {
        Json j = parse_json(text);
        for (auto &kv : j["settings"].obj) if (kv.second.type == Json::Number) out.push_back({kv.first, float(kv.second.num)});
    } catch (const ParseError &) {}
    return out;
}

std::string write_omni(const Instrument &inst, const std::vector<PcmPtr> &zone_pcm, const std::string &dir_in,
                       const std::string &source, int source_preset, const std::string &settings, const std::string &force_path) {
    std::string dir = force_path.empty() ? dir_in : path_dir(force_path);
    mkdirs(dir);
    std::string base = file_safe(inst.name), name = base, key = source + "\n" + std::to_string(source_preset);
    if (!force_path.empty()) name = path_stem(force_path);
    // the same source extracted before: reuse it (rewritten when there are settings to store); another instrument
    // of the same name: number this one
    else
        for (int k = 2;; k++) {
            std::string p = path_join(dir, name + ".omni");
            if (!exists(p)) break;
            if (omni_source_of(p) == key) {
                if (settings.empty()) return p;
                break;
            }
            name = base + " (" + std::to_string(k) + ")";
        }
    const std::string &out_dir = dir;
    std::string sdir_name = name + " Samples", sdir = path_join(out_dir, sdir_name);
    mkdirs(sdir);
    // one WAV per distinct sample
    std::vector<std::string> sample_files;
    std::vector<int> zone_sample(inst.zones.size(), -1);
    std::map<const Pcm *, int> by_pcm;
    std::set<std::string> used;
    for (size_t i = 0; i < inst.zones.size(); i++) {
        PcmPtr pcm = i < zone_pcm.size() ? zone_pcm[i] : nullptr;
        if (!pcm && inst.zones[i].sample) {
            try { pcm = inst.zones[i].sample->decode(); } catch (const std::exception &) {}
        }
        if (!pcm) continue;
        auto it = by_pcm.find(pcm.get());
        if (it == by_pcm.end()) {
            char num[16];
            std::snprintf(num, sizeof num, "%03zu ", sample_files.size() + 1);
            std::string sname = inst.zones[i].sample && !inst.zones[i].sample->name.empty() ? inst.zones[i].sample->name : inst.zones[i].name;
            // a sample of an extracted instrument already carries its "NNN " number: drop it
            if (sname.size() > 4 && isdigit((unsigned char)sname[0]) && isdigit((unsigned char)sname[1]) && isdigit((unsigned char)sname[2]) && sname[3] == ' ')
                sname = sname.substr(4);
            std::string fn = std::string(num) + file_safe(sname) + ".wav";
            if (!exists(path_join(sdir, fn))) write_file(path_join(sdir, fn), wav_bytes(*pcm));
            it = by_pcm.emplace(pcm.get(), int(sample_files.size())).first;
            sample_files.push_back(sdir_name + "/" + fn);
        }
        zone_sample[i] = it->second;
    }
    std::string zones = "[";
    for (size_t i = 0; i < inst.zones.size(); i++) {
        if (zone_sample[i] < 0) continue;
        const Zone &z = inst.zones[i];
        Obj o;
        o.str("name", z.name).num("sample", zone_sample[i]).num("key_lo", z.key_lo).num("key_hi", z.key_hi).num("root", z.root)
            .num("key_xfade_lo", z.key_xfade_lo).num("key_xfade_hi", z.key_xfade_hi).num("vel_lo", z.vel_lo).num("vel_hi", z.vel_hi)
            .num("vel_xfade_lo", z.vel_xfade_lo).num("vel_xfade_hi", z.vel_xfade_hi).num("group", z.group).num("gain_db", z.gain_db)
            .num("pan", z.pan).num("tune", z.tune).num("key_tracking", z.key_tracking).num("amp_key_tracking", z.amp_key_tracking)
            .num("amp_vel_depth", z.amp_vel_depth).num("amp_vel_curve", z.amp_vel_curve).flag("reverse", z.reverse)
            .flag("one_shot", z.one_shot).num("exclusive_group", z.exclusive_group).num("off_by", z.off_by).num("trigger", int(z.trigger))
            .num("play_logic", int(z.play_logic)).num("seq_position", z.seq_position).num("seq_length", z.seq_length)
            .num("rand_lo", z.rand_lo).num("rand_hi", z.rand_hi).num("start", double(z.start)).num("stop", double(z.stop));
        std::string loops = "[";
        for (auto &l : z.loops)
            loops += (loops.size() > 1 ? "," : "") + Obj().num("type", int(l.type)).num("start", double(l.start)).num("end", double(l.end))
                                                           .flag("until_release", l.until_release).num("crossfade", double(l.crossfade)).num("tune", l.tune).done();
        o.add("loops", loops + "]").add("amp_env", env_json(z.amp_env)).num("amp_lfo_depth", z.amp_lfo_depth).add("amp_lfo", lfo_json(z.amp_lfo))
            .num("pitch_env_depth", z.pitch_env_depth).add("pitch_env", env_json(z.pitch_env)).num("pitch_lfo_depth", z.pitch_lfo_depth)
            .add("pitch_lfo", lfo_json(z.pitch_lfo)).num("bend_up", z.bend_up).num("bend_down", z.bend_down)
            .add("filter", filter_json(z.filter)).num("midi_channel", z.midi_channel);
        zones += (zones.size() > 1 ? ",\n" : "\n") + o.done();
    }
    zones += "\n]";
    std::string groups = "[";
    for (auto &g : inst.groups) groups += (groups.size() > 1 ? "," : "") + Obj().str("name", g.name).num("trigger", int(g.trigger)).done();
    groups += "]";
    std::string samples = "[";
    for (auto &s : sample_files) samples += (samples.size() > 1 ? "," : "") + jstr(s);
    samples += "]";
    // header keys first: omni_source_of reads them from the start of the file
    std::string doc = "{\"omni\":" + std::to_string(OMNI_VERSION) + ",\"source\":" + jstr(source) + ",\"source_preset\":" + std::to_string(source_preset) + ",\"name\":" + jstr(inst.name) +
                      ",\"format\":" + jstr(inst.format) + ",\"polyphony\":" + std::to_string(inst.polyphony) + ",\"gain_db\":" + jnum(inst.gain_db) +
                      (settings.empty() ? std::string() : ",\n\"settings\":" + settings) +
                      ",\n\"groups\":" + groups + ",\n\"samples\":" + samples + ",\n\"zones\":" + zones + "}\n";
    std::string out = path_join(out_dir, name + ".omni");
    write_file(out, doc);
    return out;
}

void register_omni_native() {
    register_reader({"Omni instrument", "omni", probe_omni, list_omni, load_omni});
}

}  // namespace omni

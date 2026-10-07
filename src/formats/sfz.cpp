// Omni Sampler: SFZ. Headers <control> <global> <master> <group> <region> inherit downwards; #include and #define
// are expanded first. Opcode mapping follows ConvertWithMoss's SfzDetector (LGPL-3.0) and the sfzformat.com spec.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include "format.hpp"

namespace omni {
namespace {

using Opcodes = std::map<std::string, std::string>;

std::string read_text(Volume &vol, const std::string &path) {
    std::vector<uint8_t> b = vol.open(path)->all(16u << 20);
    size_t start = b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF ? 3 : 0;
    return std::string(b.begin() + long(start), b.end());
}

// #include "x.sfz" (relative to the including file) and #define $NAME value
std::string expand(Volume &vol, const std::string &path, std::map<std::string, std::string> &defs, std::set<std::string> &active,
                   int depth) {
    if (depth > 16 || active.count(path)) throw ParseError("SFZ include loop at " + path);
    active.insert(path);
    std::string text = read_text(vol, path), out;
    size_t i = 0;
    while (i < text.size()) {
        size_t eol = text.find('\n', i);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(i, eol - i);
        i = eol + 1;
        std::string t = trim(line);
        if (t.compare(0, 8, "#include") == 0) {
            size_t a = t.find('"'), b = t.rfind('"');
            if (a != std::string::npos && b > a) {
                std::string inc = path_resolve(path_dir(path), t.substr(a + 1, b - a - 1)), found;
                if (find_ci(vol, inc, found)) out += expand(vol, found, defs, active, depth + 1);
            }
            out += '\n';
            continue;
        }
        if (t.compare(0, 7, "#define") == 0) {
            std::string rest = trim(t.substr(7));
            size_t sp = rest.find_first_of(" \t");
            if (sp != std::string::npos) defs[rest.substr(0, sp)] = trim(rest.substr(sp));
            out += '\n';
            continue;
        }
        out += line;
        out += '\n';
    }
    active.erase(path);
    return out;
}

std::string substitute(const std::string &text, const std::map<std::string, std::string> &defs) {
    if (defs.empty()) return text;
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '$') {
            size_t best = 0;
            const std::string *val = nullptr;
            for (auto &d : defs)   // longest matching name wins ($A vs $AB)
                if (d.first.size() > best && text.compare(i, d.first.size(), d.first) == 0) { best = d.first.size(); val = &d.second; }
            if (val) { out += *val; i += best; continue; }
        }
        out += text[i++];
    }
    return out;
}

std::string strip_comments(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') i++;
            out += '\n';
        } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) i++;
            i++;
            out += ' ';
        } else out += text[i];
    }
    return out;
}

struct Header { std::string name; Opcodes ops; };

// Opcode values may contain spaces (sample=My Piano C3.wav): a value runs until the next "word=".
std::vector<Header> tokenize(const std::string &text) {
    std::vector<Header> out;
    size_t i = 0;
    while ((i = text.find('<', i)) != std::string::npos) {
        size_t close = text.find('>', i);
        if (close == std::string::npos) break;
        Header h;
        h.name = lower(trim(text.substr(i + 1, close - i - 1)));
        size_t next = text.find('<', close);
        std::string body = text.substr(close + 1, next == std::string::npos ? std::string::npos : next - close - 1);
        i = next == std::string::npos ? text.size() : next;
        // find every "key=" position
        std::vector<std::pair<size_t, size_t>> keys;   // (key start, '=' pos)
        for (size_t p = 0; p < body.size(); p++) {
            if (body[p] != '=') continue;
            size_t k = p;
            while (k > 0 && (isalnum((unsigned char)body[k - 1]) || body[k - 1] == '_' || body[k - 1] == '$')) k--;
            if (k == p) continue;
            if (k > 0 && !isspace((unsigned char)body[k - 1])) continue;
            keys.push_back({k, p});
        }
        for (size_t k = 0; k < keys.size(); k++) {
            std::string key = body.substr(keys[k].first, keys[k].second - keys[k].first);
            size_t vs = keys[k].second + 1, ve = k + 1 < keys.size() ? keys[k + 1].first : body.size();
            std::string val = trim(body.substr(vs, ve - vs));
            // a value without spaces ends at the first whitespace except for sample paths
            if (key != "sample" && key != "default_path" && key.find("label") == std::string::npos) {
                size_t sp = val.find_first_of(" \t\r\n");
                if (sp != std::string::npos) val = val.substr(0, sp);
            }
            h.ops[key] = val;
        }
        out.push_back(std::move(h));
    }
    return out;
}

int note_value(const std::string &v, int octave_offset, int note_offset) {
    if (v.empty()) return -1;
    if (isdigit((unsigned char)v[0]) || v[0] == '-') return std::atoi(v.c_str()) + octave_offset * 12 + note_offset;
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};   // a b c d e f g
    char c = char(tolower(v[0]));
    if (c < 'a' || c > 'g') return -1;
    int n = base[c - 'a'];
    size_t p = 1;
    if (p < v.size() && (v[p] == '#' || v[p] == 's')) { n++; p++; }
    else if (p < v.size() && v[p] == 'b') { n--; p++; }
    int octave = std::atoi(v.c_str() + p);
    return (octave + 1) * 12 + n + octave_offset * 12 + note_offset;   // c4 = 60
}

struct Scope {
    Opcodes merged;
    bool has(const std::string &k) const { return merged.count(k) > 0; }
    std::string str(const std::string &k, const std::string &def = "") const {
        auto it = merged.find(k); return it == merged.end() ? def : it->second;
    }
    double num(const std::string &k, double def) const {
        auto it = merged.find(k); return it == merged.end() || it->second.empty() ? def : std::atof(it->second.c_str());
    }
    double num2(const std::string &a, const std::string &b, double def) const { return has(a) ? num(a, def) : num(b, def); }
};

void read_env(const Scope &s, const std::string &p, const std::string &alt, Envelope &e, bool amp) {
    auto g = [&](const char *n, double d) { return s.num2(p + n, alt.empty() ? p + n : alt + n, d); };
    e.delay = g("delay", 0);
    e.start_level = g("start", 0) / 100.0;
    e.attack = g("attack", amp ? 0.0 : 0.0);
    e.hold = g("hold", 0);
    e.decay = g("decay", 0);
    e.sustain = g("sustain", 100) / 100.0;
    e.release = g("release", amp ? 0.001 : 0.0);
    e.attack_slope = s.num(p + "attack_shape", 0) / 10.0;
    e.decay_slope = s.num(p + "decay_shape", 0) / 10.0;
    e.release_slope = s.num(p + "release_shape", 0) / 10.0;
    e.time_vel_tracking = -s.num(p + "vel2attack", 0) / 10.0;
    for (auto &kv : s.merged) if (kv.first.compare(0, p.size(), p) == 0 || (!alt.empty() && kv.first.compare(0, alt.size(), alt) == 0)) e.set = true;
}

void read_lfo(const Scope &s, const std::string &p, Lfo &l) {
    l.set = true;
    l.wave = LfoWave::Sine;
    l.rate_hz = s.num(p + "freq", 5);
    l.delay = s.num(p + "delay", 0);
    l.fade_in = s.num(p + "fade", 0);
}

Instrument load_sfz(VolumePtr vol, const std::string &path, int) {
    std::map<std::string, std::string> defs;
    std::set<std::string> active;
    std::string text = strip_comments(expand(*vol, path, defs, active, 0));
    text = substitute(text, defs);
    std::vector<Header> headers = tokenize(text);

    Instrument inst;
    inst.format = "SFZ";
    inst.name = path_stem(path);
    inst.groups.clear();
    Opcodes control, global, master, group;
    std::string base_dir = path_dir(path);
    int octave_offset = 0, note_offset = 0;
    std::map<std::string, SampleRefPtr> refs;
    int group_index = -1;
    for (auto &h : headers) {
        if (h.name == "control") {
            control = h.ops;
            if (control.count("default_path")) base_dir = path_resolve(path_dir(path), control["default_path"]);
            octave_offset = std::atoi(control["octave_offset"].c_str());
            note_offset = std::atoi(control["note_offset"].c_str());
            continue;
        }
        if (h.name == "global") { global = h.ops; master.clear(); group.clear(); if (global.count("global_label")) inst.name = global["global_label"]; continue; }
        if (h.name == "master") { master = h.ops; group.clear(); continue; }
        if (h.name == "group") {
            group = h.ops;
            Group g;
            g.name = group.count("group_label") ? group["group_label"] : "Group " + std::to_string(inst.groups.size() + 1);
            inst.groups.push_back(g);
            group_index = int(inst.groups.size()) - 1;
            continue;
        }
        if (h.name != "region") continue;
        Scope s;
        for (auto *m : {&global, &master, &group, &h.ops}) for (auto &kv : *m) s.merged[kv.first] = kv.second;
        std::string sample = s.str("sample");
        if (sample.empty() || sample[0] == '*') continue;   // *sine etc. generators are not supported
        if (group_index < 0) { inst.groups.push_back(Group{"Group 1", Trigger::Attack}); group_index = 0; }

        Zone z;
        z.group = group_index;
        std::string sp = path_resolve(base_dir, sample), found;
        if (!find_ci(*vol, sp, found)) { inst.warnings.push_back("missing sample " + sample); continue; }
        auto it = refs.find(found);
        if (it == refs.end()) it = refs.emplace(found, file_sample_ref(vol, found)).first;
        z.sample = it->second;
        z.name = path_stem(found);

        auto key = [&](const char *k, int def) { return s.has(k) ? note_value(s.str(k), octave_offset, note_offset) : def; };
        if (s.has("key")) { int k = key("key", 60); z.key_lo = z.key_hi = z.root = k; }
        z.key_lo = key("lokey", z.key_lo);
        z.key_hi = key("hikey", z.key_hi);
        z.root = key("pitch_keycenter", z.root);
        if (s.has("xfin_lokey") && s.has("xfin_hikey")) z.key_xfade_lo = key("xfin_hikey", 0) - key("xfin_lokey", 0);
        if (s.has("xfout_lokey") && s.has("xfout_hikey")) z.key_xfade_hi = key("xfout_hikey", 0) - key("xfout_lokey", 0);
        z.vel_lo = int(s.num("lovel", 0));
        z.vel_hi = int(s.num("hivel", 127));
        if (s.has("xfin_lovel") && s.has("xfin_hivel")) z.vel_xfade_lo = int(s.num("xfin_hivel", 0) - s.num("xfin_lovel", 0));
        if (s.has("xfout_lovel") && s.has("xfout_hivel")) z.vel_xfade_hi = int(s.num("xfout_hivel", 0) - s.num("xfout_lovel", 0));

        std::string trig = s.str("trigger", "attack");
        z.trigger = trig == "release" || trig == "release_key" ? Trigger::Release : trig == "first" ? Trigger::First
                  : trig == "legato" ? Trigger::Legato : Trigger::Attack;
        if (s.str("direction") == "reverse") z.reverse = true;
        if (s.has("seq_position")) {
            z.play_logic = PlayLogic::RoundRobin;
            z.seq_position = std::max(1, int(s.num("seq_position", 1)));
            z.seq_length = std::max(1, int(s.num("seq_length", 1)));
        } else if (s.has("lorand") || s.has("hirand")) {
            z.play_logic = PlayLogic::Random;
            z.rand_lo = s.num("lorand", 0);
            z.rand_hi = s.num("hirand", 1);
        }
        z.exclusive_group = int(s.num("group", 0));
        z.off_by = int(s.num("off_by", 0));
        z.start = int64_t(s.num("offset", 0));
        if (s.has("end")) z.stop = int64_t(s.num("end", -1)) + 1;

        std::string mode = s.str("loop_mode", s.str("loopmode"));
        if (mode == "one_shot") z.one_shot = true;
        else if (mode == "loop_continuous" || mode == "loop_sustain") {
            Loop l;
            l.until_release = mode == "loop_sustain";
            std::string lt = s.str("loop_type", s.str("looptype"));
            l.type = lt == "backward" ? LoopType::Backward : lt == "alternate" ? LoopType::Alternating : LoopType::Forward;
            l.start = int64_t(s.num2("loop_start", "loopstart", -1));
            l.end = int64_t(s.num2("loop_end", "loopend", -1));
            l.crossfade = int64_t(s.num("loop_crossfade", 0) * 44100);   // seconds, sample rate unknown here
            if (l.start < 0 || l.end < 0) {
                // loop points from the file's smpl chunk, read at load time
                try {
                    AudioInfo ai = probe_audio(*vol->open(found));
                    if (!ai.loops.empty()) { Loop f = ai.loops[0]; if (l.start < 0) l.start = f.start; if (l.end < 0) l.end = f.end; }
                } catch (const std::exception &) {}
            }
            if (l.start >= 0 && l.end > l.start) z.loops.push_back(l);
        } else if (mode.empty()) {
            // SFZ default: loop when the file has loop points
            try {
                AudioInfo ai = probe_audio(*vol->open(found));
                if (!ai.loops.empty()) {
                    Loop l = ai.loops[0];
                    if (s.has("loop_start") || s.has("loopstart")) l.start = int64_t(s.num2("loop_start", "loopstart", 0));
                    if (s.has("loop_end") || s.has("loopend")) l.end = int64_t(s.num2("loop_end", "loopend", 0));
                    z.loops.push_back(l);
                }
                if (!s.has("pitch_keycenter") && !s.has("key") && ai.root >= 0) z.root = ai.root;
            } catch (const std::exception &) {}
        }

        double tune = s.num("tune", s.num("pitch", 0)) + s.num("transpose", 0) * 100.0;
        z.tune = clampd(tune, -3600, 3600) / 100.0;
        z.key_tracking = clampd(s.num("pitch_keytrack", 100), -1200, 1200) / 100.0;
        z.bend_up = int(s.num("bend_up", 200));
        z.bend_down = int(s.num("bend_down", -200));
        z.pitch_env_depth = s.num2("pitcheg_depth", "pitch_depth", 0) / MAX_ENVELOPE_DEPTH;
        if (z.pitch_env_depth != 0) read_env(s, "pitcheg_", "pitch_", z.pitch_env, false);
        double plfo = s.num("pitchlfo_depth", 0);
        if (plfo != 0) { z.pitch_lfo_depth = plfo / MAX_ENVELOPE_DEPTH; read_lfo(s, "pitchlfo_", z.pitch_lfo); }

        z.gain_db = s.num("volume", 0) + s.num("gain", 0);
        z.pan = s.num("pan", 0) / 100.0;
        z.amp_vel_depth = s.num("amp_veltrack", 100) / 100.0;
        z.amp_key_tracking = s.num("amp_keytrack", 0);
        read_env(s, "ampeg_", "", z.amp_env, true);
        double alfo = s.num("amplfo_depth", 0);
        if (alfo != 0) { z.amp_lfo_depth = alfo / MAX_VOLUME_DEPTH; read_lfo(s, "amplfo_", z.amp_lfo); }

        std::string ft = s.str("fil_type", s.str("filtype"));
        if (s.has("cutoff") || !ft.empty()) {
            Filter &f = z.filter;
            std::string t = ft.empty() ? "lpf_2p" : ft;
            f.type = t.compare(0, 3, "hpf") == 0 ? FilterType::HighPass : t.compare(0, 3, "bpf") == 0 ? FilterType::BandPass
                   : t.compare(0, 3, "brf") == 0 ? FilterType::BandReject : FilterType::LowPass;
            f.poles = t.size() >= 5 && isdigit((unsigned char)t[4]) ? t[4] - '0' : 2;
            if (f.poles <= 0) f.poles = 2;
            f.cutoff = s.num("cutoff", MAX_CUTOFF_HZ);
            f.resonance = clampd(s.num("resonance", 0) / MAX_RESONANCE_DB, 0, 1);
            f.env_depth = s.num2("fileg_depth", "fil_depth", 0) / MAX_ENVELOPE_DEPTH;
            if (f.env_depth != 0) read_env(s, "fileg_", "fil_", f.env, false);
            f.vel_depth = s.num("fil_veltrack", 0) / 9600.0;
            f.key_tracking = s.num("fil_keytrack", 0) / 100.0;
            double cc1 = s.has("cutoff_oncc1") ? s.num("cutoff_oncc1", 0) : s.num("cutoff_cc1", 0);
            f.modwheel_depth = cc1 / MAX_ENVELOPE_DEPTH;
            double flfo = s.num("fillfo_depth", 0);
            if (flfo != 0) { f.lfo_depth = flfo / MAX_ENVELOPE_DEPTH; read_lfo(s, "fillfo_", f.lfo); }
        }
        inst.zones.push_back(std::move(z));
        if (s.has("polyphony")) inst.polyphony = int(s.num("polyphony", 0));
    }
    if (inst.groups.empty()) inst.groups.push_back(Group{});
    finish_instrument(inst);
    return inst;
}

std::vector<PresetInfo> list_single(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

}  // namespace

void register_sfz() {
    register_reader({"SFZ", "sfz", nullptr, list_single, load_sfz});
}

}  // namespace omni

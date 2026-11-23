// Omni Sampler 2.4.30: Decent Sampler presets (.dspreset: XML; a .dslibrary is a ZIP of presets + samples, mounted by
// fs/zip.cpp; a .dsbundle is a plain folder). Layout from the Decent Sampler developer's guide 1.21 (in the library
// folder XTRA/Decent Sampler/Microcosm Experiments) and checked on the two libraries there (13 presets).
//
// <DecentSampler> <groups ...> <group ...> <sample .../> </group> </groups> <effects/> <ui/> <tags/> </DecentSampler>
// Attributes cascade: a <sample> takes what it does not set from its <group>, then from <groups> (key / velocity
// ranges, envelope, pan, loop, round robin, trigger, ampVelTrack, pitchKeyTrack, CC ranges). Volumes multiply over
// the three levels (linear 0..1 or "<n>dB"); tuning adds (globalTuning + groupTuning + tuning, semitones).
// sample: path (relative to the preset), rootNote / loNote / hiNote (numbers or names, C3 = 60 [G]), loVel / hiVel,
//   start, end (inclusive frames), loopEnabled, loopStart / loopEnd (inclusive), loopCrossfade (frames), pan -100..100.
// Envelope: attack / decay / release seconds, sustain 0..1; attackCurve / decayCurve / releaseCurve -100..100
//   (defaults -100, 100, 100: a fast attack, exponential decay and release).
// Round robin: seqMode round_robin | random | true_random | always, seqPosition, seqLength (0 / missing = the
//   highest seqPosition used).
// UI controls fire their bindings on load (triggerOnLoad defaults to true), so a knob's value IS the sound: amp
//   ENV_ATTACK / ENV_DECAY / ENV_SUSTAIN / ENV_RELEASE and AMP_VOLUME bindings at instrument level (all groups) or
//   group level (position = group index) are applied, through their linear / table / fixed_value translation; so are
//   effect FX_FILTER_FREQUENCY / _RESONANCE and FX_REVERB_WET_LEVEL / _ROOM_SIZE / _DAMPING bindings at instrument level
//   (the Vocal Synthesizer's Reverb knob says 25 % while its <effect> says 0.2 %).
// Effects (instrument level, and a group's own): lowpass / lowpass_4pl / highpass / bandpass / notch onto the zones'
//   filter (the first filter effect), reverb onto Omni's reverb, gain onto the level.
// <tag polyphony="N"> on a tag every group carries sets the instrument's polyphony (the "Monophonic" vocal synth);
//   silencedByTags chokes on the first named tag.
// Not read: oscillators, modulators (LFOs / envelopes), keyswitch note sequences, glide, buttons and menus, delay /
//   chorus / phaser / convolution effects, legato intervals.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include "../core/audio.hpp"
#include "../core/bytes.hpp"
#include "../core/parse.hpp"
#include "format.hpp"

namespace omni {
namespace {

constexpr uint64_t MAX_PRESET_BYTES = 16u << 20;   // presets are text: a few KB to a few hundred KB
constexpr size_t MAX_ZONES = 8192;

std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A number from an attribute; def when missing or not a number.
bool to_num(const std::string &s, double &out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    char *end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// A MIDI note: a number, or a name (C3 = 60, sharps '#', flats 'b'); -1 when unreadable
int to_note(const std::string &s) {
    std::string t = trim(s);
    if (t.empty()) return -1;
    double v;
    if (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '-') {
        if (!to_num(t, v)) return -1;
        return clampi(int(std::lround(v)), 0, 127);
    }
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
    char c = char(std::toupper(static_cast<unsigned char>(t[0])));
    if (c < 'A' || c > 'G') return -1;
    int n = base[c - 'A'];
    size_t i = 1;
    while (i < t.size() && (t[i] == '#' || t[i] == 'b')) n += t[i++] == '#' ? 1 : -1;
    if (i >= t.size() || !to_num(t.substr(i), v)) return -1;
    return clampi(n + (int(std::lround(v)) + 2) * 12, 0, 127);
}

// "6dB" / "-3.5 dB" or linear (0..16): dB; 0 linear = silent
double to_db(const std::string &s, double def_db = 0) {
    std::string t = trim(s);
    if (t.empty()) return def_db;
    std::string l = lower(t);
    bool db = l.size() >= 2 && l.compare(l.size() - 2, 2, "db") == 0;
    double v;
    if (!to_num(db ? t.substr(0, t.size() - 2) : t, v)) return def_db;
    if (db) return clampd(v, -96, 24);
    if (v <= 0) return -96;
    return clampd(20 * std::log10(v), -96, 24);
}

bool to_bool(const std::string &s, bool def) {
    std::string l = lower(trim(s));
    if (l == "true" || l == "1" || l == "yes" || l == "on") return true;
    if (l == "false" || l == "0" || l == "no" || l == "off") return false;
    return def;
}

// The <sample> -> <group> -> <groups> attribute chain
struct Chain {
    const XmlNode *s = nullptr, *g = nullptr, *gs = nullptr;
    const XmlNode *at(const std::string &n) const {
        if (s && s->has_attr(n)) return s;
        if (g && g->has_attr(n)) return g;
        if (gs && gs->has_attr(n)) return gs;
        return nullptr;
    }
    bool has(const std::string &n) const { return at(n) != nullptr; }
    std::string str(const std::string &n, const std::string &def = "") const {
        const XmlNode *x = at(n);
        return x ? x->attr(n) : def;
    }
    double num(const std::string &n, double def) const {
        double v;
        return to_num(str(n), v) ? v : def;
    }
    int note(const std::string &n, int def) const {
        int v = to_note(str(n));
        return v < 0 ? def : v;
    }
};

// A UI control's value through a binding's translation
double translate(const XmlNode &ctl, const XmlNode &b, double value, bool &ok) {
    ok = true;
    std::string tr = lower(b.attr("translation", "linear"));
    if (tr == "fixed_value") {
        double v;
        ok = to_num(b.attr("translationValue"), v);
        return v;
    }
    if (tr == "table") {
        std::vector<std::pair<double, double>> pts;
        std::string t = b.attr("translationTable");
        size_t p = 0;
        while (p <= t.size() && pts.size() < 256) {
            size_t q = t.find(';', p);
            std::string item = t.substr(p, q == std::string::npos ? std::string::npos : q - p);
            size_t c = item.find(',');
            double x, y;
            if (c != std::string::npos && to_num(item.substr(0, c), x) && to_num(item.substr(c + 1), y)) pts.push_back({x, y});
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (pts.size() < 2) { ok = false; return 0; }
        std::sort(pts.begin(), pts.end());
        if (value <= pts.front().first) return pts.front().second;
        for (size_t i = 1; i < pts.size(); i++)
            if (value <= pts[i].first) {
                double w = pts[i].first - pts[i - 1].first;
                double f = w > 0 ? (value - pts[i - 1].first) / w : 1;
                return pts[i - 1].second + f * (pts[i].second - pts[i - 1].second);
            }
        return pts.back().second;
    }
    // linear: the control's range onto the output range when one is given, else the value itself
    if (!b.has_attr("translationOutputMin") && !b.has_attr("translationOutputMax")) return value;
    double lo = ctl.attr_num("minValue", 0), hi = ctl.attr_num("maxValue", 1);
    double out_lo = b.attr_num("translationOutputMin", 0), out_hi = b.attr_num("translationOutputMax", 1);
    double f = hi > lo ? clampd((value - lo) / (hi - lo), 0, 1) : 0;
    if (to_bool(b.attr("translationReversed"), false)) f = 1 - f;
    return out_lo + f * (out_hi - out_lo);
}

// Envelope / volume values the UI sets on load: per group (index), -1 = every group
struct Overrides {
    std::map<int, std::map<std::string, double>> by_group;   // parameter -> value
    double inst_volume_db = 0;
    std::map<int, std::map<std::string, double>> effect;      // instrument-level effect index -> parameter -> value
};

void collect_controls(const XmlNode &n, Overrides &o, int depth = 0) {
    if (depth > 16) return;
    for (const auto &cp : n.children) {
        const XmlNode &c = *cp;
        if (c.name == "binding" || c.name == "state" || c.name == "option") continue;   // buttons / menus: not read
        if (c.has_attr("value")) {
            double value;
            if (to_num(c.attr("value"), value))
                for (const XmlNode *b : c.all("binding")) {
                    if (!to_bool(b->attr("enabled"), true) || !to_bool(b->attr("triggerOnLoad"), true)) continue;
                    std::string type = lower(b->attr("type")), level = lower(b->attr("level")), par = b->attr("parameter");
                    if (type == "effect" && level == "instrument" &&
                        (par == "FX_FILTER_FREQUENCY" || par == "FX_FILTER_RESONANCE" || par == "FX_REVERB_WET_LEVEL" ||
                         par == "FX_REVERB_ROOM_SIZE" || par == "FX_REVERB_DAMPING")) {
                        double pos;
                        if (!to_num(b->attr("position", b->attr("effectIndex")), pos) || pos < 0 || pos > 256) continue;
                        bool ok;
                        double v = translate(c, *b, value, ok);
                        if (ok && std::isfinite(v)) o.effect[int(pos)][par] = v;
                        continue;
                    }
                    if (type != "amp") continue;
                    if (par != "ENV_ATTACK" && par != "ENV_DECAY" && par != "ENV_SUSTAIN" && par != "ENV_RELEASE" && par != "AMP_VOLUME") continue;
                    bool ok;
                    double v = translate(c, *b, value, ok);
                    if (!ok || !std::isfinite(v)) continue;
                    if (level == "instrument") {
                        if (par == "AMP_VOLUME") o.inst_volume_db = v <= 0 ? -96 : clampd(20 * std::log10(v), -96, 24);
                        else o.by_group[-1][par] = v;
                    } else if (level == "group") {
                        double pos;
                        if (!to_num(b->attr("position", b->attr("groupIndex")), pos) || pos < 0 || pos > 4096) continue;
                        o.by_group[int(pos)][par] = v;
                    }
                }
        }
        collect_controls(c, o, depth + 1);
    }
}

// The first filter effect of an <effects> element onto a filter; reverb / gain into the outputs
struct Fx {
    bool has_filter = false;
    Filter filter;
    double gain_db = 0;
    bool reverb = false;
    double rev_wet = 0, rev_room = 0.7, rev_damp = 0.3;
    std::vector<std::string> skipped;
};

// knobs: the instrument's effects (index = position among its <effect> elements) take the values UI controls set on load
void read_effects(const XmlNode *effects, Fx &fx, const std::map<int, std::map<std::string, double>> *knobs = nullptr) {
    if (!effects) return;
    int index = -1;
    for (const XmlNode *e : effects->all("effect")) {
        index++;
        if (!to_bool(e->attr("enabled"), true)) continue;
        std::string t = lower(e->attr("type"));
        std::map<std::string, double> k;
        if (knobs) {
            auto it = knobs->find(index);
            if (it != knobs->end()) k = it->second;
        }
        auto val = [&](const char *knob, const char *attr, double def) {
            auto it = k.find(knob);
            return it != k.end() ? it->second : e->attr_num(attr, def);
        };
        if (t == "lowpass" || t == "lowpass_4pl" || t == "highpass" || t == "hipass" || t == "bandpass" || t == "notch") {
            if (fx.has_filter) continue;
            double f = clampd(val("FX_FILTER_FREQUENCY", "frequency", 22000), 20, 22000);
            FilterType ft = t == "highpass" || t == "hipass" ? FilterType::HighPass : t == "bandpass" ? FilterType::BandPass
                          : t == "notch" ? FilterType::BandReject : FilterType::LowPass;
            if (ft == FilterType::LowPass && f >= 19900) continue;   // wide open: no filter (saves the CPU)
            if (ft == FilterType::HighPass && f <= 21) continue;
            fx.has_filter = true;
            fx.filter.type = ft;
            fx.filter.poles = t == "lowpass_4pl" ? 4 : 2;
            fx.filter.cutoff = clampd(f, 20, MAX_CUTOFF_HZ);
            double q = val("FX_FILTER_RESONANCE", "resonance", 0.7);   // Q; 0.707 = no peak
            fx.filter.resonance = q > 0.707 ? clampd(20 * std::log10(q / 0.707) / MAX_RESONANCE_DB, 0, 1) : 0;
        } else if (t == "reverb") {
            double wet = clampd(val("FX_REVERB_WET_LEVEL", "wetLevel", 0.5), 0, 1);
            if (wet < 0.01) continue;
            fx.reverb = true;
            fx.rev_wet = wet;
            fx.rev_room = clampd(val("FX_REVERB_ROOM_SIZE", "roomSize", 0.7), 0, 1);
            fx.rev_damp = clampd(val("FX_REVERB_DAMPING", "damping", 0.3), 0, 1);
        } else if (t == "gain") {
            fx.gain_db += to_db(e->attr("level", "0dB"));
        } else if (!t.empty()) {
            if (std::find(fx.skipped.begin(), fx.skipped.end(), t) == fx.skipped.end()) fx.skipped.push_back(t);
        }
    }
}

std::vector<std::string> split_tags(const std::string &s) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find(',', p);
        std::string t = trim(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (!t.empty()) out.push_back(t);
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return out;
}

std::unique_ptr<XmlNode> read_preset(Volume &vol, const std::string &path) {
    BlobPtr f = vol.open(path);
    if (f->size() > MAX_PRESET_BYTES) throw ParseError("Decent Sampler: preset too large");
    std::vector<uint8_t> b = f->bytes(0, size_t(f->size()));
    std::string text(b.begin(), b.end());
    std::unique_ptr<XmlNode> root = parse_xml(text);
    if (!root || root->name != "DecentSampler") throw ParseError("not a Decent Sampler preset");
    return root;
}

std::vector<PresetInfo> list_ds(VolumePtr vol, const std::string &path) {
    read_preset(*vol, path);   // refuses what is not a preset, so the browser shows the reason
    return {{path_stem(path), 0}};
}

bool probe_ds(const std::string &lower_name, const uint8_t *h, size_t n, uint64_t) {
    if (!ends_with_ci(lower_name, ".dspreset")) return false;
    std::string head(reinterpret_cast<const char *>(h), n);
    return head.find("<DecentSampler") != std::string::npos || head.find("<decentsampler") != std::string::npos;
}

Instrument load_ds(VolumePtr vol, const std::string &path, int) {
    std::unique_ptr<XmlNode> root = read_preset(*vol, path);
    const XmlNode *groups = root->child("groups");
    if (!groups) throw ParseError("Decent Sampler: the preset has no <groups>");

    Instrument inst;
    inst.format = "Decent Sampler preset";
    inst.name = path_stem(path);
    inst.library = "Decent Sampler";

    Overrides ov;
    if (const XmlNode *ui = root->child("ui")) collect_controls(*ui, ov);
    Fx fx;
    read_effects(root->child("effects"), fx, &ov.effect);
    inst.gain_db = to_db(groups->attr("volume"), 0) + ov.inst_volume_db + fx.gain_db;
    const double global_tune = groups->attr_num("globalTuning", 0);

    // tags: polyphony, choke ids
    std::map<std::string, int> tag_poly, tag_id;
    if (const XmlNode *tags = root->child("tags"))
        for (const XmlNode *t : tags->all("tag")) {
            std::string name = trim(t->attr("name"));
            double p;
            if (!name.empty() && to_num(t->attr("polyphony"), p) && p >= 1) tag_poly[name] = int(p);
        }
    auto id_of = [&](const std::string &t) {
        auto it = tag_id.find(t);
        if (it != tag_id.end()) return it->second;
        int id = int(tag_id.size()) + 1;
        tag_id[t] = id;
        return id;
    };

    const std::string dir = path_dir(path);
    std::map<std::string, SampleRefPtr> refs;
    std::map<std::string, std::string> found_cache;
    size_t missing = 0, oscillators = 0, modulators = 0, sequences = 0;
    std::string first_missing;
    int max_rr_pos = 1;
    std::vector<bool> rr_auto;   // per zone: seqLength left to the engine's auto-detection
    std::vector<std::vector<std::string>> zone_tags;

    if (const XmlNode *m = root->child("modulators")) modulators = m->children.size();
    int gi = -1;
    for (const XmlNode *g : groups->all("group")) {
        gi++;
        if (!to_bool(g->attr("enabled"), true)) continue;
        if (g->child("oscillator")) oscillators++;
        Fx gfx = fx;   // a group's own filter wins over the instrument's
        gfx.has_filter = false;
        read_effects(g->child("effects"), gfx);
        if (!gfx.has_filter && fx.has_filter) { gfx.has_filter = true; gfx.filter = fx.filter; }
        for (const std::string &s : gfx.skipped)
            if (std::find(fx.skipped.begin(), fx.skipped.end(), s) == fx.skipped.end()) fx.skipped.push_back(s);
        const double group_db = to_db(g->attr("volume"), 0) + (gfx.gain_db - fx.gain_db);
        const double group_tune = g->attr_num("groupTuning", 0);
        for (const XmlNode *s : g->all("sample")) {
            if (inst.zones.size() >= MAX_ZONES) throw ParseError("Decent Sampler: too many samples");
            Chain c{s, g, groups};
            if (!to_bool(s->attr("enabled"), true)) continue;
            std::string ref = trim(s->attr("path"));
            if (ref.empty()) continue;
            std::string found;
            auto fc = found_cache.find(ref);
            if (fc != found_cache.end()) found = fc->second;
            else {
                std::string want = path_resolve(dir, ref);
                try { if (!find_ci(*vol, want, found)) found.clear(); } catch (const std::exception &) { found.clear(); }
                found_cache[ref] = found;
            }
            if (found.empty()) {
                if (!missing++) first_missing = ref;
                continue;
            }
            Zone z;
            auto it = refs.find(found);
            z.sample = it != refs.end() ? it->second : (refs[found] = file_sample_ref(vol, found));
            z.name = s->attr("name", path_stem(found));
            z.group = 0;
            z.root = c.note("rootNote", 60);
            z.key_lo = c.note("loNote", 0);
            z.key_hi = c.note("hiNote", 127);
            z.vel_lo = clampi(int(c.num("loVel", 0)), 0, 127);
            z.vel_hi = clampi(int(c.num("hiVel", 127)), 0, 127);
            if (z.key_lo > z.key_hi) std::swap(z.key_lo, z.key_hi);
            if (z.vel_lo > z.vel_hi) std::swap(z.vel_lo, z.vel_hi);
            z.gain_db = clampd(group_db + to_db(s->attr("volume"), 0), -96, 24);
            z.pan = clampd(c.num("pan", 0) / 100.0, -1, 1);
            z.tune = global_tune + group_tune + s->attr_num("tuning", 0);
            z.key_tracking = clampd(c.num("pitchKeyTrack", 1), 0, 1);
            z.amp_vel_depth = clampd(c.num("ampVelTrack", 1), 0, 1);

            std::string trig = lower(c.str("trigger", "attack"));
            z.trigger = trig == "release" ? Trigger::Release : trig == "first" ? Trigger::First
                      : trig == "legato" ? Trigger::Legato : Trigger::Attack;

            // MIDI CC range (loCC64 / hiCC64): the first pair found, sample then group
            for (const XmlNode *x : {s, g})
                if (z.cc_num < 0)
                    for (const auto &a : x->attrs)
                        if (a.first.size() > 4 && a.first.compare(0, 4, "loCC") == 0) {
                            int cc = std::atoi(a.first.c_str() + 4);
                            std::string hi = "hiCC" + a.first.substr(4);
                            if (cc < 0 || cc > 127 || !x->has_attr(hi)) continue;
                            z.cc_num = cc;
                            z.cc_lo = clampi(std::atoi(a.second.c_str()), 0, 127);
                            z.cc_hi = clampi(std::atoi(x->attr(hi).c_str()), 0, 127);
                            break;
                        }

            // sample window and loop
            double v;
            if (to_num(s->attr("start"), v) && v > 0) z.start = int64_t(v);
            if (to_num(s->attr("end"), v) && v > 0) z.stop = int64_t(v) + 1;
            if (z.stop >= 0 && z.stop <= z.start) z.stop = -1;
            bool has_points = s->has_attr("loopStart") || s->has_attr("loopEnd");
            bool loop_on = to_bool(c.str("loopEnabled"), has_points);
            if (loop_on) {
                Loop l;
                l.start = int64_t(std::max(0.0, c.num("loopStart", 0)));
                l.end = int64_t(c.num("loopEnd", -1));
                if (l.end < 0 && z.stop > 0) l.end = z.stop - 1;
                if (l.end < 0) {
                    // loop to the file's end: its length, from the header
                    try {
                        AudioInfo ai = probe_audio(*vol->open(found));
                        if (!has_points && !ai.loops.empty()) l = ai.loops[0];
                        else if (ai.frames > 0) l.end = ai.frames - 1;
                    } catch (const std::exception &) {}
                }
                if (z.stop > 0 && l.end >= z.stop) l.end = z.stop - 1;
                l.crossfade = int64_t(std::max(0.0, c.num("loopCrossfade", 0)));
                if (l.end > l.start) {
                    l.crossfade = std::min<int64_t>(l.crossfade, l.end - l.start);
                    z.loops.push_back(l);
                }
            }

            // envelope: the preset's, then what the UI sets on load (all groups, then this group)
            if (to_bool(c.str("ampEnvEnabled"), true)) {
                Envelope &e = z.amp_env;
                e.set = true;
                e.attack = std::max(0.0, c.num("attack", 0));
                e.decay = std::max(0.0, c.num("decay", 0));
                e.sustain = clampd(c.num("sustain", 1), 0, 1);
                e.release = std::max(0.0, c.num("release", 0.1));
                for (int key : {-1, gi}) {
                    auto o = ov.by_group.find(key);
                    if (o == ov.by_group.end()) continue;
                    for (const auto &kv : o->second) {
                        if (kv.first == "ENV_ATTACK") e.attack = std::max(0.0, kv.second);
                        else if (kv.first == "ENV_DECAY") e.decay = std::max(0.0, kv.second);
                        else if (kv.first == "ENV_SUSTAIN") e.sustain = clampd(kv.second, 0, 1);
                        else if (kv.first == "ENV_RELEASE") e.release = std::max(0.0, kv.second);
                    }
                }
                e.attack = std::min(e.attack, 60.0);
                e.decay = std::min(e.decay, 60.0);
                e.release = std::min(e.release, 60.0);
                e.attack_slope = clampd(c.num("attackCurve", -100) / 100.0, -1, 1);
                e.decay_slope = clampd(-c.num("decayCurve", 100) / 100.0, -1, 1);     // exponential = fast start
                e.release_slope = clampd(-c.num("releaseCurve", 100) / 100.0, -1, 1);
            }

            auto og = ov.by_group.find(gi);   // a group volume knob
            if (og != ov.by_group.end()) {
                auto av = og->second.find("AMP_VOLUME");
                if (av != og->second.end()) z.gain_db = clampd(z.gain_db + (av->second <= 0 ? -96 : 20 * std::log10(av->second)), -96, 24);
            }
            if (gfx.has_filter) z.filter = gfx.filter;

            // round robin
            std::string mode = lower(c.str("seqMode", "always"));
            int pos = std::max(1, int(c.num("seqPosition", 1)));
            int len = int(c.num("seqLength", 0));
            bool auto_len = len <= 0;
            if (mode == "round_robin" || mode == "random" || mode == "true_random") {
                z.play_logic = mode == "round_robin" ? PlayLogic::RoundRobin : PlayLogic::Random;
                z.seq_position = pos;
                z.seq_length = std::max(len, pos);
                max_rr_pos = std::max(max_rr_pos, pos);
            }
            rr_auto.push_back(auto_len);
            if (s->child("note-sequence") || g->child("note-sequence")) sequences++;

            std::vector<std::string> tags = split_tags(c.str("tags"));
            zone_tags.push_back(tags);
            std::vector<std::string> silenced = split_tags(c.str("silencedByTags"));
            if (!silenced.empty()) z.off_by = id_of(silenced[0]);
            inst.zones.push_back(std::move(z));
        }
    }

    // round robin lengths left to auto-detection: the highest position in use; random zones share [lo, hi)
    for (size_t i = 0; i < inst.zones.size(); i++) {
        Zone &z = inst.zones[i];
        if (z.play_logic == PlayLogic::Always) continue;
        if (rr_auto[i]) z.seq_length = std::max(z.seq_position, max_rr_pos);
        if (z.play_logic == PlayLogic::Random) {
            z.rand_lo = double(z.seq_position - 1) / z.seq_length;
            z.rand_hi = double(z.seq_position) / z.seq_length;
            z.seq_position = 1;
            z.seq_length = 1;
        }
    }
    // a zone silenced by a tag: zones carrying that tag choke it (exclusive group = the tag's id)
    for (size_t i = 0; i < inst.zones.size(); i++)
        for (const std::string &t : zone_tags[i]) {
            auto it = tag_id.find(t);
            if (it != tag_id.end()) { inst.zones[i].exclusive_group = it->second; break; }
        }
    // a tagged zone only chokes: it is itself silenced only by what its own silencedByTags names
    for (Zone &z : inst.zones)
        if (z.exclusive_group > 0 && z.off_by < 0) z.off_by = 1 << 30;
    // polyphony: a voice limit on a tag every zone carries (Monophonic presets)
    for (const auto &tp : tag_poly) {
        bool all = !zone_tags.empty();
        for (const auto &tags : zone_tags)
            if (std::find(tags.begin(), tags.end(), tp.first) == tags.end()) { all = false; break; }
        if (all) inst.polyphony = inst.polyphony ? std::min(inst.polyphony, tp.second) : tp.second;
    }

    if (fx.reverb) {
        inst.settings.push_back({"rev_type", 3.0f});   // Hall [G]: Decent Sampler's reverb is a Freeverb-style room
        inst.settings.push_back({"rev_mix", float(fx.rev_wet * 100)});
        inst.settings.push_back({"rev_size", float(fx.rev_room * 100)});
        inst.settings.push_back({"rev_damp", float(fx.rev_damp * 100)});
    }

    if (missing) inst.warnings.push_back("Decent Sampler: " + std::to_string(missing) + " sample(s) not found, e.g. " + first_missing);
    if (oscillators) inst.warnings.push_back("Decent Sampler: oscillators are not played");
    if (modulators) inst.warnings.push_back("Decent Sampler: modulators (LFOs, envelopes) are not read");
    if (sequences) inst.warnings.push_back("Decent Sampler: note sequences are not read");
    if (!fx.skipped.empty()) {
        std::string list;
        for (const std::string &s : fx.skipped) list += (list.empty() ? "" : ", ") + s;
        inst.warnings.push_back("Decent Sampler: effects not read: " + list);
    }
    if (inst.zones.empty()) {
        if (missing) throw ParseError("Decent Sampler: none of the " + std::to_string(missing) + " samples found, e.g. " + first_missing);
        if (oscillators) throw ParseError("Decent Sampler: this preset only uses oscillators (not played)");
        throw ParseError("Decent Sampler: the preset has no samples");
    }
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_decent_sampler() {
    register_reader({"Decent Sampler preset", "dspreset", probe_ds, list_ds, load_ds});
}

}  // namespace omni

// Omni Sampler 2.4.30: Decent Sampler presets (.dspreset: XML; a .dslibrary is a ZIP of presets + samples, mounted by
// fs/zip.cpp; a .dsbundle is a plain folder). Layout from the Decent Sampler developer's guide 1.21.1 (XTRA/Decent
// Sampler/Microcosm Experiments) and the 1.43.3 plugin's vocabulary (reports/decent-ref/).
//
// 2.4.39 (the user: "full mapping"): every Decent Sampler feature is mapped onto Omni; the mapping table, formulas and
// warnings are reports/DECENT_FULL_MAPPING_SPEC.md. In short:
//   - samples: key / velocity / CC ranges, start / end, loops (+ loopDuringRelease), tuning, pan, volume, triggers,
//     round robins, tags (enabled / volume / pan / polyphony / choke), amp envelope (delay, hold, curves, ampEnvEnabled),
//     reversed, MIDI channel, output and bus routing (flattened onto the main output).
//   - the UI as saved: knobs, menus, buttons, multi-state controls, XY pads and VALUE chains apply their bindings once on
//     load (group / tag ENABLED, amp, effect, modulator and general parameters), addressed by position, index or tags.
//   - effects: filters onto the zones (lowpass / 1pl / 4pl, highpass, bandpass, notch; filter envelope), and onto Omni's
//     FX: reverb, delay, chorus, phaser, EQ (peak / shelves), bit crusher, wave shaper / folder, compressor, limiter,
//     gain, convolution (as reverb), pitch shift (fully wet), stereo simulator (adt as chorus).
//   - modulators (LFO, envelope, MIDI CC, velocity, MPE pressure, random) and <midi> velocity / CC bindings: each zone's
//     own LFOs / envelopes first (exact Hz and seconds), then Omni's LFO 1 / 2 and the 8-slot MOD matrix by priority.
//   - oscillators (sine, saw, square, triangle, noise, pluck, wavetable, FM, harmonic) onto the SYNTH engine
//     (synth_patch.hpp), glide, voice mode, pitch-bend range.
//   - not loadable by design: the arpeggiator / note sequences (Omni's ARP belongs to the project), the velocity
//     preprocessor (a project setting), microtuning, granular, formants, slices, legato transition samples: each one
//     named in a warning. Encrypted .dsproduct files: decent_product.cpp.
// [G] = mapped by inference (the guide does not say); approximations are listed in the spec.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include "../core/audio.hpp"
#include "../core/bytes.hpp"
#include "../core/parse.hpp"
#include "format.hpp"
#include "synth_patch.hpp"

namespace omni {
namespace {

constexpr uint64_t MAX_PRESET_BYTES = 16u << 20;   // presets are text: a few KB to a few hundred KB
constexpr size_t MAX_ZONES = 8192;

std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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

double lin_db(double v) { return v <= 0 ? -96 : clampd(20 * std::log10(v), -96, 24); }

// "6dB" / "-3.5 dB" or linear (0..16): dB; 0 linear = silent
double to_db(const std::string &s, double def_db = 0) {
    std::string t = trim(s);
    if (t.empty()) return def_db;
    std::string l = lower(t);
    bool db = l.size() >= 2 && l.compare(l.size() - 2, 2, "db") == 0;
    double v;
    if (!to_num(db ? t.substr(0, t.size() - 2) : t, v)) return def_db;
    if (db) return clampd(v, -96, 24);
    return lin_db(v);
}

bool to_bool(const std::string &s, bool def) {
    std::string l = lower(trim(s));
    if (l == "true" || l == "1" || l == "yes" || l == "on") return true;
    if (l == "false" || l == "0" || l == "no" || l == "off") return false;
    return def;
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
bool shares_tag(const std::vector<std::string> &a, const std::vector<std::string> &b) {
    for (const auto &x : a) if (std::find(b.begin(), b.end(), x) != b.end()) return true;
    return false;
}

// Omni's knob laws (plugin.cpp lfo_hz / chorus_hz / lofi_hz): the knob value for a frequency
double lfo_knob(double hz) { return clampd(100 * std::log(std::max(hz, 1e-6) / 0.02) / std::log(1000.0), 0, 100); }
double chorus_knob(double hz) { return clampd(100 * std::log(std::max(hz, 1e-6) / 0.05) / std::log(100.0), 0, 100); }
double lofi_knob(double rate) { return clampd(100 * std::log(44100.0 / std::max(rate, 1.0)) / std::log(44.1), 0, 100); }

// ---------------------------------------------------------------------------------------------------------------
// The <sample> -> <group> -> <groups> attribute chain (with the values the UI set on load in front)

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

// ---------------------------------------------------------------------------------------------------------------
// Bindings: what the saved UI applies on load (one evaluated binding = a target, a parameter and a value)

struct Bind {
    std::string type, level, par;   // type / level lower case, parameter as written
    int pos = -1, group = -1;       // position / index; group: a group-level effect's group
    std::vector<std::string> tags;
    std::string ident;
    double v = 0;
    bool num = false;
    std::string text;               // fixed_value words ("true", "saw" ...)
    bool truth() const { return num ? v >= 0.5 : to_bool(text, false); }
};

// A UI control's value through a binding's translation; false when it gives nothing
bool translate(double lo, double hi, const XmlNode &b, double value, Bind &out) {
    std::string tr = lower(b.attr("translation", "linear"));
    if (tr == "fixed_value") {
        std::string t = b.attr("translationValue");
        out.text = t;
        out.num = to_num(t, out.v);
        return true;
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
        if (pts.size() < 2) return false;
        std::sort(pts.begin(), pts.end());
        // [G] a table written for 0..1 on a control with another range reads the normalised value
        if (pts.back().first <= 1.0001 && (lo < 0 || hi > 1.0001) && hi > lo) value = (value - lo) / (hi - lo);
        double y = pts.back().second;
        if (value <= pts.front().first) y = pts.front().second;
        else
            for (size_t i = 1; i < pts.size(); i++)
                if (value <= pts[i].first) {
                    double w = pts[i].first - pts[i - 1].first;
                    double f = w > 0 ? (value - pts[i - 1].first) / w : 1;
                    y = pts[i - 1].second + f * (pts[i].second - pts[i - 1].second);
                    break;
                }
        out.v = y;
        out.num = true;
        return std::isfinite(y);
    }
    // linear: the control's range onto the output range when one is given, else the value itself
    if (!b.has_attr("translationOutputMin") && !b.has_attr("translationOutputMax")) {
        out.v = value;
        out.num = true;
        return true;
    }
    double out_lo = b.attr_num("translationOutputMin", 0), out_hi = b.attr_num("translationOutputMax", 1);
    double f = hi > lo ? clampd((value - lo) / (hi - lo), 0, 1) : 0;
    if (to_bool(b.attr("translationReversed"), false)) f = 1 - f;
    out.v = out_lo + f * (out_hi - out_lo);
    out.num = true;
    return std::isfinite(out.v);
}

void bind_target(const XmlNode &b, Bind &o) {
    o.type = lower(b.attr("type"));
    o.level = lower(b.attr("level"));
    o.par = trim(b.attr("parameter"));
    double p;
    for (const char *k : {"position", "groupIndex", "effectIndex", "modulatorIndex", "controlIndex", "busIndex"})
        if (to_num(b.attr(k), p) && p >= 0 && p < 100000) { o.pos = int(p); break; }
    // a group-level effect: the group by groupIndex, the effect by effectIndex / position
    if (o.type == "effect" && o.level == "group") {
        double gi, ei;
        if (to_num(b.attr("groupIndex"), gi) && gi >= 0 && gi < 100000) o.group = int(gi);
        if (to_num(b.attr("effectIndex"), ei) && ei >= 0 && ei < 100000) o.pos = int(ei);
        else if (o.group >= 0 && to_num(b.attr("position"), ei) && ei >= 0 && ei < 100000) o.pos = int(ei);
    }
    o.tags = split_tags(b.attr("tags"));
    o.ident = trim(b.attr("identifier"));
}

struct Control {
    const XmlNode *n = nullptr;
    std::string kind;               // knob, button, menu, multi, xy
    double value = 0, lo = 0, hi = 1, y = 0;
    std::vector<std::string> tags;
    int tab = 0, pos = 0;           // its tab and its place among the tab's children
};

void collect_ui(const XmlNode &n, std::vector<Control> &out, int tab, int depth) {
    if (depth > 16) return;
    int pos = 0;
    for (const auto &cp : n.children) {
        const XmlNode &c = *cp;
        const std::string k = lower(c.name);
        if (k == "binding" || k == "state" || k == "option") continue;
        Control ct;
        ct.n = &c;
        ct.tab = tab;
        ct.pos = pos++;
        ct.tags = split_tags(c.attr("tags"));
        if (k == "labeled-knob" || k == "control" || k == "knob" || k == "slider") {
            ct.kind = lower(c.attr("valueType")) == "multi_state" ? "multi" : "knob";
            ct.value = c.attr_num("value", 0);
            ct.lo = c.attr_num("minValue", 0);
            ct.hi = c.attr_num("maxValue", 1);
            std::string snap = lower(c.attr("valueType"));
            if (snap == "integer") ct.value = std::round(ct.value);
            out.push_back(ct);
        } else if (k == "button") {
            ct.kind = "button";
            ct.value = c.attr_num("value", 0);
            out.push_back(ct);
        } else if (k == "menu") {
            ct.kind = "menu";
            ct.value = c.attr_num("value", 1);
            out.push_back(ct);
        } else if (k == "xypad") {
            ct.kind = "xy";
            ct.value = c.attr_num("xValue", 0);
            ct.y = c.attr_num("yValue", 0);
            out.push_back(ct);
        } else if (k == "tab") {
            collect_ui(c, out, tab + 1, depth + 1);
            continue;
        }
        if (!c.children.empty() && k != "labeled-knob" && k != "control" && k != "button" && k != "menu" && k != "xypad")
            collect_ui(c, out, tab, depth + 1);
    }
}

// the bindings a control applies at its value (a button's / menu's selected state, a multi-state control's state)
void control_bindings(const Control &c, std::vector<std::pair<const XmlNode *, double>> &out, double &lo, double &hi) {
    lo = c.lo;
    hi = c.hi;
    auto add_all = [&](const XmlNode *x, double v) {
        if (!x) return;
        for (const XmlNode *b : x->all("binding")) out.push_back({b, v});
    };
    if (c.kind == "knob") add_all(c.n, c.value);
    else if (c.kind == "button" || c.kind == "multi") {
        std::vector<const XmlNode *> st = c.n->all("state");
        int i = int(std::lround(c.value));
        if (i >= 0 && i < int(st.size())) add_all(st[size_t(i)], c.value);
        if (c.kind == "multi") add_all(c.n, c.value);
    } else if (c.kind == "menu") {
        std::vector<const XmlNode *> op = c.n->all("option");
        int i = int(std::lround(c.value)) - 1;   // 1-based
        if (i >= 0 && i < int(op.size())) add_all(op[size_t(i)], c.value);
    } else if (c.kind == "xy") {
        lo = 0;
        hi = 1;
        if (const XmlNode *x = c.n->child("x")) add_all(x, c.value);
        if (const XmlNode *y = c.n->child("y")) add_all(y, c.y);
    }
}

std::vector<Bind> evaluate_ui(const XmlNode *ui, size_t &menus_with_groups) {
    std::vector<Bind> out;
    if (!ui) return out;
    std::vector<Control> ctl;
    collect_ui(*ui, ctl, 0, 0);
    // VALUE chains (a menu option that sets a knob): one level, before everything else
    for (const Control &c : ctl) {
        std::vector<std::pair<const XmlNode *, double>> bs;
        double lo, hi;
        control_bindings(c, bs, lo, hi);
        for (const auto &bv : bs) {
            const XmlNode &b = *bv.first;
            if (lower(b.attr("type")) != "control" || trim(b.attr("parameter")) != "VALUE") continue;
            if (!to_bool(b.attr("enabled"), true) || !to_bool(b.attr("triggerOnLoad"), true)) continue;
            Bind r;
            if (!translate(lo, hi, b, bv.second, r) || !r.num) continue;
            bind_target(b, r);
            for (Control &t : ctl)
                if (&t != &c && ((!r.tags.empty() && shares_tag(r.tags, t.tags)) || (r.tags.empty() && r.pos == t.pos && t.tab == c.tab)))
                    t.value = r.v;
        }
    }
    for (const Control &c : ctl) {
        std::vector<std::pair<const XmlNode *, double>> bs;
        double lo, hi;
        control_bindings(c, bs, lo, hi);
        bool group_switch = false;
        for (const auto &bv : bs) {
            const XmlNode &b = *bv.first;
            if (!to_bool(b.attr("enabled"), true) || !to_bool(b.attr("triggerOnLoad"), true)) continue;
            Bind r;
            if (!translate(lo, hi, b, bv.second, r)) continue;
            bind_target(b, r);
            if (r.type == "control") continue;
            if (r.par == "ENABLED" || r.par == "TAG_ENABLED") group_switch = true;
            out.push_back(std::move(r));
        }
        if (group_switch && (c.kind == "menu" || c.kind == "button" || c.kind == "multi")) menus_with_groups++;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Effects

struct EffectOv { std::map<std::string, Bind> par; };   // parameter -> value set by the UI

struct ZoneFilter {
    bool on = false;
    Filter f;
    double base_hz = 22000;
};

// the instrument-wide FX (Omni has one of each); `merged` counts a second effect of a kind
struct GlobalFx {
    double gain_db = 0, eq_low = 0, eq_mid = 0, eq_high = 0, tune = 0;
    bool reverb = false, delay = false, chorus = false, phaser = false, lofi = false, drive = false, comp = false, limiter = false;
    double rev_wet = 0, rev_room = 0.7, rev_damp = 0.3;
    int rev_type = 3;
    double dly_mix = 0, dly_fb = 0, dly_secs = 0.5, dly_tone = -1;
    int dly_index = -1;
    bool dly_ping = false;
    double cho_mix = 0, cho_depth = 0.2, cho_rate = 0.2;
    double phs_mix = 0, phs_depth = 0.2, phs_rate = 0.2, phs_fb = 0.7;
    double lofi_mix = 1, lofi_bits = 24, lofi_rate = 44100;
    int drive_type = 1;
    double drive_amt = 0, comp_amt = 0;
    std::set<std::string> merged, warn;
};

double ov_num(const XmlNode &e, const EffectOv *ov, const char *knob, const char *attr, double def) {
    if (ov) {
        auto it = ov->par.find(knob);
        if (it != ov->par.end() && it->second.num) return it->second.v;
    }
    return e.attr_num(attr, def);
}
bool ov_enabled(const XmlNode &e, const EffectOv *ov) {
    if (ov) {
        auto it = ov->par.find("ENABLED");
        if (it != ov->par.end()) return it->second.truth();
    }
    return to_bool(e.attr("enabled"), true);
}

// delay seconds onto Omni's tempo divisions (design.py DLY_TIMES: 1/32 .. 1 Bar) at 120 BPM [G: the tempo at load]
int delay_index(double secs) {
    static const double BEATS[12] = {0.125, 1.0 / 6, 0.25, 0.375, 1.0 / 3, 0.5, 0.75, 2.0 / 3, 1.0, 1.5, 2.0, 4.0};
    double beats = std::max(1e-3, secs) * 2.0;   // 120 BPM: 2 beats a second
    int best = 5;
    double err = 1e9;
    for (int i = 0; i < 12; i++) {
        double e = std::fabs(std::log(beats / BEATS[i]));
        if (e < err) { err = e; best = i; }
    }
    return best;
}

double audio_seconds(Volume &vol, const std::string &dir, const std::string &ref) {
    std::string found;
    try {
        if (!find_ci(vol, path_resolve(dir, ref), found)) return -1;
        AudioInfo ai = probe_audio(*vol.open(found));
        return ai.rate > 0 ? double(ai.frames) / ai.rate : -1;
    } catch (const std::exception &) { return -1; }
}

// one <effects> list: its first filter onto `zf` (and its envelope), the rest into `g`
void read_effects(const XmlNode *effects, const std::map<int, EffectOv> &ovs, const std::vector<Bind> &tag_ovs, ZoneFilter &zf,
                  GlobalFx &g, Volume &vol, const std::string &dir, bool group_level) {
    if (!effects) return;
    int index = -1;
    for (const XmlNode *e : effects->all("effect")) {
        index++;
        EffectOv merged;
        auto it = ovs.find(index);
        if (it != ovs.end()) merged = it->second;
        std::vector<std::string> etags = split_tags(e->attr("tags"));
        for (const Bind &b : tag_ovs)
            if (shares_tag(b.tags, etags)) merged.par[b.par] = b;
        const EffectOv *ov = &merged;
        if (!ov_enabled(*e, ov)) continue;
        std::string t = lower(e->attr("type"));
        auto val = [&](const char *knob, const char *attr, double def) { return ov_num(*e, ov, knob, attr, def); };
        if (t == "lowpass" || t == "lowpass_4pl" || t == "lowpass_1pl" || t == "highpass" || t == "hipass" || t == "bandpass" || t == "notch") {
            if (zf.on) { g.merged.insert("filter"); continue; }
            double f = clampd(val("FX_FILTER_FREQUENCY", "frequency", t == "notch" ? 10000 : 22000), 20, 22000);
            FilterType ft = t == "highpass" || t == "hipass" ? FilterType::HighPass : t == "bandpass" ? FilterType::BandPass
                          : t == "notch" ? FilterType::BandReject : FilterType::LowPass;
            const bool env = e->has_attr("envelope_amount") || e->has_attr("envAmount");
            if (!env && ft == FilterType::LowPass && f >= 19900) continue;   // wide open: no filter (saves the CPU)
            if (!env && ft == FilterType::HighPass && f <= 21) continue;
            zf.on = true;
            zf.base_hz = f;
            zf.f.type = ft;
            zf.f.poles = t == "lowpass_4pl" ? 4 : t == "lowpass_1pl" ? 1 : 2;
            zf.f.cutoff = clampd(f, 20, MAX_CUTOFF_HZ);
            double q = t == "notch" ? e->attr_num("q", e->attr_num("Q", 0.7)) : val("FX_FILTER_RESONANCE", "resonance", 0.7);
            zf.f.resonance = t != "notch" && t != "lowpass_1pl" && q > 0.707 ? clampd(20 * std::log10(q / 0.707) / MAX_RESONANCE_DB, 0, 1) : 0;
            // the filter's own envelope (guide example: envelope_amount Hz above the cutoff, ADSR in seconds)
            if (env) {
                double amt = e->attr_num("envelope_amount", e->attr_num("envAmount", 0));
                if (amt != 0) {
                    double top = clampd(f + amt, 20, 22000);
                    zf.f.env_depth = clampd(1200 * std::log2(top / f) / MAX_ENVELOPE_DEPTH, -1, 1);
                    zf.f.env.set = true;
                    zf.f.env.attack = std::max(0.0, e->attr_num("envelope_attack", 0));
                    zf.f.env.decay = std::max(0.0, e->attr_num("envelope_decay", 0));
                    zf.f.env.sustain = clampd(e->attr_num("envelope_sustain", 1), 0, 1);
                    zf.f.env.release = std::max(0.0, e->attr_num("envelope_release", 0.1));
                }
            }
        } else if (t == "peak" || t == "low_shelf" || t == "high_shelf" || t == "lowshelf" || t == "highshelf") {
            double gain = val("FX_FILTER_GAIN", "gain", 1);
            double db = std::fabs(gain) > 4 ? gain : lin_db(gain);   // [G] a value above 4 is already dB
            double f = val("FX_FILTER_FREQUENCY", "frequency", 1000);
            double &band = t == "low_shelf" || t == "lowshelf" ? g.eq_low : t == "high_shelf" || t == "highshelf" ? g.eq_high
                         : f < 250 ? g.eq_low : f > 4000 ? g.eq_high : g.eq_mid;
            band = clampd(band + db, -12, 12);
        } else if (t == "gain") {
            // a bare number is dB (guide); levelUnit="linear" reads 0..8 as a factor (2.4.39: "-6" was read as linear)
            std::string lv = trim(e->attr("level", "0"));
            double v = 0;
            if (lower(e->attr("levelUnit")) == "linear") g.gain_db += to_num(lv, v) ? lin_db(v) : 0;
            else g.gain_db += to_db(ends_with_ci(lv, "db") ? lv : lv + "dB", 0);
        } else if (t == "reverb") {
            double wet = clampd(val("FX_REVERB_WET_LEVEL", "wetLevel", 0.5), 0, 1);
            if (wet < 0.01) continue;
            if (g.reverb) g.merged.insert("reverb");
            if (!g.reverb || wet > g.rev_wet) {
                g.rev_wet = wet;
                g.rev_room = clampd(val("FX_REVERB_ROOM_SIZE", "roomSize", 0.7), 0, 1);
                g.rev_damp = clampd(val("FX_REVERB_DAMPING", "damping", 0.3), 0, 1);
                g.rev_type = 3;
            }
            g.reverb = true;
        } else if (t == "convolution") {
            double mix = clampd(val("FX_MIX", "mix", 0.5), 0, 1);
            if (mix < 0.01) continue;
            double secs = audio_seconds(vol, dir, e->attr("irFile"));
            if (secs >= 0 && secs < 0.08) { g.warn.insert("Decent Sampler: convolution (cabinet / amp IR) is not supported"); continue; }
            g.warn.insert("Decent Sampler: convolution reverb replaced by Omni's algorithmic reverb");
            if (g.reverb) g.merged.insert("reverb");
            if (!g.reverb || mix > g.rev_wet) {
                g.rev_wet = mix;
                g.rev_type = secs < 0 ? 3 : secs < 0.8 ? 4 : secs < 2 ? 2 : 3;   // Room / Plate / Hall
                g.rev_room = secs < 0 ? 0.7 : clampd(secs / 6, 0, 1);
                g.rev_damp = 0.3;
            }
            g.reverb = true;
        } else if (t == "delay") {
            double wet = clampd(val("FX_WET_LEVEL", "wetLevel", val("FX_MIX", "dryWet", 0.5)), 0, 1);
            if (wet < 0.01) continue;
            if (g.delay) g.merged.insert("delay");
            g.delay = true;
            g.dly_mix = std::max(g.dly_mix, wet);
            g.dly_fb = clampd(val("FX_FEEDBACK", "feedback", 0.2), 0, 1);
            std::string fmt = lower(e->attr("delayTimeFormat", e->attr("delayUnit", "seconds")));
            double dt = val("FX_DELAY_TIME", "delayTime", 0.7);
            if (fmt.find("musical") != std::string::npos) {
                // [G] DS's musical index (0..20, 10 = 1/8) onto Omni's divisions (5 = 1/8)
                g.dly_index = clampi(int(std::lround(dt)) - 5, 0, 11);
            } else {
                g.dly_secs = dt;
                g.dly_index = delay_index(dt);
            }
            g.dly_ping = std::fabs(val("FX_STEREO_OFFSET", "stereoOffset", 0)) > 0.003;
            double fc = val("FX_FEEDBACK_CUTOFF", "feedbackCutoff", -1);
            if (fc > 0) g.dly_tone = clampd(100 * (1 - std::log(clampd(fc, 200, 20000) / 200) / std::log(100.0)), 0, 100);
        } else if (t == "chorus") {
            double mix = clampd(val("FX_MIX", "mix", 0.5), 0, 1);
            if (mix < 0.01) continue;
            if (g.chorus) g.merged.insert("chorus");
            g.chorus = true;
            g.cho_mix = std::max(g.cho_mix, mix);
            g.cho_depth = clampd(val("FX_MOD_DEPTH", "modDepth", 0.2), 0, 1);
            g.cho_rate = clampd(val("FX_MOD_RATE", "modRate", 0.2), 0, 10);
        } else if (t == "phaser") {
            double mix = clampd(val("FX_MIX", "mix", 0.5), 0, 1);
            if (mix < 0.01) continue;
            if (g.phaser) g.merged.insert("phaser");
            g.phaser = true;
            g.phs_mix = std::max(g.phs_mix, mix);
            g.phs_depth = clampd(val("FX_MOD_DEPTH", "modDepth", 0.2), 0, 1);
            g.phs_rate = clampd(val("FX_MOD_RATE", "modRate", 0.2), 0, 10);
            g.phs_fb = clampd(std::fabs(val("FX_FEEDBACK", "feedback", 0.7)), 0, 1);
        } else if (t == "bit_crusher" || t == "bitcrusher") {
            double mix = clampd(val("FX_MIX", "mix", 1), 0, 1);
            double bits = val("FX_BIT_DEPTH", "bitDepth", 24), red = std::max(1.0, val("FX_SAMPLE_RATE_REDUCTION", "sampleRateReduction", 1));
            if (mix < 0.01 || (bits >= 17 && red <= 1.0)) continue;
            if (g.lofi) g.merged.insert("bit crusher");
            g.lofi = true;
            g.lofi_mix = mix;
            g.lofi_bits = clampd(bits, 2, 16);
            g.lofi_rate = 44100.0 / red;
        } else if (t == "wave_shaper" || t == "waveshaper" || t == "wave_folder" || t == "wavefolder") {
            const bool fold = t.find("fold") != std::string::npos;
            double drive = std::max(1.0, val("FX_DRIVE", "drive", 1));
            double amt = fold ? 100 * std::log10(drive) / 2 + (0.25 - val("FX_THRESHOLD", "threshold", 0.25)) * 20
                              : 100 * std::log10(drive) / 3 + 10 * val("FX_DRIVE_BOOST", "driveBoost", 0);
            amt = clampd(amt, 0, 100);
            if (amt < 1) continue;
            if (g.drive) g.merged.insert("drive");
            if (group_level) g.warn.insert("Decent Sampler: drive effect applies to all notes");
            g.drive = true;
            g.drive_type = fold ? 4 : 3;   // Fold / Tube
            g.drive_amt = std::max(g.drive_amt, amt);
        } else if (t == "compressor") {
            double ratio = clampd(val("FX_RATIO", "ratio", 4), 1, 20), thr = std::min(0.0, val("FX_THRESHOLD", "threshold", -12));
            g.comp = true;
            g.comp_amt = std::max(g.comp_amt, clampd((ratio - 1) / 19 * 50 + std::min(1.0, -thr / 40) * 50, 0, 100));
        } else if (t == "limiter") {
            g.limiter = true;
        } else if (t == "pitch_shift" || t == "pitchshift") {
            double mix = clampd(val("FX_MIX", "mix", 0.5), 0, 1);
            if (mix < 0.01) continue;
            if (mix >= 0.95) g.tune += clampd(val("FX_PITCH_SHIFT", "pitchShift", 0), -24, 24);
            else g.warn.insert("Decent Sampler: partly-mixed pitch shifter is not supported");
        } else if (t == "stereo_simulator" || t == "stereosimulator") {
            std::string alg = lower(e->attr("algorithm", "adt"));
            if (alg != "adt") { g.warn.insert("Decent Sampler: stereo simulator (lauridsen / schroeder) is not supported"); continue; }
            if (g.chorus) { g.merged.insert("chorus"); continue; }
            g.chorus = true;
            g.cho_mix = clampd(val("FX_WIDTH", "width", 1) * 0.35, 0, 1);
            g.cho_depth = clampd(val("FX_MOD_DEPTH", "modDepth", 0.2) * 0.3, 0, 1);
            g.cho_rate = clampd(val("FX_MOD_RATE", "modRate", 0.5), 0, 10);
        } else if (!t.empty()) {
            g.warn.insert("Decent Sampler: effect \"" + t + "\" is not supported");
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Modulators and live bindings -> routes

enum Target { T_NONE, T_PITCH, T_VOLUME, T_PAN, T_CUTOFF, T_RES, T_REVERB, T_DRIVE, T_WTPOS };
enum SrcKind { K_LFO, K_ENV, K_VEL, K_CC1, K_CC, K_PRESSURE, K_TIMBRE, K_RANDOM };

struct Route {
    SrcKind kind = K_LFO;
    int mod = -1;                  // its modulator's index (LFO identity)
    Lfo lfo;                       // K_LFO / K_RANDOM periodic
    Envelope env;                  // K_ENV
    Target target = T_NONE;
    std::vector<int> groups;       // -1 = every group
    double depth = 0;              // in the target's units: semitones / dB / pan -1..1 / cents / 0..1
    int order = 0;
};

Target target_of(const std::string &par, const std::string &type) {
    if (par == "GROUP_TUNING" || par == "GLOBAL_TUNING" || par == "GLOBAL_TRANSPOSE" || par == "GROUP_TRANSPOSE") return T_PITCH;
    if (par == "AMP_VOLUME" || par == "GROUP_VOLUME" || par == "GLOBAL_VOLUME" || par == "TAG_VOLUME") return T_VOLUME;
    if (par == "PAN" || par == "GROUP_PAN" || par == "GLOBAL_PAN") return T_PAN;
    if (par == "FX_FILTER_FREQUENCY") return T_CUTOFF;
    if (par == "FX_FILTER_RESONANCE") return T_RES;
    if (par == "FX_REVERB_WET_LEVEL" || (par == "FX_MIX" && type == "effect")) return T_REVERB;
    if (par == "FX_DRIVE") return T_DRIVE;
    if (par == "OSCILLATOR_WAVETABLE_POSITION") return T_WTPOS;
    return T_NONE;
}

// the swing a binding gives at modAmount m, in the target's units (spec 3.3); base_hz = the filter's cutoff
double route_depth(const XmlNode &b, double m, Target t, double base_hz) {
    const bool range = b.has_attr("translationOutputMin") || b.has_attr("translationOutputMax");
    double lo = b.attr_num("translationOutputMin", 0), hi = b.attr_num("translationOutputMax", 1);
    double sign = to_bool(b.attr("translationReversed"), false) ? -1 : 1;
    const bool add = lower(b.attr("modBehavior", "add")) == "add";
    switch (t) {
        case T_PITCH: return range ? sign * m * std::max(std::fabs(lo), std::fabs(hi)) * (add ? 1 : 0.5) : sign * m * 12;
        case T_VOLUME: {
            double d = range ? m * std::fabs(hi - lo) / (2 * std::max(1e-6, std::max(std::fabs(lo), std::fabs(hi)))) : m;
            d = clampd(d, 0, 0.999);
            return sign * -20 * std::log10(1 - d);   // tremolo peak dB
        }
        case T_PAN: return sign * clampd(range ? m * std::fabs(hi - lo) / (std::fabs(hi) > 1.5 ? 200 : 2) : m, 0, 1);
        case T_CUTOFF: {
            double f0 = clampd(base_hz, 20, 22000);
            double a = range ? m * (hi - lo) / (add ? 1 : 2) : m * (22000 - f0);
            double top = clampd(f0 + std::fabs(a), 21, 22000);
            return sign * (a < 0 ? -1 : 1) * 1200 * std::log2(top / f0);   // cents
        }
        case T_RES: return sign * clampd(m, 0, 1);
        case T_REVERB: case T_DRIVE: case T_WTPOS: return sign * clampd(range ? m * std::fabs(hi - lo) : m, 0, 1);
        default: return 0;
    }
}

LfoWave lfo_wave(const std::string &shape) {
    std::string s = lower(shape);
    if (s == "square") return LfoWave::Square;
    if (s == "saw" || s == "sawtooth" || s == "ramp") return LfoWave::SawUp;
    if (s == "triangle") return LfoWave::Triangle;
    if (s == "random" || s == "sample_hold" || s == "s&h") return LfoWave::Random;
    return LfoWave::Sine;
}

std::vector<int> groups_of(const Bind &b, const std::vector<std::vector<std::string>> &group_tags) {
    if (b.level == "instrument" || b.level.empty()) return {-1};
    std::vector<int> out;
    if (!b.tags.empty()) {
        for (size_t i = 0; i < group_tags.size(); i++) if (shares_tag(b.tags, group_tags[i])) out.push_back(int(i));
    } else if (b.pos >= 0) out.push_back(b.pos);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------

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

SynthPatch::Wave osc_wave(const std::string &w) {
    if (w == "saw" || w == "sawtooth") return SynthPatch::SAW;
    if (w == "square") return SynthPatch::SQUARE;
    if (w == "triangle" || w == "pluck1") return SynthPatch::TRIANGLE;
    return SynthPatch::SINE;
}

Instrument load_ds(VolumePtr vol, const std::string &path, int) {
    std::unique_ptr<XmlNode> root = read_preset(*vol, path);
    const XmlNode *groups = root->child("groups");
    if (!groups) throw ParseError("Decent Sampler: the preset has no <groups>");
    const std::string dir = path_dir(path);

    Instrument inst;
    inst.format = "Decent Sampler preset";
    inst.name = path_stem(path);
    inst.library = "Decent Sampler";
    std::set<std::string> warn;

    const std::vector<const XmlNode *> gl = groups->all("group");
    std::vector<std::vector<std::string>> group_tags;
    for (const XmlNode *g : gl) group_tags.push_back(split_tags(g->attr("tags", groups->attr("tags"))));

    // ---- the UI as saved, applied once
    size_t switching = 0;
    std::vector<Bind> binds = evaluate_ui(root->child("ui"), switching);
    std::map<int, bool> group_on;
    std::map<std::string, bool> tag_on;
    std::map<std::string, double> tag_vol_lin, tag_pan;
    std::map<std::string, int> tag_poly_ui;
    std::map<int, std::map<std::string, Bind>> amp_ov;            // group (-1 every group) -> parameter -> value
    std::map<int, EffectOv> inst_fx_ov;                            // instrument effect index -> overrides
    std::map<std::pair<int, int>, EffectOv> group_fx_ov;           // (group, effect index) -> overrides
    std::vector<Bind> fx_tag_ov;                                   // effects addressed by tags
    std::map<int, std::map<std::string, Bind>> mod_ov;             // modulator index -> overrides
    std::vector<Bind> mod_tag_ov;
    bool arp_ui = false;
    double inst_vol_db = 0;
    for (const Bind &b : binds) {
        if (b.par == "ENABLED" && (b.type == "general" || b.type == "amp") && b.level == "group") {
            for (int gi : groups_of(b, group_tags)) group_on[gi] = b.truth();
        } else if (b.par == "TAG_ENABLED" || (b.par == "ENABLED" && b.level == "tag")) {
            for (const std::string &t : b.ident.empty() ? b.tags : std::vector<std::string>{b.ident}) tag_on[t] = b.truth();
        } else if (b.par == "TAG_VOLUME") {
            if (b.num) for (const std::string &t : b.ident.empty() ? b.tags : std::vector<std::string>{b.ident}) tag_vol_lin[t] = b.v;
        } else if (b.par == "TAG_POLYPHONY") {
            if (b.num) for (const std::string &t : b.ident.empty() ? b.tags : std::vector<std::string>{b.ident}) tag_poly_ui[t] = int(b.v);
        } else if (b.type == "effect") {
            if (!b.tags.empty() && b.pos < 0) fx_tag_ov.push_back(b);
            else if (b.level == "group" && b.group >= 0) group_fx_ov[{b.group, std::max(0, b.pos)}].par[b.par] = b;
            else if (b.pos >= 0) inst_fx_ov[b.pos].par[b.par] = b;
        } else if (b.type == "modulator") {
            if (!b.tags.empty() && b.pos < 0) mod_tag_ov.push_back(b);
            else if (b.pos >= 0) mod_ov[b.pos][b.par] = b;
        } else if (b.par.compare(0, 4, "ARP_") == 0 || b.par.compare(0, 4, "SEQ_") == 0 || b.type == "note_sequence") {
            arp_ui = true;
        } else if ((b.par == "AMP_VOLUME" || b.par == "GLOBAL_VOLUME") && b.level == "instrument") {
            if (b.num) inst_vol_db += lin_db(b.v);
        } else {
            for (int gi : groups_of(b, group_tags)) amp_ov[gi][b.par] = b;
        }
    }
    if (switching) warn.insert("Decent Sampler: UI menus and buttons are applied at their saved position only");
    auto amp = [&](int gi, const char *par, double &v) {   // a group's value set by the UI, else every group's
        for (int k : {gi, -1}) {
            auto a = amp_ov.find(k);
            if (a == amp_ov.end()) continue;
            auto p = a->second.find(par);
            if (p != a->second.end() && p->second.num) { v = p->second.v; return true; }
        }
        return false;
    };

    // ---- effects
    GlobalFx fx;
    ZoneFilter inst_filter;
    read_effects(root->child("effects"), inst_fx_ov, fx_tag_ov, inst_filter, fx, *vol, dir, false);
    if (const XmlNode *buses = root->child("buses")) {   // bus effects: the whole instrument's
        bool any = false;
        for (const XmlNode *bus : buses->all("bus"))
            if (const XmlNode *be = bus->child("effects")) {
                ZoneFilter dummy;
                read_effects(be, {}, {}, dummy, fx, *vol, dir, false);
                if (!be->children.empty()) any = true;
            }
        if (any) warn.insert("Decent Sampler: effects on buses are applied to the whole instrument");
    }

    inst.gain_db = to_db(groups->attr("volume"), 0) + inst_vol_db;
    double global_tune = groups->attr_num("globalTuning", 0) + fx.tune;
    double gv;
    if (amp(-1, "GLOBAL_TUNING", gv)) global_tune = gv + fx.tune;
    if (amp(-1, "GLOBAL_TRANSPOSE", gv)) global_tune += std::round(gv);
    double global_pan = 0;
    if (amp(-1, "GLOBAL_PAN", gv)) global_pan = std::fabs(gv) > 1.0001 ? gv / 100 : gv;

    // ---- tags
    std::map<std::string, int> tag_poly, tag_id;
    if (const XmlNode *tags = root->child("tags"))
        for (const XmlNode *t : tags->all("tag")) {
            std::string name = trim(t->attr("name"));
            if (name.empty()) continue;
            double p;
            if (to_num(t->attr("polyphony"), p) && p >= 1) tag_poly[name] = int(p);
            if (t->has_attr("enabled") && !tag_on.count(name)) tag_on[name] = to_bool(t->attr("enabled"), true);
            if (t->has_attr("volume") && !tag_vol_lin.count(name)) tag_vol_lin[name] = t->attr_num("volume", 1);
            if (t->has_attr("pan")) tag_pan[name] = t->attr_num("pan", 0);
        }
    for (const auto &tp : tag_poly_ui) tag_poly[tp.first] = tp.second;
    auto id_of = [&](const std::string &t) {
        auto it = tag_id.find(t);
        if (it != tag_id.end()) return it->second;
        int id = int(tag_id.size()) + 1;
        tag_id[t] = id;
        return id;
    };

    // ---- buses (routing flattened onto the main output)
    std::vector<double> bus_main;   // a bus's share of the main output
    if (const XmlNode *buses = root->child("buses"))
        for (const XmlNode *bus : buses->all("bus")) {
            double share = 0;
            bool any = false;
            for (int o = 1; o <= 8; o++) {
                std::string tk = "output" + std::to_string(o) + "Target";
                if (!bus->has_attr(tk)) continue;
                any = true;
                if (bus->attr(tk) == "MAIN_OUTPUT") share += bus->attr_num("output" + std::to_string(o) + "Volume", 1);
            }
            bus_main.push_back(bus->attr_num("busVolume", 1) * (any ? share : 1));
        }
    bool routed = false;

    // ---- modulators and live MIDI bindings -> routes
    std::vector<Route> routes;
    size_t unsupported_mods = 0;
    std::set<int> cc_knobs;   // MIDI CCs that move a UI knob live (the knob plays at its saved value)
    // the cutoff a filter route starts from: the addressed group's own first filter, else the instrument's
    auto base_cutoff = [&](int gi) {
        if (gi >= 0 && gi < int(gl.size()))
            if (const XmlNode *ge = gl[size_t(gi)]->child("effects"))
                for (const XmlNode *e : ge->all("effect")) {
                    std::string t = lower(e->attr("type"));
                    if (t.find("pass") != std::string::npos || t == "notch")
                        return clampd(e->attr_num("frequency", t == "notch" ? 10000 : 22000), 20, 22000);
                }
        return inst_filter.on ? inst_filter.base_hz : 22000.0;
    };
    auto add_routes = [&](SrcKind kind, int mi, const XmlNode &m, double m_amount, const Lfo &lfo, const Envelope &env) {
        for (const XmlNode *b : m.all("binding")) {
            if (!to_bool(b->attr("enabled"), true)) continue;
            Bind tb;
            bind_target(*b, tb);
            if (tb.type == "control") {   // a CC that turns a UI knob: the knob's saved value is what loads
                if (kind == K_CC1 || kind == K_CC) cc_knobs.insert(int(m.attr_num("number", -1)));
                continue;
            }
            Target t = target_of(tb.par, tb.type);
            if (t == T_NONE) { unsupported_mods++; continue; }
            double mm = m_amount * b->attr_num("modAmount", 1);
            if (mm == 0) continue;
            Route r;
            r.kind = kind;
            r.mod = mi;
            r.lfo = lfo;
            r.env = env;
            r.target = t;
            r.groups = tb.type == "effect" && tb.level == "group" && tb.group >= 0 ? std::vector<int>{tb.group} : groups_of(tb, group_tags);
            if (r.groups.empty()) r.groups = {-1};
            r.depth = route_depth(*b, mm, t, base_cutoff(r.groups[0]));
            r.order = int(routes.size());
            if (r.depth != 0) routes.push_back(r);
        }
    };
    if (const XmlNode *mods = root->child("modulators")) {
        int mi = -1;
        for (const auto &cp : mods->children) {
            const XmlNode &m = *cp;
            mi++;
            std::map<std::string, Bind> ov;
            auto mo = mod_ov.find(mi);
            if (mo != mod_ov.end()) ov = mo->second;
            std::vector<std::string> mtags = split_tags(m.attr("tags"));
            for (const Bind &b : mod_tag_ov) if (shares_tag(b.tags, mtags)) ov[b.par] = b;
            auto mval = [&](const char *knob, const char *attr, double def) {
                auto it = ov.find(knob);
                return it != ov.end() && it->second.num ? it->second.v : m.attr_num(attr, def);
            };
            auto men = ov.find("ENABLED");
            if (men != ov.end() ? !men->second.truth() : !to_bool(m.attr("enabled"), true)) continue;
            const double amount = mval("MOD_AMOUNT", "modAmount", 1);
            const std::string k = lower(m.name);
            Lfo lfo;
            Envelope env;
            if (k == "lfo") {
                lfo.set = true;
                lfo.wave = lfo_wave(m.attr("shape", "sine"));
                lfo.rate_hz = clampd(mval("FREQUENCY", "frequency", mval("LFO_RATE", "rate", 1)), 0.01, 50);
                lfo.delay = std::max(0.0, mval("MOD_DELAY_TIME", "delayTime", 0));
                lfo.key_sync = lower(m.attr("scope", "global")) == "voice";
                add_routes(K_LFO, mi, m, amount, lfo, env);
            } else if (k == "envelope") {
                env.set = true;
                env.attack = std::max(0.0, mval("ENV_ATTACK", "attack", 0));
                env.decay = std::max(0.0, mval("ENV_DECAY", "decay", 0));
                env.sustain = clampd(mval("ENV_SUSTAIN", "sustain", 1), 0, 1);
                env.release = std::max(0.0, mval("ENV_RELEASE", "release", 0.1));
                env.delay = std::max(0.0, mval("MOD_DELAY_TIME", "delayTime", 0));
                env.attack_slope = clampd(m.attr_num("attackCurve", -100) / 100.0, -1, 1);
                env.decay_slope = clampd(-m.attr_num("decayCurve", 100) / 100.0, -1, 1);
                env.release_slope = clampd(-m.attr_num("releaseCurve", 100) / 100.0, -1, 1);
                add_routes(K_ENV, mi, m, amount, lfo, env);
            } else if (k == "midicc") {
                add_routes(int(m.attr_num("number", -1)) == 1 ? K_CC1 : K_CC, mi, m, amount, lfo, env);
            } else if (k == "midivelocity") {
                add_routes(K_VEL, mi, m, amount, lfo, env);
            } else if (k == "mpepressure") {
                add_routes(K_PRESSURE, mi, m, amount, lfo, env);
            } else if (k == "mpetimbre") {
                warn.insert("Decent Sampler: MPE timbre modulation is not supported");
            } else if (k == "random") {
                if (lower(m.attr("mode", "note_on")) == "periodic") {
                    lfo.set = true;
                    lfo.wave = LfoWave::Random;
                    lfo.rate_hz = clampd(mval("FREQUENCY", "frequency", 1), 0.01, 50);
                    add_routes(K_LFO, mi, m, amount, lfo, env);
                } else add_routes(K_RANDOM, mi, m, amount, lfo, env);
            }
        }
    }
    bool keyswitch = false;
    if (const XmlNode *midi = root->child("midi")) {
        if (const XmlNode *v = midi->child("velocity")) add_routes(K_VEL, -1, *v, 1, Lfo(), Envelope());
        for (const XmlNode *cc : midi->all("cc")) add_routes(int(cc->attr_num("number", -1)) == 1 ? K_CC1 : K_CC, -1, *cc, 1, Lfo(), Envelope());
        if (!midi->all("note").empty()) keyswitch = true;
    }
    if (!cc_knobs.empty()) {
        std::string l;
        for (int n : cc_knobs) l += (l.empty() ? "" : ", ") + std::to_string(n);
        warn.insert("Decent Sampler: MIDI CC " + l + " turns UI knobs live; Omni plays them at their saved values");
    }
    if (keyswitch) warn.insert("Decent Sampler: keyswitches are not supported, the saved articulation is used");
    for (const Route &r : routes)
        if (r.kind == K_CC) { warn.insert("Decent Sampler: MIDI CC modulation other than the mod wheel is not supported"); break; }

    // zone-level routes (exact, per zone): the first per zone field; the rest go on to the MOD matrix
    struct ZoneMods { const Route *pitch_lfo = nullptr, *amp_lfo = nullptr, *flt_lfo = nullptr, *pitch_env = nullptr,
                      *flt_env = nullptr, *flt_vel = nullptr, *flt_wheel = nullptr, *amp_vel = nullptr; };
    std::vector<bool> taken(routes.size(), false);
    std::map<int, ZoneMods> zmods;   // group (-1 every group)
    for (size_t i = 0; i < routes.size(); i++) {
        const Route &r = routes[i];
        if (r.kind == K_CC || r.kind == K_PRESSURE || r.kind == K_RANDOM) continue;
        for (int gi : r.groups) {
            ZoneMods &zm = zmods[gi];
            const Route **slot = nullptr;
            if (r.kind == K_LFO) slot = r.target == T_PITCH ? &zm.pitch_lfo : r.target == T_VOLUME ? &zm.amp_lfo : r.target == T_CUTOFF ? &zm.flt_lfo : nullptr;
            else if (r.kind == K_ENV) slot = r.target == T_PITCH ? &zm.pitch_env : r.target == T_CUTOFF ? &zm.flt_env : nullptr;
            else if (r.kind == K_VEL) slot = r.target == T_CUTOFF ? &zm.flt_vel : r.target == T_VOLUME ? &zm.amp_vel : nullptr;
            else if (r.kind == K_CC1) slot = r.target == T_CUTOFF ? &zm.flt_wheel : nullptr;
            if (slot && (!*slot || std::fabs(r.depth) > std::fabs((*slot)->depth))) { *slot = &r; taken[i] = true; }
        }
    }

    // ---- oscillators -> the SYNTH engine
    SynthPatch sp;
    sp.source = "Decent Sampler";
    int oscs = 0;
    bool synth = false;
    auto synth_env = [&](const XmlNode *g) {
        Chain c{nullptr, g, groups};
        sp.amp.attack = std::max(0.001, c.num("attack", 0.002));
        sp.amp.decay = std::max(0.001, c.num("decay", 0.3));
        sp.amp.sustain = clampd(c.num("sustain", 1), 0, 1);
        sp.amp.release = std::max(0.001, c.num("release", 0.1));
        sp.amp_vel = clampd(c.num("ampVelTrack", 1), 0, 1);
    };

    // ---- zones
    std::map<std::string, SampleRefPtr> refs;
    std::map<std::string, std::string> found_cache;
    size_t missing = 0, legato_dropped = 0;
    std::string first_missing;
    int max_rr_pos = 1;
    std::vector<bool> rr_auto;
    std::vector<std::vector<std::string>> zone_tags;
    std::vector<int> zone_group;
    bool sequences = root->child("noteSequences") != nullptr;
    double glide_s = 0, set_wt = -1;
    std::string glide_mode = "legato";
    double bend = -1;

    int gi = -1;
    for (const XmlNode *g : gl) {
        gi++;
        bool on = to_bool(g->attr("enabled"), true);
        auto go = group_on.find(gi);
        if (go != group_on.end()) on = go->second;
        if (!on) continue;
        if (!group_tags[size_t(gi)].empty()) {   // a group whose tags are all disabled
            bool any_on = false;
            for (const std::string &t : group_tags[size_t(gi)]) { auto it = tag_on.find(t); if (it == tag_on.end() || it->second) any_on = true; }
            if (!any_on) continue;
        }
        // the group's effects: its own filter wins over the instrument's
        ZoneFilter gf;
        std::map<int, EffectOv> gov;
        for (const auto &kv : group_fx_ov) if (kv.first.first == gi) gov[kv.first.second] = kv.second;
        double before_gain = fx.gain_db;
        const std::set<std::string> before_merged = fx.merged;
        read_effects(g->child("effects"), gov, fx_tag_ov, gf, fx, *vol, dir, true);
        fx.merged = before_merged;
        const double group_fx_db = fx.gain_db - before_gain;
        fx.gain_db = before_gain;
        if (!gf.on && inst_filter.on) gf = inst_filter;
        double group_db = to_db(g->attr("volume"), 0) + group_fx_db, v;
        if (amp(gi, "GROUP_VOLUME", v) || (amp_ov.count(gi) && amp(gi, "AMP_VOLUME", v))) group_db += lin_db(v);
        double group_tune = g->attr_num("groupTuning", 0);
        if (amp(gi, "GROUP_TUNING", v)) group_tune = v;
        if (amp(gi, "GROUP_TRANSPOSE", v)) group_tune += std::round(v);
        Chain gc{nullptr, g, groups};
        if (gc.num("glideTime", 0) > glide_s) { glide_s = gc.num("glideTime", 0); glide_mode = lower(gc.str("glideMode", "legato")); }
        if (amp(gi, "GLIDE_TIME", v)) glide_s = std::max(glide_s, v);
        if (gc.has("pitchBendRange")) bend = gc.num("pitchBendRange", 2);

        // oscillators: the SYNTH engine (two oscillators, sub, noise)
        for (const XmlNode *o : g->all("oscillator")) {
            synth = true;
            std::string w = lower(o->attr("waveform", "sine"));
            double vol_lin = std::pow(10.0, group_db / 20);
            int semis = int(std::lround(group_tune));
            if (w == "wavetable") {
                // the table file through Omni's own wavetable reader (wav.cpp: band-limited cycles, every key in tune),
                // then this group's level, pan, tuning, keys and envelope; the position = Omni's WT POSITION
                std::string ref = trim(o->attr("wavetableFile")), found;
                bool ok = false;
                try {
                    if (!ref.empty() && find_ci(*vol, path_resolve(dir, ref), found)) {
                        const FormatReader *r = find_reader(*vol, found);
                        if (r) {
                            Instrument wt = r->load(vol, found, 0);
                            Chain oc{o, g, groups};
                            Envelope env;
                            env.set = true;
                            env.attack = std::max(0.0, oc.num("attack", 0.003));
                            env.decay = std::max(0.0, oc.num("decay", 1));
                            env.sustain = clampd(oc.num("sustain", 1), 0, 1);
                            env.release = std::max(0.0, oc.num("release", 0.1));
                            const int klo = oc.note("loNote", 0), khi = oc.note("hiNote", 127);
                            for (Zone z : wt.zones) {
                                if (z.key_hi < klo || z.key_lo > khi) continue;
                                z.key_lo = std::max(z.key_lo, klo);
                                z.key_hi = std::min(z.key_hi, khi);
                                z.gain_db = clampd(z.gain_db + group_db + to_db(o->attr("volume"), 0), -96, 24);
                                z.pan = clampd(oc.num("pan", 0) / 100 + global_pan, -1, 1);
                                z.tune += global_tune + group_tune;
                                z.amp_env = env;
                                z.amp_vel_depth = clampd(oc.num("ampVelTrack", 1), 0, 1);
                                if (inst_filter.on) z.filter = inst_filter.f;
                                inst.zones.push_back(z);
                                rr_auto.push_back(false);
                                zone_tags.push_back({});
                                zone_group.push_back(gi);
                                ok = true;
                            }
                            set_wt = clampd(o->attr_num("wavetablePosition", g->attr_num("wavetablePosition", 0)), 0, 1);
                        }
                    }
                } catch (const std::exception &) { ok = false; }
                if (!ok) warn.insert("Decent Sampler: wavetable file not found or unreadable: " + ref);
                continue;
            }
            if (w == "noise" || w == "white_noise") {
                sp.noise_on = true;
                sp.noise_level = clampd(vol_lin, 0, 1);
            } else if ((w == "sine" || w == "triangle" || w == "square") && (semis == -12 || semis == -24) && oscs > 0 && !sp.sub_on) {
                sp.sub_on = true;
                sp.sub_shape = w == "sine" ? 0 : w == "triangle" ? 1 : 2;
                sp.sub_oct = semis == -24 ? 2 : 1;
                sp.sub_level = clampd(vol_lin, 0, 1);
            } else if (oscs < 2) {
                SynthPatch::Osc &os = sp.osc[oscs++];
                os.on = true;
                os.level = clampd(vol_lin, 0, 1);
                os.pan = clampd(gc.num("pan", 0) / 100, -1, 1);
                os.octave = clampi(semis / 12, -4, 4);
                os.semi = semis - os.octave * 12;
                os.fine = (group_tune - semis) * 100;
                if (w == "fm6op" || g->has_attr("fmAlgorithm")) {
                    os.wave = SynthPatch::SINE;
                    // 2-operator approximation: the strongest modulator (ops 2..6) onto osc B, its level as FM depth
                    double best = 0, ratio = 1;
                    for (int op = 2; op <= 6; op++) {
                        double lv = g->attr_num("fmOp" + std::to_string(op) + "Level", 0);
                        if (lv > best) { best = lv; ratio = g->attr_num("fmOp" + std::to_string(op) + "Ratio", 1); }
                    }
                    if (best > 0 && oscs < 2) {
                        SynthPatch::Osc &mb = sp.osc[oscs++];
                        mb.on = true;
                        mb.wave = SynthPatch::SINE;
                        mb.level = 0;
                        double st = 12 * std::log2(std::max(0.01, ratio));
                        mb.octave = clampi(int(std::floor(st / 12)), -4, 4);
                        mb.semi = clampi(int(std::lround(st - mb.octave * 12)), -12, 12);
                        sp.fm = clampd(best, 0, 1);
                        sp.feedback = clampd(g->attr_num("fmOp6Feedback", g->attr_num("fmFeedback", 0)), 0, 1);
                    }
                    warn.insert("Decent Sampler: FM oscillator approximated by a 2-operator voice");
                } else if (w == "harmonic" || g->has_attr("harmonicPartial1Level")) {
                    // the partials' shape: odd ones only = square, 1/n falling = saw, the fundamental alone = sine
                    double odd = 0, even = 0;
                    for (int k = 2; k <= 64; k++) (k % 2 ? odd : even) += g->attr_num("harmonicPartial" + std::to_string(k) + "Level", 0);
                    os.wave = odd + even < 0.05 ? SynthPatch::SINE : even < 0.2 * odd ? SynthPatch::SQUARE : SynthPatch::SAW;
                    warn.insert("Decent Sampler: additive oscillator played as the nearest classic wave");
                } else {
                    os.wave = osc_wave(w);
                    if (w == "pluck1") {
                        sp.amp.sustain = 0;
                        sp.amp.decay = 0.15 + 1.85 * clampd(o->attr_num("damping", 0.5), 0, 1);
                        warn.insert("Decent Sampler: pluck1 oscillator approximated by a decaying triangle");
                    }
                }
                if (oscs == 1) synth_env(g);
            } else warn.insert("Decent Sampler: only 2 oscillators + sub + noise are played, the rest are dropped");
        }
        if (g->has_attr("formant1Freq") || g->has_attr("formantPreset")) warn.insert("Decent Sampler: formant oscillator is not supported");
        if (g->has_attr("grainSize") || g->has_attr("grainPosition")) warn.insert("Decent Sampler: granular playback is approximated by a looped slice");
        if (g->has_attr("retriggerEnabled") && to_bool(g->attr("retriggerEnabled"), false)) warn.insert("Decent Sampler: retrigger is not supported");

        for (const XmlNode *s : g->all("sample")) {
            if (inst.zones.size() >= MAX_ZONES) throw ParseError("Decent Sampler: too many samples");
            Chain c{s, g, groups};
            if (!to_bool(s->attr("enabled"), true)) continue;
            // legato transition samples: no equivalent (they would all sound together)
            if (c.has("previousNote") || c.has("previousNotes") || c.has("legatoInterval")) { legato_dropped++; continue; }
            std::vector<std::string> tags = split_tags(c.str("tags"));
            if (!tags.empty()) {
                bool any_on = false;
                for (const std::string &t : tags) { auto it = tag_on.find(t); if (it == tag_on.end() || it->second) any_on = true; }
                if (!any_on) continue;
            }
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
            if (amp(gi, "ROOT_NOTE", v)) z.root = clampi(int(v), 0, 127);
            if (amp(gi, "LO_NOTE", v)) z.key_lo = clampi(int(v), 0, 127);
            if (amp(gi, "HI_NOTE", v)) z.key_hi = clampi(int(v), 0, 127);
            if (amp(gi, "LO_VEL", v)) z.vel_lo = clampi(int(v), 0, 127);
            if (amp(gi, "HI_VEL", v)) z.vel_hi = clampi(int(v), 0, 127);
            if (z.key_lo > z.key_hi) std::swap(z.key_lo, z.key_hi);
            if (z.vel_lo > z.vel_hi) std::swap(z.vel_lo, z.vel_hi);
            double tag_db = 0, tpan = 0;
            for (const std::string &t : tags) {
                auto tv = tag_vol_lin.find(t);
                if (tv != tag_vol_lin.end()) tag_db += lin_db(tv->second);
                auto tp = tag_pan.find(t);
                if (tp != tag_pan.end()) tpan += std::fabs(tp->second) > 1.0001 ? tp->second / 100 : tp->second;
            }
            // output routing: main + buses, flattened (aux / no output alone: kept at full level)
            double route_lin = 1;
            bool any_out = false;
            double sum = 0;
            for (int o = 1; o <= 8; o++) {
                std::string tk = "output" + std::to_string(o) + "Target";
                if (!c.has(tk)) continue;
                any_out = true;
                std::string target = c.str(tk);
                double ov = c.num("output" + std::to_string(o) + "Volume", 1);
                if (target == "MAIN_OUTPUT") sum += ov;
                else if (target.compare(0, 4, "BUS_") == 0) {
                    int bi = std::atoi(target.c_str() + 4) - 1;
                    if (bi >= 0 && bi < int(bus_main.size())) sum += ov * bus_main[size_t(bi)];
                    routed = true;
                } else if (target != "NO_OUTPUT") routed = true;
            }
            if (any_out) route_lin = sum > 0 ? sum : 1;
            z.gain_db = clampd(group_db + to_db(s->attr("volume"), 0) + tag_db + lin_db(route_lin), -96, 24);
            double pan = c.num("pan", 0);
            if (amp(gi, "GROUP_PAN", v) || amp(gi, "PAN", v)) pan = std::fabs(v) > 1.0001 ? v : v * 100;
            z.pan = clampd(pan / 100.0 + global_pan + tpan, -1, 1);
            z.tune = global_tune + group_tune + s->attr_num("tuning", 0);
            z.key_tracking = clampd(c.num("pitchKeyTrack", 1), 0, 1);
            if (amp(gi, "PITCH_KEY_TRACK", v)) z.key_tracking = clampd(v, 0, 1);
            z.amp_vel_depth = clampd(c.num("ampVelTrack", 1), 0, 1);
            if (amp(gi, "AMP_VEL_TRACK", v)) z.amp_vel_depth = clampd(v, 0, 1);
            z.reverse = to_bool(c.str("reversed"), false);
            double lo_ch = c.num("loChannel", 0), hi_ch = c.num("hiChannel", 0);
            if (lo_ch >= 1 && lo_ch == hi_ch && lo_ch <= 16) z.midi_channel = int(lo_ch) - 1;
            if (bend >= 0) { z.bend_up = int(std::lround(clampd(bend, 0, 24) * 100)); z.bend_down = -z.bend_up; }

            std::string trig = lower(c.str("trigger", "attack"));
            z.trigger = trig == "release" ? Trigger::Release : trig == "first" ? Trigger::First
                      : trig == "legato" ? Trigger::Legato : Trigger::Attack;   // continuous: as attack
            if (z.trigger == Trigger::Release && c.has("releaseTriggerDecay") && std::fabs(c.num("releaseTriggerDecay", 0)) > 0)
                warn.insert("Decent Sampler: release trigger decay (volume by held time) is not supported");
            if (c.has("loHoldTime") || c.has("hiHoldTime") || c.has("onLoCC64")) warn.insert("Decent Sampler: release samples chosen by held time / pedal are not supported");

            // MIDI CC range (loCC64 / hiCC64): the first pair found, sample then group
            int cc_pairs = 0;
            for (const XmlNode *x : {s, g})
                for (const auto &a : x->attrs)
                    if (a.first.size() > 4 && a.first.compare(0, 4, "loCC") == 0) {
                        int cc = std::atoi(a.first.c_str() + 4);
                        std::string hi = "hiCC" + a.first.substr(4);
                        if (cc < 0 || cc > 127 || !x->has_attr(hi)) continue;
                        if (cc_pairs++ == 0) {
                            z.cc_num = cc;
                            z.cc_lo = clampi(std::atoi(a.second.c_str()), 0, 127);
                            z.cc_hi = clampi(std::atoi(x->attr(hi).c_str()), 0, 127);
                        }
                    }
            if (cc_pairs > 1) warn.insert("Decent Sampler: only the first CC range of a sample is used");

            // sample window and loop
            if (to_num(s->attr("start"), v) && v > 0) z.start = int64_t(v);
            if (to_num(s->attr("end"), v) && v > 0) z.stop = int64_t(v) + 1;
            if (amp(gi, "SAMPLE_START", v) && v >= 0) z.start = int64_t(v);
            if (amp(gi, "SAMPLE_END", v) && v > 0) z.stop = int64_t(v) + 1;
            if (z.stop >= 0 && z.stop <= z.start) z.stop = -1;
            bool has_points = s->has_attr("loopStart") || s->has_attr("loopEnd");
            bool loop_on = to_bool(c.str("loopEnabled"), has_points);
            if (loop_on) {
                Loop l;
                l.start = int64_t(std::max(0.0, c.num("loopStart", 0)));
                l.end = int64_t(c.num("loopEnd", -1));
                if (amp(gi, "LOOP_START", v) && v >= 0) l.start = int64_t(v);
                if (amp(gi, "LOOP_END", v) && v >= 0) l.end = int64_t(v);
                if (l.end < 0 && z.stop > 0) l.end = z.stop - 1;
                if (l.end < 0) {
                    try {
                        AudioInfo ai = probe_audio(*vol->open(found));
                        if (!has_points && !ai.loops.empty()) l = ai.loops[0];
                        else if (ai.frames > 0) l.end = ai.frames - 1;
                    } catch (const std::exception &) {}
                }
                if (z.stop > 0 && l.end >= z.stop) l.end = z.stop - 1;
                l.crossfade = int64_t(std::max(0.0, c.num("loopCrossfade", 0)));
                l.until_release = !to_bool(c.str("loopDuringRelease"), true);   // [G] 1.43: false = play on after the key
                if (l.end > l.start) {
                    l.crossfade = std::min<int64_t>(l.crossfade, l.end - l.start);
                    z.loops.push_back(l);
                }
            }
            // granular: a frozen slice at grainPosition, grainSize long [G]
            if (c.has("grainPosition") || c.has("grainSize")) {
                try {
                    AudioInfo ai = probe_audio(*vol->open(found));
                    if (ai.frames > 0 && ai.rate > 0) {
                        Loop l;
                        l.start = int64_t(clampd(c.num("grainPosition", 0), 0, 1) * double(ai.frames - 1));
                        l.end = std::min<int64_t>(ai.frames - 1, l.start + int64_t(std::max(0.005, c.num("grainSize", 100) / 1000) * ai.rate));
                        l.crossfade = (l.end - l.start) / 4;
                        if (l.end > l.start) { z.start = l.start; z.loops.assign(1, l); }
                    }
                } catch (const std::exception &) {}
            }

            // envelope: the preset's, then what the UI set on load
            bool env_on = to_bool(c.str("ampEnvEnabled"), true);
            if (amp(gi, "AMP_ENV_ENABLED", v)) env_on = v >= 0.5;
            if (env_on) {
                Envelope &e = z.amp_env;
                e.set = true;
                e.attack = std::max(0.0, c.num("attack", c.num("ampEnvAttack", 0)));
                e.decay = std::max(0.0, c.num("decay", c.num("ampEnvDecay", 0)));
                e.sustain = clampd(c.num("sustain", c.num("ampEnvSustain", 1)), 0, 1);
                e.release = std::max(0.0, c.num("release", c.num("ampEnvRelease", 0.1)));
                e.delay = std::max(0.0, c.num("ampEnvDelay", c.num("delay", 0)));
                e.hold = std::max(0.0, c.num("ampEnvHold", c.num("hold", 0)));
                double cv;
                if (amp(gi, "ENV_ATTACK", cv)) e.attack = std::max(0.0, cv);
                if (amp(gi, "ENV_DECAY", cv)) e.decay = std::max(0.0, cv);
                if (amp(gi, "ENV_SUSTAIN", cv)) e.sustain = clampd(cv, 0, 1);
                if (amp(gi, "ENV_RELEASE", cv)) e.release = std::max(0.0, cv);
                if (amp(gi, "ENV_DELAY", cv)) e.delay = std::max(0.0, cv);
                if (amp(gi, "ENV_HOLD", cv)) e.hold = std::max(0.0, cv);
                e.attack = std::min(e.attack, 60.0);
                e.decay = std::min(e.decay, 60.0);
                e.release = std::min(e.release, 60.0);
                e.delay = std::min(e.delay, 60.0);
                e.hold = std::min(e.hold, 60.0);
                double ac = c.num("attackCurve", -100), dc = c.num("decayCurve", 100), rc = c.num("releaseCurve", 100);
                if (amp(gi, "ENV_ATTACK_CURVE", cv)) ac = cv;
                if (amp(gi, "ENV_DECAY_CURVE", cv)) dc = cv;
                if (amp(gi, "ENV_RELEASE_CURVE", cv)) rc = cv;
                e.attack_slope = clampd(ac / 100.0, -1, 1);
                e.decay_slope = clampd(-dc / 100.0, -1, 1);     // exponential = fast start
                e.release_slope = clampd(-rc / 100.0, -1, 1);
            } else z.one_shot = true;   // [G] no amp envelope: the sample plays out

            if (gf.on) z.filter = gf.f;
            // zone-level modulation (exact): this group's routes, then every group's
            for (int key : {gi, -1}) {
                auto zm = zmods.find(key);
                if (zm == zmods.end()) continue;
                const ZoneMods &m = zm->second;
                if (m.pitch_lfo && !z.pitch_lfo.set) { z.pitch_lfo = m.pitch_lfo->lfo; z.pitch_lfo_depth = clampd(m.pitch_lfo->depth * 100 / MAX_ENVELOPE_DEPTH, -1, 1); }
                if (m.amp_lfo && !z.amp_lfo.set) { z.amp_lfo = m.amp_lfo->lfo; z.amp_lfo_depth = clampd(m.amp_lfo->depth / MAX_VOLUME_DEPTH, -1, 1); }
                if (m.pitch_env && !z.pitch_env.set) { z.pitch_env = m.pitch_env->env; z.pitch_env_depth = clampd(m.pitch_env->depth * 100 / MAX_ENVELOPE_DEPTH, -1, 1); }
                if (m.amp_vel) z.amp_vel_depth = clampd(std::max(z.amp_vel_depth, 1 - std::pow(10.0, -std::fabs(m.amp_vel->depth) / 20)), 0, 1);
                if (z.filter.type != FilterType::None) {
                    if (m.flt_lfo && !z.filter.lfo.set) { z.filter.lfo = m.flt_lfo->lfo; z.filter.lfo_depth = clampd(m.flt_lfo->depth / MAX_ENVELOPE_DEPTH, -1, 1); }
                    if (m.flt_env && !z.filter.env.set) { z.filter.env = m.flt_env->env; z.filter.env_depth = clampd(m.flt_env->depth / MAX_ENVELOPE_DEPTH, -1, 1); }
                    if (m.flt_vel && z.filter.vel_depth == 0) z.filter.vel_depth = clampd(m.flt_vel->depth / 9600, -1, 1);
                    if (m.flt_wheel && z.filter.modwheel_depth == 0) z.filter.modwheel_depth = clampd(m.flt_wheel->depth / MAX_ENVELOPE_DEPTH, -1, 1);
                }
            }

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
            if (s->child("note-sequence") || g->child("note-sequence")) sequences = true;

            zone_tags.push_back(tags);
            zone_group.push_back(gi);
            std::vector<std::string> silenced = split_tags(c.str("silencedByTags"));
            if (!silenced.empty()) z.off_by = id_of(silenced[0]);
            if (silenced.size() > 1) warn.insert("Decent Sampler: silencedByTags uses only its first tag");
            if (c.has("slices") || c.has("sliceGrid")) warn.insert("Decent Sampler: sample slicing is not supported");
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
    for (Zone &z : inst.zones)
        if (z.exclusive_group > 0 && z.off_by < 0) z.off_by = 1 << 30;
    // tag polyphony: on every zone = the instrument's voices; 1 on part of them = those zones choke each other
    for (const auto &tp : tag_poly) {
        size_t with = 0;
        for (const auto &tags : zone_tags) if (std::find(tags.begin(), tags.end(), tp.first) != tags.end()) with++;
        if (!with) continue;
        if (with == zone_tags.size()) inst.polyphony = inst.polyphony ? std::min(inst.polyphony, tp.second) : tp.second;
        else if (tp.second == 1) {
            int id = id_of("poly1:" + tp.first);
            for (size_t i = 0; i < inst.zones.size(); i++)
                if (std::find(zone_tags[i].begin(), zone_tags[i].end(), tp.first) != zone_tags[i].end() && inst.zones[i].exclusive_group == 0) {
                    inst.zones[i].exclusive_group = id;
                    inst.zones[i].off_by = id;
                }
        } else warn.insert("Decent Sampler: tag polyphony above 1 on part of the sounds is not limited");
    }

    // ---- settings: voice, glide, bend
    auto set = [&](const char *k, double v) { inst.settings.push_back({k, float(v)}); };
    const bool mono = inst.polyphony == 1;
    if (glide_s > 0 && glide_mode != "off") {
        if (mono) {
            set("voice_mode", glide_mode == "always" ? 1 : 2);   // Mono / Legato
            set("glide", clampd(glide_s * 1000, 0, 2000));
        } else warn.insert("Decent Sampler: glide needs a monophonic preset in Omni (not applied)");
    } else if (mono) set("voice_mode", 1);
    if (bend >= 0) set("bend", clampd(std::round(bend), 0, 24));
    if (set_wt >= 0) set("wt_pos", set_wt * 100);

    // ---- settings: FX
    if (fx.reverb) {
        set("rev_type", fx.rev_type);
        set("rev_mix", fx.rev_wet * 100);
        set("rev_size", fx.rev_room * 100);
        set("rev_damp", fx.rev_damp * 100);
    }
    if (fx.delay) {
        set("dly_type", 1);
        set("dly_mix", fx.dly_mix * 100);
        set("dly_fb", std::min(95.0, fx.dly_fb * 100));
        set("dly_time", fx.dly_index < 0 ? 5 : fx.dly_index);
        set("dly_ping", fx.dly_ping ? 1 : 0);
        if (fx.dly_tone >= 0) set("dly_tone", fx.dly_tone);
    }
    if (fx.chorus) {
        set("cho_type", 1);
        set("cho_mix", fx.cho_mix * 100);
        set("cho_depth", fx.cho_depth * 100);
        set("cho_rate", chorus_knob(fx.cho_rate));
    }
    if (fx.phaser) {
        set("phs_mix", fx.phs_mix * 100);
        set("phs_depth", fx.phs_depth * 100);
        set("phs_rate", chorus_knob(fx.phs_rate));
        set("phs_fb", std::min(90.0, fx.phs_fb * 90));
    }
    if (fx.lofi) {
        set("lofi_mix", fx.lofi_mix * 100);
        set("lofi_bits", fx.lofi_bits);
        set("lofi_rate", lofi_knob(fx.lofi_rate));
    }
    if (fx.drive) {
        set("drive_type", fx.drive_type);
        set("drive", fx.drive_amt);
    }
    if (fx.comp) {
        set("comp_type", 1);
        set("comp", fx.comp_amt);
    }
    if (fx.limiter) set("limiter", 1);
    if (fx.eq_low != 0) set("eq_low", fx.eq_low);
    if (fx.eq_mid != 0) set("eq_mid", fx.eq_mid);
    if (fx.eq_high != 0) set("eq_high", fx.eq_high);
    inst.gain_db += fx.gain_db;
    if (!fx.merged.empty()) warn.insert("Decent Sampler: several effects of one kind were merged into one");
    for (const std::string &w : fx.warn) warn.insert(w);
    if (routed) warn.insert("Decent Sampler: bus and aux outputs are mixed onto the main output");

    // ---- the MOD matrix: routes the zones did not take, by priority (cutoff, pitch, volume; then the rest)
    {
        std::vector<const Route *> rest;
        for (size_t i = 0; i < routes.size(); i++)
            if (!taken[i] && routes[i].kind != K_CC) rest.push_back(&routes[i]);
        auto cls = [](Target t) { return t == T_CUTOFF || t == T_PITCH || t == T_VOLUME ? 0 : t == T_RES || t == T_PAN || t == T_REVERB ? 1 : 2; };
        std::stable_sort(rest.begin(), rest.end(), [&](const Route *a, const Route *b) {
            if (cls(a->target) != cls(b->target)) return cls(a->target) < cls(b->target);
            return std::fabs(a->depth) > std::fabs(b->depth);
        });
        int slot = 0, dropped = 0;
        int lfo_mod[2] = {-2, -2};   // which modulator LFO 1 / LFO 2 play
        for (const Route *r : rest) {
            int src = 0;
            if (r->kind == K_LFO) {
                int li = r->mod == lfo_mod[0] ? 0 : r->mod == lfo_mod[1] ? 1 : lfo_mod[0] == -2 ? 0 : lfo_mod[1] == -2 ? 1 : -1;
                if (li < 0) { dropped++; continue; }
                if (lfo_mod[li] == -2) {
                    lfo_mod[li] = r->mod;
                    static const int WAVES[6] = {0, 1, 4, 2, 3, 5};   // model LfoWave -> design.py LFO_WAVES
                    std::string k = "lfo" + std::to_string(li + 1);
                    set((k + "_wave").c_str(), WAVES[int(r->lfo.wave)]);
                    set((k + "_rate").c_str(), lfo_knob(r->lfo.rate_hz));
                    set((k + "_sync").c_str(), 0);
                    set((k + "_retrig").c_str(), r->lfo.key_sync ? 1 : 0);
                }
                src = 1 + li;
            } else if (r->kind == K_CC1) src = 3;
            else if (r->kind == K_PRESSURE) src = 4;
            else if (r->kind == K_VEL) src = 6;
            else if (r->kind == K_RANDOM) src = 8;
            else if (r->kind == K_ENV) src = 9;   // Amp Env (Omni's matrix has no free envelope) [G]
            int dst = r->target == T_PITCH ? 1 : r->target == T_CUTOFF ? 2 : r->target == T_RES ? 3 : r->target == T_VOLUME ? 4
                    : r->target == T_PAN ? 5 : r->target == T_DRIVE ? 7 : r->target == T_REVERB ? 8 : r->target == T_WTPOS ? 11 : 0;
            if (!dst || !src) { dropped++; continue; }
            if (slot >= 8) { dropped++; continue; }
            // amounts at Omni's full scale (sampler.hpp MOD_*): pitch 12 st, cutoff 4 octaves, volume gain 1 + x
            double amt = r->target == T_PITCH ? r->depth / 12 : r->target == T_CUTOFF ? r->depth / 4800
                       : r->target == T_VOLUME ? -(1 - std::pow(10.0, -std::fabs(r->depth) / 20)) * (r->depth < 0 ? -1 : 1)
                       : r->target == T_RES ? r->depth / 0.5 : r->depth;
            std::string k = "mod" + std::to_string(++slot);
            set((k + "_src").c_str(), src);
            set((k + "_dst").c_str(), dst);
            set((k + "_amt").c_str(), clampd(amt * 100, -100, 100));
        }
        if (dropped) warn.insert("Decent Sampler: " + std::to_string(dropped) + " modulation routes dropped (Omni has 8 MOD slots, 2 LFOs)");
        if (unsupported_mods) warn.insert("Decent Sampler: " + std::to_string(unsupported_mods) + " modulators target things Omni cannot modulate");
    }

    // ---- SYNTH engine
    if (synth) {
        sp.voice_mode = mono ? (glide_mode == "always" ? 1 : 2) : 0;
        sp.glide = clampd(glide_s, 0, 2);
        sp.glide_legato = glide_mode == "legato";
        if (bend >= 0) sp.bend = int(clampd(std::round(bend), 0, 24));
        if (inst_filter.on && inst_filter.f.type == FilterType::LowPass) {
            sp.filter_on = true;
            sp.filter_type = inst_filter.f.poles >= 4 ? SynthPatch::LP24 : SynthPatch::LP12;
            sp.cutoff_hz = inst_filter.f.cutoff;
            sp.resonance = inst_filter.f.resonance;
        }
        for (const auto &kv : synth_settings(sp)) inst.settings.push_back(kv);
    }

    // ---- what cannot be loaded, said once each
    if (legato_dropped) warn.insert("Decent Sampler: legato transition samples (previousNote / legatoInterval) are not played");
    if (sequences || root->child("noteSequences")) warn.insert("Decent Sampler: note sequences are not loaded (Omni's ARP belongs to the project)");
    if (arp_ui || root->find("arpeggiator")) warn.insert("Decent Sampler: arpeggiator settings are not loaded (Omni's ARP belongs to the project)");
    if (root->has_attr("tuningSclFile") || root->find("tuning")) warn.insert("Decent Sampler: microtuning (scl / kbm) is not supported");
    if (missing) warn.insert("Decent Sampler: " + std::to_string(missing) + " sample(s) not found, e.g. " + first_missing);
    for (const std::string &w : warn) inst.warnings.push_back(w);
    if (inst.zones.empty() && !synth) {
        if (missing) throw ParseError("Decent Sampler: none of the " + std::to_string(missing) + " samples found, e.g. " + first_missing);
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

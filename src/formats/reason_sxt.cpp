// Omni Sampler: Reason NN-XT patches (.sxt). Translated from ConvertWithMoss's format/sxt (LGPL-3.0).
//
// An IFF file: FORM "PTCH" holding CAT "REFS" (one REFE per sample: its relative, database and absolute paths and its
// file name), DESC, AUTH, PARM and BODY (the groups, then every zone's parameters, then each zone's sample index).
// Numbers are big-endian; strings are a 32-bit length then Latin-1 (UTF-8 from version 1.3.0, or after a length of
// 0xFFFFFFFF). The samples are separate files (WAV, AIFF, ...) found next to the patch.
//
// Samples inside a ReFill (.rfl) are not read: ReFills are encrypted. ConvertWithMoss refuses such patches; here a patch
// whose ReFill samples also exist as plain files (by the same name, next to the patch or in a folder named after it)
// loads from those files, and samples found nowhere are reported.
#include <cmath>
#include <cstring>
#include <map>
#include "../core/audio.hpp"
#include "../core/bytes.hpp"
#include "format.hpp"
#include "ni_common.hpp"

namespace omni {
namespace {

constexpr int V100 = 1000000, V130 = 1003000, V150 = 1005000, V180 = 1008000, V190 = 1009000, V220 = 2002000,
              V300 = 3000000, V410 = 4001000;

int version(Reader &r) {   // 0xBC, major, minor, revision, one reserved byte
    if (r.u8() != 0xBC) throw ParseError("not an NN-XT patch (bad version tag)");
    int ma = r.u8(), mi = r.u8(), re = r.u8();
    r.skip(1);
    return ma * 1000000 + mi * 1000 + re;
}

std::string latin1_to_utf8(const uint8_t *p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < 0x80) s += char(p[i]);
        else { s += char(0xC0 | p[i] >> 6); s += char(0x80 | (p[i] & 0x3F)); }
    }
    return s;
}

std::string text(Reader &r, bool utf8) {
    uint32_t n = r.u32be();
    if (n == 0xFFFFFFFFu) {   // a proper UTF-8 string
        if (version(r) != V100) throw ParseError("NN-XT: unknown text version");
        uint32_t m = r.u32be();
        const uint8_t *p = r.take(m);
        return std::string(reinterpret_cast<const char *>(p), m);
    }
    const uint8_t *p = r.take(n);
    return utf8 ? std::string(reinterpret_cast<const char *>(p), n) : latin1_to_utf8(p, n);
}

std::string sub_paths(std::string base, Reader &r, bool utf8) {
    uint32_t n = r.u32be();
    for (uint32_t i = 0; i < n && i < 256; i++) base += "/" + text(r, utf8);
    return base;
}

struct SxtPaths { std::string relative, database, absolute; bool refill = false; };

SxtPaths paths(Reader &r, bool utf8) {
    SxtPaths p;
    int v = version(r);   // the relative path: the number of steps up, then the folders and the name
    if (r.u8()) {
        uint32_t up = r.u32be();
        std::string b = up == 0 ? "." : "";
        for (uint32_t i = 0; i < up && i < 64; i++) b += (i ? "/" : "") + std::string("..");
        p.relative = sub_paths(b, r, utf8);
        if (v >= V150) r.skip(1);
    }
    v = version(r);   // the database path: a ReFill's name, then the path inside it
    if (r.u8()) {
        if (!text(r, utf8).empty()) p.refill = true;
        p.database = sub_paths("", r, utf8);
        if (v >= V150) r.skip(1);
    }
    v = version(r);   // the absolute path: the volume, then the folders
    if (r.u8()) {
        std::string vol = text(r, utf8);
        r.skip(1);
        p.absolute = sub_paths(vol.empty() ? "" : vol + ":/", r, utf8);
        if (r.u8()) p.refill = true;
        if (v >= V180) { r.skip(1); uint32_t n = r.u32be(); r.skip(n); }
        if (v >= V190) r.skip(1);
    }
    return p;
}

// One sample reference: every name the patch gives it, best first
struct SxtRef { std::vector<std::string> names; bool refill = false; std::string label; };

SxtRef reference(Reader &r) {
    SxtRef ref;
    int v = version(r);
    bool utf8 = v == V130;
    SxtPaths p = paths(r, utf8);
    std::string ui = text(r, utf8), refill = text(r, utf8), url = text(r, utf8);
    if (!refill.empty() || !url.empty()) ref.refill = true;
    r.skip(1);         // reserved (13)
    text(r, utf8);     // package name
    std::string physical;
    if (v >= V130) { physical = text(r, utf8); SxtPaths q = paths(r, utf8); if (!q.relative.empty()) p = q; }
    ref.refill = ref.refill || p.refill;
    for (const std::string &s : {p.relative, physical, p.absolute, p.database, ui})
        if (!s.empty()) ref.names.push_back(s);
    ref.label = !ui.empty() ? ui : !physical.empty() ? physical : ref.names.empty() ? "?" : path_name(ref.names[0]);
    return ref;
}

struct SxtGroupRec { int poly = 8, mode = 47, mono = 0; };

struct SxtZoneRec {
    uint32_t group = 0;
    int key_lo = 36, key_hi = 96, vel_lo = 1, vel_hi = 127, root = 60, sample_tune = 0;
    uint32_t start = 0, end = 0, loop_start = 0, loop_end = 0;
    int play_mode = 0, fade_in = 0, fade_out = 128, alternate = 0;
    int32_t vel_to_cutoff = 0, vel_to_amp = 0;
    int32_t mod_delay = 0, mod_attack = 0, mod_hold = 0, mod_decay = 0, mod_sustain = 0, mod_release = 0, mod_key_decay = 0,
            mod_to_cutoff = 0, mod_to_pitch = 0;
    int mod_delay_off = 1, mod_attack_off = 0, mod_hold_off = 1;
    int bend = 7, octave = 0, semitone = 0, cent = 0;
    int32_t key_to_pitch = 100;
    int filter_on = 1, filter_type = 61;
    int32_t cutoff = 14100, resonance = 0, key_to_cutoff = 0;
    int32_t amp_delay = 0, amp_attack = 0, amp_hold = 0, amp_decay = 0, amp_sustain = 0, amp_release = 0, amp_key_decay = 0,
            amp_gain = 0;
    int amp_delay_off = 1, amp_attack_off = 0, amp_hold_off = 1;
    int32_t pan = 0;

    void read(Reader &r) {
        group = r.u32be();
        key_lo = r.u8(); key_hi = r.u8(); vel_lo = r.u8(); vel_hi = r.u8(); root = r.u8();
        sample_tune = r.s16be();
        start = r.u32be(); end = r.u32be(); loop_start = r.u32be(); loop_end = r.u32be(); r.u32be();   // the sample's size
        play_mode = r.u8(); r.u8(); fade_in = r.u8(); fade_out = r.u8(); alternate = r.u8();
        for (int i = 0; i < 6; i++) r.s32be();   // modulation wheel to filter, mod env decay, LFO 1, Q, gain, LFO 1 rate
        vel_to_cutoff = r.s32be(); r.s32be(); vel_to_amp = r.s32be(); r.s32be(); r.s32be();   // + mod env decay, attack, start
        r.skip(12);                              // mod wheel / external controller switches
        r.u8(); r.s32be(); r.u8(); r.s32be(); r.u8(); r.u8(); r.s32be(); r.s32be(); r.s32be(); r.u8();   // LFO 1
        r.s32be(); r.s32be(); r.u8(); r.s32be(); r.s32be();                                            // LFO 2
        mod_delay = r.s32be(); mod_delay_off = r.u8(); mod_attack = r.s32be(); mod_attack_off = r.u8();
        mod_hold = r.s32be(); mod_hold_off = r.u8(); mod_decay = r.s32be(); mod_sustain = r.s32be(); mod_release = r.s32be();
        mod_key_decay = r.s32be(); mod_to_cutoff = r.s32be(); mod_to_pitch = r.s32be();
        bend = r.u8(); octave = int8_t(r.u8()); semitone = r.s16be(); cent = r.s16be(); key_to_pitch = r.s32be();
        filter_on = r.u8(); cutoff = r.s32be(); resonance = r.s32be(); filter_type = r.u8(); key_to_cutoff = r.s32be();
        amp_delay = r.s32be(); amp_delay_off = r.u8(); amp_attack = r.s32be(); amp_attack_off = r.u8();
        amp_hold = r.s32be(); amp_hold_off = r.u8(); amp_decay = r.s32be(); amp_sustain = r.s32be(); amp_release = r.s32be();
        amp_key_decay = r.s32be(); amp_gain = r.s32be();
        pan = r.s32be(); r.u8(); r.s32be();   // + spread mode, spread
    }
};

double cents_to_s(int32_t c) { return std::pow(2.0, c / 1200.0); }

Envelope envelope(int32_t delay, int delay_off, int32_t attack, int attack_off, int32_t hold, int hold_off, int32_t decay,
                  int32_t sustain, int32_t release, int32_t key_decay) {
    Envelope e;
    e.set = true;
    if (!delay_off) e.delay = cents_to_s(delay);
    if (!attack_off) e.attack = cents_to_s(attack);
    if (!hold_off) e.hold = cents_to_s(hold);
    e.decay = cents_to_s(decay);
    // the sustain level is on the gain scale (-1440 = silent, 0 = full: ((v + 1440) / 1440)^3), not /1000 as
    // ConvertWithMoss reads it: the NN-XT's default 0 is a full sustain, and Ambiana.sxt stores exactly -1440
    e.sustain = clampd(std::pow(std::max(0.0, (sustain + 1440) / 1440.0), 3), 0, 1);
    e.release = cents_to_s(release);
    e.time_key_tracking = clampd(key_decay / 1000.0, -1, 1);
    return e;
}

// IFF chunk: the id (a FORM / CAT / LIST's own type), its data
struct Chunk { std::string id; Reader data; };
Chunk chunk(Reader &r) {
    Chunk c;
    std::string id(reinterpret_cast<const char *>(r.take(4)), 4);
    uint32_t n = r.u32be();
    if (id == "FORM" || id == "CAT " || id == "LIST") { id.assign(reinterpret_cast<const char *>(r.take(4)), 4); n -= 4; }
    c.id = id;
    c.data = r.sub(std::min<size_t>(n, r.left()));
    if (n % 2 && r.left()) r.skip(1);
    return c;
}

std::string find_sample(VolumePtr vol, const std::string &patch, const SxtRef &ref, NiSampleFinder &finder) {
    std::string dir = path_dir(patch), found;
    for (const std::string &n : ref.names) {   // the stored paths, from the patch's folder
        if (n.find(":/") != std::string::npos) continue;   // a volume path from the author's computer
        if (n[0] != '/' && find_ci(*vol, path_resolve(dir, n), found)) return found;
    }
    for (const std::string &n : ref.names) {   // by name: next to the patch, in a folder named after it
        std::string name = path_name(n);
        if (name.empty()) continue;
        if (find_ci(*vol, path_join(dir, name), found)) return found;
        if (find_ci(*vol, path_join(path_join(dir, path_stem(patch)), name), found)) return found;
    }
    for (const std::string &n : ref.names) { std::string f = finder.find(n); if (!f.empty()) return f; }
    return "";
}

std::vector<PresetInfo> list_sxt(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

Instrument load_sxt(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->all(16 << 20);
    Reader top(d);
    Chunk form = chunk(top);
    if (form.id != "PTCH") throw ParseError("not an NN-XT patch");
    Instrument inst;
    inst.name = path_stem(path);
    inst.format = "Reason NN-XT";
    inst.groups.clear();
    std::vector<SxtRef> refs;
    std::vector<SxtGroupRec> groups;
    std::vector<SxtZoneRec> zones;
    std::vector<int64_t> zone_sample;
    Reader &f = form.data;
    while (f.left() >= 8) {
        Chunk c = chunk(f);
        Reader &r = c.data;
        if (c.id == "REFS") {
            while (r.left() >= 8) { Chunk e = chunk(r); if (e.id == "REFE") refs.push_back(reference(e.data)); }
        } else if (c.id == "DESC") {
            bool utf8 = version(r) == V130;
            r.skip(1);
            std::string name = text(r, utf8);
            if (!trim(name).empty()) inst.name = trim(name);
        } else if (c.id == "BODY") {
            if (version(r) != V100) throw ParseError("NN-XT: unknown body version");
            int gv = version(r);
            uint32_t ng = r.u32be();
            for (uint32_t i = 0; i < ng && i < 1024; i++) {
                SxtGroupRec g;
                g.poly = r.u8(); g.mode = r.u8();
                if (gv >= V300) g.mono = r.u8();
                r.u8(); r.s32be();   // portamento, LFO 1 rate
                groups.push_back(g);
            }
            if (version(r) != V220) throw ParseError("NN-XT: unknown zone version");
            uint32_t nz = r.u32be();
            for (uint32_t i = 0; i < nz && i < 4096; i++) { SxtZoneRec z; z.read(r); zones.push_back(z); }
            for (uint32_t i = 0; i < nz && i < 4096; i++) {
                int64_t s = -1;
                if (r.u8()) {
                    if (version(r) != V410) throw ParseError("NN-XT: unknown sample reference version");
                    r.skip(1);
                    s = r.u32be();
                }
                zone_sample.push_back(s);
            }
        }
    }
    for (size_t i = 0; i < groups.size(); i++) inst.groups.push_back(Group{"Group " + std::to_string(i + 1)});
    if (inst.groups.empty()) inst.groups.push_back(Group{});
    if (!groups.empty()) inst.polyphony = groups[0].mono ? 1 : clampi(groups[0].poly, 1, 99);
    NiSampleFinder finder(vol, path);
    std::map<int64_t, SampleRefPtr> samples;
    std::map<int64_t, bool> tried;
    int missing = 0, in_refill = 0;
    for (size_t i = 0; i < zones.size(); i++) {
        int64_t si = zone_sample[i];
        if (si < 0 || si >= int64_t(refs.size())) continue;
        if (!tried[si]) {
            tried[si] = true;
            std::string file = find_sample(vol, path, refs[size_t(si)], finder);
            if (!file.empty()) samples[si] = file_sample_ref(vol, file);
            else if (refs[size_t(si)].refill) in_refill++;
            else missing++;
        }
        auto it = samples.find(si);
        if (it == samples.end()) continue;
        const SxtZoneRec &s = zones[i];
        Zone z;
        z.name = path_stem(it->second->name.empty() ? refs[size_t(si)].label : it->second->name);
        z.sample = it->second;
        z.group = int(std::min<uint32_t>(s.group, uint32_t(inst.groups.size() - 1)));
        z.key_lo = s.key_lo; z.key_hi = s.key_hi; z.vel_lo = s.vel_lo; z.vel_hi = s.vel_hi; z.root = s.root;
        z.vel_xfade_lo = s.fade_in;
        z.vel_xfade_hi = s.fade_out >= 0x80 ? 0 : 127 - s.fade_out;   // stored inverted; 0x80 = off
        z.start = s.start;
        z.stop = s.end > s.start ? int64_t(s.end) : -1;
        z.tune = s.octave * 12 + s.semitone + s.cent / 100.0 + s.sample_tune / 100.0;
        z.bend_up = s.bend * 100; z.bend_down = -s.bend * 100;
        z.key_tracking = clampd(s.key_to_pitch / 100.0, 0, 1);
        if (s.play_mode == 4) z.reverse = true;
        else if (s.play_mode > 0 && s.loop_end > s.loop_start) {
            Loop l;
            l.type = s.play_mode == 2 ? LoopType::Alternating : LoopType::Forward;
            l.until_release = s.play_mode == 3;
            l.start = s.loop_start; l.end = s.loop_end;
            z.loops.push_back(l);
        }
        Envelope mod = envelope(s.mod_delay, s.mod_delay_off, s.mod_attack, s.mod_attack_off, s.mod_hold, s.mod_hold_off,
                                s.mod_decay, s.mod_sustain, s.mod_release, s.mod_key_decay);
        if (s.mod_to_pitch > 0) { z.pitch_env = mod; z.pitch_env_depth = s.mod_to_pitch / 1000.0; }
        if (s.filter_on) {
            Filter &fl = z.filter;
            fl.poles = 2;
            switch (s.filter_type) {
            case 62: fl.type = FilterType::BandPass; break;
            case 63: fl.type = FilterType::HighPass; break;
            case 64: fl.type = FilterType::BandReject; break;
            case 61: fl.type = FilterType::LowPass; break;
            case 65: fl.type = FilterType::LowPass; fl.poles = 1; break;
            default: fl.type = FilterType::LowPass; fl.poles = 4; break;   // 60: LP24
            }
            fl.cutoff = clampd(440.0 * std::pow(2.0, (s.cutoff - 6900) / 1200.0), 20, MAX_CUTOFF_HZ);
            fl.resonance = clampd(s.resonance / 1000.0, 0, 1);
            if (s.mod_to_cutoff) { fl.env = mod; fl.env_depth = s.mod_to_cutoff / 1000.0; }
            if (s.vel_to_cutoff) fl.vel_depth = s.vel_to_cutoff / 1000.0;
            if (s.key_to_cutoff) fl.key_tracking = clampd(s.key_to_cutoff / 1200.0, 0, 1);
        }
        if (s.vel_to_amp) z.amp_vel_depth = clampd(s.vel_to_amp / 1000.0, 0, 1);
        z.amp_env = envelope(s.amp_delay, s.amp_delay_off, s.amp_attack, s.amp_attack_off, s.amp_hold, s.amp_hold_off, s.amp_decay,
                             s.amp_sustain, s.amp_release, s.amp_key_decay);
        z.gain_db = 20.0 * std::log10(std::pow(std::max(1e-6, (s.amp_gain + 1440) / 1440.0), 3));
        z.pan = clampd(s.pan / 1000.0, -1, 1);
        inst.zones.push_back(z);
    }
    if (in_refill) inst.warnings.push_back(std::to_string(in_refill) + " samples are inside a ReFill (encrypted): put them next to the patch as files");
    if (missing) inst.warnings.push_back(std::to_string(missing) + " samples not found next to the patch");
    finish_instrument(inst);
    if (inst.zones.empty())
        throw ParseError(in_refill ? "this NN-XT patch plays samples from a ReFill (encrypted, not readable)" : "this NN-XT patch's samples are missing");
    return inst;
}

bool probe_sxt(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    return n >= 12 && !std::memcmp(h, "FORM", 4) && !std::memcmp(h + 8, "PTCH", 4);
}

// A ReFill is listed so the browser can say why it does not open
std::vector<PresetInfo> list_refill(VolumePtr, const std::string &) {
    throw ParseError("Reason ReFills (.rfl) are encrypted: export their samples and NN-XT patches with Reason");
}
Instrument load_refill(VolumePtr, const std::string &, int) { throw ParseError("Reason ReFills (.rfl) are encrypted"); }

}  // namespace

void register_reason() {
    register_reader({"Reason NN-XT patch", "sxt", probe_sxt, list_sxt, load_sxt});
    register_reader({"Reason ReFill", "rfl", nullptr, list_refill, load_refill});
}

}  // namespace omni

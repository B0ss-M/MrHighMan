// Omni Sampler: Logic EXS24 / Sampler instruments (.exs). Translated from ConvertWithMoss's format/exs (EXS24Block,
// EXS24Zone, EXS24Group, EXS24Sample, EXS24Parameters, EXS24File, EXS24Detector; LGPL-3.0).
//
// The file is a run of blocks: a 84-byte header (byte 0: 0 big-endian / 1 little-endian, bytes 1-2 the version, byte 3
// the type in its low 4 bits plus flags, a 32-bit content size (bit 15 is a flag when the type has 0x80), the block's
// index, flags, the magic "SOBT"/"SOBJ" (big-endian) or "TBOS"/"JBOS" (little-endian) and a 64-byte name), then the
// content. Types: 0 instrument, 1 zone, 2 group, 3 sample, 4 parameters; the rest (tagged records, Sampler's layout
// plists) carry nothing a player needs. The samples are separate files (WAV, AIFF, CAF), found by the stored path and
// file name next to the instrument, around it, or in the folders below it.
#include <cmath>
#include <cstring>
#include <map>
#include "../core/audio.hpp"
#include "../core/bytes.hpp"
#include "format.hpp"
#include "ni_common.hpp"

namespace omni {
namespace {

enum { T_INSTRUMENT = 0, T_ZONE = 1, T_GROUP = 2, T_SAMPLE = 3, T_PARAMS = 4 };

struct Block {
    bool be = false;
    int type = 0;
    int64_t index = 0;
    std::string name;
    std::vector<uint8_t> content;
};

// little helper over a block's content: missing bytes read as 0 (files vary in how much they store)
struct In {
    const std::vector<uint8_t> &d;
    bool be;
    size_t at(size_t o) const { return o < d.size() ? d[o] : 0; }
    int u8(size_t o) const { return int(at(o)); }
    int s8(size_t o) const { return int(int8_t(at(o))); }
    uint32_t u32(size_t o) const {
        if (o + 4 > d.size()) return 0;
        return be ? be32(&d[o]) : le32(&d[o]);
    }
    std::string str(size_t o, size_t n) const {
        std::string s;
        for (size_t i = o; i < o + n && i < d.size() && d[i]; i++) s += char(d[i]);
        return s;
    }
};

bool is_magic(const uint8_t *m) {
    return !std::memcmp(m, "SOBT", 4) || !std::memcmp(m, "SOBJ", 4) || !std::memcmp(m, "TBOS", 4) || !std::memcmp(m, "JBOS", 4);
}

std::vector<Block> blocks(const std::vector<uint8_t> &d) {
    std::vector<Block> out;
    size_t p = 0;
    while (p + 84 <= d.size()) {
        Block b;
        b.be = d[p] == 0;
        int tf = d[p + 3];
        b.type = tf & 0x0F;
        uint32_t size = b.be ? be32(&d[p + 4]) : le32(&d[p + 4]);
        if (tf & 0x80) size &= 0x7FFF;   // that variant sets bit 15 of the size
        b.index = b.be ? be32(&d[p + 8]) : le32(&d[p + 8]);
        if (!is_magic(&d[p + 16])) {
            if (out.empty()) throw ParseError("not an EXS24 instrument");
            break;
        }
        for (size_t i = p + 20; i < p + 84 && d[i]; i++) b.name += char(d[i]);
        if (p + 84 + size > d.size()) throw ParseError("EXS24: truncated block");
        b.content.assign(d.begin() + long(p + 84), d.begin() + long(p + 84 + size));
        out.push_back(std::move(b));
        p += 84 + size;
    }
    return out;
}

struct ExsZone {
    int64_t id = 0;
    std::string name;
    bool pitch = true, oneshot = false, reverse = false, vel_on = false;
    int key = 60, fine = 0, pan = 0, volume = 0, key_lo = 0, key_hi = 127, vel_lo = 0, vel_hi = 127, coarse = 0;
    int64_t start = 0, end = 0, loop_start = 0, loop_end = 0, loop_xfade = 0;
    int loop_tune = 0, loop_dir = 0;
    bool loop_on = false, loop_to_end = false;
    int64_t group = -1, sample = -1;
};

ExsZone read_zone(const Block &b) {
    In in{b.content, b.be};
    ExsZone z;
    z.id = b.index;
    z.name = b.name;
    int opts = in.u8(0);
    z.pitch = !(opts & 2);
    z.oneshot = opts & 1;
    z.reverse = opts & 4;
    z.vel_on = opts & 8;
    z.key = in.u8(1); z.fine = in.s8(2); z.pan = in.s8(3); z.volume = in.s8(4);
    z.key_lo = in.u8(6); z.key_hi = in.u8(7);
    z.vel_lo = in.u8(9); z.vel_hi = in.u8(10);
    z.start = in.u32(12); z.end = in.u32(16); z.loop_start = in.u32(20); z.loop_end = in.u32(24); z.loop_xfade = in.u32(28);
    z.loop_tune = in.s8(32);
    int lo = in.u8(33);
    z.loop_on = lo & 1;
    z.loop_to_end = lo & 4;
    z.loop_dir = in.u8(34);
    z.coarse = in.s8(80);
    if (b.content.size() >= 96) { z.group = int32_t(in.u32(88)); z.sample = int32_t(in.u32(92)); }
    return z;
}

struct ExsGroup {
    std::string name;
    int volume = 0, pan = 0, exclusive = 0, vel_min = 0, vel_max = 127, random_offset = 0;
    bool release_trigger = false, extended = false, by_round_robin = false;
    int by_cc = -1, cc_lo = 0, cc_hi = 127;   // enabled by a controller (Logic: the pedal layers on CC64)
    int64_t rr_pos = -1;
    int start_note = 0, end_note = 127;
};

ExsGroup read_group(const Block &b) {
    In in{b.content, b.be};
    ExsGroup g;
    g.name = b.name;
    g.volume = in.s8(0); g.pan = in.s8(1);
    g.exclusive = in.u8(4); g.vel_min = in.u8(5); g.vel_max = in.u8(6); g.random_offset = in.u8(7);
    if (g.vel_max == 0) g.vel_max = 127;
    g.release_trigger = in.u8(73) > 0;
    if (b.content.size() >= 90) {   // newer files: round robin and the group's note range
        g.extended = true;
        g.rr_pos = int32_t(in.u32(80));
        g.by_round_robin = in.u8(84) == 2;
        if (in.u8(84) == 3) { g.by_cc = in.u8(85) & 127; g.cc_lo = in.u8(86); g.cc_hi = in.u8(87); }
        g.start_note = in.u8(88); g.end_note = in.u8(89);
        if (g.end_note == 0 && g.start_note == 0) g.end_note = 127;
    }
    return g;
}

struct ExsSample { std::string name, path, file; int rate = 44100; };

ExsSample read_sample(const Block &b) {
    In in{b.content, b.be};
    ExsSample s;
    s.name = b.name;
    s.rate = int(in.u32(8));
    if (s.rate <= 0) s.rate = 44100;
    s.path = in.str(80, 256);
    s.file = b.content.size() > 336 ? in.str(336, 256) : "";
    if (s.file.empty()) s.file = b.name;
    return s;
}

// The parameter block: a count, that many 8-bit IDs, then a 16-bit value for each; then (newer files) a count and
// pairs of 16-bit ID and value for the IDs above 0xFF. Values are signed.
std::map<int, int> read_params(const Block &b) {
    std::map<int, int> p;
    Reader r(b.content.data(), b.content.size());
    auto u32 = [&]() { return b.be ? r.u32be() : r.u32le(); };
    auto s16 = [&]() { return int(b.be ? r.s16be() : r.s16le()); };
    if (r.left() < 4) return p;
    uint32_t n = u32();
    if (n > 4096 || size_t(n) * 3 > r.left()) return p;
    const uint8_t *ids = r.take(n);
    for (uint32_t i = 0; i < n; i++) { int v = s16(); if (ids[i]) p[ids[i]] = v; }
    if (r.left() >= 4) {
        uint32_t m = u32();
        if (m > 0 && size_t(m) * 4 <= r.left())
            for (uint32_t i = 0; i < m; i++) { int id = b.be ? r.u16be() : r.u16le(); int v = s16(); if (id) p[id] = v; }
    }
    return p;
}

// parameter IDs (EXS24Parameters)
enum {
    MASTER_VOLUME = 0x07, PITCH_BEND_UP = 0x03, PITCH_BEND_DOWN = 0x04, POLYPHONY_VOICES = 0x05, VOLUME_KEYSCALE = 0x08, COARSE_TUNE = 0x0e,
    FINE_TUNE = 0x0f, FILTER1_RESO = 0x1d, FILTER1_CUTOFF = 0x1e, FILTER1_TOGGLE = 0x2c, TRANSPOSE = 0x2d,
    FILTER1_KEYTRACK = 0x2e, ENV2_HOLD = 0x38, ENV2_ATK_HI_VEL = 0x4c, ENV2_ATK_LO_VEL = 0x4d, ENV2_DECAY = 0x4e,
    ENV2_SUSTAIN = 0x4f, ENV2_RELEASE = 0x50, ENV1_SUSTAIN = 0x51, ENV1_ATK_HI_VEL = 0x52, ENV1_ATK_LO_VEL = 0x53,
    ENV1_DECAY = 0x54, ENV1_RELEASE = 0x55, ENV1_HOLD = 0x58, ENV1_VEL_SENS = 0x5a, RANDOM_SAMPLE_SEL = 0xa3,
    MOD1_DESTINATION = 0xad, MOD1_SOURCE = 0xae, MOD1_AMOUNT_LOW = 0xb0, FILTER1_TYPE = 0xf3, MOD1_BYPASS = 0xf4,
    ENV2_DELAY_START = 0x16c, ENV1_DELAY_START = 0x16d, ENV2_ATK_CURVE = 0x192, ENV1_ATK_CURVE = 0x195,
};
enum { SRC_CC1 = 1, SRC_VELOCITY = -3, SRC_KEY = -4, SRC_ENV2 = -14, DST_FILTER1_CUTOFF = 8 };   // 0..127: MIDI CCs

struct Mod { int source, destination, amount; };

// the modulation matrix: 10 slots of 6 IDs from MOD1_*, an 11th 178 IDs further on. The bypass switches are
// consecutive IDs from MOD1_BYPASS (ConvertWithMoss steps them by 6 like the others, which reads other parameters).
std::vector<Mod> read_mods(const std::map<int, int> &p) {
    std::vector<Mod> mods;
    for (int i = 0; i < 11; i++) {
        int off = i * 6 + (i == 10 ? 178 : 0);
        auto src = p.find(MOD1_SOURCE + off), dst = p.find(MOD1_DESTINATION + off);
        if (src == p.end() || dst == p.end()) continue;
        if (i < 10) { auto by = p.find(MOD1_BYPASS + i); if (by != p.end() && by->second) continue; }
        auto lo = p.find(MOD1_AMOUNT_LOW + off);
        mods.push_back({int(int16_t(src->second)), int(int16_t(dst->second)), lo == p.end() ? 0 : int(int16_t(lo->second))});
    }
    return mods;
}
const Mod *find_mod(const std::vector<Mod> &mods, int source, int destination) {
    for (auto &m : mods) if (m.source == source && m.destination == destination) return &m;
    return nullptr;
}

int param(const std::map<int, int> &p, int id, int def) { auto it = p.find(id); return it == p.end() ? def : it->second; }
bool has(const std::map<int, int> &p, int id) { return p.count(id) > 0; }

double env_time(const std::map<int, int> &p, int id) {   // 0..127 -> 0..10 s, a 4th-power curve
    if (!has(p, id)) return 0;
    double n = param(p, id, 0) / 127.0;
    return 10.0 * n * n * n * n;
}

// envelope 1 is the amplitude's, 2 the filter's (and the pitch's)
Envelope envelope(const std::map<int, int> &p, int which) {
    bool one = which == 1;
    Envelope e;
    e.set = true;
    e.delay = env_time(p, one ? ENV1_DELAY_START : ENV2_DELAY_START);
    int atk = one ? ENV1_ATK_HI_VEL : ENV2_ATK_HI_VEL, atk_lo = one ? ENV1_ATK_LO_VEL : ENV2_ATK_LO_VEL;
    e.attack = env_time(p, atk);
    e.hold = env_time(p, one ? ENV1_HOLD : ENV2_HOLD);
    e.decay = env_time(p, one ? ENV1_DECAY : ENV2_DECAY);
    int sus = one ? ENV1_SUSTAIN : ENV2_SUSTAIN;
    e.sustain = has(p, sus) ? clampd(param(p, sus, 127) / 127.0, 0, 1) : 1.0;
    e.release = env_time(p, one ? ENV1_RELEASE : ENV2_RELEASE);
    if (has(p, atk) && has(p, atk_lo)) e.time_vel_tracking = clampd((param(p, atk_lo, 0) - param(p, atk, 0)) / 127.0, -1, 1);
    int curve = one ? ENV1_ATK_CURVE : ENV2_ATK_CURVE;
    if (has(p, curve)) {
        int v = param(p, curve, 0) & 0xFFFF;
        if (v >= 0xFF00) v = v - 0xFF00 - 0x100;
        e.attack_slope = clampd(v / 99.0, -1, 1);
    }
    return e;
}

std::string find_sample(VolumePtr vol, const std::string &inst, const ExsSample &s, NiSampleFinder &finder) {
    std::string dir = path_dir(inst), found;
    if (!s.path.empty()) {   // the folder stored with it, if it is relative or on this volume
        std::string full = path_join(s.path, s.file);
        if (full[0] == '/' && find_ci(*vol, full, found)) return found;
        if (full[0] != '/' && find_ci(*vol, path_resolve(dir, full), found)) return found;
        // the last folders of a Mac path, from the instrument's folder ("Samples/Piano/x.wav")
        std::string tail = s.file, p = s.path;
        for (int i = 0; i < 3 && !p.empty() && p != "/"; i++) {
            tail = path_join(path_name(p), tail);
            p = path_dir(p);
            if (find_ci(*vol, path_join(dir, tail), found) || find_ci(*vol, path_join(path_dir(dir), tail), found)) return found;
        }
    }
    return finder.find(s.file);
}

std::vector<PresetInfo> list_exs(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

Instrument load_exs(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->all(64 << 20);
    std::vector<Block> bl = blocks(d);
    std::vector<ExsZone> zones;
    std::vector<ExsGroup> groups;
    std::vector<ExsSample> samples;
    std::map<int, int> params;
    bool have_instrument = false;
    for (auto &b : bl) {
        switch (b.type) {
        case T_INSTRUMENT: have_instrument = true; break;
        case T_ZONE: zones.push_back(read_zone(b)); break;
        case T_GROUP: groups.push_back(read_group(b)); break;
        case T_SAMPLE: samples.push_back(read_sample(b)); break;
        case T_PARAMS: params = read_params(b); break;
        default: break;
        }
    }
    if (!have_instrument) throw ParseError("not an EXS24 instrument (no instrument block)");
    if (samples.empty()) throw ParseError("this EXS24 instrument has no samples");
    Instrument inst;
    inst.name = path_stem(path);
    inst.format = "Logic EXS24";
    inst.groups.clear();
    std::map<int64_t, int> group_of;   // EXS group index -> instrument group
    std::vector<Mod> mods = read_mods(params);

    // the instrument's settings (EXS24Detector.applyGlobalParameters)
    int bend_up = has(params, PITCH_BEND_UP) ? param(params, PITCH_BEND_UP, 2) * 100 : 200;
    int bend_down = -200;
    if (has(params, PITCH_BEND_DOWN)) bend_down = param(params, PITCH_BEND_DOWN, 0) == -1 ? -bend_up : param(params, PITCH_BEND_DOWN, 0) * -100;
    inst.gain_db = clampd(param(params, MASTER_VOLUME, 0), -96, 12);   // dB (ConvertWithMoss leaves it out: 6-12 dB too loud)
    if (has(params, POLYPHONY_VOICES)) inst.polyphony = clampi(param(params, POLYPHONY_VOICES, 0), 0, 99);
    double tune_offset = param(params, TRANSPOSE, 0) + param(params, COARSE_TUNE, 0) + param(params, FINE_TUNE, 0) / 100.0;
    Envelope amp = envelope(params, 1);
    double vel_depth = has(params, ENV1_VEL_SENS) ? clampd(param(params, ENV1_VEL_SENS, 0) / -60.0, 0, 1) : 1.0;
    double amp_keytrack = clampd(param(params, VOLUME_KEYSCALE, 0) / 1000.0, -1, 1);
    bool random_select = param(params, RANDOM_SAMPLE_SEL, 0) > 0;
    Filter filter;
    if (param(params, FILTER1_TOGGLE, 0) > 0 && has(params, FILTER1_TYPE)) {
        switch (param(params, FILTER1_TYPE, 0)) {
        default:
        case 0: filter.type = FilterType::LowPass; filter.poles = 4; break;
        case 1: filter.type = FilterType::LowPass; filter.poles = 3; break;
        case 2: filter.type = FilterType::LowPass; filter.poles = 2; break;
        case 3: filter.type = FilterType::LowPass; filter.poles = 1; break;
        case 4: filter.type = FilterType::HighPass; filter.poles = 2; break;
        case 5: filter.type = FilterType::BandPass; filter.poles = 2; break;
        }
        filter.cutoff = clampd(param(params, FILTER1_CUTOFF, 1000) / 1000.0 * MAX_CUTOFF_HZ, 20, MAX_CUTOFF_HZ);
        filter.resonance = clampd(param(params, FILTER1_RESO, 0) / 1000.0, 0, 1);
        filter.env = envelope(params, 2);
        const Mod *em = find_mod(mods, SRC_ENV2, DST_FILTER1_CUTOFF);
        // only a routed filter envelope moves the cutoff (ConvertWithMoss assumes a full sweep without one)
        filter.env_depth = em ? clampd(em->amount / 1000.0, -1, 1) : 0.0;
        if (const Mod *mw = find_mod(mods, SRC_CC1, DST_FILTER1_CUTOFF)) filter.modwheel_depth = clampd(mw->amount / 1000.0, -1, 1);
        if (has(params, FILTER1_KEYTRACK)) filter.key_tracking = param(params, FILTER1_KEYTRACK, 0) / 1000.0;   // old files
        else if (const Mod *km = find_mod(mods, SRC_KEY, DST_FILTER1_CUTOFF)) filter.key_tracking = km->amount / 1000.0;
        if (const Mod *vm = find_mod(mods, SRC_VELOCITY, DST_FILTER1_CUTOFF)) filter.vel_depth = clampd(vm->amount / 1000.0, -1, 1);
    }

    NiSampleFinder finder(vol, path);
    std::map<int64_t, SampleRefPtr> refs;
    std::map<int64_t, bool> tried;
    int missing = 0;
    std::string first_missing;
    for (size_t i = 0; i < zones.size(); i++) {
        ExsZone &ez = zones[i];
        int64_t si = ez.sample >= 0 ? ez.sample : int64_t(i);   // no index stored: the zone's own
        if (si >= int64_t(samples.size())) continue;
        if (!tried[si]) {   // each sample file is searched once (consolidated instruments share one)
            tried[si] = true;
            std::string file = find_sample(vol, path, samples[size_t(si)], finder);
            if (!file.empty()) refs[si] = file_sample_ref(vol, file);
            else { if (!missing++) first_missing = samples[size_t(si)].file; }
        }
        auto it = refs.find(si);
        if (it == refs.end()) continue;
        Zone z;
        z.name = ez.name.empty() ? path_stem(samples[size_t(si)].file) : ez.name;
        z.sample = it->second;
        z.root = ez.key;
        z.key_lo = ez.key_lo; z.key_hi = ez.key_hi;
        z.vel_lo = ez.vel_on ? ez.vel_lo : 0;
        z.vel_hi = ez.vel_on ? ez.vel_hi : 127;
        z.start = ez.start;
        z.stop = ez.end > ez.start ? ez.end : -1;
        z.reverse = ez.reverse;
        z.one_shot = ez.oneshot;
        z.gain_db = ez.volume;
        z.key_tracking = ez.pitch ? 1 : 0;   // 'Pitch' off: the same pitch on every key
        if (ez.pitch) z.tune = ez.coarse + ez.fine / 100.0;
        z.pan = clampi(ez.pan, -50, 50) / 50.0;
        if (ez.loop_on && ez.loop_end > ez.loop_start) {
            Loop l;
            l.type = ez.loop_dir == 1 ? LoopType::Backward : ez.loop_dir > 1 ? LoopType::Alternating : LoopType::Forward;
            l.until_release = ez.loop_to_end;
            l.start = ez.loop_start;
            l.end = ez.loop_end - 1;
            l.crossfade = ez.loop_xfade * samples[size_t(si)].rate / 1000;   // stored in ms
            l.tune = ez.loop_tune / 100.0;
            z.loops.push_back(l);
        }
        // the group (EXS24Detector.limitByGroupAttributes)
        if (ez.group >= 0 && ez.group < int64_t(groups.size())) {
            const ExsGroup &g = groups[size_t(ez.group)];
            auto gi = group_of.find(ez.group);
            if (gi == group_of.end()) {
                Group ng;
                ng.name = trim(g.name);
                if (g.release_trigger) ng.trigger = Trigger::Release;
                inst.groups.push_back(ng);
                gi = group_of.emplace(ez.group, int(inst.groups.size()) - 1).first;
            }
            z.group = gi->second;
            z.trigger = inst.groups[size_t(z.group)].trigger;
            z.gain_db += clampi(g.volume, -96, 24);
            z.pan = clampd(z.pan + g.pan / 50.0, -1, 1);
            if (g.exclusive > 0) z.exclusive_group = g.exclusive;
            if (g.random_offset > 0) z.play_logic = PlayLogic::Random;
            if (z.vel_hi < g.vel_min || z.vel_lo > g.vel_max) continue;   // outside the group's velocities
            if (g.vel_min && z.vel_lo < g.vel_min) z.vel_lo = g.vel_min;
            if (z.vel_hi > g.vel_max) z.vel_hi = g.vel_max;
            if (g.extended) {
                if (z.key_hi < g.start_note || z.key_lo > g.end_note) continue;   // outside the group's notes
                if (g.start_note && z.key_lo < g.start_note) z.key_lo = g.start_note;
                if (z.key_hi > g.end_note) z.key_hi = g.end_note;
                if (g.by_cc >= 0) { z.cc_num = g.by_cc; z.cc_lo = g.cc_lo; z.cc_hi = g.cc_hi; }
                if (g.by_round_robin && g.rr_pos >= -1) {
                    z.play_logic = PlayLogic::RoundRobin;
                    z.seq_position = int(g.rr_pos + 2);   // -1, 0, 1, .. -> 1, 2, 3, ..
                }
            }
        } else {
            if (inst.groups.empty()) inst.groups.push_back(Group{});
            z.group = 0;
        }
        z.bend_up = bend_up; z.bend_down = bend_down;
        z.tune += tune_offset;
        z.amp_key_tracking = amp_keytrack;
        if (random_select && z.play_logic == PlayLogic::Always) z.play_logic = PlayLogic::Random;
        z.amp_env = amp;
        z.amp_vel_depth = vel_depth;
        z.filter = filter;
        inst.zones.push_back(z);
    }
    // random selection: the zones that overlap share the note's random number between them
    for (size_t i = 0; i < inst.zones.size(); i++) {
        Zone &z = inst.zones[i];
        if (z.play_logic != PlayLogic::Random) continue;
        int n = 0, me = 0;
        for (size_t j = 0; j < inst.zones.size(); j++) {
            const Zone &o = inst.zones[j];
            if (o.play_logic != PlayLogic::Random || o.key_lo > z.key_hi || o.key_hi < z.key_lo || o.vel_lo > z.vel_hi || o.vel_hi < z.vel_lo) continue;
            if (j < i) me++;
            n++;
        }
        z.rand_lo = double(me) / n;
        z.rand_hi = double(me + 1) / n;
    }
    if (inst.groups.empty()) inst.groups.push_back(Group{});
    if (missing)
        inst.warnings.push_back(std::to_string(missing) + " of " + std::to_string(samples.size()) + " samples not found (" + first_missing +
                                (missing > 1 ? ", ..." : "") + "): put them next to the instrument or in a folder below it");
    finish_instrument(inst);
    if (inst.zones.empty())
        throw ParseError("this EXS24 instrument's samples are missing (" + first_missing + "): copy them next to it");
    return inst;
}

bool probe_exs(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    return n >= 20 && (h[0] == 0 || h[0] == 1) && is_magic(h + 16);
}

}  // namespace

void register_logic() {
    register_reader({"Logic EXS24", "exs", probe_exs, list_exs, load_exs});
}

}  // namespace omni

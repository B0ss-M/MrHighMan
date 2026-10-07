// Omni Sampler: Akai MPC programs.
//  - MPC60 .SET (12-bit packed sounds at 40 kHz inside the set)
//  - MPC500/1000/2500 .PGM ("MPC1000 PGM 1.00", WAV samples next to it)
//  - MPC2000/2000XL/3000 .PGM with .SND (or .WAV) sounds, and a lone .SND
//  - MPC (Live/One/X/Force) .XPM keygroup and drum programs (XML), MPC 3 .XPJ projects / .XTY tracks (gzip JSON)
// Translated from ConvertWithMoss (LGPL-3.0): AkaiMPC60Set/Detector, AkaiMPC1000*, AkaiMPC2000*, MPCModernDetector,
// MPCFilter, MPCEnvelopesAndFilter. Drum programs map pads to their notes; their samples do not follow the key.
#include <cmath>
#include <cstring>
#include <map>
#include "../core/parse.hpp"
#include "format.hpp"

namespace omni {
namespace {

std::vector<PresetInfo> list_single(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }
double value_to_db(double v) { return v < 3e-8 ? -150 : std::max(-150.0, std::log(v) * 8.6858896380650365); }

// A sample file by name next to the program (case-insensitive, any audio extension).
struct Finder {
    VolumePtr vol;
    std::string dir;
    std::vector<DirEntry> listing;
    std::map<std::string, std::pair<SampleRefPtr, AudioInfo>> cache;
    Finder(VolumePtr v, const std::string &d) : vol(std::move(v)), dir(d), listing(vol->list(d)) {}
    bool find(const std::string &name, const char *exts, SampleRefPtr &ref, AudioInfo &ai, std::string *path = nullptr) {
        auto it = cache.find(name);
        if (it != cache.end()) { ref = it->second.first; ai = it->second.second; return true; }
        std::string want = lower(name);
        for (const auto &e : listing) {
            if (e.dir) continue;
            std::string ext = path_ext(e.name);
            if ((" " + std::string(exts) + " ").find(" " + ext + " ") == std::string::npos) continue;
            std::string stem = lower(path_stem(e.name));
            if (stem != want && trim(stem) != trim(want)) continue;
            std::string full = path_join(dir, e.name);
            ref = file_sample_ref(vol, full);
            try { ai = probe_audio(*vol->open(full)); } catch (const std::exception &) { ai = AudioInfo(); }
            cache[name] = {ref, ai};
            if (path) *path = full;
            return true;
        }
        return false;
    }
};

Envelope drum_env(int attack, int decay, int decay_mode, bool one_shot, double length_s, double scale) {
    Envelope e;
    e.set = true;
    e.attack = attack / scale * 2.0;
    if (decay_mode == 0) e.decay = decay / scale * 6.0;
    e.sustain = one_shot ? 1.0 : 0.0;
    if (decay_mode == 1) e.release = decay / scale * 2.0;
    else if (one_shot) e.release = length_s;
    return e;
}

// ---------------------------------------------------------------------------------------------------------------
// MPC500/1000/2500

Instrument load_mpc1000(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->all(1 << 20);
    Reader r(d);
    r.skip(4);
    if (r.str(16) != "MPC1000 PGM 1.00") throw ParseError("not an MPC1000 program");
    r.skip(4);
    struct S { std::string name; int level, vlo, vhi, tune, mode; };
    struct P { S s[4]; int mute, attack, decay, decay_mode, vel_level, f1type, f1freq, f1res, f1vel, level, pan; };
    std::vector<P> pads(64);
    for (auto &p : pads) {
        for (auto &s : p.s) {
            s.name = trim(r.str(16)); r.skip(1);
            s.level = r.u8(); s.vlo = r.u8(); s.vhi = r.u8(); s.tune = r.s16le(); s.mode = r.u8(); r.skip(1);
        }
        r.skip(2); r.u8(); p.mute = r.u8(); r.skip(2);
        p.attack = r.u8(); p.decay = r.u8(); p.decay_mode = r.u8(); r.skip(2);
        p.vel_level = r.u8(); r.skip(5);
        p.f1type = r.u8(); p.f1freq = r.u8(); p.f1res = r.u8(); r.skip(4); p.f1vel = r.u8();
        r.skip(3 + 4 + 1 + 14);
        p.level = r.u8(); p.pan = r.u8(); r.skip(4 + 15);
    }
    const uint8_t *notes = r.take(64);
    Instrument inst;
    inst.format = "Akai MPC1000 program";
    inst.name = trim(path_stem(path));
    Finder f(vol, path_dir(path));
    for (int i = 0; i < 64; i++) {
        const P &p = pads[size_t(i)];
        for (const S &s : p.s) {
            if (s.name.empty()) continue;
            SampleRefPtr ref;
            AudioInfo ai;
            if (!f.find(s.name, "wav", ref, ai)) { inst.warnings.push_back("missing sample " + s.name); continue; }
            Zone z;
            z.name = s.name;
            z.sample = ref;
            z.key_lo = z.key_hi = notes[i] & 0x7F;
            z.root = ai.root >= 0 ? ai.root : 60;
            z.vel_lo = s.vlo; z.vel_hi = s.vhi;
            z.exclusive_group = clampi(p.mute, 0, 32);
            z.tune = s.tune / 100.0;
            z.key_tracking = 0;
            z.gain_db = value_to_db((p.level + s.level) / 200.0);
            z.pan = (p.pan - 50) / 50.0;
            bool one_shot = s.mode == 0;
            double secs = ai.rate > 0 ? double(ai.frames) / ai.rate : 1.0;
            z.amp_env = drum_env(p.attack, p.decay, p.decay_mode, one_shot, secs, 100.0);
            z.amp_vel_depth = p.vel_level / 100.0;
            z.one_shot = one_shot;
            if (!one_shot) z.loops = ai.loops;
            FilterType ft = p.f1type == 1 || p.f1type == 4 ? FilterType::LowPass : p.f1type == 2 ? FilterType::BandPass
                          : p.f1type == 3 ? FilterType::HighPass : FilterType::None;
            if (ft != FilterType::None) {
                z.filter.type = ft;
                z.filter.poles = 4;
                z.filter.cutoff = std::max(20.0, p.f1freq / 100.0 * MAX_CUTOFF_HZ);
                z.filter.resonance = p.f1res / 100.0;
                z.filter.vel_depth = p.f1vel / 100.0;
            }
            inst.zones.push_back(z);
        }
    }
    finish_instrument(inst);
    return inst;
}

bool probe_mpc1000(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 20 && !std::memcmp(h + 4, "MPC1000 PGM 1.00", 16); }

// ---------------------------------------------------------------------------------------------------------------
// MPC2000 / 2000XL / 3000

struct SndHeader {
    std::string name;
    int level = 100, tune = 0, channels = 1, loop_mode = 0, rate = 44100;
    uint32_t start = 0, loop_end = 0, end = 0, loop_length = 0;
};

SndHeader parse_snd(const uint8_t *h, size_t n) {
    if (n < 42 || h[0] != 1 || (h[1] != 2 && h[1] != 4)) throw ParseError("not an MPC2000/3000 SND file");
    bool mpc3000 = h[1] == 2;
    Reader r(h, n);
    r.skip(2);
    SndHeader s;
    s.name = trim(r.str(16));
    r.u8();
    s.level = r.u8(); s.tune = r.s8(); s.channels = r.u8() + 1;
    s.start = r.u32le(); s.loop_end = r.u32le(); s.end = r.u32le();
    if (mpc3000) { r.u32le(); s.loop_mode = r.u8(); r.u8(); s.rate = 44100; }
    else { s.loop_length = r.u32le(); s.loop_mode = r.u8(); r.u8(); s.rate = r.u16le(); if (!s.rate) s.rate = 44100; }
    return s;
}

SampleRefPtr snd_ref(BlobPtr b, const SndHeader &s) {
    int64_t frames = int64_t((b->size() - 42) / 2 / uint64_t(std::max(1, s.channels)));
    // stereo sounds are stored left channel, then right channel
    return raw_sample_ref(b, 42, frames, s.channels, Enc::S16LE, s.rate, s.name, s.channels > 1);
}

struct Mpc2kPad { int sample = 0xFF, mode = 0, sw1 = 0, note1 = 0, sw2 = 0, note2 = 0, tune = 0, attack = 0, decay = 0, decay_mode = 0;
                  int ffreq = 100, fres = 0, fvel = 0, vel_level = 0, level = 100, pan = 50, solo = 100; };

Instrument load_mpc2000(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->all(1 << 20);
    Reader r(d);
    if (r.u8() != 7) throw ParseError("not an MPC2000/3000 program");
    int b2 = r.u8();
    bool mpc3000 = b2 == 0;
    if (!mpc3000 && b2 != 4) throw ParseError("not an MPC2000/3000 program");
    int nsamples = mpc3000 ? 64 : r.u16le();
    std::vector<std::string> names;
    for (int i = 0; i < nsamples; i++) {
        const uint8_t *p = r.take(16);
        names.push_back(p[0] ? trim(std::string(reinterpret_cast<const char *>(p), 16)) : "");
        r.skip(1);
    }
    if (mpc3000) r.skip(1600); else r.skip(2);
    std::string pname = trim(r.str(16));
    r.skip(1 + 9 + 1);
    r.skip(mpc3000 ? 35 : 5);
    std::vector<Mpc2kPad> pads(64);
    for (auto &p : pads) {
        p.sample = r.u8(); p.mode = r.u8(); p.sw1 = r.u8(); p.note1 = r.u8(); p.sw2 = r.u8(); p.note2 = r.u8();
        r.skip(3);
        p.tune = r.s16le(); p.attack = r.u8(); p.decay = r.u8(); p.decay_mode = r.u8();
        p.ffreq = r.u8(); p.fres = r.u8(); r.skip(3);
        p.vel_level = r.u8(); r.skip(2); p.fvel = r.u8(); r.skip(1);
        if (!mpc3000) r.skip(1);
    }
    for (auto &p : pads) {
        if (mpc3000) { p.level = r.u8(); p.pan = r.u8(); r.skip(2); }
        else { r.skip(2); p.level = r.u8(); p.pan = r.u8(); p.solo = r.u8(); r.skip(1); }
    }
    if (!mpc3000) r.skip(4);
    const uint8_t *notes = r.take(64);

    Instrument inst;
    inst.format = mpc3000 ? "Akai MPC3000 program" : "Akai MPC2000 program";
    inst.name = pname.empty() ? trim(path_stem(path)) : pname;
    inst.groups = {Group{"VelLayer 1"}, Group{"VelLayer 2"}, Group{"VelLayer 3"}};
    std::string dir = path_dir(path);
    std::vector<DirEntry> listing = vol->list(dir);
    std::map<std::string, std::pair<SampleRefPtr, SndHeader>> sounds;
    // sounds by file name (8.3 on MPC disks) or by the 16-character name in their header
    auto sound = [&](const std::string &name, SampleRefPtr &ref, SndHeader &hdr) -> bool {
        auto it = sounds.find(name);
        if (it != sounds.end()) { ref = it->second.first; hdr = it->second.second; return true; }
        for (int pass = 0; pass < 2; pass++)
            for (auto &e : listing) {
                std::string ext = path_ext(e.name);
                if (e.dir || (ext != "snd" && ext != "wav")) continue;
                std::string full = path_join(dir, e.name);
                if (ext == "wav") {
                    if (pass || !equals_ci(trim(path_stem(e.name)), name)) continue;
                    AudioInfo ai;
                    try { ai = probe_audio(*vol->open(full)); } catch (const std::exception &) { continue; }
                    hdr = SndHeader(); hdr.name = name; hdr.rate = ai.rate; hdr.end = uint32_t(ai.frames);
                    ref = file_sample_ref(vol, full);
                } else {
                    if (!pass && !equals_ci(trim(path_stem(e.name)), name)) continue;
                    BlobPtr b = vol->open(full);
                    std::vector<uint8_t> h = b->head(42);
                    try { hdr = parse_snd(h.data(), h.size()); } catch (const ParseError &) { continue; }
                    if (pass && !equals_ci(hdr.name, name)) continue;
                    ref = snd_ref(b, hdr);
                }
                sounds[name] = {ref, hdr};
                return true;
            }
        return false;
    };
    std::map<int, Zone> by_pad, by_note;
    for (int i = 0; i < 64; i++) {
        const Mpc2kPad &p = pads[size_t(i)];
        if (p.sample == 0xFF || p.sample >= int(names.size()) || names[size_t(p.sample)].empty()) continue;
        const std::string &sn = names[size_t(p.sample)];
        SampleRefPtr ref;
        SndHeader h;
        if (!sound(sn, ref, h)) { inst.warnings.push_back("missing sound " + sn); continue; }
        Zone z;
        z.name = sn;
        z.sample = ref;
        z.key_lo = z.key_hi = notes[i] & 0x7F;
        z.root = 60;
        z.tune = p.tune / 100.0;
        if (z.tune == 0) z.tune = clampd(h.tune / 10.0, -12, 12);
        z.key_tracking = 0;
        z.gain_db = value_to_db((p.level + p.solo) / 200.0);
        z.pan = (p.pan - 50) / 50.0;
        z.amp_env = drum_env(p.attack, p.decay, p.decay_mode, false, 1, 100.0);
        z.amp_env.sustain = 0;
        z.amp_vel_depth = p.vel_level / 100.0;
        z.start = h.start;
        if (h.end > h.start) z.stop = h.end;
        if (h.loop_mode == 1 && h.loop_end > h.loop_length) {
            Loop l; l.end = h.loop_end; l.start = int64_t(h.loop_end) - int64_t(h.loop_length);
            z.loops.push_back(l);
        }
        z.filter.type = FilterType::LowPass;
        z.filter.poles = 4;
        z.filter.cutoff = std::max(20.0, p.ffreq / 100.0 * MAX_CUTOFF_HZ);
        z.filter.resonance = p.fres / 100.0;
        z.filter.vel_depth = p.fvel / 100.0;
        if (p.ffreq >= 100 && p.fres == 0) z.filter.type = FilterType::None;
        by_pad[i] = z;
        by_note[z.key_lo] = z;
        inst.zones.push_back(z);
    }
    // velocity switching: pads 2/3 play the zones of other notes above the switch velocities
    for (auto &kv : by_pad) {
        const Mpc2kPad &p = pads[size_t(kv.first)];
        if (p.mode == 0) continue;
        Zone *base = nullptr;
        for (auto &z : inst.zones) if (z.key_lo == kv.second.key_lo && z.group == 0) base = &z;
        auto layer = [&](int note, int group, int vlo, int vhi) {
            auto it = by_note.find(note);
            if (it == by_note.end()) return;
            Zone z = it->second;
            z.key_lo = z.key_hi = kv.second.key_lo;
            z.group = group;
            z.vel_lo = vlo; z.vel_hi = vhi;
            inst.zones.push_back(z);
        };
        if (p.mode == 2 || p.mode == 3) {
            if (base && by_note.count(p.note1)) base->vel_hi = std::max(p.sw1 - 1, 0);
            layer(p.note1, 1, p.sw1, by_note.count(p.note2) ? std::max(p.sw2 - 1, 0) : 127);
            layer(p.note2, 2, p.sw2, 127);
        } else {
            layer(p.note1, 1, 0, 127);
            layer(p.note2, 2, 0, 127);
        }
    }
    finish_instrument(inst);
    return inst;
}

bool probe_mpc2000(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 4 && h[0] == 7 && (h[1] == 4 || h[1] == 0); }

Instrument load_snd(VolumePtr vol, const std::string &path, int) {
    BlobPtr b = vol->open(path);
    std::vector<uint8_t> h = b->head(42);
    SndHeader s = parse_snd(h.data(), h.size());
    Instrument inst;
    inst.format = "Akai MPC2000/3000 sound";
    inst.name = s.name.empty() ? path_stem(path) : s.name;
    Zone z;
    z.name = inst.name;
    z.sample = snd_ref(b, s);
    z.tune = clampd(s.tune / 10.0, -12, 12);
    z.start = s.start;
    if (s.end > s.start) z.stop = s.end;
    if (s.loop_mode == 1 && s.loop_end > s.loop_length) { Loop l; l.end = s.loop_end; l.start = int64_t(s.loop_end) - int64_t(s.loop_length); z.loops.push_back(l); }
    inst.zones.push_back(z);
    finish_instrument(inst);
    return inst;
}

bool probe_snd(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 42 && h[0] == 1 && (h[1] == 2 || h[1] == 4); }

// ---------------------------------------------------------------------------------------------------------------
// MPC60 .SET

Instrument load_mpc60(VolumePtr vol, const std::string &path, int) {
    BlobPtr b = vol->open(path);
    std::vector<uint8_t> d = b->head(0xC00);
    if (d.size() < 5 + 32 * 0x3B || d[0] != 2) throw ParseError("not an MPC60 set");
    uint64_t data_off = 0xC00 - 1;
    uint64_t total_frames = b->size() > data_off ? (b->size() - data_off) / 3 * 2 : 0;
    // the whole sample memory, unpacked once and shared by the pads
    auto memory = std::make_shared<SampleRef>();
    struct Pad { std::string name; uint32_t start, length; int decay, volume, pan, exclusive; };
    std::vector<Pad> pads;
    Reader r(d);
    r.seek(5);
    for (int i = 0; i < 32; i++) {
        Reader p = r.sub(0x3B);
        Pad pad;
        pad.name = trim(p.str(16));
        p.skip(2);
        pad.start = p.u24le(); p.skip(1);
        pad.length = p.u24le(); p.skip(1);
        p.skip(4 + 4 + 2 + 2 + 2 + 2 + 2 + 4);
        pad.decay = p.u8();
        p.skip(4);
        pad.volume = p.u8(); pad.pan = p.u8();
        p.skip(3);
        pad.exclusive = p.u8();
        pads.push_back(pad);
    }
    auto unpack = [b, data_off, total_frames](uint32_t start, uint32_t length) -> PcmPtr {
        auto p = std::make_shared<Pcm>();
        p->rate = 40000;
        p->channels = 1;
        if (start >= total_frames) return p;
        uint32_t n = uint32_t(std::min<uint64_t>(length, total_frames - start));
        uint64_t first = start / 2 * 3;
        std::vector<uint8_t> raw = b->bytes(data_off + first, size_t(std::min<uint64_t>((uint64_t(n) / 2 + 2) * 3, b->size() - data_off - first)));
        std::vector<int16_t> all;
        for (size_t i = 0; i + 2 < raw.size(); i += 3) {
            all.push_back(int16_t(raw[i] << 8 | (raw[i + 2] & 0x0F) << 4));
            all.push_back(int16_t(raw[i + 1] << 8 | (raw[i + 2] & 0xF0)));
        }
        size_t skip = start & 1;
        for (size_t i = 0; i < n && skip + i < all.size(); i++) p->data.push_back(all[skip + i]);
        return p;
    };
    Instrument inst;
    inst.format = "Akai MPC60 set";
    inst.name = trim(path_stem(path));
    std::map<uint32_t, SampleRefPtr> refs;
    int note = 36;
    for (auto &pad : pads) {
        if (pad.name.empty()) continue;
        auto it = refs.find(pad.start);
        if (it == refs.end()) {
            auto ref = std::make_shared<SampleRef>();
            ref->name = pad.name; ref->rate = 40000; ref->channels = 1; ref->frames = pad.length;
            ref->key = "mpc60:" + std::to_string(reinterpret_cast<uintptr_t>(b.get())) + ":" + std::to_string(pad.start);
            uint32_t s = pad.start, l = pad.length;
            ref->decode = [unpack, s, l] { return unpack(s, l); };
            it = refs.emplace(pad.start, ref).first;
        }
        Zone z;
        z.name = pad.name;
        z.sample = it->second;
        z.key_lo = z.key_hi = note++;
        z.key_tracking = 0;
        z.gain_db = value_to_db(pad.volume / 127.0);
        z.pan = pad.pan / 127.0 * 2.0 - 1.0;
        z.exclusive_group = clampi(pad.exclusive, 0, 3);
        z.amp_env.set = true;
        z.amp_env.decay = pad.decay / 255.0 * 6.0;
        z.amp_env.sustain = 0;
        z.amp_env.release = pad.decay / 255.0 * 2.0;
        inst.zones.push_back(z);
    }
    (void)memory;
    finish_instrument(inst);
    return inst;
}

bool probe_mpc60(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 0x30 && h[0] == 2; }

// ---------------------------------------------------------------------------------------------------------------
// MPC XPM (XML)

double env_time(double v, double def) {   // normalised 0..1 -> 1 ms .. 100 s, logarithmic
    if (v < 0) return def;
    return 0.001 * std::exp(clampd(v, 0, 1) * std::log(100.0 / 0.001));
}

Envelope xpm_env(const XmlNode &n, const char *a, const char *h, const char *d, const char *s, const char *r,
                 const char *ac, const char *dc, const char *rc) {
    Envelope e;
    e.set = true;
    e.attack = n.child(a) ? env_time(n.child_num(a, 0), 0) : 0;
    e.hold = n.child(h) ? env_time(n.child_num(h, 0), 0) : 0;
    e.decay = n.child(d) ? env_time(n.child_num(d, 0), 0) : 0;
    e.release = env_time(n.child_num(r, 0.63), 0.63);
    e.sustain = clampd(n.child_num(s, 1), 0, 1);
    e.attack_slope = clampd(n.child_num(ac, 0.5) * 2 - 1, -1, 1);
    e.decay_slope = clampd(n.child_num(dc, 0.5) * 2 - 1, -1, 1);
    e.release_slope = clampd(n.child_num(rc, 0.5) * 2 - 1, -1, 1);
    return e;
}

double denorm_cutoff(double v) { return clampd(2.0 * 440.0 * std::pow(2.0, (v * 140.0 - 57.0) / 12.0), 32.7, MAX_CUTOFF_HZ); }

bool mpc_filter(int id, FilterType &t, int &poles) {
    static const struct { FilterType t; int p; } F[30] = {
        {FilterType::None, 0}, {FilterType::LowPass, 1}, {FilterType::LowPass, 2}, {FilterType::LowPass, 4}, {FilterType::LowPass, 6},
        {FilterType::LowPass, 8}, {FilterType::HighPass, 1}, {FilterType::HighPass, 2}, {FilterType::HighPass, 4}, {FilterType::HighPass, 6},
        {FilterType::HighPass, 8}, {FilterType::BandPass, 2}, {FilterType::BandPass, 4}, {FilterType::BandPass, 6}, {FilterType::BandPass, 8},
        {FilterType::BandReject, 2}, {FilterType::BandReject, 4}, {FilterType::BandReject, 6}, {FilterType::BandReject, 8},
        {FilterType::None, 0}, {FilterType::None, 0}, {FilterType::None, 0}, {FilterType::None, 0}, {FilterType::None, 0},
        {FilterType::None, 0}, {FilterType::None, 0}, {FilterType::None, 0}, {FilterType::None, 0}, {FilterType::None, 0},
        {FilterType::LowPass, 4}};
    if (id <= 0 || id >= 30 || F[id].t == FilterType::None) return false;
    t = F[id].t;
    poles = F[id].p;
    return true;
}

std::string read_text_file(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(64 << 20);
    return std::string(d.begin(), d.end());
}

// The keygroup layout some preset generators write (<Program Type="5"> with <Keygroups>, each keygroup carrying its
// own <SynthSection>): envelope times in ms, sustain 0..1, filter type as in MPC's list. Seen with "MPC XPM preset
// generation" tool output (Version 3.9.0). LFO depth scales are not published: full scale is taken as +-12 st of
// pitch, +-4 octaves of cutoff and +-24 dB of volume.
Envelope ms_env(const XmlNode &s, const char *prefix) {
    Envelope e;
    auto v = [&](const char *what, double def) { return s.child_num(std::string(prefix) + what, def); };
    e.set = true;
    e.delay = v("Delay", 0) / 1000.0;
    e.attack = v("Attack", 0) / 1000.0;
    e.hold = v("Hold", 0) / 1000.0;
    e.decay = v("Decay", 0) / 1000.0;
    e.sustain = clampd(v("Sustain", 1), 0, 1);
    e.release = v("Release", 10) / 1000.0;
    return e;
}

LfoWave lfo_wave(const std::string &t) {
    std::string l = lower(t);
    if (l.find("tri") != std::string::npos) return LfoWave::Triangle;
    if (l.find("square") != std::string::npos || l.find("pulse") != std::string::npos) return LfoWave::Square;
    if (l.find("saw") != std::string::npos || l.find("ramp") != std::string::npos)
        return l.find("down") != std::string::npos ? LfoWave::SawDown : LfoWave::SawUp;
    if (l.find("random") != std::string::npos || l.find("s&h") != std::string::npos) return LfoWave::Random;
    return LfoWave::Sine;
}

Instrument load_xpm_keygroups(VolumePtr vol, const std::string &path, const XmlNode &prog) {
    const XmlNode *kgs = prog.child("Keygroups");
    Instrument inst;
    inst.format = "Akai MPC keygroup program";
    inst.name = prog.attr("Name", path_stem(path));
    if (inst.name.empty()) inst.name = path_stem(path);
    Finder f(vol, path_dir(path));
    std::map<std::string, int> layer_groups;
    inst.groups.clear();
    for (const XmlNode *kg : kgs->all("Keygroup")) {
        if (kg->child("Active") && kg->child_text("Active") != "True") continue;
        int lo = int(kg->child_num("LoKey", 0)), hi = int(kg->child_num("HiKey", 127));
        int kvlo = int(kg->child_num("VelocityStart", 0)), kvhi = int(kg->child_num("VelocityEnd", 127));
        int excl = int(kg->child_num("ExclusivityGroup", 0));
        const XmlNode *syn = kg->child("SynthSection");
        Envelope amp, fenv;
        Filter filt;
        double vel_amp = 1, bend_st = 0;
        Lfo lfo;
        double lfo_pitch = 0, lfo_cut = 0, lfo_vol = 0;
        if (syn) {
            amp = ms_env(*syn, "Volume");
            FilterType ft = FilterType::None;
            int poles = 2;
            if (mpc_filter(int(syn->child_num("FilterType", 0)), ft, poles)) {
                filt.type = ft;
                filt.poles = poles;
                filt.cutoff = denorm_cutoff(syn->child_num("Cutoff", 1));
                filt.resonance = clampd(syn->child_num("Resonance", 0), 0, 1);
                filt.key_tracking = syn->child_num("FilterKeytrack", 0);
                double amt = clampd(syn->child_num("FilterEnvAmt", 0), -1, 1);
                if (amt != 0) { filt.env_depth = amt; filt.env = ms_env(*syn, "Filter"); }
            }
            vel_amp = clampd(syn->child_num("VelocitySensitivity", 1), 0, 1);
            bend_st = syn->child_num("PitchBendRange", 0);
            for (const XmlNode *l : syn->all("LFO")) {
                if (int(l->attr_num("LfoNum", 0)) != 0) continue;   // one LFO per voice in our model: the first
                lfo.set = true;
                lfo.wave = lfo_wave(l->child_text("Type", "Sine"));
                lfo.rate_hz = clampd(l->child_num("Rate", 1), 0.01, 50);
                lfo.delay = l->child_num("LfoDelay", 0) / 1000.0;
                lfo.fade_in = l->child_num("FadeIn", 0) / 1000.0;
                lfo.key_sync = l->child_text("Reset", "True") == "True";
                double level = l->child_num("LfoLevel", 1);
                lfo_pitch = clampd(l->child_num("LfoPitch", 0), -1, 1) * level;
                lfo_cut = clampd(l->child_num("LfoCutoff", 0), -1, 1) * level;
                lfo_vol = clampd(l->child_num("LfoVolume", 0), -1, 1) * level;
            }
        }
        const XmlNode *layers = kg->child("Layers");
        if (!layers) continue;
        for (const XmlNode *ly : layers->all("Layer")) {
            if (ly->child("Active") && ly->child_text("Active") != "True") continue;
            std::string sample = trim(ly->child_text("SampleFile"));
            if (sample.empty()) sample = trim(ly->child_text("SampleName"));
            if (sample.empty()) continue;
            SampleRefPtr ref;
            AudioInfo ai;
            std::string base = path_name(sample);
            std::string stem = is_audio_ext(path_ext(base)) ? path_stem(base) : base;
            if (!f.find(stem, "wav aif aiff flac", ref, ai)) { inst.warnings.push_back("missing sample " + sample); continue; }
            Zone z;
            z.name = stem;
            z.sample = ref;
            z.key_lo = lo; z.key_hi = hi;
            z.vel_lo = std::max(kvlo, int(ly->child_num("VelStart", 0)));
            z.vel_hi = std::min(kvhi, int(ly->child_num("VelEnd", 127)));
            if (z.vel_lo > z.vel_hi) continue;
            double v = ly->child_num("Volume", 1);
            z.gain_db = v > 0 ? 20 * std::log10(v) : -96;
            z.pan = clampd(ly->child_num("Pan", 0.5) * 2 - 1, -1, 1);
            z.tune = ly->child_num("Pitch", 0) + ly->child_num("TuneCoarse", 0) + ly->child_num("TuneFine", 0) / 100.0;
            z.root = int(ly->child_num("RootNote", ai.root >= 0 ? ai.root : 60));
            z.key_tracking = ly->child_text("KeyTrack", "True") == "True" ? 1 : 0;
            z.exclusive_group = excl;
            int sl = int(ly->child_num("SliceLoop", 0));
            if (sl > 0 && ai.frames > 1) {
                if (!ai.loops.empty()) z.loops = ai.loops;
                else {
                    Loop l;
                    l.start = int64_t(ly->child_num("SliceLoopStart", 0));
                    l.end = ai.frames - 1;
                    if (l.end > l.start) z.loops.push_back(l);
                }
                if (sl == 2 && !z.loops.empty()) z.loops[0].type = LoopType::Alternating;
            }
            if (syn) {
                z.amp_env = amp;
                z.amp_vel_depth = vel_amp;
                z.filter = filt;
                if (lfo.set && lfo_pitch != 0) { z.pitch_lfo = lfo; z.pitch_lfo_depth = lfo_pitch * 1200.0 / 12000.0; }
                if (lfo.set && lfo_cut != 0 && filt.type != FilterType::None) { z.filter.lfo = lfo; z.filter.lfo_depth = lfo_cut * 4800.0 / 12000.0; }
                if (lfo.set && lfo_vol != 0) { z.amp_lfo = lfo; z.amp_lfo_depth = lfo_vol * 24.0 / 96.0; }
            }
            if (bend_st > 0) { z.bend_up = int(std::lround(bend_st * 100)); z.bend_down = -z.bend_up; }
            std::string vkey = std::to_string(z.vel_lo) + "-" + std::to_string(z.vel_hi);
            auto g = layer_groups.find(vkey);
            if (g == layer_groups.end()) {
                g = layer_groups.emplace(vkey, int(inst.groups.size())).first;
                inst.groups.push_back(Group{"Layer " + std::to_string(inst.groups.size() + 1)});
            }
            z.group = g->second;
            inst.zones.push_back(z);
        }
    }
    finish_instrument(inst);
    return inst;
}

Instrument load_xpm_xml(VolumePtr vol, const std::string &path, const std::string &text) {
    auto root = parse_xml(text);
    if (root->name == "Program" && root->child("Keygroups")) return load_xpm_keygroups(vol, path, *root);
    const XmlNode *prog = root->name == "MPCVObject" ? root->child("Program") : root->find("Program");
    if (!prog) throw ParseError("no Program in XPM");
    std::string type = prog->attr("type", "Keygroup");
    bool drum = type == "Drum";
    if (type != "Keygroup" && !drum) throw ParseError("XPM program type " + type + " has no samples");
    Instrument inst;
    inst.format = drum ? "Akai MPC drum program" : "Akai MPC keygroup program";
    inst.name = prog->child_text("ProgramName", path_stem(path));
    if (inst.name.empty()) inst.name = path_stem(path);
    const XmlNode *insts = prog->child("Instruments");
    if (!insts) throw ParseError("no Instruments in XPM");
    int numkg = int(prog->child_num("KeygroupNumKeygroups", 128));
    double bend = prog->child_num("KeygroupPitchBendRange", 0);
    std::map<int, int> padnote;
    if (const XmlNode *pnm = prog->child("PadNoteMap"))
        for (auto *pn : pnm->all("PadNote")) padnote[int(pn->attr_num("number", 0))] = int(pn->child_num("Note", -1));
    Finder f(vol, path_dir(path));
    std::map<std::string, int> layer_groups;
    inst.groups.clear();
    for (const XmlNode *in : insts->all("Instrument")) {
        int number = int(in->attr_num("number", 0));
        if (number > numkg) continue;
        int lo = number, hi = number;
        if (!drum) { lo = int(in->child_num("LowNote", 0)); hi = int(in->child_num("HighNote", 0)); }
        else {
            auto it = padnote.find(number + 1);   // pads are 1-based in the map, instruments 0-based
            if (it == padnote.end()) it = padnote.find(number);
            if (it != padnote.end() && it->second >= 0) lo = hi = it->second;
            else lo = hi = 36 + number;
        }
        PlayLogic play = PlayLogic::Always;
        int zp = int(in->child_num("ZonePlay", 1));
        if (zp == 0) play = PlayLogic::RoundRobin; else if (zp == 2) play = PlayLogic::Random;
        bool one_shot = false;
        Trigger trig = Trigger::Attack;
        int tm = int(in->child_num("TriggerMode", -1));
        if (tm < 0) one_shot = in->child_text("OneShot") == "True";
        else if (tm == 0) one_shot = true;
        else if (tm == 1) trig = Trigger::Release;
        bool ignore_base = in->child_text("IgnoreBaseNote") == "True";
        Envelope vol_env = xpm_env(*in, "VolumeAttack", "VolumeHold", "VolumeDecay", "VolumeSustain", "VolumeRelease",
                                   "VolumeAttackCurve", "VolumeDecayCurve", "VolumeReleaseCurve");
        Envelope pitch_env = xpm_env(*in, "PitchAttack", "PitchHold", "PitchDecay", "PitchSustain", "PitchRelease",
                                     "PitchAttackCurve", "PitchDecayCurve", "PitchReleaseCurve");
        double pitch_amt = in->child_num("PitchEnvAmount", 0.5);
        Filter filt;
        FilterType ft = FilterType::None;
        int poles = 2;
        if (mpc_filter(int(in->child_num("FilterType", -1)), ft, poles)) {
            filt.type = ft;
            filt.poles = poles;
            filt.cutoff = denorm_cutoff(in->child_num("Cutoff", 1));
            filt.resonance = clampd(in->child_num("Resonance", 0), 0, 1);
            double amt = in->child_num("FilterEnvAmt", 0);
            if (amt > 0) {
                filt.env_depth = amt;
                filt.env = xpm_env(*in, "FilterAttack", "FilterHold", "FilterDecay", "FilterSustain", "FilterRelease",
                                   "FilterAttackCurve", "FilterDecayCurve", "FilterReleaseCurve");
            }
            double vf = in->child_num("VelocityToFilter", 0);
            if (vf > 0) filt.vel_depth = vf;
            filt.key_tracking = in->child_num("FilterKeytrack", 0);
        }
        double vel_amp = in->child_num("VelocitySensitivity", 0);
        const XmlNode *layers = in->child("Layers");
        if (!layers) continue;
        std::vector<Zone> rr_zones;
        for (const XmlNode *ly : layers->all("Layer")) {
            const XmlNode *sn = ly->child("SampleName");
            if (!sn || trim(sn->text).empty()) continue;
            if (ly->child("Active") && ly->child_text("Active") != "True") continue;
            std::string sample = sn->text;
            SampleRefPtr ref;
            AudioInfo ai;
            if (!f.find(sample, "wav aif aiff flac", ref, ai) && !f.find(trim(sample), "wav aif aiff flac", ref, ai)) {
                inst.warnings.push_back("missing sample " + sample);
                continue;
            }
            Zone z;
            z.name = trim(sample);
            z.sample = ref;
            z.key_lo = lo; z.key_hi = hi;
            z.vel_lo = int(ly->child_num("VelStart", 0));
            z.vel_hi = int(ly->child_num("VelEnd", 127));
            z.play_logic = play;
            z.trigger = trig;
            if (ly->child("Volume") && !trim(ly->child_text("Volume")).empty())
                z.gain_db = (ly->child_num("Volume", 1) - 0.353) * 18.0 / (1.0 - 0.353) - 12.0;
            if (ly->child("Pan") && !trim(ly->child_text("Pan")).empty()) z.pan = clampd(ly->child_num("Pan", 0.5) * 2 - 1, -1, 1);
            if (ly->child("Pitch") && !trim(ly->child_text("Pitch")).empty()) z.tune = ly->child_num("Pitch", 0);
            else z.tune = ly->child_num("TuneCoarse", 0) + ly->child_num("TuneFine", 0);
            std::string rn = trim(ly->child_text("RootNote"));
            if (!rn.empty()) z.root = std::atoi(rn.c_str()) - 1;
            else z.root = ai.root >= 0 ? ai.root : (drum ? lo : 60);
            if (z.root < 0) z.root = ai.root >= 0 ? ai.root : (drum ? lo : 60);
            if (drum) z.key_tracking = 0;
            if (ignore_base && ly->child("KeyTrack")) z.key_tracking = ly->child_text("KeyTrack") == "True" ? 1 : 0;
            if (ly->child("SliceStart")) z.start = int64_t(ly->child_num("SliceStart", 0));
            if (ly->child("SliceEnd") && ly->child_num("SliceEnd", 0) > 0) z.stop = int64_t(ly->child_num("SliceEnd", 0));
            if (z.stop > 0 && z.stop <= z.start) z.stop = -1;
            z.amp_env = vol_env;
            if (vel_amp > 0) z.amp_vel_depth = vel_amp;
            if (pitch_amt != 0.5) { z.pitch_env_depth = (pitch_amt - 0.5) * 2.0; z.pitch_env = pitch_env; }
            z.one_shot = one_shot;
            if (!one_shot) {
                int sl = int(ly->child_num("SliceLoop", -1));
                if (sl > 0) {
                    if (sl == 3) z.reverse = true;
                    Loop l;
                    l.start = int64_t(ly->child_num("SliceLoopStart", 0));
                    l.end = z.stop > 0 ? z.stop : ai.frames - 1;
                    if (l.end > l.start) z.loops.push_back(l);
                }
            }
            z.filter = filt;
            if (bend != 0) { z.bend_up = int(std::lround(bend * 1200)); z.bend_down = -z.bend_up; }
            std::string vkey = std::to_string(z.vel_lo) + "-" + std::to_string(z.vel_hi);
            auto g = layer_groups.find(vkey);
            if (g == layer_groups.end()) {
                g = layer_groups.emplace(vkey, int(inst.groups.size())).first;
                inst.groups.push_back(Group{"Layer " + std::to_string(inst.groups.size() + 1)});
            }
            z.group = g->second;
            if (play == PlayLogic::RoundRobin) z.seq_position = int(rr_zones.size()) + 1;
            rr_zones.push_back(z);
        }
        // cycle / random: each layer is one of the round robin positions over all velocities
        for (auto &z : rr_zones) {
            if (play == PlayLogic::RoundRobin) { z.seq_length = int(rr_zones.size()); z.vel_lo = 0; z.vel_hi = 127; }
            if (play == PlayLogic::Random) {
                size_t i = size_t(&z - &rr_zones[0]);
                z.rand_lo = double(i) / rr_zones.size(); z.rand_hi = double(i + 1) / rr_zones.size();
                z.vel_lo = 0; z.vel_hi = 127;
            }
            inst.zones.push_back(z);
        }
    }
    finish_instrument(inst);
    return inst;
}

// ---------------------------------------------------------------------------------------------------------------
// MPC 3 JSON (.xpm JSON, .xpj project, .xty track): "ACVS" header lines then JSON, gzip compressed

struct AcvsDoc { std::string kind; Json data; };

AcvsDoc read_acvs(Volume &vol, const std::string &path) {
    std::vector<uint8_t> raw = vol.open(path)->all(256 << 20);
    std::vector<uint8_t> d = raw.size() >= 2 && raw[0] == 0x1F && raw[1] == 0x8B ? inflate_gzip(raw.data(), raw.size()) : raw;
    std::string text(d.begin(), d.end());
    std::vector<std::string> header;
    size_t pos = 0;
    for (int i = 0; i < 5; i++) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) throw ParseError("not an MPC project/track/program file");
        header.push_back(trim(text.substr(pos, e - pos)));
        pos = e + 1;
    }
    if (header[0] != "ACVS" || header[3] != "json") throw ParseError("not an MPC project/track/program file");
    AcvsDoc doc;
    doc.kind = header[2] == "SerialisableProjectData" ? "Project" : header[2] == "SerialisableTrackData" ? "Track" : "Program";
    Json root = parse_json(text.substr(pos));
    doc.data = root["data"];
    if (doc.data.type == Json::Null) throw ParseError("MPC file without data");
    return doc;
}

std::vector<const Json *> keygroup_programs(const AcvsDoc &doc) {
    std::vector<const Json *> out;
    if (doc.kind == "Program") out.push_back(&doc.data);
    else if (doc.kind == "Track") { const Json &p = doc.data["program"]; if (p["type"].as_int() == 1) out.push_back(&p); }
    else for (auto &t : doc.data["tracks"].arr) { const Json &p = t["program"]; if (p["type"].as_int() == 1) out.push_back(&p); }
    return out;
}

double json_env_value(const Json &node, const char *attr, double mn, double mx, double def, bool logarithmic) {
    const Json &a = node[attr];
    if (a.type == Json::Null) return def;
    const Json &v = a["value0"];
    if (v.type == Json::Null) return def;
    double x = v.as_num();
    return logarithmic ? mn * std::exp(clampd(x, 0, 1) * std::log(mx / mn)) : mn + clampd(x, 0, 1) * (mx - mn);
}
bool json_env_bool(const Json &node, const char *attr) { return node[attr]["value0"].as_bool(false); }

bool json_env(const Json &synth, const char *name, Envelope &e) {
    const Json &n = synth[name];
    if (n.type == Json::Null || json_env_bool(n, "OneShot")) return false;
    e = Envelope();
    e.set = true;
    if (json_env_bool(n, "AD")) e.sustain = 0;
    else {
        e.delay = json_env_value(n, "Delay", 0.001, 100, 0, true);
        e.hold = json_env_value(n, "Hold", 0.001, 100, 0, true);
        e.sustain = json_env_value(n, "Sustain", 0, 1, 1, false);
    }
    e.attack = json_env_value(n, "Attack", 0.001, 100, 0, true);
    e.decay = json_env_value(n, "Decay", 0.001, 100, 0, true);
    e.release = json_env_value(n, "Release", 0.001, 100, 0.63, true);
    e.attack_slope = clampd(json_env_value(n, "AttackCurve", 0, 1, 0.5, false) * 2 - 1, -1, 1);
    e.decay_slope = clampd(json_env_value(n, "DecayCurve", 0, 1, 0.5, false) * 2 - 1, -1, 1);
    e.release_slope = clampd(json_env_value(n, "ReleaseCurve", 0, 1, 0.5, false) * 2 - 1, -1, 1);
    return true;
}

bool json_filter(const Json &synth, Filter &f) {
    const Json &v = synth["filterData"]["value0"];
    FilterType t;
    int poles = 2;
    if (v.type == Json::Null || !mpc_filter(v["filterType"].as_int(), t, poles)) return false;
    f = Filter();
    f.type = t;
    f.poles = poles;
    f.cutoff = denorm_cutoff(v["filterCutoff"].as_num(1));
    f.resonance = clampd(v["filterResonance"].as_num(), 0, 1);
    Envelope fe;
    if (v["filterEnvelopeAmount"].as_num() > 0 && json_env(synth, "filterEnvelope", fe)) { f.env_depth = v["filterEnvelopeAmount"].as_num(); f.env = fe; }
    if (v["filterVelocity"].as_num() > 0) f.vel_depth = v["filterVelocity"].as_num();
    f.key_tracking = v["filterKeytrack"].as_num();
    return true;
}

std::vector<PresetInfo> list_acvs(VolumePtr vol, const std::string &path) {
    AcvsDoc doc = read_acvs(*vol, path);
    std::vector<PresetInfo> out;
    int i = 0;
    for (const Json *p : keygroup_programs(doc)) out.push_back({(*p)["name"].as_str("Program " + std::to_string(i + 1)), i++});
    if (out.empty()) throw ParseError("no keygroup programs in this MPC file");
    return out;
}

Instrument load_acvs(VolumePtr vol, const std::string &path, int index) {
    AcvsDoc doc = read_acvs(*vol, path);
    auto progs = keygroup_programs(doc);
    if (index < 0 || size_t(index) >= progs.size()) throw ParseError("no such program");
    const Json &prog = *progs[size_t(index)];
    Instrument inst;
    inst.format = "Akai MPC " + doc.kind;
    inst.name = prog["name"].as_str(path_stem(path));
    std::string sample_dir = path_join(path_dir(path), path_stem(path) + "_[" + doc.kind + "Data]");
    if (!vol->is_dir(sample_dir)) sample_dir = path_dir(path);
    Finder f(vol, sample_dir);
    std::map<std::string, std::pair<int, double>> infos;   // sample name -> root, tune
    for (auto &s : doc.data["samples"].arr) {
        const Json &md = s["metadata"];
        if (md.type != Json::Null) infos[s["name"].as_str()] = {md["rootNote"].as_int(60), md["tune"].as_num()};
    }
    double transpose = prog["transpose"].as_num();
    const Json &kg = prog["keygroup"];
    int bend_up = 200, bend_down = 200;
    Filter gfilter;
    bool gfilter_on = false;
    Envelope gamp, gpitch;
    bool gamp_on = false, gpitch_on = false;
    double gpitch_amt = 0;
    if (kg.type != Json::Null) {
        transpose += kg["transpose"].as_num();
        bend_up = kg["pitchBendPositiveRange"].as_int(2) * 100;
        bend_down = kg["pitchBendNegativeRange"].as_int(2) * 100;
        const Json &syn = kg["synthSection"];
        if (json_env_bool(syn, "ampEnvelopeGlobal")) gamp_on = json_env(syn, "ampEnvelope", gamp);
        if (json_env_bool(syn, "pitchEnvelopeGlobal") && json_env(syn, "pitchEnvelope", gpitch)) {
            gpitch_on = true; gpitch_amt = syn["pitchEnvelopeAmount"].as_num(0.5) * 2 - 1;
        }
        if (json_env_bool(syn, "filterEnvelopeGlobal")) gfilter_on = json_filter(syn, gfilter);
    }
    for (auto &in : prog["drum"]["instruments"].arr) {
        double tune = transpose + in["coarseTune"].as_int() + in["fineTune"].as_int() / 100.0;
        int lo = in["lowNote"].as_int(0), hi = in["highNote"].as_int(127);
        const Json &syn = in["synthSection"];
        Envelope amp, pitch;
        bool amp_on = json_env(syn, "ampEnvelope", amp), pitch_on = json_env(syn, "pitchEnvelope", pitch);
        Filter filt;
        bool filt_on = json_filter(syn, filt);
        for (auto &ly : in["layersv"].arr) {
            if (!ly["active"].as_bool()) continue;
            std::string sname = ly["sampleName"].as_str(), sfile = ly["sampleFile"].as_str();
            if (sname.empty() || sfile.empty()) continue;
            SampleRefPtr ref;
            AudioInfo ai;
            if (!f.find(path_stem(sfile), "wav aif aiff flac", ref, ai)) { inst.warnings.push_back("missing sample " + sfile); continue; }
            Zone z;
            z.name = sname;
            z.sample = ref;
            z.bend_up = bend_up; z.bend_down = -bend_down;
            int rn = ly["rootNote"].as_int();
            auto inf = infos.find(sname);
            z.root = rn > 0 ? rn - 1 : inf != infos.end() ? inf->second.first - 1 : (ai.root >= 0 ? ai.root : 60);
            z.tune = ly["coarseTune"].as_int() + ly["fineTune"].as_int() / 100.0 + tune + (inf != infos.end() ? inf->second.second : 0);
            double gc = ly["volume"]["gainCoefficient"].as_num(1);
            if (gc > 0) z.gain_db = 20 * std::log10(gc);
            z.pan = clampd(ly["pan"].as_num(0.5) * 2 - 1, -1, 1);
            z.key_lo = lo; z.key_hi = hi;
            z.vel_lo = ly["velocityStart"].as_int(0); z.vel_hi = ly["velocityEnd"].as_int(127);
            int start = std::max(0, ly["offset"].as_int()) + ly["sampleStart"].as_int();
            if (start > 0) z.start = start;
            if (ly["sampleEnd"].as_int() > 0) z.stop = ly["sampleEnd"].as_int();
            z.reverse = ly["direction"].as_int() > 0;
            z.key_tracking = ly["keyTrackEnable"].as_bool(true) ? 1 : 0;
            int mode, ls, le;
            if (ly["layerLoopModeOverridesSliceLoopMode"].as_bool()) { mode = ly["loopMode"].as_int(); ls = ly["loopStart"].as_int(); le = ly["loopEnd"].as_int(); }
            else { const Json &si = ly["sliceInfo"]; mode = si["LoopMode"].as_int(); ls = si["LoopStart"].as_int(); le = si["End"].as_int(); }
            if (mode > 0 && le > ls) { Loop l; l.start = ls; l.end = le; z.loops.push_back(l); }
            if (gfilter_on) z.filter = gfilter; else if (filt_on) z.filter = filt;
            if (gamp_on) z.amp_env = gamp; else if (amp_on) z.amp_env = amp;
            if (gpitch_on) { z.pitch_env = gpitch; z.pitch_env_depth = gpitch_amt; }
            else if (pitch_on) { z.pitch_env = pitch; z.pitch_env_depth = syn["pitchEnvelopeAmount"].as_num(0.5) * 2 - 1; }
            inst.zones.push_back(z);
        }
    }
    finish_instrument(inst);
    return inst;
}

std::vector<PresetInfo> list_xpm(VolumePtr vol, const std::string &path) {
    std::vector<uint8_t> h = vol->open(path)->head(16);
    if (h.size() >= 2 && h[0] == 0x1F && h[1] == 0x8B) return list_acvs(vol, path);
    return list_single(vol, path);
}

Instrument load_xpm(VolumePtr vol, const std::string &path, int index) {
    std::vector<uint8_t> h = vol->open(path)->head(16);
    if (h.size() >= 2 && h[0] == 0x1F && h[1] == 0x8B) return load_acvs(vol, path, index);
    std::string text = read_text_file(*vol, path);
    if (text.compare(0, 4, "ACVS") == 0) return load_acvs(vol, path, index);
    return load_xpm_xml(vol, path, text);
}

}  // namespace

void register_akai_mpc() {
    register_reader({"Akai MPC1000 program", "pgm", probe_mpc1000, list_single, load_mpc1000});
    register_reader({"Akai MPC2000/3000 program", "pgm", probe_mpc2000, list_single, load_mpc2000});
    register_reader({"Akai MPC2000/3000 sound", "snd", probe_snd, list_single, load_snd});
    register_reader({"Akai MPC60 set", "set", probe_mpc60, list_single, load_mpc60});
    register_reader({"Akai MPC program", "xpm", nullptr, list_xpm, load_xpm});
    register_reader({"Akai MPC project", "xpj xty", nullptr, list_acvs, load_acvs});
}

}  // namespace omni

// Omni Sampler: Akai S5000/S6000 and Z4/Z8 programs (.akp) and multis (.akm). Samples are WAV files next to the
// program; root keys and loops come from their smpl chunks. Field offsets and conversions are translated from
// ConvertWithMoss's AkpFile/AkpKeygroup/AkpTuning/AkpModulations/AkpOutput/AkmFile/AkmPart (LGPL-3.0).
#include <cmath>
#include <cstring>
#include <map>
#include "format.hpp"

namespace omni {
namespace {

struct Chunk { std::string id; std::vector<uint8_t> data; };

// RIFF with S5000's zero top size allowed: the chunks run to the end of the file.
std::vector<Chunk> riff_chunks(const std::vector<uint8_t> &d, const char *type) {
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, type, 4))
        throw ParseError(std::string("not a ") + type + " file");
    std::vector<Chunk> out;
    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        Chunk c;
        c.id.assign(reinterpret_cast<const char *>(d.data() + pos), 4);
        size_t len = le32(d.data() + pos + 4);
        size_t body = pos + 8;
        if (len > d.size() - body) len = d.size() - body;
        c.data.assign(d.begin() + long(body), d.begin() + long(body + len));
        out.push_back(std::move(c));
        pos = body + len + (len & 1);
    }
    return out;
}

const FilterType AKP_FILTER[16] = {FilterType::LowPass, FilterType::LowPass, FilterType::LowPass, FilterType::BandPass,
                                   FilterType::BandPass, FilterType::BandPass, FilterType::HighPass, FilterType::HighPass,
                                   FilterType::HighPass, FilterType::None, FilterType::None, FilterType::None,
                                   FilterType::BandReject, FilterType::BandReject, FilterType::BandReject, FilterType::BandReject};
const int AKP_POLES[16] = {2, 4, 2, 2, 4, 2, 1, 2, 1, 0, 0, 0, 1, 2, 3, 4};

double akp_seconds(int v) { return v / 100.0 * 6.0; }

// Find "<name>.wav", also with the multiple spaces some programs collapse ("PIANO  C3" stored as "PIANO C3").
bool find_wav(Volume &vol, const std::string &dir, const std::vector<DirEntry> &listing, const std::string &name, std::string &out) {
    auto norm = [](std::string s) {
        std::string o;
        for (char c : lower(trim(s))) if (!(c == ' ' && !o.empty() && o.back() == ' ')) o += c;
        return o;
    };
    std::string want = norm(name);
    for (auto &e : listing) {
        if (e.dir || !is_audio_ext(path_ext(e.name))) continue;
        if (norm(path_stem(e.name)) == want) { out = path_join(dir, e.name); return true; }
    }
    (void)vol;
    return false;
}

void akp_into(VolumePtr vol, const std::string &path, Instrument &inst, int group, int channel, int clip_lo, int clip_hi,
              double pan_offset, double gain_offset) {
    std::vector<uint8_t> d = vol->open(path)->all(4 << 20);
    std::vector<Chunk> chunks = riff_chunks(d, "APRG");
    std::vector<uint8_t> tune(64, 0), mods(64, 0), out(8, 0);
    out[7] = 25;
    std::vector<const Chunk *> kgrps;
    for (auto &c : chunks) {
        if (c.id == "tune") { tune = c.data; tune.resize(std::max<size_t>(tune.size(), 64)); }
        else if (c.id == "mods") { mods = c.data; mods.resize(std::max<size_t>(mods.size(), 64)); }
        else if (c.id == "out ") { out = c.data; out.resize(std::max<size_t>(out.size(), 8)); }
        else if (c.id == "kgrp") kgrps.push_back(&c);
    }
    auto tv = [&](int pos) { return int(int8_t(tune[size_t(pos - 0x32)])); };
    auto tu = [&](int pos) { return int(tune[size_t(pos - 0x32)]); };
    auto mu = [&](int pos) { return int(mods[size_t(pos - 0x78)]); };
    double global_tune = tv(0x33) + tv(0x34) / 100.0;
    int bend_up = tu(0x41), bend_down = tu(0x42);
    bool pm1_aux = mu(0x93) == 11, pm2_aux = mu(0x94) == 11;
    int vel_sens = int8_t(out[7]);
    std::string dir = path_dir(path);
    std::vector<DirEntry> listing = vol->list(dir);
    std::map<std::string, std::pair<SampleRefPtr, AudioInfo>> samples;

    for (const Chunk *kc : kgrps) {
        std::vector<uint8_t> k = kc->data;
        k.resize(std::max<size_t>(k.size(), 0x400), 0);
        auto v = [&](int pos) { return int(int8_t(k[size_t(pos - 0xA6)])); };
        auto u = [&](int pos) { return int(k[size_t(pos - 0xA6)]); };
        int low = u(0xB2), high = u(0xB3);
        double kg_tune = global_tune + v(0xB4) + v(0xB5) / 100.0;
        int pm1 = v(0xB8), pm2 = v(0xB9);
        Envelope amp;
        amp.set = true;
        amp.attack = akp_seconds(u(0xC7)); amp.decay = akp_seconds(u(0xC9)); amp.release = akp_seconds(u(0xCA));
        amp.sustain = u(0xCD) / 100.0;
        Envelope fenv;
        fenv.set = true;
        fenv.attack = akp_seconds(u(0xE1)); fenv.decay = akp_seconds(u(0xE3)); fenv.release = akp_seconds(u(0xE4));
        fenv.sustain = u(0xE7) / 100.0;
        int fenv_depth = v(0xE9);
        Envelope aux;
        aux.set = true;
        aux.attack = akp_seconds(u(0xFB)); aux.hold = akp_seconds(u(0xFC)); aux.decay = akp_seconds(u(0xFD)); aux.release = akp_seconds(u(0xFE));
        aux.start_level = u(0xFF) / 100.0; aux.sustain = u(0x101) / 100.0; aux.end_level = u(0x102) / 100.0;
        int fmode = u(0x115), fcut = u(0x116), fres = u(0x117);
        double fkey = clampd(v(0x118) / 12.0, -1, 1);
        double cutoff = std::pow(2.0, fcut / 100.0 * std::log2(MAX_CUTOFF_HZ));
        int zp = 0x11E;
        for (int i = 0; i < 4; i++) {
            if (zp - 0xA6 + 0x40 > int(k.size()) || v(zp) == 0) break;
            int zone_size = u(zp + 4);
            int nlen = u(zp + 9);
            if (nlen > 0) {
                std::string sname;
                for (int c = 0; c < nlen && c < 20; c++) sname += char(v(zp + 10 + c));
                sname = trim(sname);
                std::string file;
                auto it = samples.find(sname);
                if (it == samples.end()) {
                    if (find_wav(*vol, dir, listing, sname, file)) {
                        AudioInfo ai;
                        try { ai = probe_audio(*vol->open(file)); } catch (const std::exception &) {}
                        it = samples.emplace(sname, std::make_pair(file_sample_ref(vol, file), ai)).first;
                    }
                }
                if (it == samples.end()) inst.warnings.push_back("missing sample " + sname + ".wav");
                else {
                    Zone z;
                    z.name = sname;
                    z.group = group;
                    z.sample = it->second.first;
                    const AudioInfo &ai = it->second.second;
                    z.key_lo = std::max(low, clip_lo);
                    z.key_hi = std::min(high, clip_hi);
                    z.vel_lo = u(zp + 0x2A);
                    z.vel_hi = u(zp + 0x2B);
                    z.root = ai.root >= 0 ? ai.root : 60;
                    z.tune = kg_tune + v(zp + 0x2D) + v(zp + 0x2C) / 100.0 + ai.fine / 100.0;
                    if (fmode < 16 && AKP_FILTER[fmode] != FilterType::None) {
                        Filter &f = z.filter;
                        f.type = AKP_FILTER[fmode];
                        f.poles = AKP_POLES[fmode];
                        f.cutoff = cutoff;
                        f.resonance = std::pow(clampd(fres / 12.0, 0, 1), 1.0 / 3.0) / 4.0;
                        f.key_tracking = fkey;
                        f.env_depth = fenv_depth / 100.0;
                        f.env = fenv;
                    }
                    z.pan = clampd(v(zp + 0x2F) / 50.0 + pan_offset, -1, 1);
                    int loop_type = u(zp + 0x30);
                    z.one_shot = loop_type == 1;
                    if (loop_type > 1 && !ai.loops.empty()) {
                        Loop l = ai.loops[0];
                        l.until_release = loop_type == 3;
                        z.loops.push_back(l);
                    }
                    z.gain_db = v(zp + 0x32) / 100.0 * 6.0 + gain_offset;
                    z.amp_env = amp;
                    z.amp_vel_depth = clampd(vel_sens / 100.0, 0, 1);
                    if (pm1_aux || pm2_aux) { z.pitch_env_depth = (pm1_aux ? pm1 : pm2) / 100.0; z.pitch_env = aux; }
                    z.bend_up = bend_up * 100;
                    z.bend_down = -bend_down * 100;
                    z.midi_channel = channel;
                    // move whole semitones of tuning into the root key (programs with C3 roots and +/- pitch)
                    int semis = int(std::lround(z.tune));
                    if (std::abs(semis) >= 12) { z.root = clampi(z.root - semis, 0, 127); z.tune -= semis; }
                    if (z.key_lo <= z.key_hi) inst.zones.push_back(z);
                }
            }
            zp += zone_size + 8;
        }
    }
}

std::vector<PresetInfo> list_single(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

Instrument load_akp(VolumePtr vol, const std::string &path, int) {
    Instrument inst;
    inst.format = "Akai S5000/S6000 program";
    std::string n = path_stem(path);
    if (!n.empty() && n.back() == '-') n.pop_back();
    inst.name = n;
    akp_into(vol, path, inst, 0, -1, 0, 127, 0, 0);
    finish_instrument(inst);
    return inst;
}

Instrument load_akm(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->all(1 << 20);
    std::vector<Chunk> chunks = riff_chunks(d, "AMUL");
    Instrument inst;
    inst.format = "Akai S5000/S6000 multi";
    inst.name = path_stem(path);
    inst.groups.clear();
    std::string dir = path_dir(path);
    std::vector<DirEntry> listing = vol->list(dir);
    for (auto &c : chunks) {
        if (c.id != "part" || c.data.size() < 0x28) continue;
        std::string preset = trim(Reader(c.data.data() + 2, 32).str(32));
        if (preset.empty()) continue;
        int chan = c.data[0x22] % 16, pan = int8_t(c.data[0x23]), lo = c.data[0x24], hi = c.data[0x25], vol100 = c.data[0x26];
        std::string file;
        for (auto &e : listing)
            if (!e.dir && path_ext(e.name) == "akp" && equals_ci(trim(path_stem(e.name)), preset)) file = path_join(dir, e.name);
        if (file.empty()) { inst.warnings.push_back("missing program " + preset + ".AKP"); continue; }
        int group = int(inst.groups.size());
        inst.groups.push_back(Group{preset, Trigger::Attack});
        double gain = vol100 > 0 ? 20.0 * std::log10(vol100 / 100.0) : -96;
        // parts on MIDI channel 1A play from any channel: MPC sends one channel per track
        akp_into(vol, file, inst, group, chan == 0 ? -1 : chan, lo, hi, pan / 50.0, gain);
    }
    finish_instrument(inst);
    return inst;
}

bool probe_akp(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 12 && !std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "APRG", 4); }
bool probe_akm(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 12 && !std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "AMUL", 4); }

}  // namespace

void register_akai_akp() {
    register_reader({"Akai S5000/S6000 program", "akp", probe_akp, list_single, load_akp});
    register_reader({"Akai S5000/S6000 multi", "akm", probe_akm, list_single, load_akm});
}

}  // namespace omni

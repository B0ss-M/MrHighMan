// Omni Sampler: Image-Line DirectWave programs (.dwp, FL Studio / FL Studio Mobile). Layout from ConvertWithMoss's
// format/directwave (DirectWaveDetector, DirectWaveTag, DirectWaveChunk; LGPL-3.0) and its design note
// DIRECTWAVE_DWP_FORMAT.md, which were measured there on 117 real programs (versions 0x24, 0x25, 0x26). Not yet
// checked here against real files: no .dwp is in the workspace (only the synthetic test file this reader was
// written with, testdata/directwave/).
//
// Everything is little-endian. "DwPr" at 0, the version byte at 4, then a preamble (0x5A bytes; 0x5E in version 0x24),
// then a gapless stream of blocks {u32 tag, u32 length, u32 0, payload} to the end of the file. The stream start is
// found as CWM does it: the first offset 0x50..0x70 holding the tag 0x66 (instrument name) from which the blocks
// account for every byte of the file. Top-level tags: 0x66 name, 0x03 sample container (one per zone; its payload is
// a nested block stream), 0x02 terminator; the rest are program parameters a player does not need.
// Inside a container:
//   0x1F4 zone mapping: u8 root, u8 low key, u8 high key, u8 low vel, u8 high vel, f32 gain @9 (linear, 1 = 0 dB),
//         f32 pan @13 (0.5 = centre)
//   0x1F5 sample name (no extension), 0x1F6 sample path (of the machine that saved it: only its last two parts are used)
//   0x1F7 audio format: u32 frames @0, u32 channels @8, f32 rate @16, u32 loop mode @20 (0 off, 1 one-shot, 2 forward,
//         3 sustained = loop until release, 4 bounce), u32 loop start @24, u32 loop end @28, u32 bits @36
//   0x1FB filter (twice; the first is read): u32 type (0 off, 1 LP, 2 HP, 3 BP, 4 notch), f32 cutoff knob
//         (45 Hz..20 kHz, exponential), f32 resonance knob
//   0x1FD amp envelope: 4 knob floats A, D, S, R; time = 9.07 s * knob^3.96 (CWM's measurement), S a level; a block
//         equal to the version's defaults (0x25: 0, .5, 1, .25; 0x26: 0, 1, 1, .18) leaves Omni's default envelope
//   0x203 zone LFO 1 / 2: u32 wave, f32 rate knob (Hz = 20 * knob^2); 0x204 modulation matrix slot (16): u16 source,
//         u16 target, f32 amount (0.5 = none; strength = (2 |amount - 0.5|)^3, signed). Sources 11/12 = zone LFO 1/2;
//         targets 1 pitch (2400 cents at full strength), 2 gain (linear index), 7 filter 1 cutoff (12800 cents).
//   0x205 embedded PCM (planar, frames + 512 per channel), 0x206 embedded FLAC (u32 length, u32 0, FLAC stream):
//         "monolithic" programs. Both hold 256 silent guard frames before and after the sample; loop points do not
//         count the guard, so the zone starts after it.
// Samples: the embedded audio when present; else the file named by the path (or name + ".wav") beside the program,
// in a folder named like the program, or in a folder named like the stored path's folder.
// Not read: the second filter, filter envelope (Zone Env 1/2), the mod wheel's control of LFO depth, other matrix
// routings, program-level parameters, and .dwb banks.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include "../core/audio.hpp"
#include "../core/bytes.hpp"
#include "format.hpp"

namespace omni {
namespace {

constexpr uint32_t TAG_NAME = 0x66, TAG_CONTAINER = 0x03;
constexpr uint32_t TAG_MAPPING = 0x1F4, TAG_SAMPLE_NAME = 0x1F5, TAG_SAMPLE_PATH = 0x1F6, TAG_FORMAT = 0x1F7;
constexpr uint32_t TAG_FILTER = 0x1FB, TAG_AMP_ENV = 0x1FD, TAG_LFO = 0x203, TAG_MOD = 0x204;
constexpr uint32_t TAG_EMBEDDED_PCM = 0x205, TAG_EMBEDDED_FLAC = 0x206;
constexpr size_t MAX_BLOCKS = 50000, MAX_ZONES = 4096, MAX_SMALL = 4096;

struct Block {
    uint32_t tag = 0;
    uint64_t offset = 0;    // of the payload
    uint64_t length = 0;
};

// A block stream from `start` to `end`; false when the blocks do not account for every byte (not a stream).
bool walk(const Blob &f, uint64_t start, uint64_t end, std::vector<Block> &out, size_t &budget) {
    out.clear();
    uint64_t p = start;
    uint8_t h[12];
    while (p + 12 <= end) {
        if (out.size() >= MAX_BLOCKS) return false;
        if (!budget--) throw ParseError("DirectWave: too many blocks");
        f.read(p, h, 12);
        Block b;
        b.tag = le32(h);
        b.length = le32(h + 4);
        b.offset = p + 12;
        if (b.length > end - b.offset) return false;
        out.push_back(b);
        p = b.offset + b.length;
    }
    return p == end;
}

std::vector<uint8_t> payload(const Blob &f, const Block &b, size_t limit = MAX_SMALL) {
    return f.bytes(b.offset, size_t(std::min<uint64_t>(b.length, limit)));
}

std::string text(const Blob &f, const Block &b) {
    std::vector<uint8_t> d = payload(f, b, 1024);
    std::string s;
    for (uint8_t c : d) {
        if (!c) break;
        s += char(c);   // Latin-1 / ASCII
    }
    return trim(s);
}

float f32(const std::vector<uint8_t> &d, size_t o) {
    if (o + 4 > d.size()) return 0;
    uint32_t u = le32(&d[o]);
    float v;
    std::memcpy(&v, &u, 4);
    return std::isfinite(v) ? v : 0.0f;
}
uint32_t u32(const std::vector<uint8_t> &d, size_t o) { return o + 4 <= d.size() ? le32(&d[o]) : 0; }
int u16(const std::vector<uint8_t> &d, size_t o) { return o + 2 <= d.size() ? int(le16(&d[o])) : 0; }

struct Program {
    int version = 0;
    std::string name;
    std::vector<Block> top;
};

Program read_program(const Blob &f) {
    std::vector<uint8_t> h = f.head(0x70 + 12);
    if (h.size() < 0x50 + 12 || std::memcmp(h.data(), "DwPr", 4)) throw ParseError("not a DirectWave program");
    Program p;
    p.version = h[4];
    size_t budget = 400000;   // block headers read, over all candidate starts
    for (size_t o = 0x50; o + 4 <= h.size() && o <= 0x70; o++) {
        if (le32(&h[o]) != TAG_NAME) continue;
        if (walk(f, o, f.size(), p.top, budget)) break;
        p.top.clear();
    }
    if (p.top.empty()) throw ParseError("DirectWave: no readable block stream (a .dwb bank or a damaged file)");
    for (const Block &b : p.top)
        if (b.tag == TAG_NAME) { p.name = text(f, b); break; }
    return p;
}

std::vector<PresetInfo> list_dwp(VolumePtr vol, const std::string &path) {
    Program p = read_program(*vol->open(path));
    return {{p.name.empty() ? path_stem(path) : p.name, 0}};
}

bool probe_dwp(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    return n >= 8 && size >= 0x50 + 12 && !std::memcmp(h, "DwPr", 4);
}

double knob_time(double pos) { return 9.07 * std::pow(clampd(pos, 0, 1), 3.96); }

double strength(double amount) {   // bipolar around 0.5, cubic
    double d = clampd(amount, 0, 1) - 0.5;
    double s = clampd(std::fabs(d) * 2, 0, 1);
    return (d < 0 ? -1 : 1) * s * s * s;
}

bool lfo_from(const std::vector<uint8_t> &b, Lfo &lfo) {
    if (b.size() < 8) return false;
    double pos = clampd(f32(b, 4), 0, 1);
    double rate = 20 * pos * pos;
    if (rate <= 0) return false;
    static const LfoWave waves[8] = {LfoWave::Sine, LfoWave::Sine, LfoWave::Triangle, LfoWave::Square,
                                     LfoWave::SawUp, LfoWave::SawDown, LfoWave::Random, LfoWave::Random};
    uint32_t w = u32(b, 0);
    lfo.set = true;
    lfo.wave = w < 8 ? waves[w] : LfoWave::Sine;
    lfo.rate_hz = rate;
    return true;
}

// A sample file: by the stored path's file name (or name + ".wav") beside the program, in "<program stem>/", or in a
// folder named like the stored path's folder.
std::string find_sample(Volume &vol, const std::string &dwp, const std::string &name, const std::string &stored) {
    std::string file, folder;
    if (!stored.empty()) {
        std::string s = stored;
        std::replace(s.begin(), s.end(), '\\', '/');
        file = path_name(s);
        folder = path_name(path_dir(s));
    }
    if (file.empty() && !name.empty()) file = name + ".wav";
    if (file.empty() || file == "." || file == "..") return "";
    std::string dir = path_dir(dwp);
    std::vector<std::string> cands = {path_join(dir, file), path_join(path_join(dir, path_stem(dwp)), file)};
    if (!folder.empty() && folder != "." && folder != ".." && folder.find(':') == std::string::npos)
        cands.push_back(path_join(path_join(dir, folder), file));
    for (const std::string &c : cands) {
        std::string found;
        try { if (find_ci(vol, c, found)) return found; } catch (const std::exception &) {}
    }
    return "";
}

SampleRefPtr flac_ref(BlobPtr f, uint64_t off, uint64_t len, const std::string &key, const std::string &name) {
    auto r = std::make_shared<SampleRef>();
    r->key = key;
    r->name = name;
    r->decode = [f, off, len]() -> PcmPtr { return decode_audio(*make_slice(f, off, len)); };
    return r;
}

Instrument load_dwp(VolumePtr vol, const std::string &path, int) {
    BlobPtr f = vol->open(path);
    Program prog = read_program(*f);
    Instrument inst;
    inst.format = "DirectWave program";
    inst.name = prog.name.empty() ? path_stem(path) : prog.name;
    const bool v26 = prog.version == 0x26;
    const float env_defaults[4] = {0.0f, v26 ? 1.0f : 0.5f, 1.0f, v26 ? 0.18f : 0.25f};

    size_t containers = 0, missing = 0, damaged = 0, bad_embedded = 0;
    std::string first_missing;
    bool wheel_skipped = false;
    std::map<std::string, SampleRefPtr> refs;
    std::vector<Block> inner;
    size_t budget = 400000;   // block headers read in all containers
    for (const Block &cb : prog.top) {
        if (cb.tag != TAG_CONTAINER) continue;
        if (++containers > MAX_ZONES) throw ParseError("DirectWave: too many zones");
        if (!walk(*f, cb.offset, cb.offset + cb.length, inner, budget)) { damaged++; continue; }
        std::vector<uint8_t> mapping, format, filter, env;
        std::vector<std::vector<uint8_t>> lfos, mods;
        std::string sname, spath;
        const Block *pcm = nullptr, *flac = nullptr;
        for (const Block &b : inner) {
            switch (b.tag) {
            case TAG_MAPPING: mapping = payload(*f, b, 64); break;
            case TAG_SAMPLE_NAME: sname = text(*f, b); break;
            case TAG_SAMPLE_PATH: spath = text(*f, b); break;
            case TAG_FORMAT: format = payload(*f, b, 64); break;
            case TAG_FILTER: if (filter.empty()) filter = payload(*f, b, 64); break;
            case TAG_AMP_ENV: env = payload(*f, b, 64); break;
            case TAG_LFO: if (lfos.size() < 2) lfos.push_back(payload(*f, b, 64)); break;
            case TAG_MOD: if (mods.size() < 64) mods.push_back(payload(*f, b, 64)); break;
            case TAG_EMBEDDED_PCM: pcm = &b; break;
            case TAG_EMBEDDED_FLAC: flac = &b; break;
            default: break;
            }
        }
        Zone z;
        z.name = sname;
        const int64_t frames = format.size() >= 4 ? int64_t(u32(format, 0)) : 0;
        int64_t guard = 0;   // embedded audio: silent frames before the sample

        // embedded audio first (its loop points are exact for it), then a sample file
        if (flac && flac->length > 8 + 4) {
            uint64_t off = flac->offset + 8, avail = flac->length - 8;
            std::vector<uint8_t> hd = f->bytes(flac->offset, 12);
            uint64_t stored = le32(hd.data());
            uint64_t len = stored > 0 && stored <= avail ? stored : avail;
            if (!std::memcmp(hd.data() + 8, "fLaC", 4)) {
                try {
                    AudioInfo ai = probe_audio(*make_slice(f, off, len));
                    if (frames > 0 && ai.frames > frames) guard = (ai.frames - frames) / 2;
                    z.sample = flac_ref(f, off, len, "dwp:" + std::to_string(reinterpret_cast<uintptr_t>(f.get())) + ":" + std::to_string(off), sname);
                } catch (const ParseError &) { bad_embedded++; }
            } else bad_embedded++;
        }
        if (!z.sample && pcm) {
            int channels = format.size() >= 12 ? int(u32(format, 8)) : 0;
            double rate = format.size() >= 20 ? f32(format, 16) : 0;
            int bits = format.size() >= 40 ? int(u32(format, 36)) : 0;
            Enc enc = bits == 16 ? Enc::S16LE : bits == 24 ? Enc::S24LE : Enc::F32LE;
            int bps = bits == 16 ? 2 : bits == 24 ? 3 : 4;
            if (frames > 0 && channels >= 1 && channels <= 2 && rate >= 1000 && rate <= 384000 &&
                (bits == 16 || bits == 24 || bits == 32) && pcm->length % uint64_t(channels) == 0) {
                uint64_t plane = pcm->length / uint64_t(channels);
                int64_t plane_frames = int64_t(plane / uint64_t(bps));
                if (plane % uint64_t(bps) == 0 && plane_frames >= frames) {
                    guard = (plane_frames - frames) / 2;
                    z.sample = raw_sample_ref(f, pcm->offset, plane_frames, channels, enc, int(std::lround(rate)),
                                              sname.empty() ? inst.name : sname, true);
                } else bad_embedded++;
            } else bad_embedded++;
        }
        if (!z.sample) {
            std::string p = find_sample(*vol, path, sname, spath);
            if (p.empty()) {
                if (!missing++) first_missing = spath.empty() ? sname : spath;
                continue;
            }
            auto it = refs.find(p);
            z.sample = it != refs.end() ? it->second : (refs[p] = file_sample_ref(vol, p));
            if (z.name.empty()) z.name = path_stem(p);
        }

        if (mapping.size() >= 5) {
            z.root = std::min(127, int(mapping[0]));
            z.key_lo = std::min(127, int(mapping[1]));
            z.key_hi = std::min(127, int(mapping[2]));
            z.vel_lo = std::min(127, int(mapping[3]));
            z.vel_hi = std::min(127, int(mapping[4]));
            if (mapping.size() >= 17) {
                double gain = f32(mapping, 9), pan = f32(mapping, 13);
                if (gain >= 0 && std::fabs(gain - 1) > 0.001) z.gain_db = std::max(-96.0, 20 * std::log10(std::max(1.58e-5, gain)));
                if (std::fabs(pan - 0.5) > 0.001) z.pan = clampd((pan - 0.5) * 2, -1, 1);
            }
        }
        if (guard > 0) {
            z.start = guard;
            z.stop = guard + frames;
        }
        if (format.size() >= 32) {
            uint32_t mode = u32(format, 20);
            if (mode == 1) z.one_shot = true;
            else if (mode >= 2 && mode <= 4) {
                Loop l;
                l.type = mode == 4 ? LoopType::Alternating : LoopType::Forward;
                l.until_release = mode == 3;
                l.start = int64_t(u32(format, 24));
                l.end = int64_t(u32(format, 28));
                if (frames > 0) l.end = std::min(l.end, frames - 1);   // a few factory loops end past the sample
                if (l.end > l.start) {
                    l.start += guard;
                    l.end += guard;
                    z.loops.push_back(l);
                }
            }
        }
        if (env.size() >= 16) {
            float pos[4];
            bool is_default = true;
            for (int i = 0; i < 4; i++) {
                pos[i] = float(clampd(f32(env, size_t(i) * 4), 0, 1));
                if (std::fabs(pos[i] - env_defaults[i]) > 0.005f) is_default = false;
            }
            if (!is_default) {
                z.amp_env.set = true;
                z.amp_env.attack = knob_time(pos[0]);
                z.amp_env.decay = knob_time(pos[1]);
                z.amp_env.sustain = pos[2];
                z.amp_env.release = knob_time(pos[3]);
            }
        }
        if (filter.size() >= 12) {
            uint32_t t = u32(filter, 0);
            if (t >= 1 && t <= 4) {
                static const FilterType types[5] = {FilterType::None, FilterType::LowPass, FilterType::HighPass,
                                                    FilterType::BandPass, FilterType::BandReject};
                z.filter.type = types[t];
                z.filter.poles = 4;
                z.filter.cutoff = clampd(45.0 * std::pow(20000.0 / 45.0, clampd(f32(filter, 4), 0, 1)), 45, 20000);
                z.filter.resonance = clampd(f32(filter, 8), 0, 1);
            }
        }
        for (size_t s = 0; s < mods.size(); s++) {
            const std::vector<uint8_t> &m = mods[s];
            if (m.size() < 8) continue;
            int src = u16(m, 0), dst = u16(m, 2);
            int li = src == 11 ? 0 : src == 12 ? 1 : -1;
            if (li < 0 || size_t(li) >= lfos.size()) continue;
            double st = strength(f32(m, 4));
            for (const auto &w : mods)   // a mod wheel slot aimed at this slot's amount (Mod Amt = 32 + slot)
                if (w.size() >= 8 && u16(w, 0) == 3 && u16(w, 2) == int(32 + s)) wheel_skipped = true;
            if (std::fabs(st) < 1e-4) continue;
            Lfo lfo;
            if (!lfo_from(lfos[size_t(li)], lfo)) continue;
            if (dst == 1) {
                z.pitch_lfo = lfo;
                z.pitch_lfo_depth = clampd(st * 2400.0 / MAX_ENVELOPE_DEPTH, -1, 1);
            } else if (dst == 2) {
                double idx = std::min(std::fabs(st), 0.9999);
                double db = -20 * std::log10(1 - idx);
                z.amp_lfo = lfo;
                z.amp_lfo_depth = clampd((st < 0 ? -db : db) / MAX_VOLUME_DEPTH, -1, 1);
            } else if (dst == 7 && z.filter.type != FilterType::None) {
                z.filter.lfo = lfo;
                z.filter.lfo_depth = clampd(st * 12800.0 / MAX_ENVELOPE_DEPTH, -1, 1);
            }
        }
        inst.zones.push_back(std::move(z));
    }
    if (missing) inst.warnings.push_back("DirectWave: " + std::to_string(missing) + " sample(s) not found, e.g. " + first_missing);
    if (damaged) inst.warnings.push_back("DirectWave: " + std::to_string(damaged) + " damaged zone(s) skipped");
    if (bad_embedded) inst.warnings.push_back("DirectWave: " + std::to_string(bad_embedded) + " embedded sample(s) unreadable");
    if (wheel_skipped) inst.warnings.push_back("DirectWave: mod wheel control of LFO depth is not read");
    inst.warnings.push_back("DirectWave: second filter, zone envelopes and effects are not read");
    if (inst.zones.empty()) {
        if (!containers) throw ParseError("DirectWave: the program has no zones");
        if (missing == containers) throw ParseError("DirectWave: none of the " + std::to_string(missing) + " samples found, e.g. " + first_missing);
        throw ParseError("DirectWave: no playable zones (" + std::to_string(damaged) + " damaged, " +
                         std::to_string(bad_embedded) + " unreadable embedded, " + std::to_string(missing) + " missing)");
    }
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_directwave() {
    register_reader({"DirectWave program", "dwp", probe_dwp, list_dwp, load_dwp});
}

}  // namespace omni

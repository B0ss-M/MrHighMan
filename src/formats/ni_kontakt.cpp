// Omni Sampler: Native Instruments Kontakt instruments and multis (.nki .nkm), all generations:
//  - Kontakt 1 (NiSS XML, zlib), Kontakt 2 - 4.1 (K2 XML, zlib, optional monolith with the samples inside),
//  - Kontakt 4.2 (FastLZ-compressed binary preset chunks), Kontakt 5 - 8 (NI container 'hsin' with preset chunks),
//  - Kontakt 5+ monoliths ("/\ NI FC MTD  /\" file container with the .nki and its samples).
// Samples: WAV, AIFF, NCW. Encrypted (commercial, Kontakt Player) libraries cannot be read and are reported so.
// Translated from ConvertWithMoss's format/ni/kontakt and format/ni/nicontainer packages (LGPL-3.0).
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include "../core/parse.hpp"
#include "format.hpp"
#include "ni_common.hpp"

namespace omni {
namespace {

constexpr uint32_t K1_INST_LE = 0x5EE56EB3, K1_INST_BE = 0xB36EE55E, K1_MULTI_LE = 0x5AE5D6A4, K1_MULTI_BE = 0xA4D6E55A;
constexpr uint32_t K2_INST_LE = 0x1290A87F, K2_INST_BE = 0x7FA89012, K2_MULTI_LE = 0x01EF85AB, K2_MULTI_BE = 0xAB85EF01;
constexpr uint32_t K5_MONOLITH = 0x2F5C204E;
constexpr uint32_t NKR_HEADER = 0x5E70AC54, NKR_WALLPAPER = 0x2AE905FA, NKR_NKI = 0x4916E63C, NKR_SAMPLE = 0x16CCF80A, NKR_SAMPLE_RAW = 0x0040179F;

// ---------------------------------------------------------------------------------------------------------------
// the result of parsing: programs in a neutral form, then converted to the model

struct KLoop { int mode = 0, start = 0, length = 0; bool alternating = false; float tuning = 1; int xfade = 0; };
struct KEnv { bool set = false; float attack = 0, hold = 0, decay = 500, sustain = 1, release = 300, curve = 0; bool ahd = false; double depth = 1; };
struct KZone {
    int group = 0, sample_start = 0, sample_end = 0, lo_vel = 0, hi_vel = 127, lo_key = 0, hi_key = 127;
    int fade_lo_vel = 0, fade_hi_vel = 0, fade_lo_key = 0, fade_hi_key = 0, root = 60;
    float volume = 1, pan = 0, tune = 1;
    int file = -1, frames = 0;
    std::vector<KLoop> loops;
    bool has_range = true, from_xml = false;
};
struct KGroup {
    std::string name;
    float volume = 1, pan = 0, tune = 1;
    bool key_tracking = true, reverse = false, release_trigger = false, muted = false, soloed = false, one_shot = false;
    int voice_group = -1, rr_position = -1;
    bool round_robin = false;
    int bend_cents = -1;
    double vel_amp = -1;
    KEnv amp, pitch, cutoff;
    bool has_filter = false; FilterType ftype = FilterType::None; int fpoles = 2; double fcutoff = 20000, fres = 0;
};
struct KProgram {
    std::string name;
    float volume = 1, pan = 0, tune = 1;
    bool group_solo = false;
    int midi_channel = -1, clip_lo = 0, clip_hi = 127;
    std::vector<KGroup> groups;
    std::vector<KZone> zones;
};
struct KDoc {
    std::vector<KProgram> programs;
    std::vector<std::string> files;                         // sample paths (binary format)
    std::map<std::string, std::pair<uint64_t, uint64_t>> monolith;   // file name (lower) -> (offset, size) in the .nki
    bool xml_tuning_k2 = true;
    std::string version;
};

// ---------------------------------------------------------------------------------------------------------------
// small readers

std::string ascii_len(Reader &r) {
    uint32_t n = r.u32le();
    if (n > r.left()) throw ParseError("bad string length");
    return std::string(reinterpret_cast<const char *>(r.take(n)), n);
}

// ---------------------------------------------------------------------------------------------------------------
// Kontakt 4.2 / 5+ preset chunks

enum : int { C_PAR_MOD_BASE = 0x00, C_BANK = 0x03, C_GROUP = 0x04, C_PAR_SCRIPT = 0x06, C_EXT_MOD = 0x0C, C_INT_MOD = 0x0D,
             C_FX_SEND = 0x17, C_PAR_FX = 0x25, C_PROGRAM = 0x28, C_PROGRAM_CONTAINER = 0x29, C_ZONE = 0x2C, C_VOICE_GROUPS = 0x32,
             C_GROUP_LIST = 0x33, C_ZONE_LIST = 0x34, C_PROGRAM_LIST = 0x36, C_SLOT_LIST = 0x37, C_LOOP_ARRAY = 0x39,
             C_PARAM_8 = 0x3A, C_PARAM_16 = 0x3B, C_PARAM_32 = 0x3C, C_FILENAME_LIST = 0x3D, C_INSERT_BUS = 0x45,
             C_SAVE_SETTINGS = 0x47, C_MULTI_CONFIG = 0x48, C_FILENAME_LIST_EX = 0x4B, C_QUICK_BROWSE = 0x4E };

struct Chunk {
    int id = -1, version = -1;
    std::vector<uint8_t> pub, priv;
    std::vector<Chunk> children;
};

void read_structure(Reader &r, size_t size, Chunk &c);
void read_chunk(Reader &r, Chunk &c, int depth);

void read_array(Reader &r, size_t size, bool has_ref, Chunk &c) {
    uint32_t n = r.u32le();
    for (uint32_t i = 0; i < n && !r.eof(); i++) {
        Chunk child;
        child.id = has_ref ? int(r.u32le()) : -1;
        read_structure(r, size, child);
        c.children.push_back(std::move(child));
    }
}

void read_structure(Reader &r, size_t size, Chunk &c) {
    bool structured = r.u8() > 0;
    if (!structured) {
        if (size > 0) { size_t n = std::min(size - 1, r.left()); const uint8_t *p = r.take(n); c.pub.assign(p, p + n); }
        return;
    }
    c.version = r.u16le();
    uint32_t pn = r.u32le();
    const uint8_t *pp = r.take(pn);
    c.priv.assign(pp, pp + pn);
    uint32_t un = r.u32le();
    const uint8_t *up = r.take(un);
    c.pub.assign(up, up + un);
    uint32_t cn = r.u32le();
    Reader cr = r.sub(cn);
    while (!cr.eof()) {
        Chunk child;
        read_chunk(cr, child, 0);
        c.children.push_back(std::move(child));
    }
}

void read_chunk(Reader &r, Chunk &c, int depth) {
    if (depth > 64) throw ParseError("Kontakt chunks nested too deep");
    c.id = r.u16le();
    uint32_t size = r.u32le();
    Reader o = r.sub(size);
    if (size == 0) return;
    switch (c.id) {
    case C_GROUP_LIST: read_array(o, size, false, c); for (auto &ch : c.children) ch.id = C_GROUP; break;
    case C_ZONE_LIST: read_array(o, size, true, c); break;
    case C_BANK: case C_PROGRAM_CONTAINER: case C_PROGRAM: case C_PAR_SCRIPT: case C_FX_SEND: case C_VOICE_GROUPS:
    case C_PARAM_8: case C_INSERT_BUS: case C_SAVE_SETTINGS: case C_QUICK_BROWSE:
        read_structure(o, size, c);
        break;
    case C_PARAM_16: case C_PARAM_32: {
        int count = c.id == C_PARAM_16 ? 16 : 32;
        if (o.u8() != 0) throw ParseError("bad Kontakt parameter array");
        c.version = o.u16le();
        for (int i = 0; i < count && !o.eof(); i++)
            if (o.u8() > 0) {
                Chunk ch;
                ch.id = o.u16le();
                uint32_t ds = o.u32le();
                const uint8_t *p = o.take(ds);
                ch.pub.assign(p, p + ds);
                c.children.push_back(std::move(ch));
            }
        break;
    }
    default: { const uint8_t *p = o.take(size); c.pub.assign(p, p + size); break; }
    }
}

void find_all(const std::vector<Chunk> &cs, int id, std::vector<const Chunk *> &out) {
    for (auto &c : cs) {
        if (c.id == id) out.push_back(&c);
        else find_all(c.children, id, out);
    }
}

KEnv read_int_mod(const Chunk &c, std::string &target) {
    KEnv env;
    Reader r(c.pub);
    if (c.id == C_PAR_MOD_BASE) r.u32le();
    int main = r.u8();
    if (main == 0) main = r.u8();
    if (main != 1) throw ParseError("unknown internal modulator");
    int version = r.u16le();
    if (version < 0x80 || version > 0x81) throw ParseError("unknown internal modulator version");
    uint32_t s1 = r.u32le();
    Reader b1 = r.sub(s1);
    struct MP { std::string name, desc; float intensity; bool points = false; };
    std::vector<MP> params;
    uint32_t count = b1.u32le();
    // the padding rules of ConvertWithMoss's InternalModulator.needsPadding, reduced to the cases that matter
    auto padding = [](size_t pos, const MP &m) -> int {
        static const std::map<std::string, std::set<std::string>> needs = {
            {"eqGain1", {"LFO_SINE_EQ_GAIN_1"}},
            {"filterCutoff", {"", "<none>", "LFO_SINE_CUTOFF", "STEP_CUTOFF", "ENV_AHDSR_CUTOFF", "LFO_SAW_CUTOFF", "LFO_RECT_CUTOFF", "LFO_MULTI_CUTOFF", "ENV_DBD_CUTOFF", "ENV FLW_CUTOFF"}},
            {"filterQ", {"LFO_SINE_RESONANCE", "STEP_RESONANCE"}}, {"frequency", {"ENV_DBD_FREQUENCY", "ENV_AHDSR_FREQUENCY"}},
            {"formantTalk", {"LFO_SINE_TALK"}}, {"intensity", {"LFO_SINE_FREQUENCY"}}, {"loopStart", {"ENV_AHDSR_LOOP_START"}},
            {"pitch", {"LFO_RECT_PITCH"}}, {"vfType", {"LFO_MULTI_3X2_TYPE"}}, {"volume", {"GLIDE_VOLUME", "LFO_SAW_VOLUME"}},
            {"bitdepth", {"ENV_AHDSR_BITDEPTH"}}, {"dyx_morph", {"ENV_AHDSR_MORPH", "LFO_MULTI_MORPH", "LFO_TRI_MORPH"}},
            {"dyx_dry", {"ENV_AHDSR_MORPH"}}, {"downsample", {"LFO_SINE_SAMPLE_RATE"}}};
        static const std::map<std::string, std::set<std::string>> none = {
            {"filterCutoff", {"ENV FLW_CUTOFF", "ENV_FILTER"}}, {"formantShift", {"<none>", "ENV_AHDSR_FORMANT"}},
            {"grainSpeed", {"LFO_MULTI_CUTOFF", "LFO_MULTI_SPEED", "LFO_SAW_SPEED", "ENV_AHDSR_SPEED"}},
            {"pan", {"<none>", "LFO_SINE_PAN", "LFO_TRI_PAN", "LFO_RECT_PAN", "LFO_TRI_VOLUME", "ENV_DBD_PAN"}},
            {"pitch", {"", "<none>", "GLIDE_PITCH", "LFO_SINE_PITCH", "LFO_TRI_PITCH", "LFO_RAND_PITCH", "STEP_PITCH", "ENV_AHDSR_PITCH", "ENV_DBD_PITCH", "ENV_PITCH_MIX"}},
            {"volume", {"<none>", "ENV_AHDSR_VOLUME", "GLIDE_VOLUME", "LFO_SINE_VOLUME", "LFO_RECT_VOLUME", "STEP_VOLUME", "VOL_GATE"}}};
        if (pos > 0) {
            if (m.name == "filterCutoff" && m.desc == "ENV_AHDSR_CUTOFF") return 2;
            if (m.name == "volume" && (m.desc == "LFO_SINE_VOLUME" || m.desc == "STEP_VOLUME")) return 1;
            if ((m.name == "pan" && m.desc == "ENV_DBD_PAN") || (m.name == "filterCutoff" && m.desc == "LFO_MULTI_CUTOFF")) return 2;
            if (m.name == "vfType" && m.desc == "LFO_MULTI_3X2_TYPE") return 2;
        }
        auto a = needs.find(m.name);
        if (a != needs.end() && a->second.count(m.desc)) return 1;
        auto b = none.find(m.name);
        if (b != none.end() && b->second.count(m.desc)) return 0;
        return (m.name.size() + m.desc.size()) % 2 == 1 ? 1 : 0;
    };
    for (uint32_t i = 0; i < count; i++) {
        MP m;
        m.name = ascii_len(b1);
        m.intensity = b1.f32le();
        if (b1.u16le() != 0xFFFF) throw ParseError("unknown internal modulator block");
        int flags = b1.u8();
        b1.s16le();
        if (flags & 8) b1.skip(5);
        else {
            m.desc = ascii_len(b1);
            int pad = padding(i, m);
            if (pad) b1.skip(size_t(pad));
            if (b1.u8() > 0) {
                int type = b1.u8();
                b1.u8();
                if (type == 1) b1.skip(128 * 4);
                else if (type == 2) { int np = b1.u8(); b1.skip(size_t(np) * 12); m.points = true; }
                else { params.push_back(m); break; }
            }
        }
        params.push_back(m);
    }
    std::string source = "ENV_AHDSR";
    int source_index = 0;
    try {
        if (!params.empty()) {
            if (version != 0x81 || !params.back().points) b1.u8();
            b1.u8(); b1.u8(); b1.u8(); b1.u8();
            b1.u32le();
            source = ascii_len(b1);
            source_index = int(b1.u32le());
        }
    } catch (const ParseError &) {}
    if (r.u32le() != 0) throw ParseError("internal modulator separator");
    uint32_t s2 = r.u32le();
    Reader b2 = r.sub(s2);
    const MP *mp = nullptr;
    for (auto &m : params) if (m.name == "volume" || m.name == "filterCutoff" || m.name == "pitch") { mp = &m; break; }
    bool is_env = source_index == 0 || source_index == 2;
    if (!mp || !is_env || b2.left() < 59) return env;
    if (source == "ENV_AHDSR" || source == "<none>" || source.empty()) {
        b2.skip(34);
        env.curve = b2.f32le(); env.attack = b2.f32le(); env.decay = b2.f32le(); env.hold = b2.f32le();
        env.release = b2.f32le(); env.sustain = b2.f32le(); env.ahd = b2.u8() > 0;
        env.set = true;
        env.depth = mp->intensity;
        target = mp->name;
    }
    return env;
}

void read_ext_mod(const Chunk &c, KGroup &g) {
    Reader r(c.pub);
    try {
        r.skip(3);
        uint32_t dest = r.u32le();
        r.u32le();
        ascii_len(r);
        float intensity = r.f32le();
        r.skip(3);
        r.u16le();
        std::string mod = ascii_len(r);
        if (!mod.empty()) r.u8();
        r.u16le();
        ascii_len(r);
        r.u32le();
        uint32_t source = r.u32le();
        if (source == 0x01 && (dest == 0x3D || dest == 0x38)) g.bend_cents = int(std::lround(intensity * 12)) * 100;
        else if ((source == 0x00 || source == 0x06) && (dest == 0x3E || dest == 0x3B || dest == 0x41)) g.vel_amp = intensity;
    } catch (const ParseError &) {}
}

KGroup parse_group(const Chunk &c) {
    if (c.version > 0x9C) throw ParseError("unsupported Kontakt group version");
    Reader r(c.pub);
    KGroup g;
    g.name = utf16_len(r);
    g.volume = r.f32le(); g.pan = r.f32le(); g.tune = r.f32le();
    g.key_tracking = r.u8() > 0; g.reverse = r.u8() > 0; g.release_trigger = r.u8() > 0; r.u8();
    r.s32le(); r.s16le();
    g.voice_group = r.s32le();
    r.s32le();
    g.muted = r.u8() > 0; g.soloed = r.u8() > 0;
    for (auto &ch : c.children) {
        if (ch.id == C_PARAM_16) {
            for (auto &m : ch.children) {
                if (m.id != C_INT_MOD && m.id != C_PAR_MOD_BASE) continue;
                try {
                    std::string target;
                    KEnv e = read_int_mod(m, target);
                    if (!e.set) continue;
                    if (target == "volume") g.amp = e;
                    else if (target == "pitch") g.pitch = e;
                    else if (target == "filterCutoff") g.cutoff = e;
                } catch (const ParseError &) {}
            }
        } else if (ch.id == C_PARAM_32) {
            for (auto &m : ch.children) if (m.id == C_EXT_MOD) read_ext_mod(m, g);
        }
    }
    return g;
}

KZone parse_zone(const Chunk &c) {
    if (c.version > 0x9C) throw ParseError("unsupported Kontakt zone version");
    Reader r(c.pub);
    KZone z;
    z.group = c.id;
    z.sample_start = int(r.u32le()); z.sample_end = int(r.u32le()); r.u32le();
    z.lo_vel = r.u16le(); z.hi_vel = r.u16le(); z.lo_key = r.u16le(); z.hi_key = r.u16le();
    z.fade_lo_vel = r.u16le(); z.fade_hi_vel = r.u16le(); z.fade_lo_key = r.u16le(); z.fade_hi_key = r.u16le();
    z.root = r.u16le();
    z.volume = r.f32le(); z.pan = r.f32le(); z.tune = r.f32le();
    if (c.version >= 0x9A) { r.u8(); r.u8(); r.u32le(); if (r.eof()) return z; }
    z.file = int(r.u32le());
    r.u32le(); r.u32le(); r.u8();
    z.frames = int(r.u32le());
    for (auto &ch : c.children) {
        if (ch.id != C_LOOP_ARRAY || ch.pub.size() < 2) continue;
        Reader lr(ch.pub);
        int enabled = lr.u16le();
        for (int i = 0; i < 8; i++) {
            if (!(enabled & (1 << i))) continue;
            if (lr.u16le() != 0x60) throw ParseError("unknown Kontakt loop format");
            KLoop l;
            l.mode = lr.s32le(); l.start = int(lr.u32le()); l.length = int(lr.u32le()); lr.u32le();
            l.alternating = lr.u8() > 0; l.tuning = lr.f32le(); l.xfade = int(lr.u32le());
            if (!lr.eof()) lr.u8();
            z.loops.push_back(l);
        }
    }
    return z;
}

KProgram parse_program(const Chunk &c) {
    KProgram p;
    Reader r(c.pub);
    p.name = utf16_len(r);
    r.f64le();
    r.u8();
    p.volume = r.f32le(); p.pan = r.f32le(); p.tune = r.f32le();
    r.u8(); r.u8();
    p.clip_lo = r.u8(); p.clip_hi = r.u8();
    r.u16le(); r.u32le(); r.u32le(); r.u32le(); r.u32le();
    p.group_solo = r.u8() > 0;
    for (auto &ch : c.children) {
        if (ch.id == C_GROUP_LIST) for (auto &g : ch.children) p.groups.push_back(parse_group(g));
        else if (ch.id == C_ZONE_LIST) for (auto &z : ch.children) p.zones.push_back(parse_zone(z));
    }
    return p;
}

std::string read_file_segment_path(Reader &r) {
    std::string out;
    uint32_t segs = r.u32le();
    for (uint32_t s = 0; s < segs; s++) {
        int t = r.u8();
        switch (t) {
        case 0: { std::string d = trim(r.str(2)); out += d.empty() ? "/" : d + ":/"; break; }
        case 1: { std::string d = utf16_len(r); out += d.empty() ? "/" : d + ":/"; break; }
        case 2: out += utf16_len(r) + "/"; break;
        case 3: out += "../"; break;
        case 4: case 8: case 9: out += utf16_len(r); break;
        case 6: break;
        default: throw ParseError("unsupported Kontakt file path segment " + std::to_string(t));
        }
    }
    return out;
}

std::vector<std::string> parse_file_list(const Chunk &c) {
    Reader r(c.pub);
    std::vector<std::string> special, samples;
    if (c.id == C_FILENAME_LIST_EX) {
        int version = r.u16le();
        if (version < 2 || version > 3) throw ParseError("unsupported Kontakt file list version");
        if (version == 3) {
            int32_t n = r.s32le();
            r.skip(8);
            for (int32_t i = 0; i < n - 1; i++) {
                special.push_back(read_file_segment_path(r));
                if (!r.eof()) { r.skip(4); r.u32le(); r.skip(20); }
            }
            return special;
        }
    }
    if (!r.eof()) { int32_t n = r.s32le(); for (int32_t i = 0; i < n; i++) special.push_back(read_file_segment_path(r)); }
    if (!r.eof()) { int32_t n = r.s32le(); for (int32_t i = 0; i < n; i++) samples.push_back(read_file_segment_path(r)); }
    return samples;
}

void read_preset_chunks(const std::vector<uint8_t> &data, KDoc &doc) {
    std::vector<Chunk> top;
    Reader r(data);
    while (r.left() >= 6) { Chunk c; read_chunk(r, c, 0); top.push_back(std::move(c)); }
    std::vector<const Chunk *> progs;
    find_all(top, C_PROGRAM, progs);
    for (auto *p : progs) doc.programs.push_back(parse_program(*p));
    // multis keep their programs in the slot list of a bank
    for (auto &c : top) {
        if (c.id != C_BANK) continue;
        for (auto &ch : c.children) {
            if (ch.id != C_SLOT_LIST || ch.pub.size() < 8) continue;
            Reader sr(ch.pub);
            uint64_t flags = sr.u64le();
            for (int i = 0; i < 64; i++) {
                if (!(flags >> i & 1)) continue;
                Chunk pc;
                read_chunk(sr, pc, 0);
                if (pc.id != C_PROGRAM_CONTAINER) continue;
                for (auto &pl : pc.children) {
                    if (pl.id != C_PROGRAM_LIST) continue;
                    Reader pr(pl.pub);
                    Chunk arr;
                    read_array(pr, pl.pub.size(), false, arr);
                    for (auto &prog : arr.children) { KProgram kp = parse_program(prog); kp.midi_channel = i % 16; doc.programs.push_back(kp); }
                }
            }
        }
    }
    for (auto &c : top)
        if (c.id == C_FILENAME_LIST || c.id == C_FILENAME_LIST_EX) { doc.files = parse_file_list(c); break; }
}

// ---------------------------------------------------------------------------------------------------------------
// NI container ("hsin")

void read_container(const std::vector<uint8_t> &d, KDoc &doc) {
    NiContainer f = read_ni_container(d);
    if (f.app >= 0 && f.app != NI_APP_KONTAKT) throw ParseError("this NI file was not made by Kontakt");
    if (f.preset.empty()) {
        if (f.encrypted) throw ParseError("encrypted Kontakt library (Kontakt Player / NKS): cannot be read");
        throw ParseError("no Kontakt preset in this file");
    }
    read_preset_chunks(f.preset, doc);
}

// ---------------------------------------------------------------------------------------------------------------
// Kontakt 1 / 2 XML

const XmlNode *child(const XmlNode *n, const char *name) { return n ? n->child(name) : nullptr; }

std::map<std::string, std::string> value_map(const XmlNode *e) {
    std::map<std::string, std::string> m;
    if (!e) return m;
    for (auto *v : e->all("V")) m[v->attr("name")] = v->attr("value");
    return m;
}
std::map<std::string, std::string> params_of(const XmlNode *e) { return value_map(child(e, "Parameters")); }
double num(const std::map<std::string, std::string> &m, const char *k, double def) {
    auto it = m.find(k);
    return it == m.end() || it->second.empty() ? def : std::atof(it->second.c_str());
}

std::string decode_sample_path(const std::string &enc) {
    if (enc.empty() || enc[0] != '@') { std::string s = enc; for (char &c : s) if (c == '\\') c = '/'; return s; }
    std::string path;
    size_t i = 1;
    auto folder = [&]() {
        if (i + 3 > enc.size()) throw ParseError("bad Kontakt sample path");
        int len = std::atoi(enc.substr(i, 3).c_str());
        i += 3;
        std::string f = enc.substr(i, size_t(len));
        i += size_t(len);
        return f;
    };
    while (i < enc.size()) {
        char id = enc[i++];
        if (id == 'v') { std::string d = folder(); if (d.find(':') == std::string::npos) path += '/'; path += d + "/"; }
        else if (id == 'b') path += "../";
        else if (id == 'd') path += folder() + "/";
        else if (id == 'F' || id == 'f') {
            i += id == 'f' ? 6 : 11;
            if (i <= enc.size()) path += enc.substr(i);
            // %XXXX escapes
            std::string out;
            for (size_t k = 0; k < path.size(); k++) {
                if (path[k] == '%' && k + 4 < path.size() + 0 && isxdigit((unsigned char)path[k + 1])) {
                    uint16_t u = uint16_t(std::strtoul(path.substr(k + 1, 4).c_str(), nullptr, 16));
                    out += utf16_to_utf8(&u, 1);
                    k += 4;
                } else out += path[k];
            }
            return out;
        } else if (id == 'm') { folder(); path.clear(); }
        else throw ParseError("bad Kontakt sample path");
    }
    return path;
}

KEnv xml_env(const XmlNode *env, double depth) {
    KEnv e;
    if (!env) return e;
    auto m = value_map(env);
    if (!m.count("attack")) return e;
    e.set = true;
    e.curve = float(-num(m, "atkCurving", 0));
    e.attack = float(num(m, "attack", 0)); e.hold = float(num(m, "hold", 0)); e.decay = float(num(m, "decay", 0));
    e.ahd = env->attr("type") == "ahd";
    e.sustain = float(e.ahd ? 0 : num(m, "sustain", 1));
    e.release = float(e.ahd ? e.decay : num(m, "release", 300));
    e.depth = depth;
    return e;
}

void read_xml(const std::string &text, bool k2, KDoc &doc) {
    auto root = parse_xml(text);
    std::vector<const XmlNode *> programs;
    if (k2) {
        std::vector<const XmlNode *> containers;
        if (root->name == "K2_Container") containers.push_back(root.get());
        else for (auto *c : root->all("K2_Container")) containers.push_back(c);
        if (containers.empty()) if (const XmlNode *c = root->find("K2_Container")) containers.push_back(c);
        for (auto *c : containers) if (const XmlNode *ps = c->child("Programs")) for (auto *p : ps->all("K2_Program")) programs.push_back(p);
    } else {
        if (root->name == "NiSS_Program") programs.push_back(root.get());
        else for (auto *p : root->all("NiSS_Program")) programs.push_back(p);
    }
    const char *G = k2 ? "K2_Group" : "NiSS_Group", *Z = k2 ? "K2_Zone" : "NiSS_Zone";
    const char *INT = k2 ? "K2_IntMod" : "NiSS_IntMod", *EXT = k2 ? "K2_ExtMod" : "NiSS_ExtMod";
    auto pan_norm = [k2](double v) { return k2 ? v : (v - 0.5) / 0.5; };
    auto top_params = params_of(root.get());
    int prog_index = 0;
    for (auto *pe : programs) {
        if (!pe->has_attr("name")) continue;
        KProgram p;
        p.name = pe->attr("name");
        auto pp = params_of(pe);
        p.volume = float(num(pp, k2 ? "volume" : "masterVolume", 1));
        p.tune = float(num(pp, k2 ? "tune" : "masterTune", 1));
        p.pan = float(pan_norm(num(pp, k2 ? "pan" : "masterPan", k2 ? 0 : 0.5)));
        char key[32];
        std::snprintf(key, sizeof key, "midiChannel_slot0%02d", prog_index);
        if (top_params.count(key)) p.midi_channel = (std::atoi(top_params[key].c_str()) - 1) % 16;
        if (!k2 && pp.count("midiChannel")) p.midi_channel = (std::atoi(pp["midiChannel"].c_str()) - 1) % 16;
        if (pp.count("lowKey")) p.clip_lo = std::atoi(pp["lowKey"].c_str());
        if (pp.count("highKey")) p.clip_hi = std::atoi(pp["highKey"].c_str());
        const XmlNode *groups = pe->child("Groups"), *zones = pe->child("Zones");
        if (!groups || !zones) continue;
        std::vector<const XmlNode *> zlist = zones->all(Z);
        for (auto *ge : groups->all(G)) {
            KGroup g;
            g.name = ge->attr("name");
            auto gp = params_of(ge);
            g.volume = float(num(gp, "volume", 1));
            g.pan = float(pan_norm(num(gp, "pan", k2 ? 0 : 0.5)));
            g.tune = float(num(gp, "tune", 1));
            g.key_tracking = gp.count("keyTracking") ? gp["keyTracking"] == "yes" : true;
            g.reverse = gp["reverse"] == "yes";
            if (gp.count("voiceGroup")) g.voice_group = std::atoi(gp["voiceGroup"].c_str());
            g.release_trigger = k2 && gp["releaseTrigger"] == "yes";
            // internal modulators (envelopes)
            if (const XmlNode *ims = ge->child("IntModulators"))
                for (auto *im : ims->all(INT)) {
                    const XmlNode *env = im->child("Envelope");
                    auto mp = value_map(im);
                    if (!env || mp["bypass"] == "yes") continue;
                    std::string target;
                    double intensity = 0;
                    if (k2) {
                        if (const XmlNode *ts = im->child("Targets"))
                            for (auto *t : ts->all("Target")) {
                                auto tm = value_map(t);
                                if (tm.count("target")) { target = tm["target"]; intensity = num(tm, "intensity", 0); break; }
                            }
                    } else { target = mp["target"]; intensity = num(mp, "intensity", 0); }
                    if (target == "volume" && value_map(env)["noteOffLessMode"] == "yes") g.one_shot = true;
                    if (intensity == 0) continue;
                    KEnv e = xml_env(env, intensity);
                    if (!e.set) continue;
                    if (target == "volume") g.amp = e;
                    else if (target == "pitch") g.pitch = e;
                    else if (target == "filterCutoff") g.cutoff = e;
                }
            if (const XmlNode *ems = ge->child("ExtModulators"))
                for (auto *em : ems->all(EXT)) {
                    auto mp = value_map(em);
                    if (mp["bypass"] == "yes" || !mp.count("source")) continue;
                    std::vector<std::map<std::string, std::string>> targets;
                    if (mp.count("target")) targets.push_back(mp);
                    else if (const XmlNode *ts = em->child("Targets")) for (auto *t : ts->all("Target")) targets.push_back(value_map(t));
                    for (auto &t : targets) {
                        double in = num(t, "intensity", 0);
                        if (mp["source"] == "pitchBend" && t["target"] == "pitch") g.bend_cents = clampi(int(std::lround(in * 1200)), -9600, 9600);
                        if (mp["source"] == "velocity" && t["target"] == "volume") g.vel_amp = in;
                    }
                }
            if (!k2)
                if (const XmlNode *fe = ge->child("Filter")) {
                    auto fm = value_map(fe);
                    std::string t = fm["type"];
                    if (t.size() > 6 && t.compare(t.size() - 4, 4, "pole") == 0) {
                        std::string kind = t.substr(0, 2);
                        g.has_filter = true;
                        g.ftype = kind == "hp" ? FilterType::HighPass : kind == "bp" ? FilterType::BandPass : FilterType::LowPass;
                        g.fpoles = std::atoi(t.substr(2).c_str());
                        g.fcutoff = std::pow(2.0, clampd(num(fm, "cutoff", 1), 0, 1) * std::log2(21800.0));
                        g.fres = num(fm, "resonance", 0);
                    }
                }
            if (k2)
                if (const XmlNode *gs = ge->child("GroupStart"))
                    if (const XmlNode *sc = gs->child("StartCriteria")) {
                        auto cm = value_map(sc);
                        if (cm["mode"].compare(0, 5, "cycle") == 0) { g.round_robin = true; g.rr_position = cm.count("cycleClass") ? std::atoi(cm["cycleClass"].c_str()) : -1; }
                    }
            int index = int(ge->attr_num("index", -1));
            int gi = int(p.groups.size());
            p.groups.push_back(g);
            for (auto *ze : zlist) {
                if (int(ze->attr_num("groupIdx", -1)) != index) continue;
                const XmlNode *se = ze->child("Sample");
                if (!se) continue;
                auto sm = value_map(se);
                std::string enc = sm.count(k2 ? "file_ex2" : "file") ? sm[k2 ? "file_ex2" : "file"] : sm[k2 ? "file_ex2" : "file_ex"];
                if (enc.empty()) continue;
                auto zp = params_of(ze);
                KZone z;
                z.from_xml = true;
                z.group = gi;
                z.file = int(doc.files.size());
                doc.files.push_back(decode_sample_path(enc));
                if (!zp.count("rootKey")) continue;
                z.root = int(num(zp, "rootKey", 60)); z.lo_key = int(num(zp, "lowKey", 0)); z.hi_key = int(num(zp, "highKey", 127));
                z.lo_vel = int(num(zp, "lowVelocity", 0)); z.hi_vel = int(num(zp, "highVelocity", 127));
                z.fade_lo_key = int(num(zp, "fadeLowKey", 0)); z.fade_hi_key = int(num(zp, "fadeHighKey", 0));
                z.fade_lo_vel = int(num(zp, "fadeLowVelo", 0)); z.fade_hi_vel = int(num(zp, "fadeHighVelo", 0));
                int ss = int(num(zp, "sampleStart", 0)), se2 = int(num(zp, "sampleEnd", 0));
                if (se2 > ss) { z.sample_start = ss; z.frames = se2; z.sample_end = 0; z.has_range = true; }
                else z.has_range = false;
                z.volume = float(num(zp, "zoneVolume", 1));
                z.tune = float(num(zp, "zoneTune", 1));
                z.pan = float(pan_norm(num(zp, "zonePan", k2 ? 0 : 0.5)));
                if (const XmlNode *ls = ze->child("Loops"))
                    for (auto *le : ls->all("Loop")) {
                        auto lm = value_map(le);
                        if (!lm.count("loopStart") || !lm.count("mode")) continue;
                        KLoop l;
                        std::string mode = lm["mode"];
                        l.mode = mode == "oneshot" ? 3 : mode == "until_release" ? 2 : mode == "until_end" ? 1 : 0;
                        l.start = int(num(lm, "loopStart", 0)); l.length = int(num(lm, "loopLength", 0));
                        l.alternating = lm["alternatingLoop"] == "yes";
                        l.tuning = float(num(lm, "loopTuning", 1)); l.xfade = int(num(lm, "xfadeLength", 0));
                        z.loops.push_back(l);
                    }
                p.zones.push_back(z);
            }
        }
        doc.programs.push_back(p);
        prog_index++;
    }
}

std::string inflate_text(const uint8_t *p, size_t n) {
    if (n < 2) throw ParseError("missing Kontakt XML");
    std::vector<uint8_t> x = inflate_raw(p + 2, n - 2);   // zlib header skipped: data may follow the stream
    return trim(std::string(x.begin(), x.end()));
}

// ---------------------------------------------------------------------------------------------------------------
// file reading

uint32_t rd32(const uint8_t *p, bool be) { return be ? be32(p) : le32(p); }
uint16_t rd16(const uint8_t *p, bool be) { return be ? be16(p) : le16(p); }

// Kontakt 2-4.1 monolith: directories, samples, then the NKI (zlib) at the end
size_t read_k2_monolith(const std::vector<uint8_t> &d, size_t pos, bool be, KDoc &doc) {
    std::vector<std::string> sample_names;
    std::vector<std::pair<uint64_t, uint64_t>> samples;
    while (pos + 8 <= d.size()) {
        uint32_t magic = rd32(&d[pos], be);
        if (magic == NKR_HEADER) {
            size_t p = pos + 4 + 10;
            uint32_t n = rd32(&d[p], be);
            p += 8;
            for (uint32_t i = 0; i < n && p + 8 <= d.size(); i++) {
                int len = rd16(&d[p], be);
                int type = int16_t(rd16(&d[p + 6], be));
                std::vector<uint16_t> u;
                for (int k = 0; k + 1 < len - 8; k += 2) u.push_back(rd16(&d[p + 8 + size_t(k)], be));
                size_t ul = 0;
                while (ul < u.size() && u[ul]) ul++;
                std::string name = utf16_to_utf8(u.data(), ul);
                std::string ln = lower(name);
                bool audio = ends_with_ci(ln, ".wav") || ends_with_ci(ln, ".ncw") || ends_with_ci(ln, ".aif");
                if (type == 2 || (type == 3 && audio)) sample_names.push_back(name);
                p += size_t(len);
            }
            pos = p;
        } else if (magic == NKR_WALLPAPER) {
            pos += 14;
            uint64_t len = be ? be64(&d[pos]) : le64(&d[pos]);
            pos += 8 + size_t(len);
        } else if (magic == NKR_NKI) {
            return pos + 27 + 170;
        } else if (magic == NKR_SAMPLE) {
            size_t p = pos + 4 + 2 + 13;
            uint32_t len = rd32(&d[p], be);
            p += 4 + 8;
            samples.push_back({p, len});
            pos = p + len;
        } else if (magic == NKR_SAMPLE_RAW) {
            size_t p = pos + 4 + 2 + 18, next = 0;
            for (size_t i = p; i + 6 <= d.size(); i++)
                if (rd32(&d[i], be) == NKR_SAMPLE && rd16(&d[i + 4], be) == 0x110) { next = i; break; }
            if (!next) throw ParseError("damaged Kontakt monolith");
            pos = next;
        } else throw ParseError("unknown Kontakt monolith block");
    }
    for (size_t i = 0; i < samples.size() && i < sample_names.size(); i++) doc.monolith[lower(path_name(sample_names[i]))] = samples[i];
    throw ParseError("no instrument in this Kontakt monolith");
}

KDoc read_doc(Volume &vol, const std::string &path, std::vector<uint8_t> &d) {
    KDoc doc;
    d = vol.open(path)->all(1u << 30);
    if (d.size() < 16) throw ParseError("not a Kontakt file");
    uint32_t id = le32(d.data());
    if (!std::memcmp(&d[12], "hsin", 4)) { doc.version = "Kontakt 5+"; read_container(d, doc); return doc; }
    if (!std::memcmp(d.data(), "/\\ NI FC MTD  /\\", 16)) {
        // Kontakt 5+ monolith: header, table of contents, files
        Reader r(d);
        r.skip(16 + 248 + 8);
        uint64_t count = r.u64le();
        r.u64le();
        if (!r.starts("/\\ NI FC TOC  /\\")) throw ParseError("bad Kontakt monolith");
        r.skip(16 + 600);
        struct F { std::string name; uint64_t end; };
        std::vector<F> files;
        for (uint64_t i = 0; i < count; i++) {
            r.u64le(); r.skip(16);
            std::string name = trim(r.utf16le(300));
            r.u64le();
            files.push_back({name, r.u64le()});
        }
        r.skip(8 + 16 + 16 + 592);
        uint64_t base = r.pos(), prev = 0;
        std::string main;
        size_t main_off = 0, main_len = 0;
        for (auto &f : files) {
            uint64_t off = base + prev, len = f.end - prev;
            prev = f.end;
            std::string ln = lower(f.name);
            if (ends_with_ci(ln, ".nki") || ends_with_ci(ln, ".nkm")) { if (main.empty()) { main = ln; main_off = size_t(off); main_len = size_t(len); } }
            else doc.monolith[lower(path_name(f.name))] = {off, len};
        }
        if (main.empty() || main_off + main_len > d.size()) throw ParseError("no instrument in this Kontakt monolith");
        std::vector<uint8_t> inner(d.begin() + long(main_off), d.begin() + long(main_off + main_len));
        doc.version = "Kontakt 5+ monolith";
        read_container(inner, doc);
        return doc;
    }
    if (id == K1_INST_LE || id == K1_MULTI_LE || id == K1_INST_BE || id == K1_MULTI_BE) {
        doc.version = "Kontakt 1";
        read_xml(inflate_text(&d[36], d.size() - 36), false, doc);
        return doc;
    }
    if (id == K2_INST_LE || id == K2_INST_BE || id == K2_MULTI_LE || id == K2_MULTI_BE) {
        bool be = id == K2_INST_BE || id == K2_MULTI_BE;
        if (rd32(&d[4], be) == 0x24) { doc.version = "Kontakt 1.5"; read_xml(inflate_text(&d[36], d.size() - 36), false, doc); return doc; }
        if (d.size() < 230) throw ParseError("truncated Kontakt file");
        uint32_t clen = rd32(&d[4], be);
        bool k42 = rd16(&d[8], be) == 0x110;
        bool monolith = rd32(&d[42], be) == 1;
        if (k42) {
            doc.version = "Kontakt 4.2";
            uint32_t ulen = rd32(&d[186], be);
            size_t at = 222;
            if (at + clen > d.size()) throw ParseError("truncated Kontakt file");
            read_preset_chunks(fastlz_decompress(&d[at], clen, ulen), doc);
            return doc;
        }
        doc.version = monolith ? "Kontakt 2-4 monolith" : "Kontakt 2-4";
        size_t at = 170;
        if (monolith) at = read_k2_monolith(d, at, be, doc);
        read_xml(inflate_text(&d[at], d.size() - at), true, doc);
        return doc;
    }
    if (id == K5_MONOLITH) throw ParseError("this Kontakt monolith type is not supported");
    throw ParseError("unknown Kontakt file type");
}

// ---------------------------------------------------------------------------------------------------------------
// sample lookup and conversion

SampleRefPtr locate(VolumePtr vol, const std::string &nki, const KDoc &doc, BlobPtr nki_blob, const std::string &file, NiSampleFinder &finder) {
    std::string base = lower(path_name(file));
    auto m = doc.monolith.find(base);
    if (m != doc.monolith.end()) {
        auto r = std::make_shared<SampleRef>();
        r->name = path_stem(file);
        r->key = "nkimono:" + nki + ":" + std::to_string(m->second.first);
        BlobPtr slice = make_slice(nki_blob, m->second.first, m->second.second);
        r->decode = [slice]() { return decode_audio(*slice); };
        return r;
    }
    std::string found = finder.find(file);
    if (!found.empty()) return file_sample_ref(vol, found);
    return nullptr;
}

Envelope to_env(const KEnv &k) {
    Envelope e;
    e.set = true;
    e.attack_slope = clampd(k.curve, -1, 1);
    e.attack = k.attack / 1000.0; e.hold = k.hold / 1000.0; e.decay = k.decay / 1000.0;
    e.sustain = k.ahd ? 0 : k.sustain;
    e.release = (k.ahd ? k.decay : k.release) / 1000.0;
    return e;
}

double log2tune(double f) { return f > 0 ? 12.0 * std::log2(f) : 0; }

std::vector<PresetInfo> list_kontakt(VolumePtr vol, const std::string &path) {
    std::vector<uint8_t> d;
    KDoc doc = read_doc(*vol, path, d);
    std::vector<PresetInfo> out;
    for (size_t i = 0; i < doc.programs.size(); i++)
        out.push_back({doc.programs[i].name.empty() ? "Instrument " + std::to_string(i + 1) : doc.programs[i].name, int(i)});
    if (out.empty()) throw ParseError("no instruments in this Kontakt file");
    if (out.size() == 1) out[0].name = path_stem(path);
    return out;
}

Instrument load_kontakt(VolumePtr vol, const std::string &path, int index) {
    std::vector<uint8_t> d;
    KDoc doc = read_doc(*vol, path, d);
    d.clear();
    d.shrink_to_fit();
    if (index < 0 || size_t(index) >= doc.programs.size()) throw ParseError("no such instrument");
    BlobPtr nki_blob = vol->open(path);
    const KProgram &p = doc.programs[size_t(index)];
    Instrument inst;
    inst.format = "NI " + doc.version;
    inst.name = p.name.empty() || (p.name == "Instrument 1" && p.groups.size() == 1) ? path_stem(path) : p.name;
    if (doc.programs.size() > 1 && !p.name.empty()) inst.name = p.name;
    inst.groups.clear();
    bool solo = false;
    if (p.group_solo) for (auto &g : p.groups) if (g.soloed) solo = true;
    for (auto &g : p.groups) inst.groups.push_back(Group{g.name, g.release_trigger ? Trigger::Release : Trigger::Attack});
    if (inst.groups.empty()) inst.groups.push_back(Group{});
    NiSampleFinder finder(vol, path);
    std::map<int, SampleRefPtr> refs;
    std::map<int, AudioInfo> infos;
    int missing = 0;
    for (const KZone &kz : p.zones) {
        if (kz.file < 0 || kz.file >= int(doc.files.size())) continue;
        if (kz.group < 0 || kz.group >= int(p.groups.size())) continue;
        const KGroup &g = p.groups[size_t(kz.group)];
        if (solo ? !g.soloed : g.muted) continue;
        auto r = refs.find(kz.file);
        if (r == refs.end()) {
            SampleRefPtr ref = locate(vol, path, doc, nki_blob, doc.files[size_t(kz.file)], finder);
            if (!ref) { missing++; continue; }
            r = refs.emplace(kz.file, ref).first;
        }
        Zone z;
        z.name = r->second->name.empty() ? path_stem(doc.files[size_t(kz.file)]) : r->second->name;
        z.group = kz.group;
        z.sample = r->second;
        z.trigger = g.release_trigger ? Trigger::Release : Trigger::Attack;
        if (kz.from_xml) {
            // WAV root/loops first, then the zone's own values
            auto ai = infos.find(kz.file);
            if (ai == infos.end()) {
                AudioInfo info;
                try {
                    auto m = doc.monolith.find(lower(path_name(doc.files[size_t(kz.file)])));
                    if (m != doc.monolith.end()) info = probe_audio(*make_slice(nki_blob, m->second.first, m->second.second));
                } catch (const std::exception &) {}
                ai = infos.emplace(kz.file, info).first;
            }
            z.loops = ai->second.loops;
            if (kz.has_range) { z.start = kz.sample_start; z.stop = kz.frames; }
        } else {
            z.start = kz.sample_start;
            if (kz.frames > 0) z.stop = kz.frames - kz.sample_end;
        }
        z.key_lo = kz.lo_key; z.key_hi = kz.hi_key; z.root = kz.root;
        z.vel_lo = kz.lo_vel; z.vel_hi = kz.hi_vel;
        z.key_xfade_lo = kz.fade_lo_key; z.key_xfade_hi = kz.fade_hi_key;
        z.vel_xfade_lo = kz.fade_lo_vel; z.vel_xfade_hi = kz.fade_hi_vel;
        double vol_lin = double(p.volume) * g.volume * kz.volume;
        z.gain_db = vol_lin > 0 ? 20 * std::log10(vol_lin) : -96;
        z.pan = clampd(double(p.pan) + g.pan + kz.pan, -1, 1);
        z.tune = log2tune(double(kz.tune) * g.tune * p.tune);
        z.key_tracking = g.key_tracking ? 1 : 0;
        z.reverse = g.reverse;
        z.exclusive_group = g.voice_group < 0 ? 0 : g.voice_group + 1;
        if (g.bend_cents >= 0) { z.bend_up = g.bend_cents; z.bend_down = -g.bend_cents; }
        if (g.vel_amp >= 0) { z.amp_vel_depth = clampd(g.vel_amp, 0, 1); z.amp_vel_curve = 1; }
        else if (!kz.from_xml) z.amp_vel_depth = 0;
        if (g.amp.set) z.amp_env = to_env(g.amp);
        if (g.pitch.set) { z.pitch_env = to_env(g.pitch); z.pitch_env_depth = g.pitch.depth; }
        if (g.has_filter) {
            z.filter.type = g.ftype; z.filter.poles = g.fpoles; z.filter.cutoff = g.fcutoff; z.filter.resonance = g.fres;
            if (g.cutoff.set) { z.filter.env = to_env(g.cutoff); z.filter.env_depth = g.cutoff.depth; }
        }
        if (g.round_robin) { z.play_logic = PlayLogic::RoundRobin; if (g.rr_position > -1) z.seq_position = g.rr_position; }
        if (g.one_shot) z.one_shot = true;
        if (!kz.loops.empty()) z.loops.clear();
        for (auto &l : kz.loops) {
            if (l.mode == 3) { z.one_shot = true; continue; }
            if (l.mode != 1 && l.mode != 2) continue;
            Loop lp;
            lp.type = l.alternating ? LoopType::Alternating : LoopType::Forward;
            lp.until_release = l.mode == 2;
            lp.start = l.start;
            lp.end = l.start + l.length - 1;
            lp.crossfade = l.xfade;
            if (lp.end > lp.start) z.loops.push_back(lp);
        }
        z.key_lo = std::max(z.key_lo, p.clip_lo);
        z.key_hi = std::min(z.key_hi, p.clip_hi);
        if (z.key_lo > z.key_hi) continue;
        inst.zones.push_back(z);
    }
    // a K2 cycle without a class: the zones of the group are positions 1..n of a round robin
    for (size_t gi = 0; gi < p.groups.size(); gi++)
        if (p.groups[gi].round_robin && p.groups[gi].rr_position < 0) {
            int pos = 0;
            for (auto &z : inst.zones) if (z.group == int(gi)) z.seq_position = ++pos;
        }
    if (missing) inst.warnings.push_back(std::to_string(missing) + " sample files not found");
    finish_instrument(inst);
    if (inst.zones.empty() && missing) throw ParseError("the samples of this Kontakt instrument were not found next to it");
    return inst;
}

bool probe_kontakt(const std::string &name, const uint8_t *h, size_t n, uint64_t) {
    if (n < 16) return false;
    if (!std::memcmp(h, "/\\ NI FC MTD  /\\", 16)) return true;
    if (!std::memcmp(h + 12, "hsin", 4)) return !ends_with_ci(name, ".mxsnd") && !ends_with_ci(name, ".msnd");
    uint32_t id = le32(h);
    return id == K1_INST_LE || id == K1_INST_BE || id == K1_MULTI_LE || id == K1_MULTI_BE || id == K2_INST_LE || id == K2_INST_BE ||
           id == K2_MULTI_LE || id == K2_MULTI_BE;
}

}  // namespace

void register_ni_kontakt() {
    register_reader({"NI Kontakt", "nki nkm", probe_kontakt, list_kontakt, load_kontakt});
}

}  // namespace omni

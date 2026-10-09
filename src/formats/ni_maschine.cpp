// Omni Sampler: Native Instruments Maschine sounds that use the built-in Sampler:
//  - Maschine 1 (.msnd): tagged data sections ("-in-" / "-ni-"),
//  - Maschine 2 and 3 (.mxsnd): an NI container holding a boost-serialized parameter array.
// One group; the Sampler's global settings (envelope, filter, tune, bend) apply to every zone.
// Translated from ConvertWithMoss's format/ni/maschine package (LGPL-3.0).
#include <cmath>
#include <algorithm>
#include <cstring>
#include <map>
#include "../core/parse.hpp"
#include "format.hpp"
#include "ni_common.hpp"

namespace omni {
namespace {

float input_to_db(float v) { return v <= 0 ? -82.3f : std::max(80.05f * std::log10(v / 0.75f), -82.3f); }

float attack_ms(float v) {
    float x = std::min(std::max(v, 0.f), 1.f);
    if (x <= 0.5f) return 171.f / 0.5f * x;
    if (x <= 0.85f) return 171.f + (2100.f - 171.f) / 0.35f * (x - 0.5f);
    return 2100.f + (7700.f - 2100.f) / 0.15f * (x - 0.85f);
}
float decay_ms(float v) {
    float x = std::min(std::max(v, 0.f), 1.f);
    if (x <= 0.29f) return 2.9f + (72.7f - 2.9f) / 0.29f * x;
    if (x <= 0.50f) return 72.7f + (217.f - 72.7f) / 0.21f * (x - 0.29f);
    if (x <= 0.77f) return 217.f + (1500.f - 217.f) / 0.27f * (x - 0.50f);
    return 1500.f + (12300.f - 1500.f) / 0.23f * (x - 0.77f);
}
double cutoff_hz(double v) { return clampd(51.0917 * std::exp(5.9497 * v), 43.7, 19600); }

// the Sampler's settings shared by all zones
struct Globals {
    bool set = false;
    int env_type = 2;   // 0 one-shot, 1 AHD, 2 ADSR
    float attack = 0, hold = 0, decay = 0, sustain = 1, release = 0;            // amp envelope, seconds
    float mattack = 0, mhold = 0, mdecay = 0, msustain = 1, mrelease = 0;       // modulation envelope
    float pitch_mod = 0, cutoff_mod = 0, vel_cutoff = 0, vel_volume = 0;
    double tune = 0;
    int bend = 200;
    bool reverse = false;
    FilterType ftype = FilterType::None;
    double cutoff = 20000, resonance = 0;
};

struct MZone {
    std::string path;
    int64_t start = 0, stop = 0;   // Maschine 1: stop and loop end relative to the sample end (<= 0)
    bool loop = false;
    int64_t loop_start = 0, loop_end = 0;
    int loop_xfade = 0;
    int root = 60, lo_key = 0, hi_key = 127, lo_vel = 1, hi_vel = 127;
    float gain_db = 0, pan = 0, tune = 0;
};

struct MSound {
    std::string name, version;
    std::vector<MZone> zones;
    Globals g;
    bool relative_ends = false;
};

Envelope make_env(int type, float a, float h, float d, float s, float r) {
    Envelope e;
    e.set = true;
    e.attack = a;
    e.decay = d;
    if (type == 1) { e.hold = h; e.sustain = 0; e.release = d; }
    else { e.sustain = s; e.release = r; }
    return e;
}

// ---------------------------------------------------------------------------------------------------------------
// Maschine 2 / 3: boost archive with a flat array of parameter rows

int varint(Reader &r) {   // a signed length byte (-4..4: negative = a negative number), then that many little-endian bytes
    int n = int8_t(r.u8());
    bool neg = n < 0;
    if (neg) n = -n;
    if (n > 4) throw ParseError("bad Maschine number");
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= uint32_t(r.u8()) << (8 * i);
    return neg ? -int32_t(v) : int32_t(v);   // e.g. ff 01 = -1: "to the sample's end"
}

struct ParamArray {
    std::vector<std::vector<uint8_t>> rows;
    bool old_format = false;

    Reader row(size_t i) const {   // newer versions start each row with an extra 00
        if (i >= rows.size()) throw ParseError("Maschine parameter out of range");
        const auto &d = rows[i];
        if (old_format || d.empty()) return Reader(d.data(), d.size());
        return Reader(d.data() + 1, d.size() - 1);
    }
    int integer(size_t i) const { Reader r = row(i); return varint(r); }
    float flt(size_t i) const { Reader r = row(i); return r.f32le(); }
};

ParamArray parse_param_array(const std::vector<uint8_t> &data) {
    Reader r(data);
    uint32_t size = r.u32le();
    if (size != r.left()) throw ParseError("bad Maschine preset size");
    r.u8();
    int ml = r.u8();
    if (std::string(reinterpret_cast<const char *>(r.take(size_t(ml))), size_t(ml)) != "serialization::archive")
        throw ParseError("not a Maschine preset");
    Reader vr = r.sub(7);
    int version = varint(vr);
    r.skip(6);
    ParamArray pa;
    pa.old_format = version < 0x0D;
    if (varint(r) != 0) throw ParseError("bad Maschine parameter array");
    // rows are separated by their index (01 nn or 02 nn nn); anything else is row data
    const uint8_t *p = r.data() + r.pos();
    size_t n = r.left(), i = 0;
    int expected = 1;
    std::vector<uint8_t> cur;
    while (i < n) {
        if (p[i] == 1 && i + 1 < n && p[i + 1] == expected) { pa.rows.push_back(std::move(cur)); cur.clear(); i += 2; expected++; continue; }
        if (p[i] == 2 && i + 2 < n && (p[i + 1] | p[i + 2] << 8) == expected) { pa.rows.push_back(std::move(cur)); cur.clear(); i += 3; expected++; continue; }
        cur.push_back(p[i++]);
    }
    pa.rows.push_back(std::move(cur));
    return pa;
}

const char DATA_TAG[] = "NI::MASCHINE::DATA::";

int find_device(const ParamArray &pa, size_t from, std::string &name) {
    const size_t tl = sizeof DATA_TAG - 1;
    for (size_t i = from; i < pa.rows.size(); i++) {
        const auto &d = pa.rows[i];
        if (d.size() < tl) continue;
        for (size_t k = 1; k + tl <= d.size(); k++)
            if (!std::memcmp(&d[k], DATA_TAG, tl)) {
                size_t len = std::min<size_t>(d[k - 1], d.size() - k);
                name = std::string(reinterpret_cast<const char *>(&d[k]), len);
                name = name.size() > tl ? name.substr(tl) : "";
                return int(i);
            }
    }
    return -1;
}

void read_globals(const ParamArray &pa, size_t off, Globals &g) {
    bool old = pa.old_format;
    size_t first = off + (old ? 9 : 13);
    if (first < pa.rows.size() && pa.rows[first].empty()) {   // sometimes the block starts 4 rows later
        off += 4;
        first += 4;
        if (first >= pa.rows.size() || pa.rows[first].empty()) return;
    }
    float nb = std::min(std::max(pa.flt(first), 0.f), 1.f);
    g.bend = int(std::lround(1200.f * nb * nb));
    g.tune = pa.flt(off + (old ? 23 : 31)) * 36.0;
    g.env_type = pa.integer(off + (old ? 17 : 23));
    g.attack = attack_ms(pa.flt(off + (old ? 84 : 112))) / 1000.f;
    g.hold = attack_ms(pa.flt(off + (old ? 87 : 116))) / 1000.f;
    g.decay = decay_ms(pa.flt(off + (old ? 90 : 120))) / 1000.f;
    g.sustain = pa.flt(off + (old ? 93 : 124));
    g.release = decay_ms(pa.flt(off + (old ? 96 : 128))) / 1000.f;
    g.reverse = pa.integer(off + (old ? 20 : 27)) == 1;
    g.vel_cutoff = pa.flt(off + (old ? 66 : 88));
    g.vel_volume = pa.flt(off + (old ? 69 : 92));
    g.mattack = attack_ms(pa.flt(off + (old ? 111 : 148))) / 1000.f;
    g.mdecay = decay_ms(pa.flt(off + (old ? 117 : 156))) / 1000.f;
    g.msustain = pa.flt(off + (old ? 120 : 160));
    g.mrelease = decay_ms(pa.flt(off + (old ? 123 : 164))) / 1000.f;
    g.pitch_mod = pa.flt(off + (old ? 126 : 168));
    g.cutoff_mod = pa.flt(off + (old ? 129 : 172));
    int ft = pa.integer(off + (old ? 42 : 56));
    if (ft >= 1 && ft <= 3) {
        g.ftype = ft == 1 ? FilterType::LowPass : ft == 2 ? FilterType::BandPass : FilterType::HighPass;
        g.cutoff = cutoff_hz(pa.flt(off + (old ? 45 : 60)));
        g.resonance = clampd(pa.flt(off + (old ? 48 : 64)), 0, 1);
    }
    g.set = true;
}

ParamArray maschine2_params(const std::vector<uint8_t> &d) {
    NiContainer c = read_ni_container(d);
    if (c.app >= 0 && c.app != NI_APP_MASCHINE) throw ParseError("this NI file was not made by Maschine");
    if (c.preset.empty()) {
        if (c.encrypted) throw ParseError("encrypted Maschine library: cannot be read");
        throw ParseError("no Maschine sound in this file");
    }
    return parse_param_array(c.preset);
}
constexpr int M2_PLUGIN_INFO_OLD = 109, M2_PLUGIN_INFO = 665, M2_FIRST_ZONE_OLD = 111, M2_FIRST_ZONE = 667;

MSound sampler_at(const ParamArray &pa, int base, size_t *end_row);

MSound read_maschine2(const std::vector<uint8_t> &d) {
    ParamArray pa = maschine2_params(d);
    const int PLUGIN_INFO = pa.old_format ? M2_PLUGIN_INFO_OLD : M2_PLUGIN_INFO;
    // find the Sampler among the sound's devices
    size_t from = 0;
    int base = 0;
    for (;;) {
        std::string name;
        int at = find_device(pa, from, name);
        if (at < 0) throw ParseError("this Maschine sound does not use the Sampler (a drum synth or plugin: it cannot play here)");
        base = at - PLUGIN_INFO;
        if (name == "Sampler") break;
        from = size_t(at) + 1;
    }
    return sampler_at(pa, base, nullptr);
}

bool has_audio_path(const std::vector<uint8_t> &row) {
    std::string t(row.begin(), row.end());
    for (char &c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const char *e : {".wav", ".aif", ".flac", ".ogg", ".ncw"}) if (t.find(e) != std::string::npos) return true;
    return false;
}

// the first UTF-16 text in a row ("" if none)
std::string first_utf16(const std::vector<uint8_t> &row) {
    std::string t;
    for (size_t k = 0; k + 1 < row.size(); k++) {
        if (row[k] >= 32 && row[k] < 127 && row[k + 1] == 0) { t += char(row[k]); k++; continue; }
        if (t.size() >= 2) return t;
        t.clear();
    }
    return t.size() >= 2 ? t : "";
}
bool has_utf16(const std::vector<uint8_t> &row, const char *s) {
    std::vector<uint8_t> w;
    for (const char *p = s; *p; p++) { w.push_back(uint8_t(*p)); w.push_back(0); }
    return std::search(row.begin(), row.end(), w.begin(), w.end()) != row.end();
}

// The sounds of a Maschine 2 group (16 pads) or project (groups of 16), in pad order; a pad whose sound doesn't use the
// Sampler (a drum synth, a plugin, empty) has no zones. Boost writes a device's class name only the first time, so:
// each sound's source device (Sampler, drum synth, plugin) has an info row with no vendor text, just before its data
// (only the very first sound, written with its class header, has its sample first and its info row later); a Sampler is
// found by its first sample (its zone list starts a fixed number of rows after the device). A group's own info row
// follows its 16 sounds. pads_ok: the pads could be told apart (else the Sampler sounds are given in order).
std::vector<MSound> read_maschine2_sounds(const std::vector<uint8_t> &d, bool project, bool *pads_ok) {
    ParamArray pa = maschine2_params(d);
    const int FIRST_ZONE = pa.old_format ? M2_FIRST_ZONE_OLD : M2_FIRST_ZONE;
    struct Slot { long info = -1; MSound snd; bool has = false; };
    std::vector<Slot> slots;
    std::vector<MSound> samplers;
    for (size_t r = 0; r < pa.rows.size(); r++) {
        const auto &row = pa.rows[r];
        if (row.size() > 100 && first_utf16(row) == "\\@color" && has_utf16(row, "@devicetypeflags")) { slots.push_back(Slot{long(r), MSound(), false}); continue; }
        if (r < size_t(FIRST_ZONE) || !has_audio_path(row)) continue;
        try {
            size_t end = r;
            MSound snd = sampler_at(pa, int(r) - FIRST_ZONE, &end);
            if (snd.zones.empty()) continue;
            samplers.push_back(snd);
            if (!slots.empty() && !slots.back().has && slots.back().info >= 0 && long(r) - slots.back().info < 300) {
                slots.back().snd = std::move(snd);
                slots.back().has = true;
            } else slots.push_back(Slot{-1, std::move(snd), true});
            r = std::max(r, end);
        } catch (const ParseError &) {}
    }
    std::vector<MSound> out;
    size_t per = project ? 17 : 0;
    if (!project && (slots.size() == 16 || slots.size() == 17)) {
        for (size_t i = 0; i < 16; i++) out.push_back(slots[i].snd);
    } else if (project && !slots.empty() && slots.size() % per == 0) {
        for (size_t i = 0; i < slots.size(); i++) if (i % per < 16) out.push_back(slots[i].snd);
    }
    if (pads_ok) *pads_ok = !out.empty();
    return out.empty() ? samplers : out;
}

// The Sampler whose device data starts at row base: its zones and global settings. end_row: the row after its zones.
MSound sampler_at(const ParamArray &pa, int base, size_t *end_row) {
    const bool old = pa.old_format;
    const int NUM_SAMPLES = old ? 110 : 666, FIRST_ZONE = old ? M2_FIRST_ZONE_OLD : M2_FIRST_ZONE;
    int zone_size = old ? 59 : 80;
    if (base < 0) throw ParseError("bad Maschine Sampler position");
    MSound s;
    s.version = old ? "Maschine 2" : "Maschine 2.x/3";
    // the first object of its class carries boost's class header (the sample count row starts with 6 or 5 zero fields,
    // the first zone row with 00 00); later ones in the same file (a group's other sounds) are written without it
    const auto &fz = pa.rows.at(size_t(base + FIRST_ZONE));
    bool header = fz.size() >= 2 && fz[0] == 0 && fz[1] == 0;
    Reader vr = pa.row(size_t(base + NUM_SAMPLES));
    int vals[7] = {};
    int nv = header ? (old ? 7 : 6) : 1;
    for (int i = 0; i < nv; i++) vals[i] = varint(vr);
    int count = header ? vals[old ? 6 : 5] : vals[0];
    if (count < 0 || count > 512) throw ParseError("bad Maschine sample count");
    int zone_off = 0;
    struct Off { int start, end, loop_on, loop_start, loop_end, xfade, root, lo_key, hi_key, lo_vel, hi_vel, gain, pan, tune; };
    const Off o = old ? Off{3, 5, 13, 8, 10, 16, 25, 28, 30, 33, 35, 38, 41, 44} : Off{4, 7, 18, 11, 14, 22, 34, 38, 41, 45, 48, 52, 56, 60};
    for (int si = 0; si < count;) {
        size_t zr = size_t(base + FIRST_ZONE + zone_off);
        if (zr >= pa.rows.size()) break;
        const auto &raw = pa.rows[zr];
        Reader ir(raw);
        if (si == 0 && header) {
            if (ir.left() < 2) throw ParseError("this Maschine Sampler has no samples");
            if (ir.u8() != 0 || ir.u8() != 0) throw ParseError("unknown Maschine sample info");
        }
        if (ir.eof()) break;
        if (ir.u8() > 0) ir.u8();   // library reference
        int len = varint(ir);
        std::string path(reinterpret_cast<const char *>(ir.take(size_t(len))), size_t(len));
        if ((path.empty() || path[0] == 0) && si == 1 && zone_size == 59) {   // some 2.0.0.0 zones are a row longer
            zone_size = 60;
            zone_off = 60;
            continue;
        }
        size_t b = zr;
        MZone z;
        z.path = path;
        z.start = pa.integer(b + size_t(o.start));
        z.stop = pa.integer(b + size_t(o.end));
        if (pa.integer(b + size_t(o.loop_on)) == 1) {
            z.loop = true;
            z.loop_start = pa.integer(b + size_t(o.loop_start));
            z.loop_end = pa.integer(b + size_t(o.loop_end));
            z.loop_xfade = pa.integer(b + size_t(o.xfade));
        }
        z.root = pa.integer(b + size_t(o.root));
        z.lo_key = pa.integer(b + size_t(o.lo_key)); z.hi_key = pa.integer(b + size_t(o.hi_key));
        z.lo_vel = pa.integer(b + size_t(o.lo_vel)); z.hi_vel = pa.integer(b + size_t(o.hi_vel));
        z.gain_db = input_to_db(pa.flt(b + size_t(o.gain)));
        z.pan = pa.flt(b + size_t(o.pan));
        z.tune = pa.flt(b + size_t(o.tune));
        s.zones.push_back(z);
        zone_off += zone_size;
        si++;
    }
    size_t pos = size_t(base + FIRST_ZONE + zone_off);
    if (end_row) *end_row = pos;
    if (!old) {   // library references
        pos += 4;
        try { Reader rr = pa.row(pos); int refs[10]; for (int &v : refs) v = varint(rr); pos += size_t(32 * refs[9]); }
        catch (const ParseError &) {}
    }
    try { read_globals(pa, pos, s.g); } catch (const ParseError &) {}
    // the sound info: the first long row after the Sampler data
    for (size_t i = pos; i < pa.rows.size(); i++) {
        const auto &row = pa.rows[i];
        if (row.size() <= 100 || row[0] == 0) continue;
        try {
            Reader r(row);
            r.u16le(); r.u16le(); r.skip(11);
            std::string name = trim(utf16_len(r));
            if (!name.empty() && name != "Sampler") s.name = name;
        } catch (const ParseError &) {}
        break;
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// Maschine 1: nested sections ("data" headers), each holding a tree of 4-character tags with typed values

struct M1Section {
    std::string name;
    std::vector<uint8_t> data;
    M1Section *parent = nullptr;
    std::vector<std::unique_ptr<M1Section>> children;
};
struct M1Param { std::string name; int type = -1; int i = 0; float f = 0; std::string s; };
struct M1Tag {
    std::string name;
    M1Tag *parent = nullptr;
    std::vector<std::unique_ptr<M1Tag>> children;
    M1Param param;
    bool has_param = false;
};

std::string tag4(Reader &r, bool be) {   // stored reversed in little-endian files
    std::string t(reinterpret_cast<const char *>(r.take(4)), 4);
    if (!be) std::reverse(t.begin(), t.end());
    return t;
}
bool is_lower(const std::string &s) {   // as ConvertWithMoss: equal to its lower case form
    for (char c : s) if (c >= 'A' && c <= 'Z') return false;
    return true;
}
uint32_t u32e(Reader &r, bool be) { return be ? r.u32be() : r.u32le(); }

const M1Section *find_section(const M1Section &s, const std::string &name, int depth = 0) {
    if (s.name == name) return &s;
    if (depth > 64) return nullptr;
    for (auto &c : s.children) if (const M1Section *f = find_section(*c, name, depth + 1)) return f;
    return nullptr;
}
const M1Section *section_path(const M1Section *from, std::initializer_list<const char *> path) {
    const M1Section *cur = from;
    for (const char *p : path) {
        const M1Section *next = nullptr;
        for (auto &c : cur->children) if (c->name == p) { next = c.get(); break; }
        if (!next) return nullptr;
        cur = next;
    }
    return cur;
}
// follows a path of tag names; the first name is looked up among `tags`
const M1Tag *tag_path(const std::vector<std::unique_ptr<M1Tag>> &tags, std::initializer_list<const char *> path) {
    const std::vector<std::unique_ptr<M1Tag>> *cur = &tags;
    const M1Tag *found = nullptr;
    for (const char *p : path) {
        found = nullptr;
        for (auto &t : *cur) if (t->name == p) { found = t.get(); break; }
        if (!found) return nullptr;
        cur = &found->children;
    }
    return found;
}
// the same, starting with the tag itself
const M1Tag *tag_path(const M1Tag &t, std::initializer_list<const char *> path) {
    auto it = path.begin();
    if (it == path.end() || t.name != *it) return nullptr;
    const M1Tag *cur = &t;
    for (++it; it != path.end(); ++it) {
        const M1Tag *next = nullptr;
        for (auto &c : cur->children) if (c->name == *it) { next = c.get(); break; }
        if (!next) return nullptr;
        cur = next;
    }
    return cur;
}

std::unique_ptr<M1Tag> read_tags(const std::vector<uint8_t> &data, bool be) {
    auto top = std::make_unique<M1Tag>();
    M1Tag *cur = top.get();
    Reader r(data);
    u32e(r, be);
    auto close = [&](const std::string &end) {   // an end tag: up to the matching start tag's parent
        M1Tag *t = cur;
        while (lower(t->name) != lower(end)) {
            if (!t->parent) throw ParseError("bad Maschine 1 parameter section");
            t = t->parent;
        }
        cur = t->parent ? t->parent : top.get();
    };
    while (r.left() >= 4) {
        std::string t1 = tag4(r, be);
        if (!is_lower(t1)) { close(t1); continue; }
        std::string t2 = tag4(r, be);
        if (t1 == t2) {
            u32e(r, be);   // version
            auto child = std::make_unique<M1Tag>();
            child->name = t1;
            child->parent = cur;
            M1Tag *raw = child.get();
            cur->children.push_back(std::move(child));
            cur = raw;
            if (t1 == "osid") u32e(r, be);
        } else if (t2 == "vt  ") {   // a value of the open tag, named t1
            if (tag4(r, be) != "vt  ") throw ParseError("bad Maschine 1 parameter");
            if (u32e(r, be) != 0 || r.u8() != 1) throw ParseError("bad Maschine 1 parameter");
            M1Param p;
            p.name = t1;
            p.type = int(u32e(r, be));
            switch (p.type) {
            case 0: {
                uint32_t n = u32e(r, be);
                if (n > r.left() / 2) throw ParseError("bad Maschine 1 string");
                std::vector<uint16_t> u(n);
                for (auto &c : u) c = be ? r.u16be() : r.u16le();
                p.s = utf16_to_utf8(u.data(), u.size());
                break;
            }
            case 1: p.f = be ? r.f32be() : r.f32le(); break;
            case 2: case 3: p.i = int32_t(u32e(r, be)); break;
            case 4: p.i = r.u8(); break;
            default: throw ParseError("unknown Maschine 1 parameter type");
            }
            cur->param = p;
            cur->has_param = true;
        } else if (!is_lower(t2)) close(t2);
        else throw ParseError("bad Maschine 1 parameter section");
    }
    return top;
}

void vri_params(const M1Tag *t, std::map<std::string, M1Param> &out) {
    if (!t) return;
    for (auto &c : t->children) {
        const M1Tag *v = tag_path(*c, {"prp ", "vr  ", "vri "});
        if (v && v->has_param) out[v->param.name] = v->param;
    }
}

std::string url_decode(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}

void collect_sections(const M1Section &s, const char *name, std::vector<const M1Section *> &out) {
    if (s.name == name) out.push_back(&s);
    for (auto &c : s.children) collect_sections(*c, name, out);
}
MSound maschine1_sampler(const M1Section *zs, bool be);

// all: every sound's Sampler (a group), else just the first
MSound read_maschine1(const std::vector<uint8_t> &d, std::vector<MSound> *all = nullptr) {
    Reader r(d);
    std::string start(reinterpret_cast<const char *>(r.take(4)), 4);
    bool be = start == "-ni-";
    if (!be && start != "-in-") throw ParseError("not a Maschine 1 sound");
    if (u32e(r, be) != 2) throw ParseError("unsupported Maschine 1 file version");
    if (trim(std::string(reinterpret_cast<const char *>(r.take(48)), 48)).find("#NI#CS#Document##NI#SoundShell#Sound#") != 0)
        throw ParseError("not a Maschine 1 sound");
    u32e(r, be);
    M1Section top;
    M1Section *cur = &top;
    while (r.left() >= 16) {
        if (tag4(r, be) != "data") throw ParseError("bad Maschine 1 file");
        if (u32e(r, be) != 2) throw ParseError("unknown Maschine 1 data version");
        auto sec = std::make_unique<M1Section>();
        sec->name = tag4(r, be);
        tag4(r, be);
        uint32_t s1 = u32e(r, be), s2 = u32e(r, be);
        if (s1 != s2 || s1 > r.left()) throw ParseError("bad Maschine 1 file");
        const uint8_t *p = r.take(s1);
        sec->data.assign(p, p + s1);
        if (sec->name != "info" && sec->name != "brqy" && sec->data.size() >= 4) sec->data.resize(sec->data.size() - 4);
        if (is_lower(sec->name)) {
            sec->parent = cur;
            M1Section *raw = sec.get();
            cur->children.push_back(std::move(sec));
            cur = raw;
        } else if (cur->parent) cur = cur->parent;
    }
    std::vector<const M1Section *> blocks;
    collect_sections(top, "gznc", blocks);
    if (blocks.empty() || blocks[0]->data.empty() || !blocks[0]->parent) throw ParseError("no Sampler zones in this Maschine 1 sound");
    if (all) for (const M1Section *b : blocks) if (!b->data.empty() && b->parent) all->push_back(maschine1_sampler(b, be));
    return maschine1_sampler(blocks[0], be);
}

MSound maschine1_sampler(const M1Section *zs, bool be) {
    auto ztags = read_tags(zs->data, be);
    MSound s;
    s.version = "Maschine 1";
    s.relative_ends = true;
    for (auto &zt : ztags->children) {
        const M1Tag *gsl = tag_path(*zt, {"gzn ", "gsl "}), *dfp = tag_path(*zt, {"gzn ", "dfp "});
        if (!gsl || !dfp) throw ParseError("bad Maschine 1 zone");
        std::map<std::string, M1Param> pm;
        vri_params(tag_path(*gsl, {"gsl ", "dfp ", "prc "}), pm);
        vri_params(tag_path(*dfp, {"dfp ", "prc "}), pm);
        auto I = [&](const char *k) { auto it = pm.find(k); return it == pm.end() ? 0 : it->second.i; };
        auto F = [&](const char *k, float def) { auto it = pm.find(k); return it == pm.end() ? def : it->second.f; };
        MZone z;
        z.path = url_decode(pm["rlur"].s);
        if (z.path.empty()) continue;
        z.start = I("zsst"); z.stop = I("zsed");
        if (I("zslm") > 0) { z.loop = true; z.loop_start = I("zsls"); z.loop_end = I("zsle"); z.loop_xfade = I("zslx"); }
        z.root = I("zrky"); z.lo_key = I("zlky"); z.hi_key = I("zhky"); z.lo_vel = I("zlvl"); z.hi_vel = I("zhvl");
        z.gain_db = input_to_db(F("zvol", 0.75f));
        z.pan = F("zpan", 0);
        z.tune = float(12.0 * std::log2(clampd(F("ztun", 1), 0.125, 8)));
        s.zones.push_back(z);
    }
    // the Sampler's global parameters, keyed by the last byte of their tag
    const M1Section *ps = section_path(zs->parent, {"gemo", "prst"});
    if (ps && !ps->data.empty()) {
        auto ptags = read_tags(ps->data, be);
        std::map<int, M1Param> gp;
        if (const M1Tag *pac = tag_path(ptags->children, {"dfp ", "pac "}))
            for (auto &c : pac->children)
                if (const M1Tag *v = tag_path(*c, {"par ", "osid", "vr  ", "vri "}))
                    if (v->has_param && v->param.name.size() == 4) gp[uint8_t(v->param.name[3])] = v->param;
        if (gp.size() == 43 || gp.size() == 54) {
            Globals &g = s.g;
            g.env_type = gp[10].i;
            if (gp.size() > 43) { float nb = gp[48].f; g.bend = int(std::lround(1200.f * nb * nb)); }
            else { g.env_type++; g.bend = 200; }   // older sounds: no bend range stored, 2 semitones
            g.mattack = attack_ms(gp[26].f) / 1000.f; g.mhold = attack_ms(gp[27].f) / 1000.f; g.mdecay = decay_ms(gp[28].f) / 1000.f;
            g.msustain = gp[29].f; g.mrelease = decay_ms(gp[30].f) / 1000.f;
            int ft = gp[20].i;
            if (ft >= 2 && ft <= 4) {
                g.ftype = ft == 2 ? FilterType::LowPass : ft == 3 ? FilterType::BandPass : FilterType::HighPass;
                g.cutoff = cutoff_hz(gp[21].f);
                g.resonance = clampd(gp[22].f, 0, 1);
                g.vel_cutoff = gp[5].f;
                g.cutoff_mod = std::max(0.f, gp[32].f);
            }
            g.attack = attack_ms(gp[11].f) / 1000.f; g.hold = attack_ms(gp[12].f) / 1000.f; g.decay = decay_ms(gp[13].f) / 1000.f;
            g.sustain = gp[14].f; g.release = decay_ms(gp[15].f) / 1000.f;
            g.vel_volume = gp[6].f;
            g.tune = gp[7].f * 72.0 - 36.0;
            g.reverse = gp[9].i > 0;
            g.pitch_mod = std::max(0.f, gp[31].f);
            g.set = true;
        }
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------

MSound read_sound(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(256u << 20);
    if (d.size() < 16) throw ParseError("not a Maschine sound");
    if (!std::memcmp(d.data(), "-in-", 4) || !std::memcmp(d.data(), "-ni-", 4)) return read_maschine1(d);
    if (!std::memcmp(&d[12], "hsin", 4)) return read_maschine2(d);
    throw ParseError("unknown Maschine file type");
}

std::vector<PresetInfo> list_maschine(VolumePtr vol, const std::string &path) {
    MSound s = read_sound(*vol, path);
    return {{s.name.empty() ? path_stem(path) : s.name, 0}};
}

// a sound's zones, with its Sampler's global settings, added to inst (group: their group); returns how many
size_t add_sound(Instrument &inst, VolumePtr vol, NiSampleFinder &finder, const MSound &s, int group, int &missing) {
    const Globals &g = s.g;
    size_t first = inst.zones.size();
    for (const MZone &mz : s.zones) {
        std::string found = finder.find(mz.path);
        if (found.empty()) { missing++; continue; }
        Zone z;
        z.sample = file_sample_ref(vol, found);
        z.name = path_stem(found);
        int64_t frames = 0;
        if (s.relative_ends) {   // Maschine 1: end points count back from the sample's end
            try { frames = probe_audio(*vol->open(found)).frames; } catch (const std::exception &) {}
        }
        z.start = mz.start;
        int64_t stop = mz.stop + (s.relative_ends ? frames : 0);
        if (stop > z.start) z.stop = stop;
        if (mz.loop) {
            Loop l;
            l.start = mz.loop_start;
            l.end = mz.loop_end + (s.relative_ends ? frames : 0);
            l.crossfade = mz.loop_xfade;
            if (l.end > l.start) z.loops.push_back(l);
        }
        z.root = mz.root; z.key_lo = mz.lo_key; z.key_hi = mz.hi_key;
        z.vel_lo = mz.lo_vel; z.vel_hi = mz.hi_vel;
        z.gain_db = mz.gain_db;
        z.pan = clampd(mz.pan, -1, 1);
        z.tune = mz.tune + g.tune;
        if (g.set) {
            z.reverse = g.reverse;
            z.bend_up = g.bend; z.bend_down = -g.bend;
            if (g.env_type == 0) z.one_shot = true;
            else z.amp_env = make_env(g.env_type, g.attack, g.hold, g.decay, g.sustain, g.release);
            z.amp_vel_depth = clampd(g.vel_volume, 0, 1);
            if (g.pitch_mod != 0) {
                z.pitch_env = make_env(g.env_type == 1 ? 1 : 2, g.mattack, g.mhold, g.mdecay, g.msustain, g.mrelease);
                z.pitch_env_depth = g.pitch_mod;
            }
            if (g.ftype != FilterType::None) {
                z.filter.type = g.ftype;
                z.filter.poles = 2;
                z.filter.cutoff = g.cutoff;
                z.filter.resonance = g.resonance;
                if (g.cutoff_mod != 0) {
                    z.filter.env = make_env(g.env_type == 1 ? 1 : 2, g.mattack, g.mhold, g.mdecay, g.msustain, g.mrelease);
                    z.filter.env_depth = g.cutoff_mod;
                    z.filter.vel_depth = g.vel_cutoff;
                }
            }
        }
        z.group = group;
        inst.zones.push_back(z);
    }
    return inst.zones.size() - first;
}

Instrument load_maschine(VolumePtr vol, const std::string &path, int) {
    MSound s = read_sound(*vol, path);
    Instrument inst;
    inst.name = s.name.empty() ? path_stem(path) : s.name;
    inst.format = "NI " + s.version;
    NiSampleFinder finder(vol, path);
    int missing = 0;
    add_sound(inst, vol, finder, s, 0, missing);
    if (missing) inst.warnings.push_back(std::to_string(missing) + " sample files not found");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError(missing ? "the samples of this Maschine sound were not found" : "this Maschine sound has no samples");
    return inst;
}

// A Maschine group (a drum kit: 16 sounds) or project (its groups' sounds): sound n on pad n, note 36 + n (MPC's pads;
// a project's next groups on the next pad banks). Maschine plays a pad's sound at C3 (60): each sound keeps the zones
// that play 60, moved to its pad and tuned to sound as they do at 60.
std::vector<MSound> read_sounds(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(256u << 20);
    if (d.size() < 16) throw ParseError("not a Maschine file");
    if (!std::memcmp(d.data(), "-in-", 4) || !std::memcmp(d.data(), "-ni-", 4)) {
        std::vector<MSound> all;
        read_maschine1(d, &all);
        return all;
    }
    if (!std::memcmp(&d[12], "hsin", 4)) return read_maschine2_sounds(d, ends_with_ci(path, ".mxprj") || ends_with_ci(path, ".mprj"), nullptr);
    throw ParseError("unknown Maschine file type");
}

std::vector<PresetInfo> list_maschine_kit(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

Instrument load_maschine_kit(VolumePtr vol, const std::string &path, int) {
    std::vector<MSound> sounds = read_sounds(*vol, path);
    bool any = false;
    for (auto &snd : sounds) any = any || !snd.zones.empty();
    if (!any) throw ParseError("this Maschine kit has no sampled sounds (drum synths or plugins only: they cannot play here)");
    Instrument inst;
    inst.name = path_stem(path);
    bool project = ends_with_ci(path, ".mxprj") || ends_with_ci(path, ".mprj");
    inst.format = "NI " + sounds[0].version + (project ? " project" : " group");
    inst.groups.clear();
    NiSampleFinder finder(vol, path);
    int missing = 0;
    for (size_t n = 0; n < sounds.size() && 36 + n <= 127; n++) {
        const MSound &snd = sounds[n];
        int pad = 36 + int(n);
        if (snd.zones.empty()) continue;   // a drum synth, plugin or empty pad: silent here
        std::string nm = !snd.name.empty() ? snd.name : snd.zones.empty() ? "" : path_stem(snd.zones[0].path);
        inst.groups.push_back(Group{nm.empty() ? "Pad " + std::to_string(n % 16 + 1) : nm});
        size_t first = inst.zones.size();
        size_t added = add_sound(inst, vol, finder, snd, int(inst.groups.size()) - 1, missing);
        bool any60 = false;   // the zones that play C3; none (a split sound): all of them
        for (size_t i = first; i < first + added; i++) any60 = any60 || (inst.zones[i].key_lo <= 60 && inst.zones[i].key_hi >= 60);
        std::vector<Zone> keep;
        for (size_t i = first; i < first + added; i++) {
            Zone z = inst.zones[i];
            if (any60 && !(z.key_lo <= 60 && z.key_hi >= 60)) continue;
            z.root += pad - 60;
            z.key_lo = z.key_hi = pad;
            keep.push_back(z);
        }
        inst.zones.resize(first);
        inst.zones.insert(inst.zones.end(), keep.begin(), keep.end());
    }
    if (inst.groups.empty()) inst.groups.push_back(Group{});
    if (missing) inst.warnings.push_back(std::to_string(missing) + " sample files not found");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError(missing ? "the samples of this Maschine kit were not found" : "this Maschine kit has no samples");
    return inst;
}

bool probe_maschine_kit(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    return n >= 16 && (!std::memcmp(h, "-in-", 4) || !std::memcmp(h, "-ni-", 4) || !std::memcmp(h + 12, "hsin", 4));
}

// NI Massive presets are synthesizer patches: no samples to play. Listed so the browser can say so.
std::vector<PresetInfo> list_massive(VolumePtr, const std::string &) {
    throw ParseError("NI Massive presets are synthesizer patches (no samples): this sampler cannot play them");
}
Instrument load_massive(VolumePtr, const std::string &, int) { throw ParseError("NI Massive presets cannot be played"); }

bool probe_maschine(const std::string &name, const uint8_t *h, size_t n, uint64_t) {
    if (n < 16) return false;
    if (!std::memcmp(h, "-in-", 4) || !std::memcmp(h, "-ni-", 4)) return true;
    // Maschine 2+ shares the NI container with Kontakt: tell them apart by extension
    return !std::memcmp(h + 12, "hsin", 4) && (ends_with_ci(name, ".mxsnd") || ends_with_ci(name, ".msnd"));
}

}  // namespace

void register_ni_maschine() {
    register_reader({"NI Maschine", "mxsnd msnd", probe_maschine, list_maschine, load_maschine});
    register_reader({"NI Maschine kit", "mxgrp mgrp mxprj mprj", probe_maschine_kit, list_maschine_kit, load_maschine_kit});
    register_reader({"NI Massive preset", "nmsv nmsx", nullptr, list_massive, load_massive});
}

}  // namespace omni

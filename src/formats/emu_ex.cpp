// Omni Sampler: E-mu Emulator X / X2 / X3 / Proteus X banks (.exb) with their 'SamplePool' folder of .ebl sample
// files, and a lone .ebl sample. Translated from ConvertWithMoss's EmulatorXDetector / EmulatorXChunk /
// EmulatorXSampleFile / EmulatorXConstants (LGPL-3.0).
#include <cmath>
#include <cstring>
#include <map>
#include "format.hpp"

namespace omni {
namespace {

constexpr int TOC_OFFSET = 20, TOC_ENTRY = 78, CHUNK_OVERHEAD = 10, SAMPLE_PAYLOAD = 108, SAMPLE_DATA = 188;

float f32be(const uint8_t *p) { uint32_t v = be32(p); float f; std::memcpy(&f, &v, 4); return f; }

std::string ex_name(const uint8_t *p, size_t avail) {
    size_t n = std::min<size_t>(64, avail) / 2;
    std::vector<uint16_t> u(n);
    for (size_t i = 0; i < n; i++) u[i] = le16(p + 2 * i);
    size_t len = 0;
    while (len < n && u[len]) len++;
    return trim(utf16_to_utf8(u.data(), len));
}

// A chunk view into a buffer: tag, payload offset/size.
struct Ck {
    const std::vector<uint8_t> *d = nullptr;
    std::string tag;
    size_t off = 0, size = 0;
    bool is(const char *t) const { return tag == t; }
    bool is_list(const char *type) const { return tag == "LIST" && size >= 4 && !std::memcmp(&(*d)[off], type, 4); }
    std::vector<Ck> children() const {
        std::vector<Ck> out;
        size_t skip = tag == "LIST" ? 4 : 0, pos = off + skip, end = std::min(off + size, d->size());
        while (pos + 8 <= end) {
            size_t sz = be32(&(*d)[pos + 4]);
            if (pos + 8 + sz > end) break;
            Ck c;
            c.d = d;
            c.tag.assign(reinterpret_cast<const char *>(&(*d)[pos]), 4);
            c.off = pos + 8;
            c.size = sz;
            out.push_back(c);
            pos += 8 + sz;
        }
        return out;
    }
    bool child(const char *t, Ck &out) const { for (auto &c : children()) if (c.is(t)) { out = c; return true; } return false; }
    bool list(const char *t, Ck &out) const { for (auto &c : children()) if (c.is_list(t)) { out = c; return true; } return false; }
    int u8(size_t p) const { return p < size ? (*d)[off + p] : 0; }
    int s8(size_t p) const { return p < size ? int8_t((*d)[off + p]) : 0; }
    int u16(size_t p) const { return p + 2 <= size ? be16(&(*d)[off + p]) : 0; }
    float f(size_t p) const { return p + 4 <= size ? f32be(&(*d)[off + p]) : 0.f; }
};

struct ExSample { bool ok = false; std::string name, path; int rate = 44100, channels = 1, frames = 0; bool loop = false; int loop_start = 0, loop_end = 0;
                  uint64_t left = 0, right = 0; };

ExSample read_ebl_header(Volume &vol, const std::string &path) {
    ExSample s;
    BlobPtr b = vol.open(path);
    std::vector<uint8_t> h = b->head(4096);
    if (h.size() < TOC_OFFSET + TOC_ENTRY || std::memcmp(h.data(), "FORM", 4) || std::memcmp(h.data() + 8, "E5B0TOC2", 8)) return s;
    if (be32(&h[16]) < uint32_t(TOC_ENTRY) || std::memcmp(&h[TOC_OFFSET], "E5S1", 4)) return s;
    size_t payload = be32(&h[TOC_OFFSET + 8]) + CHUNK_OVERHEAD;
    if (payload + SAMPLE_DATA > h.size()) h = b->head(payload + SAMPLE_DATA);
    if (payload + SAMPLE_DATA > h.size()) return s;
    const uint8_t *p = &h[payload];
    s.name = ex_name(p + 4, 64);
    s.rate = int(le32(p + 104));
    int64_t ls = le32(p + 72), le_ = le32(p + 80), rs = le32(p + 76), re = le32(p + 84);
    int64_t llen = le_ - ls, rlen = re - rs;
    bool stereo = rs != ls && rlen > 0 && rlen == llen;
    if (s.rate <= 0 || llen <= 0 || payload + uint64_t(ls + llen) > b->size()) return s;
    s.channels = stereo ? 2 : 1;
    s.frames = int(llen / 2);
    s.left = payload + uint64_t(ls);
    s.right = payload + uint64_t(rs);
    if (le16(p + 110) > 0) {
        int st = int((int64_t(le32(p + 88)) - ls) / 2), en = int((int64_t(le32(p + 96)) - ls) / 2);
        if (st >= 0 && en > st && st < s.frames) { s.loop = true; s.loop_start = st; s.loop_end = std::min(en, s.frames - 1); }
    }
    s.path = path;
    s.ok = true;
    return s;
}

SampleRefPtr ebl_ref(VolumePtr vol, const ExSample &s) {
    auto r = std::make_shared<SampleRef>();
    r->name = s.name; r->rate = s.rate; r->channels = s.channels; r->frames = s.frames;
    r->key = "ebl:" + std::to_string(reinterpret_cast<uintptr_t>(vol.get())) + ":" + s.path;
    ExSample c = s;
    r->decode = [vol, c]() -> PcmPtr {
        BlobPtr b = vol->open(c.path);
        if (c.channels == 1) {
            std::vector<uint8_t> raw = b->bytes(c.left, size_t(c.frames) * 2);
            return pcm_from_raw(raw.data(), c.frames, 1, Enc::S16LE, c.rate);
        }
        std::vector<uint8_t> l = b->bytes(c.left, size_t(c.frames) * 2), rr = b->bytes(c.right, size_t(c.frames) * 2);
        auto p = std::make_shared<Pcm>();
        p->rate = c.rate; p->channels = 2; p->data.resize(size_t(c.frames) * 2);
        for (int i = 0; i < c.frames; i++) { p->data[2 * size_t(i)] = int16_t(le16(&l[2 * size_t(i)])); p->data[2 * size_t(i) + 1] = int16_t(le16(&rr[2 * size_t(i)])); }
        return p;
    };
    return r;
}

struct ExBank { std::vector<uint8_t> d; std::vector<Ck> presets; std::vector<int> sample_indices; };

ExBank parse_bank(Volume &vol, const std::string &path) {
    ExBank b;
    b.d = vol.open(path)->all(256u << 20);
    const auto &d = b.d;
    if (d.size() < TOC_OFFSET || std::memcmp(d.data(), "FORM", 4) || std::memcmp(d.data() + 8, "E5B0TOC2", 8)) throw ParseError("not an Emulator X bank");
    size_t toc = be32(&d[16]), toc_end = TOC_OFFSET + toc;
    if (!toc || toc_end > d.size()) throw ParseError("not an Emulator X bank");
    for (size_t pos = TOC_OFFSET; pos + TOC_ENTRY <= toc_end; pos += TOC_ENTRY) {
        size_t size = be32(&d[pos + 4]), off = be32(&d[pos + 8]);
        int index = be16(&d[pos + 12]);
        if (off + CHUNK_OVERHEAD + size > d.size()) continue;
        if (!std::memcmp(&d[pos], "E5P1", 4)) { Ck c; c.d = &b.d; c.tag = "E5P1"; c.off = off + CHUNK_OVERHEAD; c.size = size; b.presets.push_back(c); }
        else if (!std::memcmp(&d[pos], "E5SL", 4)) b.sample_indices.push_back(index);
    }
    return b;
}

void window(const std::vector<Ck> &ws, int index, int out[4]) {
    int pos = 0;
    for (auto &w : ws)
        if (w.is("ETW ") && pos++ == index) { for (int i = 0; i < 4; i++) out[i] = w.u8(4 + size_t(i)); return; }
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 127;
}

bool read_env(const Ck &voice, int index, Envelope &e) {
    Ck el;
    if (!voice.list("EvL ", el)) return false;
    int pos = 0;
    for (auto &c : el.children()) {
        if (!c.is("E5Ev") || pos++ != index) continue;
        double t[6], l[6];
        for (int s = 0; s < 6; s++) { size_t so = 10 + size_t(s) * 9; t[s] = std::max(0.f, c.f(so)); l[s] = clampd(c.f(so + 4) / 100.0, 0, 1); }
        e = Envelope();
        e.set = true;
        e.attack = t[0] + t[1];
        bool plateau = std::fabs(l[2] - l[1]) < 0.01;
        e.hold = plateau ? t[2] : 0;
        e.decay = plateau ? t[3] : t[2] + t[3];
        e.sustain = l[3];
        e.release = t[4] + t[5];
        return true;
    }
    return false;
}

std::vector<PresetInfo> list_exb(VolumePtr vol, const std::string &path) {
    ExBank b = parse_bank(*vol, path);
    std::vector<PresetInfo> out;
    for (size_t i = 0; i < b.presets.size(); i++) {
        Ck h;
        std::string n = b.presets[i].child("Phdr", h) ? ex_name(&b.d[h.off + 4], std::min<size_t>(64, h.size - 4)) : "";
        out.push_back({n.empty() ? "Preset " + std::to_string(i + 1) : n, int(i)});
    }
    if (out.empty()) throw ParseError("no presets in this bank");
    return out;
}

Instrument load_exb(VolumePtr vol, const std::string &path, int index) {
    ExBank b = parse_bank(*vol, path);
    if (index < 0 || size_t(index) >= b.presets.size()) throw ParseError("no such preset");
    const Ck &preset = b.presets[size_t(index)];
    Instrument inst;
    inst.format = "E-mu Emulator X bank";
    Ck hdr;
    inst.name = preset.child("Phdr", hdr) ? ex_name(&b.d[hdr.off + 4], std::min<size_t>(64, hdr.size - 4)) : "";
    if (inst.name.empty()) inst.name = path_stem(path);
    inst.groups.clear();
    // samples: <folder>/SamplePool/<bank>SL###.ebl
    std::string pool, found;
    std::string bank_name = path_stem(path);
    std::map<int, ExSample> samples;
    std::map<int, SampleRefPtr> refs;
    if (find_ci(*vol, path_join(path_dir(path), "SamplePool"), pool)) {
        for (int si : b.sample_indices) {
            char fn[16];
            std::snprintf(fn, sizeof fn, "SL%03d.ebl", si);
            if (find_ci(*vol, path_join(pool, bank_name + fn), found)) {
                ExSample s = read_ebl_header(*vol, found);
                if (s.ok) { samples[si] = s; refs[si] = ebl_ref(vol, s); }
            }
        }
    }
    int controllers[16];
    std::fill(controllers, controllers + 16, -1);
    Ck ic;
    if (preset.child("E5IC", ic)) for (int i = 0; i < 16; i++) { int v = ic.u8(4 + size_t(i)); if (v != 0xFF) controllers[i] = v; }
    Ck vl;
    if (!preset.list("E5VL", vl)) throw ParseError("preset without voices");
    std::map<std::pair<int, int>, std::vector<int>> groups_by_vel;
    std::vector<std::pair<int, int>> group_keys;   // per group: key range check uses its zones
    int missing = 0;
    for (auto &voice : vl.children()) {
        if (!voice.is("E5V1")) continue;
        Ck wl;
        std::vector<Ck> ws = voice.list("TWL ", wl) ? wl.children() : std::vector<Ck>();
        int kw[4], vw[4];
        window(ws, 0, kw);
        window(ws, 1, vw);
        Ck osc, amp, flt, cl, zl;
        bool has_osc = voice.child("E5Oc", osc), has_amp = voice.child("E5Am", amp);
        double tune = has_osc ? osc.s8(14) + osc.s8(15) + osc.f(16) / 100.0 : 0;
        double pan = has_amp ? clampd(amp.s8(8) / 64.0, -1, 1) : 0;
        double gain = has_amp ? clampd(amp.f(4), -96, 10) : 0;
        Envelope aenv;
        bool has_aenv = read_env(voice, 0, aenv);
        // modulation cords
        double vel_vol = 0, vel_cut = 0, env_cut = 0, ctrl_cut = 0;
        bool cut_mod = false;
        if (voice.list("CrdL", cl))
            for (auto &c : cl.children()) {
                if (!c.is("E5Cd")) continue;
                int src = c.u8(4), dst = c.u8(5);
                double amt = c.f(6) / 100.0;
                if (src == 0x0C && dst == 0x40 && vel_vol == 0) vel_vol = clampd(std::fabs(amt), 0, 1);
                if (dst == 0x38) {
                    if (src == 0x0C && vel_cut == 0) vel_cut = clampd(amt, -1, 1);
                    if ((src == 0x50 || src == 0x51) && env_cut == 0) env_cut = clampd(amt, -1, 1);
                    int ctl = src - 0x14;
                    if (ctl >= 0 && ctl < 16 && controllers[ctl] >= 0) ctrl_cut += controllers[ctl] / 127.0 * amt;
                    if (amt != 0) cut_mod = true;
                }
            }
        Filter filt;
        if (voice.child("E5Fl", flt)) {
            struct { int id; FilterType t; int p; } F[] = {{0, FilterType::LowPass, 4}, {1, FilterType::LowPass, 2}, {2, FilterType::LowPass, 6},
                                                           {8, FilterType::HighPass, 2}, {9, FilterType::HighPass, 4}, {16, FilterType::BandPass, 2},
                                                           {17, FilterType::BandPass, 4}, {18, FilterType::BandReject, 6}};
            int ft = flt.u8(4);
            for (auto &f : F) if (f.id == ft) { filt.type = f.t; filt.poles = f.p; }
            if (filt.type != FilterType::None) {
                double start = clampd(flt.f(5) + ctrl_cut, 0, 1);
                if ((start < 0.01 && env_cut == 0 && cut_mod) || (filt.type == FilterType::LowPass && start >= 1)) filt.type = FilterType::None;
                else {
                    filt.cutoff = 57.0 * std::pow(20000.0 / 57.0, start);
                    Envelope fe;
                    if (env_cut != 0 && read_env(voice, 1, fe)) { filt.env_depth = env_cut; filt.env = fe; }
                    filt.vel_depth = vel_cut;
                }
            }
        }
        if (!voice.list("E5ZL", zl)) continue;
        bool have_hdr = false;
        Ck zh;
        for (auto &c : zl.children()) {
            if (c.is("Zhdr")) { zh = c; have_hdr = true; continue; }
            if (!have_hdr || !c.is_list("TWL ")) continue;
            have_hdr = false;
            int si = zh.u16(4);
            if (!si) continue;
            auto it = samples.find(si);
            if (it == samples.end()) { missing++; continue; }
            const ExSample &s = it->second;
            std::vector<Ck> zw = c.children();
            int zk[4], zv[4];
            window(zw, 0, zk);
            window(zw, 1, zv);
            Zone z;
            z.name = s.name;
            z.sample = refs[si];
            int klo = std::max(kw[0], zk[0]), khi = std::min(kw[3], zk[3]);
            z.key_lo = clampi(klo, 0, 127); z.key_hi = clampi(std::max(klo, khi), 0, 127);
            z.key_xfade_lo = std::max(kw[1], zk[1]); z.key_xfade_hi = std::max(kw[2], zk[2]);
            int vlo = std::max(vw[0], zv[0]), vhi = std::min(vw[3], zv[3]);
            z.vel_lo = std::max(1, clampi(vlo, 0, 127)); z.vel_hi = std::max(1, clampi(std::max(vlo, vhi), 0, 127));
            z.vel_xfade_lo = std::max(vw[1], zv[1]); z.vel_xfade_hi = std::max(vw[2], zv[2]);
            z.root = zh.u8(10);
            z.stop = s.frames;
            z.tune = tune;
            z.pan = pan;
            z.gain_db = gain;
            if (s.loop) { Loop l; l.start = s.loop_start; l.end = s.loop_end; z.loops.push_back(l); }
            if (has_aenv) z.amp_env = aenv;
            z.amp_vel_depth = vel_vol;
            z.filter = filt;
            // groups: one per velocity range, a new one when keys would overlap
            auto &cands = groups_by_vel[{z.vel_lo, z.vel_hi}];
            int group = -1;
            for (int g : cands) {
                bool overlap = false;
                for (auto &o : inst.zones) if (o.group == g && z.key_lo <= o.key_hi && o.key_lo <= z.key_hi) { overlap = true; break; }
                if (!overlap) { group = g; break; }
            }
            if (group < 0) {
                group = int(inst.groups.size());
                inst.groups.push_back(Group{"Group " + std::to_string(group + 1)});
                cands.push_back(group);
            }
            z.group = group;
            inst.zones.push_back(z);
        }
    }
    if (missing) inst.warnings.push_back(std::to_string(missing) + " samples missing from SamplePool");
    finish_instrument(inst);
    return inst;
}

std::vector<PresetInfo> list_single(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

Instrument load_ebl(VolumePtr vol, const std::string &path, int) {
    ExSample s = read_ebl_header(*vol, path);
    if (!s.ok) throw ParseError("not an Emulator X sample");
    Instrument inst;
    inst.format = "E-mu Emulator X sample";
    inst.name = s.name.empty() ? path_stem(path) : s.name;
    Zone z;
    z.name = inst.name;
    z.sample = ebl_ref(vol, s);
    z.stop = s.frames;
    if (s.loop) { Loop l; l.start = s.loop_start; l.end = s.loop_end; z.loops.push_back(l); }
    inst.zones.push_back(z);
    finish_instrument(inst);
    return inst;
}

bool probe_ex(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 16 && !std::memcmp(h, "FORM", 4) && !std::memcmp(h + 8, "E5B0TOC2", 8); }

}  // namespace

void register_emu_ex() {
    register_reader({"E-mu Emulator X bank", "exb", probe_ex, list_exb, load_exb});
    register_reader({"E-mu Emulator X sample", "ebl", probe_ex, list_single, load_ebl});
}

}  // namespace omni

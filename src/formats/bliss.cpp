// Omni Sampler: discoDSP Bliss presets (.zbp) and banks (.zbb) (2.1.1). Facts from discoDSP's own reader in SFZero v3
// (Modules/SFZero/sfzero/ZBPReader.cpp in github.com/reales/retromulator, MIT); own code:
//   - Both are ZIP archives. A .zbp holds program.xml (<program name=...><zones><zone .../>...</zones></program>) and its
//     samples as program_000/zone_NNN.flac (NNN = the zone's index, 3 digits). A .zbb holds bank.xml
//     (<bank><programs><program name= num_zones=>...</programs></bank>) and program_PPP/zone_NNN.flac per program;
//     programs named "---" with no zones are empty slots. A zone without its FLAC names an outside file in path=.
//   - A zone's attributes: lo_input_range / hi_input_range children (midi_key, midi_vel), midi_root_key,
//     midi_keycents (100 = normal key tracking), midi_coarse_tune (semitones), midi_fine_tune (cents), midi_trigger
//     (1 = on release), loop_mode (0 one shot (the release still ends it), 1 forward, 2 bidirectional, 3 backward,
//     4 sustained = loops while held, 5 crossfaded), loop_start / loop_end (frames), mp_gain (dB), mp_pan (-100..100),
//     vel_amp (velocity to amplitude 0..1), sys_pit_rng (bend range, x 2400 cents), res_group / res_offby (choke
//     groups), amp_env_att / _dec / _sus / _rel children (value= 0..1; times 0.001 + x^4 x 15.999 s).
//   - Cues: num_cues, a cue_pos child (val0 = .. valN-1 = frame offsets), cue_stop (1 = a slice ends at the next cue),
//     cue_white (1 = slices on white keys only), num_samples. Two or more cues: one zone per slice from the root key up,
//     each at its own pitch; one cue: the zone starts there.
// Assumed (not in that reader): loop_end is the last frame of the loop, as in SFZ; mode 5's crossfade length is not
// stored there, so it gets a short one (a quarter of the loop, at most 100 ms).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include "../core/parse.hpp"
#include "format.hpp"

namespace omni {
namespace {

// ---- a ZIP archive's directory (stored and deflated entries; no ZIP64, no encryption)

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint64_t comp = 0, size = 0, local = 0;
};

uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }

std::vector<ZipEntry> zip_dir(const Blob &f) {
    const uint64_t size = f.size();
    const size_t tail = size_t(std::min<uint64_t>(size, 65557));
    std::vector<uint8_t> t = f.head(tail, size - tail);
    size_t eocd = std::string::npos;
    if (t.size() >= 22)
        for (size_t i = t.size() - 21; i-- > 0;)
            if (rd32(&t[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) throw ParseError("Bliss: not a ZIP archive (no end of directory)");
    uint16_t count = rd16(&t[eocd + 10]);
    uint32_t dsize = rd32(&t[eocd + 12]), doff = rd32(&t[eocd + 16]);
    if (doff == 0xFFFFFFFFu || uint64_t(doff) + dsize > size) throw ParseError("Bliss: ZIP64 or damaged archive");
    std::vector<uint8_t> d = f.bytes(doff, dsize);
    std::vector<ZipEntry> out;
    size_t p = 0;
    for (int i = 0; i < count && p + 46 <= d.size(); i++) {
        if (rd32(&d[p]) != 0x02014b50) throw ParseError("Bliss: damaged ZIP directory");
        ZipEntry e;
        e.method = rd16(&d[p + 10]);
        e.comp = rd32(&d[p + 20]);
        e.size = rd32(&d[p + 24]);
        size_t nl = rd16(&d[p + 28]), xl = rd16(&d[p + 30]), cl = rd16(&d[p + 32]);
        e.local = rd32(&d[p + 42]);
        if (p + 46 + nl > d.size()) break;
        e.name.assign(reinterpret_cast<const char *>(&d[p + 46]), nl);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        out.push_back(std::move(e));
        p += 46 + nl + xl + cl;
    }
    return out;
}

const ZipEntry *zip_find(const std::vector<ZipEntry> &dir, const std::string &name) {
    for (const ZipEntry &e : dir) if (e.name == name) return &e;
    for (const ZipEntry &e : dir)   // case or a leading "./" may differ
        if (e.name.size() >= name.size() && !strcasecmp(e.name.c_str() + e.name.size() - name.size(), name.c_str()) &&
            (e.name.size() == name.size() || e.name[e.name.size() - name.size() - 1] == '/'))
            return &e;
    return nullptr;
}

// where an entry's data starts (after its local header, whose name / extra lengths may differ from the directory's)
uint64_t zip_data_at(const Blob &f, const ZipEntry &e) {
    std::vector<uint8_t> h = f.bytes(e.local, 30);
    if (rd32(h.data()) != 0x04034b50) throw ParseError("Bliss: damaged ZIP entry " + e.name);
    return e.local + 30 + rd16(&h[26]) + rd16(&h[28]);
}

// an entry's bytes as a blob: a slice of the archive when stored, else inflated
BlobPtr zip_open(const BlobPtr &f, const ZipEntry &e) {
    uint64_t at = zip_data_at(*f, e);
    if (at + e.comp > f->size()) throw ParseError("Bliss: ZIP entry past the end: " + e.name);
    if (e.method == 0) return make_slice(f, at, e.comp);
    if (e.method != 8) throw ParseError("Bliss: ZIP method " + std::to_string(e.method) + " (" + e.name + ")");
    std::vector<uint8_t> c = f->bytes(at, size_t(e.comp));
    return make_mem_blob(inflate_raw(c.data(), c.size(), size_t(e.size)));
}

std::string zip_text(const BlobPtr &f, const ZipEntry &e) {
    std::vector<uint8_t> b = zip_open(f, e)->all();
    return std::string(b.begin(), b.end());
}

// ---- the XML

struct Doc {
    std::unique_ptr<XmlNode> root;
    std::vector<const XmlNode *> programs;   // with their indices in the archive (program_PPP)
    std::vector<int> index;
    bool bank = false;
};

Doc read_doc(const BlobPtr &f, const std::vector<ZipEntry> &dir) {
    Doc d;
    const ZipEntry *x = zip_find(dir, "bank.xml");
    d.bank = x != nullptr;
    if (!x) x = zip_find(dir, "program.xml");
    if (!x) throw ParseError("Bliss: no program.xml or bank.xml in the archive");
    d.root = parse_xml(zip_text(f, *x));
    if (d.bank) {
        if (d.root->name != "bank") throw ParseError("Bliss: bank.xml has no <bank>");
        const XmlNode *ps = d.root->child("programs");
        if (!ps) throw ParseError("Bliss: bank.xml has no <programs>");
        int i = 0;
        for (const XmlNode *p : ps->all("program")) {
            if (p->attr_num("num_zones", 0) > 0 || p->attr("name", "---") != "---") { d.programs.push_back(p); d.index.push_back(i); }
            i++;
        }
    } else {
        if (d.root->name != "program") throw ParseError("Bliss: program.xml has no <program>");
        d.programs.push_back(d.root.get());
        d.index.push_back(0);
    }
    return d;
}

double env_time(double x) { return x <= 0 ? 0.001 : 0.001 + x * x * x * x * 15.999; }
int iattr(const XmlNode *n, const char *k, int def) { return n ? int(std::lround(n->attr_num(k, def))) : def; }
double child_value(const XmlNode *z, const char *k, double def) {
    const XmlNode *c = z->child(k);
    return c ? c->attr_num("value", def) : def;
}

bool probe_bliss(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 4 && !std::memcmp(h, "PK\x03\x04", 4); }

std::vector<PresetInfo> list_bliss(VolumePtr vol, const std::string &path) {
    BlobPtr f = vol->open(path);
    Doc d = read_doc(f, zip_dir(*f));
    std::vector<PresetInfo> out;
    for (size_t i = 0; i < d.programs.size(); i++) {
        std::string name = d.programs[i]->attr("name", "");
        if (name.empty() || name == "---") name = d.bank ? "Program " + std::to_string(d.index[i] + 1) : path_stem(path);
        if (d.bank) { char num[16]; std::snprintf(num, sizeof num, "%03d ", d.index[i] + 1); name = num + name; }
        out.push_back({name, int(i)});
    }
    if (out.empty()) throw ParseError("Bliss: the bank has no programs");
    return out;
}

Instrument load_bliss(VolumePtr vol, const std::string &path, int index) {
    BlobPtr f = vol->open(path);
    std::vector<ZipEntry> dir = zip_dir(*f);
    Doc d = read_doc(f, dir);
    if (index < 0 || index >= int(d.programs.size())) throw ParseError("Bliss: no such program");
    const XmlNode *prog = d.programs[size_t(index)];
    const int pi = d.index[size_t(index)];
    Instrument inst;
    inst.format = d.bank ? "Bliss bank" : "Bliss preset";
    inst.name = prog->attr("name", "");
    if (inst.name.empty() || inst.name == "---") inst.name = path_stem(path);
    if (d.bank) inst.library = path_stem(path);
    const XmlNode *zones = prog->child("zones");
    int zi = 0, missing = 0;
    for (const XmlNode *zx : zones ? zones->all("zone") : std::vector<const XmlNode *>{}) {
        const int this_zone = zi++;
        char ename[48];
        std::snprintf(ename, sizeof ename, "program_%03d/zone_%03d.flac", pi, this_zone);
        SampleRefPtr ref;
        if (const ZipEntry *e = zip_find(dir, ename)) {
            ref = std::make_shared<SampleRef>();
            ref->name = zx->attr("name", "");
            if (ref->name.empty()) ref->name = inst.name + " " + std::to_string(this_zone + 1);
            ref->key = "zbp:" + path + ":" + e->name;
            if (e->method == 0) {   // stored: the stream info is cheap
                try {
                    AudioInfo ai = probe_audio(*zip_open(f, *e));
                    ref->rate = ai.rate; ref->channels = ai.channels; ref->frames = ai.frames;
                } catch (const std::exception &) {}
            }
            ZipEntry ent = *e;
            ref->decode = [f, ent]() { return decode_audio(*zip_open(f, ent)); };
        } else {
            std::string ext = zx->attr("path", "");
            std::string found;
            if (!ext.empty() && find_ci(*vol, path_resolve(path_dir(path), ext), found)) ref = file_sample_ref(vol, found);
        }
        if (!ref) { missing++; continue; }

        Zone z;
        z.name = ref->name;
        z.sample = ref;
        const XmlNode *lo = zx->child("lo_input_range"), *hi = zx->child("hi_input_range");
        z.key_lo = clampi(iattr(lo, "midi_key", 0), 0, 127);
        z.key_hi = clampi(iattr(hi, "midi_key", 127), z.key_lo, 127);
        z.vel_lo = clampi(iattr(lo, "midi_vel", 0), 0, 127);
        z.vel_hi = clampi(iattr(hi, "midi_vel", 127), z.vel_lo, 127);
        z.root = clampi(iattr(zx, "midi_root_key", 60), 0, 127);
        z.key_tracking = zx->attr_num("midi_keycents", 100) / 100.0;
        z.tune = zx->attr_num("midi_coarse_tune", 0) + zx->attr_num("midi_fine_tune", 0) / 100.0;
        if (iattr(zx, "midi_trigger", 0) == 1) z.trigger = Trigger::Release;
        z.gain_db = zx->attr_num("mp_gain", 0);
        z.pan = clampd(zx->attr_num("mp_pan", 0) / 100.0, -1, 1);
        z.amp_vel_depth = clampd(zx->attr_num("vel_amp", 1), 0, 1);
        int bend = int(std::lround(zx->attr_num("sys_pit_rng", 1.0 / 12) * 2400));
        z.bend_up = bend; z.bend_down = -bend;
        z.exclusive_group = std::max(0, iattr(zx, "res_group", 0));
        z.off_by = std::max(0, iattr(zx, "res_offby", 0));
        if (zx->child("amp_env_att") || zx->child("amp_env_dec") || zx->child("amp_env_sus") || zx->child("amp_env_rel")) {
            z.amp_env.set = true;
            z.amp_env.attack = env_time(child_value(zx, "amp_env_att", 0));
            z.amp_env.decay = env_time(child_value(zx, "amp_env_dec", 0.5));
            z.amp_env.sustain = clampd(child_value(zx, "amp_env_sus", 1), 0, 1);
            z.amp_env.release = env_time(child_value(zx, "amp_env_rel", 0.2));
        }
        int mode = iattr(zx, "loop_mode", 0);
        int64_t ls = int64_t(zx->attr_num("loop_start", 0)), le = int64_t(zx->attr_num("loop_end", 0));
        if (mode >= 1 && mode <= 5 && le > ls) {
            Loop l;
            l.start = ls; l.end = le;
            l.type = mode == 2 ? LoopType::Alternating : mode == 3 ? LoopType::Backward : LoopType::Forward;
            l.until_release = mode == 4;
            if (mode == 5) l.crossfade = std::min<int64_t>((le - ls) / 4, ref->rate > 0 ? ref->rate / 10 : 4410);
            z.loops.push_back(l);
        }

        // cues
        int ncues = iattr(zx, "num_cues", 0), nsamp = iattr(zx, "num_samples", 0);
        std::vector<int64_t> cues;
        if (const XmlNode *cp = zx->child("cue_pos"))
            for (int c = 0; c < ncues; c++) cues.push_back(int64_t(cp->attr_num("val" + std::to_string(c), 0)));
        if (cues.size() > 1) {
            static const bool white[12] = {1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1};
            const bool only_white = iattr(zx, "cue_white", 0) != 0, stop = iattr(zx, "cue_stop", 1) != 0;
            int key = z.root;
            for (size_t c = 0; c < cues.size() && key <= 127; c++, key++) {
                if (only_white) while (key <= 127 && !white[key % 12]) key++;
                if (key > 127) break;
                Zone s = z;
                s.key_lo = s.key_hi = s.root = key;
                s.key_tracking = 0;
                s.start = cues[c];
                if (stop && c + 1 < cues.size()) s.stop = cues[c + 1];
                else if (nsamp > 0) s.stop = nsamp;
                s.loops.clear();
                inst.zones.push_back(std::move(s));
            }
            continue;
        }
        if (cues.size() == 1) z.start = cues[0];
        inst.zones.push_back(std::move(z));
    }
    if (missing) inst.warnings.push_back(std::to_string(missing) + " zone(s) without their sample left out");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("Bliss: this program has no samples");
    return inst;
}

}  // namespace

void register_bliss() {
    register_reader({"Bliss", "zbp zbb", probe_bliss, list_bliss, load_bliss});
}

}  // namespace omni

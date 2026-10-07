// Omni Sampler: Roland samplers.
//  - S-700 series (S-750 / S-760 / S-770 / SP-700 / DJ-70): CD-ROM and hard-disk images ("S770" at offset 4: FAT,
//    directories, then the volume / performance / patch / partial / sample parameter areas and the audio area) and
//    single floppies.
//  - S-500 family (S-50 / S-51 / S-550 / S-330 / W-30): floppy images and "LAND" CD-ROMs holding many floppies.
// An image mounts as folders: S-700 volumes (banks) with their patches and performances, plus every patch; S-500
// disks with their patches. Each entry is a small virtual file the reader below turns into an instrument, reading
// only the samples that instrument uses. Translated from ConvertWithMoss's format/roland/s7xx and s5xx (LGPL-3.0).
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include "../core/audio.hpp"
#include "../fs/fs.hpp"
#include "format.hpp"

namespace omni {
namespace {

std::string clean(const uint8_t *p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        char c = p[i] >= 0x20 && p[i] < 0x7F ? char(p[i]) : ' ';
        if (c == ' ' && !s.empty() && s.back() == ' ') continue;   // names are padded inside too ("Guitar #1    JP")
        s += c;
    }
    return trim(s);
}
std::string safe_name(std::string s) {
    for (char &c : s) {
        if (c == ':') c = ' ';   // Roland category prefixes ("PNO:Grand")
        else if (c == '/' || c == '\\' || c == '?' || c == '*') c = '_';
    }
    s = trim(s);
    return s.empty() ? "unnamed" : s;
}
std::vector<PresetInfo> list_single(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }
double to_db(double v) { return v < 3e-8 ? -150 : std::max(-150.0, std::log(v) * 8.6858896380650365); }
double denorm_cutoff(double v) { return clampd(2.0 * 440.0 * std::pow(2.0, (v * 140.0 - 57.0) / 12.0), 32.7, MAX_CUTOFF_HZ); }

// ---------------------------------------------------------------------------------------------------------------
// S-700

const int S7_TIMES[128] = {30, 32, 34, 36, 38, 41, 43, 46, 49, 52, 55, 58, 62, 66, 70, 74, 79, 84, 89, 95, 101, 107, 114, 121,
    128, 136, 145, 154, 163, 174, 184, 196, 208, 221, 235, 250, 265, 282, 299, 318, 338, 359, 381, 405, 431, 457, 486, 516,
    549, 583, 619, 658, 699, 743, 789, 838, 890, 946, 1005, 1068, 1134, 1205, 1280, 1360, 1445, 1535, 1631, 1733, 1841, 1956,
    2078, 2208, 2346, 2492, 2648, 2813, 2989, 3175, 3373, 3584, 3808, 4045, 4298, 4566, 4851, 5154, 5475, 5817, 6180, 6566,
    6976, 7411, 7874, 8365, 8887, 9442, 10031, 10657, 11323, 12029, 12780, 13578, 14425, 15326, 16282, 17298, 18378, 19525,
    20744, 22038, 23414, 24875, 26428, 28077, 29830, 31692, 33670, 35771, 38004, 40376, 42896, 45573, 48418, 51440, 54650,
    58061, 61685, 65535};
double s7_time(int v) { return S7_TIMES[std::max(0, std::min(127, v))] / 3000.0; }
const int S7_RATES[6] = {48000, 44100, 24000, 22050, 30000, 15000};
constexpr uint64_t S7_BLOCK = 0x2400;

struct S7Sample {
    std::string name;
    uint32_t start = 0, sus_ls = 0, sus_le = 0, rel_ls = 0, rel_le = 0;
    int loop_mode = 0, sus_tune = 0, seg_top = 0, seg_len = 0, freq = 1, key = 60;
    std::vector<std::pair<uint64_t, uint64_t>> ext;   // byte extents of the wave data in the image
};
struct S7Section { int sel = -1, pitch_kf = 8, level = 127, pan = 0, coarse = 0, fine = 0, vlo = 0, vfl = 0, vhi = 127, vfh = 0; };
struct S7Env { int levels[4] = {}, times[4] = {}; int time_kf = 0, time_vel = 0; };
struct S7Partial {
    std::string name;
    S7Section s[4];
    int mix = 127, level = 127, pan = 0, coarse = 0, fine = 0;
    int tvf_mode = 0, cutoff = 127, reso = 0, tvf_vcurve = 0, cutoff_vel = 0, env_tvf = 0, env_pitch = 0, cutoff_kf = 0;
    S7Env fenv;
    int tva_vcurve = 0, level_kf = 0;
    S7Env aenv;
};
struct S7Patch {
    std::string name;
    bool active = true;
    int mix = 127, total_pan = 0, level = 127, octave = 0, coarse = 0, fine = 0, bend_up = 2, bend_down = 2;
    int keys[88] = {}, assign[88] = {};
};
struct S7Perf {
    std::string name;
    int patch[32], channel[32], level[32], lo[32], hi[32], pan[32];
    bool enabled[32];
};
struct S7Volume { std::string name; int perf[64]; };

struct S7Image {
    BlobPtr img;
    bool diskette = false;
    std::string disk_name;
    std::vector<S7Volume> volumes;
    std::vector<S7Perf> perfs;
    std::vector<S7Patch> patches;
    std::vector<S7Partial> partials;
    std::vector<S7Sample> samples;
};

void read_env(Reader &r, S7Env &e, bool tva) {
    for (int &l : e.levels) l = r.u8();
    for (int &t : e.times) t = r.u8();
    (void)tva;
}

S7Partial read_partial(Reader &r) {
    S7Partial p;
    auto section = [&](S7Section &s) {
        s.sel = int16_t(r.u16le()); s.pitch_kf = int8_t(r.u8()); s.level = r.u8(); s.pan = int8_t(r.u8());
        s.coarse = int8_t(r.u8()); s.fine = int8_t(r.u8()); s.vlo = r.u8(); s.vfl = r.u8(); s.vhi = r.u8(); s.vfh = r.u8();
    };
    p.name = clean(r.take(16), 16);
    section(p.s[0]);
    r.u8(); r.u8(); p.mix = r.u8(); p.level = r.u8(); r.u8();
    section(p.s[1]);
    r.u8(); p.pan = int8_t(r.u8()); p.coarse = int8_t(r.u8()); p.fine = int8_t(r.u8()); r.u8();
    section(p.s[2]);
    r.skip(5);
    section(p.s[3]);
    // TVF
    p.tvf_mode = r.u8(); p.cutoff = r.u8(); p.reso = r.u8(); p.tvf_vcurve = r.u8(); r.u8();
    p.fenv.time_vel = int8_t(r.u8()); p.cutoff_vel = int8_t(r.u8());
    read_env(r, p.fenv, false);
    p.env_tvf = int8_t(r.u8()); p.env_pitch = int8_t(r.u8()); r.u8(); p.fenv.time_kf = int8_t(r.u8()); r.u8(); p.cutoff_kf = int8_t(r.u8());
    // TVA
    p.tva_vcurve = r.u8(); r.u8(); p.aenv.time_vel = int8_t(r.u8());
    read_env(r, p.aenv, true);
    r.u8(); r.u8(); p.aenv.time_kf = int8_t(r.u8()); r.u8(); p.level_kf = int8_t(r.u8());
    r.skip(9 + 7);   // LFO, padding
    return p;
}

S7Patch read_patch(Reader &r, bool diskette) {
    S7Patch p;
    p.name = clean(r.take(16), 16);
    r.u8(); p.mix = r.u8(); p.total_pan = int8_t(r.u8()); p.level = r.u8(); r.u8(); r.u8(); r.u8(); r.u8();
    p.octave = int8_t(r.u8()); p.coarse = int8_t(r.u8()); p.fine = int8_t(r.u8());
    r.skip(4 + 1);
    for (int &k : p.keys) k = int8_t(r.u8());
    r.skip(8);
    for (int &a : p.assign) a = r.u8();
    r.skip(8);
    p.bend_up = r.u8(); p.bend_down = r.u8(); r.skip(2);   // bender
    r.skip(7 + 4 + 1 + 8 + 8);                              // aftertouch, modulation, controller
    if (!diskette) {
        for (int &k : p.keys) k = int16_t(r.u16le());
        r.skip(0x50);
    }
    return p;
}

S7Perf read_perf(Reader &r, bool diskette) {
    S7Perf p;
    p.name = clean(r.take(16), 16);
    for (int &v : p.patch) v = int8_t(r.u8());
    for (int i = 0; i < 16; i++) { int v = r.u8(); p.channel[2 * i] = v & 15; p.channel[2 * i + 1] = v >> 4 & 15; }
    for (int i = 0; i < 32; i++) { int v = r.u8(); p.enabled[i] = v & 0x80; p.level[i] = v & 0x7F; }
    for (int &v : p.lo) v = r.u8();
    for (int &v : p.hi) v = r.u8();
    r.skip(32 + 32 + 16 + 16);   // fade widths, the per-channel switches, velocity curves
    for (int &v : p.pan) v = 0;
    if (!diskette) {
        for (int &v : p.patch) v = int16_t(r.u16le());
        r.skip(32 * 3 + 32);
        for (int &v : p.pan) v = int8_t(r.u8());
        r.skip(32);
    }
    return p;
}

S7Sample read_sample(Reader &r) {
    S7Sample s;
    s.name = clean(r.take(16), 16);
    s.start = r.u32le() >> 8; s.sus_ls = r.u32le() >> 8; s.sus_le = r.u32le() >> 8; s.rel_ls = r.u32le() >> 8; s.rel_le = r.u32le() >> 8;
    s.loop_mode = r.u8(); r.u8(); s.sus_tune = int8_t(r.u8()); r.u8();
    s.seg_top = r.u16le(); s.seg_len = r.u16le(); s.freq = r.u8() & 15; s.key = r.u8(); r.skip(2);
    return s;
}

std::shared_ptr<S7Image> parse_s7(BlobPtr img) {
    auto im = std::make_shared<S7Image>();
    im->img = img;
    uint64_t size = img->size();
    im->diskette = size <= 1474560;
    if (im->diskette) {
        std::vector<uint8_t> d = img->head(size_t(size));
        if (d.size() < 0x1F800) throw ParseError("truncated Roland S-700 floppy");
        int idx = d[0x100], ndisks = d[0x101];
        if (idx != 0) throw ParseError("this is a continuation floppy: open the first disk of the set");
        if (ndisks > 0) throw ParseError("multi-floppy Roland S-700 sets are not supported yet");
        int np = le16(&d[0x108]), npat = le16(&d[0x10A]), npar = le16(&d[0x10C]), ns = le16(&d[0x10E]);
        im->disk_name = clean(&d[0x180], 16);
        Reader r(d);
        r.seek(0x4E00);
        for (int i = 0; i < std::min(np, 64); i++) { Reader rr = r.sub(256); im->perfs.push_back(read_perf(rr, true)); }
        r.seek(0x8E00);
        for (int i = 0; i < std::min(npat, 128); i++) { Reader rr = r.sub(256); im->patches.push_back(read_patch(rr, true)); }
        r.seek(0x10E00);
        for (int i = 0; i < std::min(npar, 256); i++) { Reader rr = r.sub(128); im->partials.push_back(read_partial(rr)); }
        r.seek(0x18E00);
        uint64_t blocks = (size - 0x1F800) / S7_BLOCK;
        for (int i = 0; i < ns; i++) {
            Reader rr = r.sub(48);
            S7Sample s = read_sample(rr);
            if ((i + 1) % 10 == 0) r.skip(32);
            uint64_t phys = uint64_t(s.seg_top) / 256 * blocks + uint64_t(s.seg_top) % 256;
            uint64_t off = 0x1F800 + phys * S7_BLOCK, len = uint64_t(s.seg_len) * S7_BLOCK;
            if (off + len <= size && len) s.ext.push_back({off, len});
            im->samples.push_back(std::move(s));
        }
        S7Volume v;
        v.name = im->disk_name.empty() ? "Disk" : im->disk_name;
        for (int i = 0; i < 64; i++) v.perf[i] = i < int(im->perfs.size()) ? i : -1;
        im->volumes.push_back(v);
        return im;
    }
    // CD-ROM / hard disk: header 0x200, reserved and program text, FAT (0x10000 x u16), directories, parameters
    constexpr uint64_t FAT_OFF = 0x200 + 0x600 + 0x80000, DIR_OFF = FAT_OFF + 0x20000;
    constexpr int NV = 128, NP = 512, NPAT = 1024, NPAR = 4096, NS = 8192;
    constexpr uint64_t PARAM_OFF = DIR_OFF + uint64_t(NV + NP + NPAT + NPAR + NS) * 32, AUDIO_OFF = 0x2B5800;
    if (size < AUDIO_OFF) throw ParseError("truncated Roland S-700 image");
    std::vector<uint8_t> h = img->head(0x200);
    im->disk_name = clean(&h[0x100], 16);
    std::vector<uint8_t> fat = img->bytes(FAT_OFF, 0x20000);
    std::vector<uint8_t> dir = img->bytes(DIR_OFF, size_t(PARAM_OFF - DIR_OFF));
    std::vector<uint8_t> par = img->bytes(PARAM_OFF, size_t(AUDIO_OFF - PARAM_OFF));
    auto dir_free = [&](int base, int i) { uint8_t c = dir[size_t(base + i) * 32]; return c == 0 || c == 0xFE; };
    const int DV = 0, DP = NV, DPAT = NV + NP, DPAR = DPAT + NPAT, DS = DPAR + NPAR;
    Reader r(par);
    for (int i = 0; i < NV; i++) {
        Reader rr = r.sub(256);
        S7Volume v;
        v.name = clean(rr.take(16), 16);
        rr.skip(16);
        for (int &p : v.perf) { int x = rr.u16le(); p = x >= NP ? -1 : x; }
        if (!dir_free(DV, i) && !v.name.empty()) im->volumes.push_back(v);
    }
    for (int i = 0; i < NP; i++) {
        Reader rr = r.sub(512);
        S7Perf p = read_perf(rr, false);
        if (dir_free(DP, i)) p.name.clear();
        im->perfs.push_back(p);
    }
    for (int i = 0; i < NPAT; i++) {
        Reader rr = r.sub(512);
        S7Patch p = read_patch(rr, false);
        p.active = !dir_free(DPAT, i);
        im->patches.push_back(p);
    }
    for (int i = 0; i < NPAR; i++) { Reader rr = r.sub(128); im->partials.push_back(read_partial(rr)); }
    for (int i = 0; i < NS; i++) {
        Reader rr = r.sub(48);
        S7Sample s = read_sample(rr);
        if (!dir_free(DS, i) && dir[size_t(DS + i) * 32 + 16] == 0x44) {
            const uint8_t *e = &dir[size_t(DS + i) * 32];
            int fat_idx = le16(e + 28), n = le16(e + 30);
            for (int k = 0; k < n && fat_idx >= 2 && fat_idx <= 0xFFF5; k++) {
                uint64_t off = AUDIO_OFF + uint64_t(fat_idx - 2) * S7_BLOCK;
                if (off >= size) break;
                uint64_t len = std::min<uint64_t>(S7_BLOCK, size - off);
                if (!s.ext.empty() && s.ext.back().first + s.ext.back().second == off) s.ext.back().second += len;
                else s.ext.push_back({off, len});
                int next = le16(&fat[size_t(fat_idx) * 2]);
                if (next >= 0xFFF8) break;
                fat_idx = next;
            }
        }
        im->samples.push_back(std::move(s));
    }
    return im;
}

Envelope s7_env(const S7Env &e, double &peak) {
    Envelope v;
    v.set = true;
    peak = std::max(1, e.levels[1]) / 127.0;
    v.start_level = e.levels[0] / 127.0 / peak;
    v.sustain = clampd(e.levels[2] / 127.0 / peak, 0, 1);
    v.end_level = clampd(e.levels[3] / 127.0 / peak, 0, 1);
    v.delay = s7_time(e.times[0]);
    v.attack = s7_time(e.times[1]);
    v.decay = s7_time(e.times[2]);
    v.release = s7_time(e.times[3]);
    v.time_key_tracking = clampd(e.time_kf / 63.0, -1, 1);
    v.time_vel_tracking = clampd(e.time_vel / 63.0, -1, 1);
    return v;
}

SampleRefPtr s7_sample_ref(const S7Image &im, int idx) {
    const S7Sample &s = im.samples[size_t(idx)];
    if (s.ext.empty()) return nullptr;
    uint64_t bytes = 0;
    for (auto &e : s.ext) bytes += e.second;
    BlobPtr b = s.ext.size() == 1 ? make_slice(im.img, s.ext[0].first, s.ext[0].second) : make_extents(im.img, s.ext);
    int rate = S7_RATES[s.freq < 6 ? s.freq : 1];
    auto ref = raw_sample_ref(b, 0, int64_t(bytes / 2), 1, Enc::S16LE, rate, s.name.empty() ? "sample " + std::to_string(idx + 1) : s.name);
    return ref;
}

// zones of one S-700 patch into inst (group base: the patch's 4 layers start at inst.groups.size())
void s7_patch_zones(const S7Image &im, int pi, Instrument &inst, int channel, int clip_lo, int clip_hi, double part_db, double part_pan,
                    std::map<int, SampleRefPtr> &refs, int &missing) {
    const S7Patch &p = im.patches[size_t(pi)];
    int gbase = int(inst.groups.size());
    for (int i = 0; i < 4; i++) inst.groups.push_back(Group{(p.name.empty() ? "Patch" : p.name) + " layer " + std::to_string(i + 1)});
    int key = 21;
    while (key <= 108) {
        int pid = p.keys[key - 21], hi = key;
        while (hi + 1 <= 108 && p.keys[hi + 1 - 21] == pid) hi++;
        if (pid >= 0 && pid < int(im.partials.size())) {
            const S7Partial &pa = im.partials[size_t(pid)];
            for (int li = 0; li < 4; li++) {
                const S7Section &sec = pa.s[li];
                if (sec.sel < 0 || sec.sel >= int(im.samples.size())) continue;
                auto it = refs.find(sec.sel);
                if (it == refs.end()) it = refs.emplace(sec.sel, s7_sample_ref(im, sec.sel)).first;
                if (!it->second) { missing++; continue; }
                const S7Sample &s = im.samples[size_t(sec.sel)];
                Zone z;
                z.name = s.name;
                z.sample = it->second;
                z.group = gbase + li;
                z.key_lo = key; z.key_hi = hi;
                z.vel_lo = sec.vlo; z.vel_hi = std::max(sec.vlo, sec.vhi);
                z.vel_xfade_lo = sec.vfl; z.vel_xfade_hi = sec.vfh;
                z.root = s.key;
                z.start = s.start;
                z.midi_channel = channel;
                if (s.loop_mode == 2) z.one_shot = true;
                else if (s.loop_mode <= 6 && s.sus_le > s.sus_ls) {
                    Loop l;
                    l.type = s.loop_mode == 4 ? LoopType::Alternating : s.loop_mode >= 5 ? LoopType::Backward : LoopType::Forward;
                    l.until_release = s.loop_mode == 1;
                    l.start = s.sus_ls; l.end = s.sus_le;
                    l.tune = s.sus_tune / 100.0;
                    z.loops.push_back(l);
                }
                if (sec.pitch_kf >= 0 && sec.pitch_kf <= 8) z.key_tracking = sec.pitch_kf / 8.0;
                z.tune = p.coarse + pa.coarse + sec.coarse + (p.fine + pa.fine + sec.fine) / 100.0 + 12.0 * p.octave;
                double peak = 1;
                z.amp_env = s7_env(pa.aenv, peak);
                double lev = p.level / 127.0 * (p.mix / 127.0) * (pa.level / 127.0) * (pa.mix / 127.0) * (sec.level / 127.0) * peak;
                z.gain_db = to_db(lev) + part_db;
                double pan = p.total_pan / 31.0 + pa.pan / 32.0 + (sec.pan <= 32 ? sec.pan / 32.0 : 0) + part_pan;
                z.pan = clampd(pan, -1, 1);
                z.amp_vel_depth = pa.tva_vcurve == 0 ? 0 : 1;
                z.amp_key_tracking = clampd(pa.level_kf / 63.0, -1, 1);
                if (pa.tvf_mode <= 2) {
                    double fpeak = 1;
                    z.filter.type = pa.tvf_mode == 0 ? FilterType::LowPass : pa.tvf_mode == 1 ? FilterType::BandPass : FilterType::HighPass;
                    z.filter.poles = 4;
                    z.filter.cutoff = denorm_cutoff(pa.cutoff / 127.0);
                    z.filter.resonance = pa.reso / 127.0;
                    z.filter.env = s7_env(pa.fenv, fpeak);
                    z.filter.env_depth = pa.env_tvf / 63.0 * fpeak;
                    z.filter.vel_depth = pa.tvf_vcurve == 0 ? 0 : pa.cutoff_vel / 63.0;
                    z.filter.key_tracking = clampd(pa.cutoff_kf / 63.0, -1, 1);
                }
                if (pa.env_pitch != 0) {
                    double ppeak = 1;
                    z.pitch_env = s7_env(pa.fenv, ppeak);
                    z.pitch_env_depth = pa.env_pitch / 63.0 * ppeak;
                }
                z.bend_up = p.bend_up * 100; z.bend_down = -p.bend_down * 100;
                // the octave shift moves the key range; tune above compensates the pitch
                z.key_lo = std::max(0, z.key_lo - 12 * p.octave);
                z.key_hi = std::min(127, z.key_hi - 12 * p.octave);
                z.key_lo = std::max(z.key_lo, clip_lo);
                z.key_hi = std::min(z.key_hi, clip_hi);
                if (z.key_lo <= z.key_hi) inst.zones.push_back(z);
            }
        }
        key = hi + 1;
    }
    bool mono = false, any = false;
    for (int k = 0; k < 88; k++) if (p.keys[k] >= 0) { any = true; mono = p.assign[k] == 1; if (!mono) break; }
    if (any && mono && channel < 0) inst.polyphony = 1;
}

// ---------------------------------------------------------------------------------------------------------------
// S-500 family

enum class S5Type { S550, S330, S50, W30, Land, Unknown };
S5Type s5_type(const uint8_t *id) {
    std::string s(reinterpret_cast<const char *>(id), 4);
    if (s == "S550") return S5Type::S550;
    if (s == "S330") return S5Type::S330;
    if (s == "S-50" || s == "S-51" || s == "S500") return S5Type::S50;
    if (s == "W-30") return S5Type::W30;
    if (s == "LAND") return S5Type::Land;
    return S5Type::Unknown;
}

constexpr uint64_t S5_PATCHES = 64512, S5_TONES = 69120, S5_TONE_LIST = 0x11E00, S5_WAVE_FLOPPY = 0x12000, S5_WAVE_CD = 0x2400;
constexpr int S5_SEG = 12288;

struct S5Tone {
    std::string name;
    bool disabled = true;
    int out = 0, source = 0, sub = 0, freq = 0, key = 60, bank = 0, seg_top = 0, seg_len = 0;
    int start = 0, end = 0, loop_point = 0, loop_mode = 0, loop_len = 0, loop_tune = 0;
    int transpose = 0, fine = 0, cutoff = 127, reso = 0, tvf_kf = 0, eg_depth = 0, eg_pol = 0, tvf_sw = 0;
    int a_sus = 0, a_end = 0, a_lev[8] = {}, a_rate[8] = {}, level = 127, pitch_follow = 1;
    int f_sus = 0, f_end = 0, f_lev[8] = {}, f_rate[8] = {};
};
struct S5Patch { std::string name; int bend = 2, key_mode = 0, vel_sw = 64, octave = 0, level = 127, detune = 0; int t1[128], t2[128]; };
struct S5Disk {
    std::string name;
    S5Type type = S5Type::S550;
    uint64_t base = 0;         // where the disk's patch area offsets count from (CD virtual disks: missing header)
    uint64_t wave = 0;         // wave data area
    bool cd = false;
    std::vector<S5Patch> patches;
    std::vector<S5Tone> tones;
};
struct S5Image { BlobPtr img; std::vector<S5Disk> disks; };

uint32_t be24(const uint8_t *p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }

S5Disk parse_s5_disk(const BlobPtr &img, uint64_t base, S5Type type, bool cd, uint64_t wave) {
    S5Disk d;
    d.type = type; d.base = base; d.cd = cd; d.wave = wave;
    bool s50 = type == S5Type::S50;
    int bs = s50 ? 512 : 256, n = s50 ? 8 : 16;
    std::vector<uint8_t> pa = img->bytes(base + S5_PATCHES, size_t(bs * n));
    for (int i = 0; i < n; i++) {
        Reader r(&pa[size_t(i * bs)], size_t(bs));
        S5Patch p;
        p.name = clean(r.take(12), 12);
        p.bend = r.u8(); r.u8(); r.u8();
        if (s50) r.skip(4);
        p.key_mode = r.u8(); p.vel_sw = r.u8();
        if (s50) r.skip(19);
        auto table = [&](int *t) {
            if (s50) { for (int k = 0; k < 128; k++) t[k] = int8_t(r.u8()); return; }
            for (int k = 12; k < 121; k++) t[k] = int8_t(r.u8());
            for (int k = 0; k < 12; k++) t[k] = t[12];
            for (int k = 121; k < 128; k++) t[k] = t[120];
        };
        table(p.t1); table(p.t2);
        r.u8(); p.octave = int8_t(r.u8()); p.level = r.u8(); r.u8(); p.detune = int8_t(r.u8());
        d.patches.push_back(p);
    }
    std::vector<uint8_t> tl = img->bytes(base + S5_TONE_LIST, 32 * 16), tn = img->bytes(base + S5_TONES, 32 * 128);
    for (int i = 0; i < 32; i++) {
        S5Tone t;
        t.disabled = clean(&tl[size_t(i) * 16], 8).empty();
        Reader r(&tn[size_t(i) * 128], 128);
        t.name = clean(r.take(8), 8);
        t.out = r.u8(); t.source = r.u8(); t.sub = r.u8(); t.freq = r.u8(); t.key = r.u8(); t.bank = r.u8(); t.seg_top = r.u8(); t.seg_len = r.u8();
        t.start = int(be24(r.take(3))); t.end = int(be24(r.take(3))); t.loop_point = int(be24(r.take(3)));
        t.loop_mode = r.u8();
        r.skip(10);   // LFO
        t.transpose = int8_t(r.u8()); t.fine = int8_t(r.u8());
        t.cutoff = r.u8(); t.reso = r.u8(); t.tvf_kf = int8_t(r.u8()); r.u8(); r.u8(); t.eg_depth = r.u8(); t.eg_pol = r.u8();
        r.skip(4);
        t.tvf_sw = r.u8(); r.u8();
        t.a_sus = r.u8(); t.a_end = r.u8();
        for (int k = 0; k < 8; k++) { t.a_lev[k] = r.u8(); t.a_rate[k] = r.u8(); }
        r.u8(); r.u8(); t.level = r.u8(); r.u8();
        r.skip(3 + 9 + 3);
        t.loop_tune = int8_t(r.u8()); r.u8();
        r.skip(12);
        t.loop_len = int(be24(r.take(3)));
        t.pitch_follow = r.u8(); r.u8();
        t.f_sus = r.u8(); t.f_end = r.u8();
        for (int k = 0; k < 8; k++) { t.f_lev[k] = r.u8(); t.f_rate[k] = r.u8(); }
        d.tones.push_back(t);
    }
    return d;
}

std::shared_ptr<S5Image> parse_s5(BlobPtr img) {
    auto im = std::make_shared<S5Image>();
    im->img = img;
    std::vector<uint8_t> h = img->head(0x200 + 3 * 32);
    S5Type type = s5_type(&h[4]);
    if (type == S5Type::Unknown) throw ParseError("not a Roland S-500 disk");
    if (type != S5Type::Land) {
        S5Disk d = parse_s5_disk(img, 0, type, false, S5_WAVE_FLOPPY);
        std::vector<uint8_t> label = img->bytes(68659, 12);
        d.name = clean(label.data(), 12);
        im->disks.push_back(std::move(d));
        return im;
    }
    // LAND CD: three section headers after the 256-byte header; the sound directory lists the virtual disks
    uint64_t dir_off = 0, dir_size = 0;
    for (int i = 0; i < 3; i++) {
        const uint8_t *s = &h[256 + size_t(i) * 32];
        std::string name = clean(s, 16);
        if (name == "Sound Directory" || name == "SoundDirectory") { dir_off = uint64_t(be32(s + 16)) * 512; dir_size = uint64_t(be32(s + 20)) * 512; }
    }
    if (!dir_size) throw ParseError("no sound directory on this Roland CD");
    std::vector<uint8_t> dir = img->bytes(dir_off, size_t(std::min<uint64_t>(dir_size, 1 << 20)));
    for (size_t off = 0; off + 64 <= dir.size(); off += 64) {
        if (dir[off] > 127 || !dir[off]) continue;
        std::string name = clean(&dir[off], 32);
        if (name.empty()) continue;
        uint64_t vdisk = uint64_t(be32(&dir[off + 48])) * 512;
        if (vdisk < S5_PATCHES || vdisk + S5_WAVE_CD + 36ull * S5_SEG * 2 > img->size()) continue;
        try {
            S5Disk d = parse_s5_disk(img, vdisk - S5_PATCHES, S5Type::S550, true, vdisk + S5_WAVE_CD);
            d.name = name;
            im->disks.push_back(std::move(d));
        } catch (const ParseError &) {}
    }
    if (im->disks.empty()) throw ParseError("no disks in this Roland CD");
    return im;
}

double s5_seg_time(int from, int to, int rate) {
    double ms = 0.5 * std::abs(to - from) * std::pow(2.0, (127.0 - std::max(1, std::min(127, rate))) / 18.0);
    return ms < 1 ? 0 : ms / 1000.0;
}
Envelope s5_env(const int *lev, const int *rate, int sus, int end) {
    Envelope e;
    e.set = true;
    sus = std::max(0, std::min(7, sus));
    end = std::max(sus, std::min(7, end));
    double peak = std::max(1, lev[0]) / 127.0;
    e.attack = s5_seg_time(0, lev[0], rate[0]);
    int ds = 1;
    if (sus > 1 && lev[0] == lev[1]) { e.hold = s5_seg_time(lev[0], lev[1], rate[1]); ds = 2; }
    double t = 0;
    for (int i = ds; i <= sus; i++) t += s5_seg_time(lev[i - 1], lev[i], rate[i]);
    e.decay = t;
    e.sustain = clampd(lev[sus] / 127.0 / peak, 0, 1);
    t = 0;
    if (lev[sus] == 0 && sus > 0) t = s5_seg_time(lev[sus - 1], 0, rate[end]);
    else for (int i = sus + 1; i <= end; i++) t += s5_seg_time(lev[i - 1], lev[i], rate[i]);
    e.release = std::max(t, 0.005);
    return e;
}

SampleRefPtr s5_sample_ref(const S5Image &im, const S5Disk &d, const S5Tone &t) {
    int top = (t.bank == 1 ? 18 : 0) + t.seg_top, n = t.seg_len;
    if (n <= 0 || top + n > 36) return nullptr;
    int seg_bytes = d.cd ? S5_SEG * 2 : S5_SEG * 3 / 2;
    uint64_t off = d.wave + uint64_t(top) * uint64_t(seg_bytes);
    int64_t frames = std::min<int64_t>(int64_t(n) * S5_SEG, int64_t(t.end) + 1);
    BlobPtr img = im.img;
    bool cd = d.cd;
    auto r = std::make_shared<SampleRef>();
    r->name = t.name;
    r->key = "roland5:" + std::to_string(reinterpret_cast<uintptr_t>(img.get())) + ":" + std::to_string(off) + ":" + std::to_string(frames);
    r->rate = t.freq == 0 ? 30000 : 15000;
    r->channels = 1;
    r->frames = frames;
    int rate = r->rate;
    r->decode = [img, off, n, seg_bytes, frames, cd, rate]() {
        std::vector<uint8_t> raw = img->bytes(off, size_t(n) * size_t(seg_bytes));
        auto pcm = std::make_shared<Pcm>();
        pcm->rate = rate;
        pcm->channels = 1;
        pcm->data.resize(size_t(frames));
        for (int64_t i = 0; i < frames; i++) {
            if (cd) { pcm->data[size_t(i)] = int16_t(raw[size_t(i) * 2] | raw[size_t(i) * 2 + 1] << 8); continue; }
            // 12-bit packed, per 18432-byte segment
            int64_t seg = i / S5_SEG, k = i % S5_SEG;
            const uint8_t *p = &raw[size_t(seg) * size_t(seg_bytes)];
            size_t idx = size_t(k * 3 / 2);
            uint8_t lo = p[idx], hi = p[idx + 1];
            pcm->data[size_t(i)] = k % 2 == 0 ? int16_t(lo << 8 | (hi & 0xF0)) : int16_t(hi << 8 | (lo & 0x0F) << 4);
        }
        return PcmPtr(pcm);
    };
    return r;
}

Instrument s5_patch(const S5Image &im, const S5Disk &d, int pi) {
    const S5Patch &p = d.patches[size_t(pi)];
    Instrument inst;
    inst.name = p.name;
    inst.format = d.type == S5Type::S50 ? "Roland S-50" : d.type == S5Type::S330 ? "Roland S-330" : d.type == S5Type::W30 ? "Roland W-30" : "Roland S-550";
    inst.groups = {Group{"Layer 1"}, Group{"Layer 2"}};
    std::map<int, SampleRefPtr> refs;
    for (int layer = 0; layer < 2; layer++) {
        const int *t = layer == 0 ? p.t1 : p.t2;
        if (layer == 1 && p.key_mode == 0) break;   // normal mode: layer 1 only
        int key = 0;
        while (key < 128) {
            int tid = t[key], hi = key;
            while (hi + 1 < 128 && t[hi + 1] == tid) hi++;
            if (tid >= 0 && tid < 32 && !d.tones[size_t(tid)].disabled) {
                const S5Tone &tone = d.tones[size_t(tid)];
                const S5Tone &src = tone.sub == 1 && tone.source < 32 ? d.tones[size_t(tone.source)] : tone;
                auto it = refs.find(tid);
                if (it == refs.end()) it = refs.emplace(tid, s5_sample_ref(im, d, src)).first;
                if (it->second) {
                    Zone z;
                    z.name = tone.name;
                    z.sample = it->second;
                    z.group = layer;
                    z.key_lo = key; z.key_hi = hi;
                    z.root = tone.key;
                    z.start = tone.start;
                    z.reverse = tone.loop_mode == 3;
                    z.one_shot = tone.loop_mode == 2;
                    if (tone.loop_mode < 2 && tone.loop_len > 0) {
                        Loop l;
                        l.type = tone.loop_mode == 0 ? LoopType::Forward : LoopType::Alternating;
                        l.start = tone.loop_point; l.end = tone.loop_point + tone.loop_len - 1;   // loop_len frames: end is inclusive
                        l.tune = tone.loop_tune / 100.0;   // cents: corrects the pitch of short loops
                        z.loops.push_back(l);
                    }
                    z.tune = tone.fine / 100.0 + p.octave * 12.0;
                    // the octave shift moves which keys play the tone (the hardware reads the table at key + shift)
                    z.key_lo = std::max(0, key - 12 * p.octave);
                    z.key_hi = std::min(127, hi - 12 * p.octave);
                    if (tone.pitch_follow == 0) { z.key_tracking = 0; z.tune += tone.transpose; }
                    double peak = std::max(1, tone.a_lev[0]) / 127.0;
                    z.gain_db = to_db(tone.level / 127.0 * peak * (p.level / 127.0));
                    z.amp_env = s5_env(tone.a_lev, tone.a_rate, tone.a_sus, tone.a_end);
                    if (d.type != S5Type::S50 && tone.tvf_sw) {
                        z.filter.type = FilterType::LowPass;
                        z.filter.poles = 2;
                        z.filter.cutoff = denorm_cutoff(tone.cutoff / 127.0);
                        z.filter.resonance = tone.reso / 127.0;
                        z.filter.env = s5_env(tone.f_lev, tone.f_rate, tone.f_sus, tone.f_end);
                        double depth = tone.eg_depth / 127.0;
                        z.filter.env_depth = tone.eg_pol == 1 ? -depth : depth;
                        if (tone.tvf_kf) z.filter.key_tracking = clampd(tone.tvf_kf > 0 ? tone.tvf_kf / 63.0 : tone.tvf_kf / 64.0, -1, 1);
                    }
                    z.bend_up = p.bend * 100; z.bend_down = -p.bend * 100;
                    if (layer == 1) {
                        if (p.key_mode == 1) z.tune += p.detune / 100.0;
                        if (p.key_mode == 2 || p.key_mode == 3) z.vel_lo = p.vel_sw;
                    } else if (p.key_mode == 2 || p.key_mode == 3) z.vel_hi = std::max(0, p.vel_sw - 1);
                    if (z.vel_lo <= z.vel_hi && z.key_lo <= z.key_hi) inst.zones.push_back(z);
                }
            }
            key = hi + 1;
        }
    }
    finish_instrument(inst);
    return inst;
}

// ---------------------------------------------------------------------------------------------------------------
// the mounted volume: folders and virtual entries ("R7P <patch>", "R7F <performance>", "R5P <disk> <patch>")

class RolandVolume : public TreeVolume {
public:
    explicit RolandVolume(std::string kind) : TreeVolume(std::move(kind)) {}
    std::shared_ptr<S7Image> s7;
    std::shared_ptr<S5Image> s5;
};

void add_entry(TreeVolume &v, const std::string &path, const std::string &text) {
    std::vector<uint8_t> d(text.begin(), text.end());
    v.add_file(path, make_mem_blob(std::move(d)));
}

bool probe_roland(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    if (n < 16) return false;
    if (!std::memcmp(h + 4, "S770", 4)) return size >= 0x1F800;
    return s5_type(h + 4) != S5Type::Unknown && size >= 0x12000;
}

VolumePtr mount_roland(BlobPtr image) {
    std::vector<uint8_t> h = image->head(16);
    if (!std::memcmp(&h[4], "S770", 4)) {
        auto v = std::make_shared<RolandVolume>("Roland S-700");
        v->s7 = parse_s7(image);
        const S7Image &im = *v->s7;
        std::set<std::string> used;
        auto unique = [&](const std::string &dir, std::string name) {
            std::string p = dir + "/" + name;
            for (int k = 2; used.count(lower(p)); k++) p = dir + "/" + name + " (" + std::to_string(k) + ")";
            used.insert(lower(p));
            return p;
        };
        char num[32];
        for (size_t vi = 0; vi < im.volumes.size(); vi++) {
            const S7Volume &vol = im.volumes[vi];
            std::snprintf(num, sizeof num, "%03zu ", vi + 1);
            std::string dir = std::string(num) + safe_name(vol.name);
            v->add_dir(dir);
            std::set<int> pats;
            for (int pf : vol.perf) {
                if (pf < 0 || pf >= int(im.perfs.size()) || im.perfs[size_t(pf)].name.empty()) continue;
                const S7Perf &perf = im.perfs[size_t(pf)];
                add_entry(*v, unique(dir + "/Performances", safe_name(perf.name) + ".r7f"), "R7F " + std::to_string(pf));
                for (int pi : perf.patch) if (pi >= 0 && pi < int(im.patches.size()) && im.patches[size_t(pi)].active) pats.insert(pi);
            }
            for (int pi : pats) add_entry(*v, unique(dir, safe_name(im.patches[size_t(pi)].name) + ".r7p"), "R7P " + std::to_string(pi));
        }
        int count = 0;
        for (size_t pi = 0; pi < im.patches.size(); pi++) {
            if (!im.patches[pi].active || im.patches[pi].name.empty()) continue;
            std::snprintf(num, sizeof num, "%04zu ", pi + 1);
            add_entry(*v, unique("All Patches", std::string(num) + safe_name(im.patches[pi].name) + ".r7p"), "R7P " + std::to_string(pi));
            count++;
        }
        if (!count) throw ParseError("no patches on this Roland S-700 disk");
        return v;
    }
    auto v = std::make_shared<RolandVolume>("Roland S-500");
    v->s5 = parse_s5(image);
    std::set<std::string> used;
    for (size_t di = 0; di < v->s5->disks.size(); di++) {
        const S5Disk &d = v->s5->disks[di];
        std::string dir = v->s5->disks.size() > 1 ? safe_name(d.name) : "";
        for (int k = 2; !dir.empty() && used.count(lower(dir)); k++) dir = safe_name(d.name) + " (" + std::to_string(k) + ")";
        used.insert(lower(dir));
        if (!dir.empty()) v->add_dir(dir);
        for (size_t pi = 0; pi < d.patches.size(); pi++) {
            if (d.patches[pi].name.empty()) continue;
            char id[32];
            if (d.type == S5Type::S330) std::snprintf(id, sizeof id, "P%zu%zu ", pi / 8 + 1, pi % 8 + 1);
            else std::snprintf(id, sizeof id, "P%zu%zu ", pi / 8 + 1, pi % 8 + 1);
            std::string name = std::string(id) + safe_name(d.patches[pi].name) + ".r5p";
            add_entry(*v, dir.empty() ? name : dir + "/" + name, "R5P " + std::to_string(di) + " " + std::to_string(pi));
        }
    }
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// the reader of the virtual entries

RolandVolume *roland_volume(VolumePtr &vol) {
    auto *rv = dynamic_cast<RolandVolume *>(vol.get());
    if (!rv) throw ParseError("Roland entries only work inside their disk image");
    return rv;
}

Instrument load_roland(VolumePtr vol, const std::string &path, int) {
    RolandVolume *rv = roland_volume(vol);
    std::vector<uint8_t> d = vol->open(path)->all(64);
    std::string text(d.begin(), d.end());
    char kind[4] = {};
    int a = -1, b = -1;
    if (std::sscanf(text.c_str(), "%3s %d %d", kind, &a, &b) < 2) throw ParseError("bad Roland entry");
    if (!std::strcmp(kind, "R5P")) {
        if (!rv->s5 || a < 0 || a >= int(rv->s5->disks.size())) throw ParseError("bad Roland entry");
        const S5Disk &disk = rv->s5->disks[size_t(a)];
        if (b < 0 || b >= int(disk.patches.size())) throw ParseError("bad Roland entry");
        return s5_patch(*rv->s5, disk, b);
    }
    if (!rv->s7) throw ParseError("bad Roland entry");
    const S7Image &im = *rv->s7;
    Instrument inst;
    inst.format = im.diskette ? "Roland S-700 floppy" : "Roland S-700";
    inst.groups.clear();
    std::map<int, SampleRefPtr> refs;
    int missing = 0;
    if (!std::strcmp(kind, "R7P")) {
        if (a < 0 || a >= int(im.patches.size())) throw ParseError("bad Roland entry");
        inst.name = im.patches[size_t(a)].name;
        s7_patch_zones(im, a, inst, -1, 0, 127, 0, 0, refs, missing);
    } else if (!std::strcmp(kind, "R7F")) {
        if (a < 0 || a >= int(im.perfs.size())) throw ParseError("bad Roland entry");
        const S7Perf &pf = im.perfs[size_t(a)];
        inst.name = pf.name;
        inst.format += " performance";
        std::set<int> chans;
        for (int p = 0; p < 32; p++) if (pf.patch[p] >= 0 && pf.enabled[p]) chans.insert(pf.channel[p]);
        bool one = chans.size() <= 1;   // all parts on one channel: play them on any channel (MPC tracks send one)
        for (int p = 0; p < 32; p++) {
            int pi = pf.patch[p];
            if (pi < 0 || pi >= int(im.patches.size()) || !pf.enabled[p]) continue;
            s7_patch_zones(im, pi, inst, one ? -1 : pf.channel[p], pf.lo[p], std::max(pf.lo[p], pf.hi[p]), to_db(pf.level[p] / 127.0),
                           pf.pan[p] / 32.0, refs, missing);
        }
    } else throw ParseError("bad Roland entry");
    if (inst.groups.empty()) inst.groups.push_back(Group{});
    if (missing) inst.warnings.push_back(std::to_string(missing) + " samples have no data on this disk");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("this Roland patch has no playable samples");
    return inst;
}

bool probe_entry(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    return size < 64 && n >= 4 && (!std::memcmp(h, "R7P ", 4) || !std::memcmp(h, "R7F ", 4) || !std::memcmp(h, "R5P ", 4));
}

}  // namespace

void register_roland() {
    register_reader({"Roland sampler", "r7p r7f r5p", probe_entry, list_single, load_roland});
}

void register_roland_images() {
    register_image_type({"Roland sampler disk", probe_roland, mount_roland});
}

}  // namespace omni

// Omni Sampler: E-mu Emulator IV / EOS banks (.e4b; E4, E4X, E-Synth, E6400, e5000, Proteus 2000 user banks).
// IFF "FORM....E4B0" with E4P1 preset chunks and E3S1 sample chunks. Translated from ConvertWithMoss's
// Emulator4Detector / Emulator4Constants (LGPL-3.0).
#include <cmath>
#include <cstring>
#include <map>
#include "format.hpp"

namespace omni {
namespace {

constexpr int SAMPLE_HEADER = 94, SAMPLE_STRUCT = 92, PRESET_HEADER = 82, VOICE = 284, VOICE_PZT = 110, VOICE_MOD = 190, ZONE = 22;

std::string e4_name(const uint8_t *p) {
    std::string s;
    for (int i = 0; i < 16 && p[i]; i++) s += (p[i] >= 0x20 && p[i] < 0x7F) ? char(p[i]) : '?';
    return trim(s);
}

double env_time(int rate) { return rate <= 0 ? 0 : 0.0310 * std::exp(0.0581 * std::min(rate, 127)); }
double cutoff_hz(int c) { return 57.0 * std::pow(20000.0 / 57.0, clampi(c, 0, 255) / 255.0); }
int pitch_offset(int rate) { return rate <= 0 ? 0 : int(std::lround(768.0 * std::log2(double(rate) / 44100.0))); }

int note_from_name(const std::string &name, std::string &base) {
    // "Piano_C3": the root key is appended to the sample name (C3 = 60)
    static const char *notes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    size_t us = name.find_last_of('_');
    if (us == std::string::npos || us + 2 > name.size()) return -1;
    std::string suf = name.substr(us + 1);
    for (int i = 11; i >= 0; i--) {
        size_t nl = std::strlen(notes[i]);
        if (suf.compare(0, nl, notes[i]) != 0) continue;
        std::string oct = suf.substr(nl);
        if (oct.empty() || !(isdigit((unsigned char)oct[0]) || oct[0] == '-')) continue;
        char *end;
        long o = std::strtol(oct.c_str(), &end, 10);
        if (*end) continue;
        int midi = int((o + 2) * 12 + i);
        if (midi < 0 || midi > 127) return -1;
        base = name.substr(0, us);
        return midi;
    }
    return -1;
}

struct E4Sample { std::string name; SampleRefPtr ref; int frames = 0, root = 60; double tuning = 0; bool loop = false; int loop_start = 0, loop_end = 0; };
struct E4Bank { std::vector<std::pair<size_t, size_t>> presets; std::map<int, E4Sample> samples; };

E4Bank parse_bank(BlobPtr blob) {
    std::vector<uint8_t> h = blob->head(12);
    if (h.size() < 12 || std::memcmp(h.data(), "FORM", 4) || std::memcmp(h.data() + 8, "E4B0", 4)) throw ParseError("not an Emulator IV bank");
    E4Bank bank;
    uint64_t pos = 12, size = blob->size();
    std::map<std::string, int> used;
    while (pos + 8 <= size) {
        std::vector<uint8_t> ch = blob->bytes(pos, 8);
        uint64_t len = be32(ch.data() + 4), end = pos + 8 + len;
        if (end > size) break;
        if (!std::memcmp(ch.data(), "E4P1", 4)) bank.presets.push_back({size_t(pos + 8), size_t(len)});
        else if (!std::memcmp(ch.data(), "E3S1", 4) && len >= SAMPLE_HEADER) {
            std::vector<uint8_t> s = blob->bytes(pos + 8, SAMPLE_HEADER);
            int index = be16(&s[0]);
            std::string display = e4_name(&s[2]);
            int rate = int(le32(&s[54]));
            int poff = int16_t(le16(&s[58]));
            int options = le16(&s[60]);
            bool left = options & 0x20;
            uint32_t ls = le32(&s[left ? 38 : 42]), le = le32(&s[left ? 46 : 50]);
            int frames = int((len - SAMPLE_HEADER) / 2);
            if (frames > 0 && rate > 0) {
                E4Sample smp;
                smp.frames = frames;
                double dev = (poff - pitch_offset(rate)) / 64.0;
                smp.tuning = std::fabs(dev) <= 1.0 ? dev : 0;
                std::string base;
                int root = note_from_name(display, base);
                smp.root = root >= 0 ? root : 60;
                std::string name = root >= 0 && !trim(base).empty() ? base : display;
                if (used[name]++) name = display + (used[display]++ ? " " + std::to_string(index) : "");
                smp.name = name;
                smp.ref = raw_sample_ref(blob, pos + 8 + SAMPLE_HEADER, frames, 1, Enc::S16LE, rate, display);
                if (options & 1) {
                    smp.loop_start = int((int64_t(ls) - SAMPLE_STRUCT) / 2);
                    smp.loop_end = std::min(int((int64_t(le) - SAMPLE_STRUCT) / 2), frames - 1);
                    smp.loop = smp.loop_start >= 0 && smp.loop_start < frames && smp.loop_end > smp.loop_start;
                }
                bank.samples[index] = smp;
            }
        }
        pos = end + (len & 1);
    }
    return bank;
}

Envelope e4_env(const uint8_t *p) {
    double t[6];
    int l[6];
    for (int s = 0; s < 6; s++) { t[s] = env_time(p[s * 2]); l[s] = int8_t(p[s * 2 + 1]); }
    Envelope e;
    e.set = true;
    e.attack = t[0] + t[1];
    bool plateau = l[2] == l[1];
    e.hold = plateau ? t[2] : 0;
    e.decay = plateau ? t[3] : t[2] + t[3];
    e.sustain = clampd(l[3] / 127.0, 0, 1);
    e.release = t[4] + t[5];
    return e;
}

std::vector<PresetInfo> list_e4b(VolumePtr vol, const std::string &path) {
    BlobPtr b = vol->open(path);
    E4Bank bank = parse_bank(b);
    std::vector<PresetInfo> out;
    for (size_t i = 0; i < bank.presets.size(); i++) {
        if (bank.presets[i].second < PRESET_HEADER) continue;
        std::vector<uint8_t> h = b->bytes(bank.presets[i].first, PRESET_HEADER);
        std::string n = e4_name(&h[2]);
        char num[8];
        std::snprintf(num, sizeof num, "%03d ", int(be16(&h[0])));
        out.push_back({std::string(num) + (n.empty() ? "Preset" : n), int(i)});
    }
    return out;
}

Instrument load_e4b(VolumePtr vol, const std::string &path, int index) {
    BlobPtr blob = vol->open(path);
    E4Bank bank = parse_bank(blob);
    if (index < 0 || size_t(index) >= bank.presets.size()) throw ParseError("no such preset");
    std::vector<uint8_t> body = blob->bytes(bank.presets[size_t(index)].first, bank.presets[size_t(index)].second);
    if (body.size() < PRESET_HEADER) throw ParseError("malformed preset");
    Instrument inst;
    inst.format = "E-mu Emulator IV bank";
    std::string pname = e4_name(&body[2]);
    inst.name = pname.empty() ? path_stem(path) : pname;
    inst.groups.clear();
    int voices = be16(&body[20]);
    size_t off = PRESET_HEADER;
    std::map<int, bool> missing;
    for (int v = 0; v < voices; v++) {
        if (off + VOICE > body.size()) break;
        int table_end = be16(&body[off + 2]);
        int nzones = (table_end - VOICE) / ZONE;
        if (table_end < VOICE || off + VOICE + size_t(nzones) * ZONE > body.size()) break;
        const uint8_t *vo = &body[off];
        double coarse = int8_t(vo[34]) + double(int8_t(vo[35]));
        bool fixed = vo[38] == 1;
        bool multi = nzones > 1;
        double vfine = int8_t(vo[36]) / 64.0;
        int vvol = int8_t(vo[54]);
        double vpan = clampd(int8_t(vo[55]) / 64.0, -1, 1);
        int vklo = vo[14], vkhi = vo[17], vvlo = vo[18], vvhi = vo[21];
        double vel_amp = 0, fenv_depth = 0, fkey = 0, fvel = 0;
        bool lost_cutoff_mod = false;
        for (int s = 0; s < 20; s++) {
            const uint8_t *c = vo + VOICE_MOD + s * 4;
            int src = c[0], dst = c[1], amt = int8_t(c[2]);
            if (!amt) continue;
            bool vel = src >= 0x0A && src <= 0x0C;
            if (dst == 0x40 && vel) vel_amp = clampd(std::abs(amt) / 127.0, 0, 1);
            else if (dst == 0x38 && src == 0x50) fenv_depth = clampd(amt / 127.0, -1, 1);
            else if (dst == 0x38 && src == 0x08) fkey = clampd(amt / 127.0 * 0.713, 0, 1);
            else if (dst == 0x38 && vel) fvel = clampd(amt / 127.0, -1, 1);
            if (dst == 0x38 && src != 0x50 && src != 0x08) lost_cutoff_mod = true;
        }
        Envelope amp = e4_env(vo + VOICE_PZT + 0);
        Filter filt;
        int ftype = vo[58], fcut = vo[60], fres = vo[61];
        struct { int id; FilterType t; int p; } F[] = {{0x00, FilterType::LowPass, 4}, {0x01, FilterType::LowPass, 2}, {0x02, FilterType::LowPass, 6},
                                                       {0x08, FilterType::HighPass, 2}, {0x09, FilterType::HighPass, 4}, {0x10, FilterType::BandPass, 2},
                                                       {0x11, FilterType::BandPass, 4}, {0x12, FilterType::BandReject, 2}};
        for (auto &f : F)
            if (f.id == ftype) { filt.type = f.t; filt.poles = f.p; }
        if (filt.type != FilterType::None) {
            if (ftype == 0 && fcut == 255 && fres == 0 && fenv_depth == 0 && fkey == 0 && fvel == 0) filt.type = FilterType::None;
            else {
                double hz = cutoff_hz(fcut);
                int parked = filt.type == FilterType::BandReject ? 0 : hz <= 100 && filt.type != FilterType::HighPass ? 1 : hz >= 9000 && filt.type != FilterType::LowPass ? -1 : 0;
                if (parked != 0 && fenv_depth * parked <= 0 && lost_cutoff_mod) filt.type = FilterType::None;
                else {
                    filt.cutoff = hz;
                    filt.resonance = clampd(fres / 127.0, 0, 1);
                    filt.key_tracking = fkey;
                    filt.vel_depth = fvel;
                    if (fenv_depth != 0) { filt.env_depth = fenv_depth; filt.env = e4_env(vo + VOICE_PZT + 14); }
                }
            }
        }
        int group = int(inst.groups.size());
        inst.groups.push_back(Group{"Voice " + std::to_string(v + 1)});
        for (int z = 0; z < nzones; z++) {
            const uint8_t *e = vo + VOICE + z * ZONE;
            int si = be16(e + 10);
            if (!si) continue;
            auto it = bank.samples.find(si);
            if (it == bank.samples.end()) { missing[si] = true; continue; }
            const E4Sample &s = it->second;
            int klo = std::max<int>(e[2], vklo), khi = std::min<int>(e[5], vkhi);
            if (klo > khi) continue;
            Zone zn;
            zn.name = s.name;
            zn.group = group;
            zn.sample = s.ref;
            zn.key_lo = std::min(klo, 127); zn.key_hi = std::min(khi, 127);
            int vlo = std::max<int>(e[6], vvlo), vhi = std::min<int>(e[9], vvhi);
            if (vlo <= vhi && vhi > 0) { zn.vel_lo = std::max(1, vlo); zn.vel_hi = std::min(127, vhi); }
            int root = e[14];
            zn.root = root > 0 && root < 128 ? root : s.root;
            zn.stop = s.frames;
            double fine = multi ? int16_t(be16(e + 12)) / 64.0 : vfine;
            zn.tune = coarse + fine + s.tuning;
            zn.gain_db = multi ? int8_t(e[15]) : vvol;
            zn.pan = multi ? clampd(int8_t(e[16]) / 64.0, -1, 1) : vpan;
            if (fixed) zn.key_tracking = 0;
            if (s.loop) { Loop l; l.start = s.loop_start; l.end = s.loop_end; zn.loops.push_back(l); }
            zn.amp_env = amp;
            zn.amp_vel_depth = vel_amp;
            zn.filter = filt;
            inst.zones.push_back(zn);
        }
        off += VOICE + size_t(nzones) * ZONE;
    }
    for (auto &m : missing) inst.warnings.push_back("missing sample #" + std::to_string(m.first));
    finish_instrument(inst);
    return inst;
}

bool probe_e4b(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 12 && !std::memcmp(h, "FORM", 4) && !std::memcmp(h + 8, "E4B0", 4); }

}  // namespace

void register_emu_e4() {
    register_reader({"E-mu Emulator IV bank", "e4b e4a eos", probe_e4b, list_e4b, load_e4b});
}

}  // namespace omni

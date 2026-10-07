// Omni Sampler: SoundFont 2 (.sf2, E-mu's format) and the SF2-style DLS-less banks. Generators merge per the SF2.04
// spec: instrument global zone + local zone set values, preset global + local zones add to them. Unit conversions
// follow ConvertWithMoss's Sf2Detector (LGPL-3.0); attenuation uses E-mu's 0.4 factor like the hardware.
#include <algorithm>
#include <cmath>
#include <cstring>
#include "format.hpp"

namespace omni {
namespace {

enum Gen {
    START_OFS = 0, END_OFS = 1, LOOP_START_OFS = 2, LOOP_END_OFS = 3, START_COARSE = 4, MOD_LFO_PITCH = 5,
    VIB_LFO_PITCH = 6, MOD_ENV_PITCH = 7, CUTOFF = 8, RESONANCE = 9, MOD_LFO_CUTOFF = 10, MOD_ENV_CUTOFF = 11,
    END_COARSE = 12, MOD_LFO_VOL = 13, PAN = 17, MOD_LFO_DELAY = 21, MOD_LFO_FREQ = 22, VIB_LFO_DELAY = 23,
    VIB_LFO_FREQ = 24, MOD_DELAY = 25, MOD_ATTACK = 26, MOD_HOLD = 27, MOD_DECAY = 28, MOD_SUSTAIN = 29,
    MOD_RELEASE = 30, KEY_MOD_HOLD = 31, KEY_MOD_DECAY = 32, VOL_DELAY = 33, VOL_ATTACK = 34, VOL_HOLD = 35,
    VOL_DECAY = 36, VOL_SUSTAIN = 37, VOL_RELEASE = 38, KEY_VOL_HOLD = 39, KEY_VOL_DECAY = 40, INSTRUMENT = 41,
    KEY_RANGE = 43, VEL_RANGE = 44, LOOP_START_COARSE = 45, KEYNUM = 46, VELOCITY = 47, ATTENUATION = 48,
    LOOP_END_COARSE = 50, COARSE_TUNE = 51, FINE_TUNE = 52, SAMPLE_ID = 53, SAMPLE_MODES = 54, SCALE_TUNE = 56,
    EXCLUSIVE = 57, ROOT_KEY = 58, GEN_COUNT = 61
};

struct Gens {
    int16_t v[GEN_COUNT];
    bool set[GEN_COUNT];
    Gens() { std::memset(v, 0, sizeof v); std::memset(set, 0, sizeof set); }
    void put(int g, int16_t x) { if (g >= 0 && g < GEN_COUNT) { v[g] = x; set[g] = true; } }
    int lo(int g) const { return uint16_t(v[g]) & 0xFF; }
    int hi(int g) const { return uint16_t(v[g]) >> 8; }
};

struct Sf2 {
    struct Preset { std::string name; int program, bank; size_t bag_lo, bag_hi; };
    struct Inst { std::string name; size_t bag_lo, bag_hi; };
    struct Sample { std::string name; uint32_t start, end, loop_start, loop_end, rate; int pitch; int correction; int link, type; };
    struct Bag { size_t gen_lo, gen_hi; };
    struct GenRec { int oper; int16_t amount; };
    std::vector<Preset> presets;
    std::vector<Inst> insts;
    std::vector<Sample> samples;
    std::vector<Bag> pbags, ibags;
    std::vector<GenRec> pgens, igens;
    uint64_t smpl_off = 0, smpl_len = 0, sm24_off = 0, sm24_len = 0;
    std::string bank_name;
};

void parse_bags(Reader r, size_t rec, std::vector<Sf2::Bag> &out) {
    std::vector<size_t> idx;
    while (r.left() >= rec) { idx.push_back(r.u16le()); r.skip(rec - 2); }
    for (size_t i = 0; i + 1 < idx.size(); i++) out.push_back({idx[i], idx[i + 1]});
}

Sf2 parse(const Blob &blob) {
    Sf2 sf;
    std::vector<uint8_t> h = blob.head(12);
    if (h.size() < 12 || std::memcmp(h.data(), "RIFF", 4) || std::memcmp(h.data() + 8, "sfbk", 4))
        throw ParseError("not a SoundFont 2 file");
    uint64_t pos = 12, size = blob.size();
    while (pos + 12 <= size) {
        std::vector<uint8_t> ch = blob.bytes(pos, 12);
        uint64_t len = le32(ch.data() + 4);
        if (std::memcmp(ch.data(), "LIST", 4)) { pos += 8 + len + (len & 1); continue; }
        uint64_t end = std::min(size, pos + 8 + len), p = pos + 12;
        std::string list(reinterpret_cast<char *>(ch.data() + 8), 4);
        while (p + 8 <= end) {
            std::vector<uint8_t> sh = blob.bytes(p, 8);
            uint64_t sl = le32(sh.data() + 4), body = p + 8;
            std::string id(reinterpret_cast<char *>(sh.data()), 4);
            if (list == "sdta") {
                if (id == "smpl") { sf.smpl_off = body; sf.smpl_len = std::min(sl, size - body); }
                if (id == "sm24") { sf.sm24_off = body; sf.sm24_len = std::min(sl, size - body); }
            } else if (list == "INFO" && id == "INAM") {
                sf.bank_name = Reader(blob.bytes(body, size_t(std::min<uint64_t>(sl, 256)))).str(size_t(std::min<uint64_t>(sl, 256)));
            } else if (list == "pdta") {
                std::vector<uint8_t> d = blob.bytes(body, size_t(sl));
                Reader r(d);
                if (id == "phdr") {
                    std::vector<std::pair<Sf2::Preset, size_t>> tmp;
                    while (r.left() >= 38) {
                        Sf2::Preset pr;
                        pr.name = r.str(20);
                        pr.program = r.u16le();
                        pr.bank = r.u16le();
                        size_t bag = r.u16le();
                        r.skip(12);
                        tmp.push_back({pr, bag});
                    }
                    for (size_t i = 0; i + 1 < tmp.size(); i++) {   // last record is the EOP terminal
                        Sf2::Preset pr = tmp[i].first;
                        pr.bag_lo = tmp[i].second;
                        pr.bag_hi = tmp[i + 1].second;
                        sf.presets.push_back(pr);
                    }
                } else if (id == "inst") {
                    std::vector<std::pair<std::string, size_t>> tmp;
                    while (r.left() >= 22) { std::string n = r.str(20); tmp.push_back({n, r.u16le()}); }
                    for (size_t i = 0; i + 1 < tmp.size(); i++) sf.insts.push_back({tmp[i].first, tmp[i].second, tmp[i + 1].second});
                } else if (id == "pbag") parse_bags(r, 4, sf.pbags);
                else if (id == "ibag") parse_bags(r, 4, sf.ibags);
                else if (id == "pgen" || id == "igen") {
                    auto &g = id == "pgen" ? sf.pgens : sf.igens;
                    while (r.left() >= 4) { int op = r.u16le(); g.push_back({op, r.s16le()}); }
                } else if (id == "shdr") {
                    while (r.left() >= 46) {
                        Sf2::Sample s;
                        s.name = r.str(20);
                        s.start = r.u32le(); s.end = r.u32le(); s.loop_start = r.u32le(); s.loop_end = r.u32le();
                        s.rate = r.u32le(); s.pitch = r.u8(); s.correction = r.s8(); s.link = r.u16le(); s.type = r.u16le();
                        sf.samples.push_back(s);
                    }
                    if (!sf.samples.empty()) sf.samples.pop_back();   // EOS terminal
                }
            }
            p = body + sl + (sl & 1);
        }
        pos = end + (len & 1);
    }
    if (!sf.smpl_off) throw ParseError("SoundFont without sample data");
    return sf;
}

double timecents(const Gens &g, int gen, double def_tc = -12000) {
    double tc = g.set[gen] ? g.v[gen] : def_tc;
    if (tc <= -12000) return 0;
    return std::pow(2.0, tc / 1200.0);
}
double sustain_level(const Gens &g, int gen) {   // centibels of attenuation -> linear
    double cb = clampd(g.set[gen] ? g.v[gen] : 0, 0, 1440);
    return std::pow(10.0, -cb / 200.0);
}
double cents_hz(double c) { return 8.176 * std::pow(2.0, c / 1200.0); }

SampleRefPtr sample_ref(BlobPtr blob, const Sf2 &sf, const Sf2::Sample &s, size_t index) {
    auto r = std::make_shared<SampleRef>();
    r->name = s.name;
    r->rate = int(s.rate);
    r->channels = 1;
    r->frames = s.end > s.start ? int64_t(s.end - s.start) : 0;
    r->key = "sf2:" + std::to_string(reinterpret_cast<uintptr_t>(blob.get())) + ":" + std::to_string(index);
    uint64_t off = sf.smpl_off + uint64_t(s.start) * 2, frames = r->frames;
    uint64_t off24 = sf.sm24_off ? sf.sm24_off + s.start : 0;
    uint64_t smpl_end = sf.smpl_off + sf.smpl_len;
    r->decode = [blob, off, frames, off24, smpl_end, rate = s.rate]() -> PcmPtr {
        uint64_t n = frames;
        if (off + n * 2 > smpl_end) n = off < smpl_end ? (smpl_end - off) / 2 : 0;
        std::vector<uint8_t> b = blob->bytes(off, size_t(n * 2));
        auto p = pcm_from_raw(b.data(), int64_t(n), 1, Enc::S16LE, int(rate));
        (void)off24;   // the low 8 bits of 24-bit banks are below the int16 output; 16-bit data is exact
        return p;
    };
    return r;
}

std::vector<PresetInfo> list_sf2(VolumePtr vol, const std::string &path) {
    Sf2 sf = parse(*vol->open(path));
    std::vector<size_t> order(sf.presets.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return sf.presets[a].bank != sf.presets[b].bank ? sf.presets[a].bank < sf.presets[b].bank
                                                        : sf.presets[a].program < sf.presets[b].program;
    });
    std::vector<PresetInfo> out;
    for (size_t i : order) {
        char num[16];
        snprintf(num, sizeof num, "%03d ", sf.presets[i].program + 1);
        out.push_back({std::string(num) + sf.presets[i].name, int(i)});
    }
    return out;
}

Gens collect(const std::vector<Sf2::GenRec> &gens, const Sf2::Bag &bag) {
    Gens g;
    for (size_t i = bag.gen_lo; i < bag.gen_hi && i < gens.size(); i++) g.put(gens[i].oper, gens[i].amount);
    return g;
}

Instrument load_sf2(VolumePtr vol, const std::string &path, int index) {
    BlobPtr blob = vol->open(path);
    Sf2 sf = parse(*blob);
    if (index < 0 || size_t(index) >= sf.presets.size()) throw ParseError("no such preset");
    const Sf2::Preset &pr = sf.presets[size_t(index)];
    Instrument inst;
    inst.format = "SoundFont 2";
    inst.name = pr.name;
    inst.groups.clear();
    std::vector<SampleRefPtr> refs(sf.samples.size());

    Gens pglobal;
    for (size_t b = pr.bag_lo; b < pr.bag_hi && b < sf.pbags.size(); b++) {
        Gens pz = collect(sf.pgens, sf.pbags[b]);
        if (!pz.set[INSTRUMENT]) { if (b == pr.bag_lo) pglobal = pz; continue; }
        for (int i = 0; i < GEN_COUNT; i++) if (!pz.set[i] && pglobal.set[i]) pz.put(i, pglobal.v[i]);
        size_t ii = uint16_t(pz.v[INSTRUMENT]);
        if (ii >= sf.insts.size()) continue;
        const Sf2::Inst &in = sf.insts[ii];
        int group = int(inst.groups.size());
        inst.groups.push_back(Group{in.name, Trigger::Attack});
        Gens iglobal;
        for (size_t ib = in.bag_lo; ib < in.bag_hi && ib < sf.ibags.size(); ib++) {
            Gens g = collect(sf.igens, sf.ibags[ib]);
            if (!g.set[SAMPLE_ID]) { if (ib == in.bag_lo) iglobal = g; continue; }
            for (int i = 0; i < GEN_COUNT; i++) if (!g.set[i] && iglobal.set[i]) g.put(i, iglobal.v[i]);
            size_t si = uint16_t(g.v[SAMPLE_ID]);
            if (si >= sf.samples.size()) continue;
            const Sf2::Sample &s = sf.samples[si];
            if (s.type & 0x8000) continue;   // ROM samples are not in the file
            // key/velocity ranges intersect between preset and instrument
            int klo = g.set[KEY_RANGE] ? g.lo(KEY_RANGE) : 0, khi = g.set[KEY_RANGE] ? g.hi(KEY_RANGE) : 127;
            int vlo = g.set[VEL_RANGE] ? g.lo(VEL_RANGE) : 0, vhi = g.set[VEL_RANGE] ? g.hi(VEL_RANGE) : 127;
            if (pz.set[KEY_RANGE]) { klo = std::max(klo, pz.lo(KEY_RANGE)); khi = std::min(khi, pz.hi(KEY_RANGE)); }
            if (pz.set[VEL_RANGE]) { vlo = std::max(vlo, pz.lo(VEL_RANGE)); vhi = std::min(vhi, pz.hi(VEL_RANGE)); }
            if (klo > khi || vlo > vhi) continue;
            // preset-level generators are additive (except ranges and the index generators)
            auto sum = [&](int gen, int def = 0) -> double {
                return (g.set[gen] ? g.v[gen] : def) + (pz.set[gen] ? pz.v[gen] : 0);
            };
            Gens m = g;   // merged for envelope helpers
            for (int i = 0; i < GEN_COUNT; i++) if (pz.set[i] && i != KEY_RANGE && i != VEL_RANGE && i != SAMPLE_ID &&
                                                    i != INSTRUMENT && i != SAMPLE_MODES && i != EXCLUSIVE && i != ROOT_KEY) {
                int def = (i >= MOD_DELAY && i <= MOD_RELEASE && i != MOD_SUSTAIN) || (i >= VOL_DELAY && i <= VOL_RELEASE && i != VOL_SUSTAIN) ||
                          i == MOD_LFO_DELAY || i == VIB_LFO_DELAY ? -12000 : i == CUTOFF ? 13500 : 0;
                int base = g.set[i] ? g.v[i] : def;
                m.put(i, int16_t(clampi(base + pz.v[i], -32768, 32767)));
            }
            Zone z;
            z.group = group;
            z.name = s.name;
            if (!refs[si]) refs[si] = sample_ref(blob, sf, s, si);
            z.sample = refs[si];
            z.key_lo = klo; z.key_hi = khi; z.vel_lo = vlo; z.vel_hi = vhi;
            z.root = g.set[ROOT_KEY] && g.v[ROOT_KEY] >= 0 ? g.v[ROOT_KEY] : (s.pitch <= 127 ? s.pitch : 60);
            if (g.set[KEYNUM] && g.v[KEYNUM] >= 0) { z.key_tracking = 0; z.root = g.v[KEYNUM]; }
            z.tune = sum(COARSE_TUNE) + (sum(FINE_TUNE) + s.correction) / 100.0;
            if (g.set[SCALE_TUNE] || pz.set[SCALE_TUNE]) z.key_tracking = clampd(sum(SCALE_TUNE, 100) / 100.0, 0, 1);
            z.pan = clampd(sum(PAN) / 500.0, -1, 1);
            z.gain_db = -sum(ATTENUATION) * 0.04;
            z.exclusive_group = g.set[EXCLUSIVE] ? g.v[EXCLUSIVE] : 0;
            int64_t start_off = int64_t(sum(START_OFS)) + int64_t(sum(START_COARSE)) * 32768;
            int64_t end_off = int64_t(sum(END_OFS)) + int64_t(sum(END_COARSE)) * 32768;
            z.start = std::max<int64_t>(0, start_off);
            z.stop = int64_t(s.end) - int64_t(s.start) + end_off;
            int modes = g.set[SAMPLE_MODES] ? g.v[SAMPLE_MODES] & 3 : 0;
            if (modes == 1 || modes == 3) {
                Loop l;
                l.until_release = modes == 3;
                l.start = int64_t(s.loop_start) - int64_t(s.start) + int64_t(sum(LOOP_START_OFS)) + int64_t(sum(LOOP_START_COARSE)) * 32768;
                l.end = int64_t(s.loop_end) - int64_t(s.start) + int64_t(sum(LOOP_END_OFS)) + int64_t(sum(LOOP_END_COARSE)) * 32768 - 1;
                if (l.end > l.start && l.start >= 0) z.loops.push_back(l);
            }
            Envelope &a = z.amp_env;
            a.set = true;
            a.delay = timecents(m, VOL_DELAY); a.attack = timecents(m, VOL_ATTACK); a.hold = timecents(m, VOL_HOLD);
            a.decay = timecents(m, VOL_DECAY); a.release = timecents(m, VOL_RELEASE); a.sustain = sustain_level(m, VOL_SUSTAIN);
            a.decay_slope = -1; a.release_slope = -1;   // SF2 decays/releases are linear in dB
            double cutoff_c = m.set[CUTOFF] ? m.v[CUTOFF] : 13500;
            int mod_cut = m.set[MOD_ENV_CUTOFF] ? m.v[MOD_ENV_CUTOFF] : 0, lfo_cut = m.set[MOD_LFO_CUTOFF] ? m.v[MOD_LFO_CUTOFF] : 0;
            if (cutoff_c < 13500 || mod_cut || lfo_cut) {
                Filter &f = z.filter;
                f.type = FilterType::LowPass;
                f.poles = 2;
                f.cutoff = std::min(MAX_CUTOFF_HZ, cents_hz(cutoff_c));
                f.resonance = clampd((m.set[RESONANCE] ? m.v[RESONANCE] : 0) / 10.0 / MAX_RESONANCE_DB, 0, 1);
                f.env_depth = mod_cut / MAX_ENVELOPE_DEPTH;
                Envelope &e = f.env;
                e.set = mod_cut != 0;
                e.delay = timecents(m, MOD_DELAY); e.attack = timecents(m, MOD_ATTACK); e.hold = timecents(m, MOD_HOLD);
                e.decay = timecents(m, MOD_DECAY); e.release = timecents(m, MOD_RELEASE);
                e.sustain = 1.0 - clampd((m.set[MOD_SUSTAIN] ? m.v[MOD_SUSTAIN] : 0) / 1000.0, 0, 1);
                if (lfo_cut) {
                    f.lfo_depth = lfo_cut / MAX_ENVELOPE_DEPTH;
                    f.lfo.set = true; f.lfo.wave = LfoWave::Triangle;
                    f.lfo.rate_hz = cents_hz(m.set[MOD_LFO_FREQ] ? m.v[MOD_LFO_FREQ] : 0);
                    f.lfo.delay = timecents(m, MOD_LFO_DELAY);
                }
                // SF2 default modulator: the cutoff falls by up to 2400 cents as the velocity falls; the model's
                // velocity depth raises it with velocity, so start two octaves lower and rise by 2400 cents
                f.cutoff = std::max(20.0, f.cutoff / 4.0);
                f.vel_depth = 2400 / 9600.0;
            }
            int env_pitch = m.set[MOD_ENV_PITCH] ? m.v[MOD_ENV_PITCH] : 0;
            if (env_pitch) {
                z.pitch_env_depth = env_pitch / MAX_ENVELOPE_DEPTH;
                Envelope &e = z.pitch_env;
                e.set = true;
                e.delay = timecents(m, MOD_DELAY); e.attack = timecents(m, MOD_ATTACK); e.hold = timecents(m, MOD_HOLD);
                e.decay = timecents(m, MOD_DECAY); e.release = timecents(m, MOD_RELEASE);
                e.sustain = 1.0 - clampd((m.set[MOD_SUSTAIN] ? m.v[MOD_SUSTAIN] : 0) / 1000.0, 0, 1);
            }
            int vib = m.set[VIB_LFO_PITCH] ? m.v[VIB_LFO_PITCH] : 0;
            if (vib) {
                z.pitch_lfo_depth = vib / MAX_ENVELOPE_DEPTH;
                z.pitch_lfo.set = true; z.pitch_lfo.wave = LfoWave::Triangle;
                z.pitch_lfo.rate_hz = cents_hz(m.set[VIB_LFO_FREQ] ? m.v[VIB_LFO_FREQ] : 0);
                z.pitch_lfo.delay = timecents(m, VIB_LFO_DELAY);
            }
            int trem = m.set[MOD_LFO_VOL] ? m.v[MOD_LFO_VOL] : 0;
            if (trem) {
                z.amp_lfo_depth = trem / 10.0 / MAX_VOLUME_DEPTH;
                z.amp_lfo.set = true; z.amp_lfo.wave = LfoWave::Triangle;
                z.amp_lfo.rate_hz = cents_hz(m.set[MOD_LFO_FREQ] ? m.v[MOD_LFO_FREQ] : 0);
                z.amp_lfo.delay = timecents(m, MOD_LFO_DELAY);
            }
            z.amp_vel_curve = 1;   // SF2 default: concave velocity to attenuation (about x^3)
            inst.zones.push_back(std::move(z));
        }
    }
    finish_instrument(inst);
    return inst;
}

bool probe_sf2(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    return n >= 12 && !std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "sfbk", 4);
}

}  // namespace

void register_sf2() {
    register_reader({"SoundFont 2", "sf2 sbk", probe_sf2, list_sf2, load_sf2});
}

}  // namespace omni

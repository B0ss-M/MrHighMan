// Omni Sampler: Akai S900/S950, S1000/S1100/S3000/S3200/CD3000 programs and samples, and MESA .S3P programs.
// Structures and the conversion to the model are translated from ConvertWithMoss (LGPL-3.0, Jürgen Moßgraber):
// AkaiS1000Program/Keygroup/KeygroupSample/Sample/Envelope, AkaiS1000ProgramConverter, AkaiS900*, S3pFile.
// Differences: S3000 sample headers are 192 bytes (CWM always skips 150), chosen from the file size; a sample's
// pitch offset is one s16 in 1/256 semitone (CWM: cents + semitones); loop mode 1 (LOOP UNTIL REL) loops until
// release. The disc layout notes of AKAI-CD-ISO-MAKER (MIT, docs/FORMAT.md, verified on commercial CDs) confirm these.
#include <cmath>
#include <cstring>
#include <map>
#include "../fs/fs.hpp"
#include "format.hpp"

namespace omni {
namespace {
#include "akai_tables.inc"

// ---------------------------------------------------------------------------------------------------------------
// S1000 / S3000 structures

struct S1kEnv { int attack = 0, decay = 0, sustain = 99, release = 0, vel_attack = 0, vel_release = 0, off_vel_release = 0, key_dr = 0; };
struct S1kZone { std::string name; int lovel = 0, hivel = 127, tune_cents = 0, tune_semis = 0, loudness = 0, filter = 0, pan = 0, loop_mode = 0; };
struct S1kKeygroup {
    int lokey = 24, hikey = 127, tune_cents = 0, tune_semis = 0, filter = 99, key_filter = 0, vel_filter = 0, press_filter = 0, env2_filter = 0;
    S1kEnv env1, env2;
    int vel_env2_filter = 0, env2_pitch = 0;
    S1kZone zones[4];
    bool keytrack[4] = {true, true, true, true};
};
struct S1kProgram {
    std::string name;
    int midi_prog = 0, midi_chan = 0, polyphony = 0, low_key = 24, high_key = 127, octave_shift = 0;
    int stereo_level = 99, mix_pan = 0, volume = 80, vel_to_vol = 0, key_to_vol = 0, bend_to_pitch = 2, num_kg = 0;
    std::vector<S1kKeygroup> keygroups;
};
struct S1kLoop { uint32_t marker = 0, coarse = 0; };
struct S1kSample {
    std::string name;
    int root = 60, tune_cents = 0, tune_semis = 0, active_loops = 0, first_loop = 0, loop_mode = 2;
    uint32_t frames = 0, start = 0, end = 0;
    S1kLoop loops[8];
    int rate = 44100;
    uint64_t data_offset = 150;
};

S1kEnv read_env(Reader &r) {
    S1kEnv e;
    e.attack = r.u8(); e.decay = r.s8(); e.sustain = r.s8(); e.release = r.s8();
    e.vel_attack = r.s8(); e.vel_release = r.s8(); e.off_vel_release = r.s8(); e.key_dr = r.s8();
    return e;
}

void read_program_header(Reader &r, S1kProgram &p) {
    if (r.u8() != 1) throw ParseError("not an Akai program");
    r.skip(2);
    p.name = akai_to_ascii(r.take(12), 12);
    p.midi_prog = r.u8(); p.midi_chan = r.u8(); p.polyphony = r.u8(); r.skip(1);
    p.low_key = r.u8(); p.high_key = r.u8(); p.octave_shift = r.s8(); r.skip(1);
    p.stereo_level = r.u8(); p.mix_pan = r.s8(); p.volume = r.u8(); p.vel_to_vol = r.s8(); p.key_to_vol = r.s8();
    r.skip(1 + 3 + 1 + 3 + 3);   // pressure>vol, pan LFO, key>pan, LFO rate/depth/delay, mod/press/vel>LFO depth
    p.bend_to_pitch = r.u8();
    r.skip(2);                   // pressure>pitch, keygroup crossfade
    p.num_kg = r.u8();
}

S1kKeygroup read_keygroup(Reader &r) {
    S1kKeygroup k;
    if (r.u8() != 2) throw ParseError("not an Akai keygroup");
    r.skip(2);
    k.lokey = r.u8(); k.hikey = r.u8(); k.tune_cents = r.s8(); k.tune_semis = r.s8();
    k.filter = r.u8(); k.key_filter = r.u8(); k.vel_filter = r.s8(); k.press_filter = r.s8(); k.env2_filter = r.s8();
    k.env1 = read_env(r);
    k.env2 = read_env(r);
    k.vel_env2_filter = r.s8(); k.env2_pitch = r.s8();
    r.skip(2 + 2);   // velocity zone crossfade, zones used, FF FF
    for (auto &z : k.zones) {
        z.name = akai_to_ascii(r.take(12), 12);
        z.lovel = r.u8(); z.hivel = r.u8(); z.tune_cents = r.s8(); z.tune_semis = r.s8();
        z.loudness = r.s8(); z.filter = r.s8(); z.pan = r.s8(); z.loop_mode = r.u8();
        r.skip(4);
    }
    r.skip(2);   // beat detune, hold attack until loop
    for (auto &t : k.keytrack) t = r.u8() == 0;
    return k;
}

S1kProgram parse_program(const std::vector<uint8_t> &d, bool s3000_hint) {
    Reader r(d);
    S1kProgram p;
    read_program_header(r, p);
    // header 150 (S1000) or 192 (S3000); keygroups of 150 (S1000) or 192 (S3000, per Ohsaki's notes) back to back.
    // Some tools write S3000 programs with 150-byte keygroups, so take the combination the file size fits.
    size_t hs = s3000_hint ? 192 : 150, ks = hs, n = size_t(p.num_kg);
    for (size_t h : {size_t(192), size_t(150)})
        for (size_t k : {size_t(192), size_t(150)})
            if (d.size() == h + n * k) { hs = h; ks = k; }
    for (int i = 0; i < p.num_kg; i++) {
        size_t off = hs + ks * size_t(i);
        if (off + 120 > d.size()) break;
        Reader kr = r.sub_at(off, std::min<size_t>(ks, d.size() - off));
        try { p.keygroups.push_back(read_keygroup(kr)); } catch (const ParseError &) { break; }
    }
    return p;
}

S1kSample parse_sample_header(const uint8_t *h, size_t n, uint64_t file_size, bool s3000_hint) {
    Reader r(h, n);
    S1kSample s;
    int id = r.u8();
    if (id != 3 && id != 1) throw ParseError("not an Akai sample");
    int rate_index = r.u8();
    s.root = r.u8();
    s.name = akai_to_ascii(r.take(12), 12);
    r.skip(1);
    s.active_loops = r.u8(); s.first_loop = r.u8(); r.skip(1);
    s.loop_mode = r.u8();
    s.tune_cents = r.u8();   // 0x14: s16 pitch offset in 1/256 semitone (low byte fraction, high byte semitones)
    s.tune_semis = r.s8();
    r.skip(4);
    s.frames = r.u32le(); s.start = r.u32le(); s.end = r.u32le();
    for (auto &l : s.loops) { l.marker = r.u32le(); r.skip(2); l.coarse = r.u32le(); r.skip(2); }
    r.skip(4);
    s.rate = r.u16le();
    if (s.rate == 0) s.rate = rate_index == 0 ? 22050 : 44100;
    // header size from the file size (the most reliable sign), else from the volume type
    if (file_size == 192 + uint64_t(s.frames) * 2) s.data_offset = 192;
    else if (file_size == 190 + uint64_t(s.frames) * 2) s.data_offset = 190;
    else if (file_size == 150 + uint64_t(s.frames) * 2) s.data_offset = 150;
    else s.data_offset = s3000_hint ? 192 : 150;
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// conversion (AkaiS1000ProgramConverter)

constexpr double DB_PER_STEP = 0.2372;
constexpr int FULL_LEVEL = 255;

double loudness_db(int loudness) {
    int m = S1000_PARAMETER_SCALE[clampi(std::abs(loudness), 0, 99)];
    return (loudness < 0 ? -m : m) * DB_PER_STEP;
}
double sustain_level(int sustain) {
    int s = S1000_PARAMETER_SCALE[clampi(sustain, 0, 99)];
    return s <= 0 ? 0 : std::pow(10.0, (s - FULL_LEVEL) * DB_PER_STEP / 20.0);
}
double env_seconds(int value) { return 32767.0 * 128.0 / S1000_ENVELOPE_RATE[99 - clampi(value, 0, 99)] / 44100.0; }
double tuning(int semis, int fraction) { return semis + (fraction & 0xFF) / 256.0; }

Envelope convert_env(const S1kEnv &a) {
    Envelope e;
    e.set = true;
    e.attack = env_seconds(a.attack);
    e.decay = env_seconds(a.decay);
    e.sustain = sustain_level(a.sustain);
    e.release = env_seconds(a.release);
    e.time_vel_tracking = clampd(-a.vel_attack / 50.0, -1, 1);
    e.time_key_tracking = clampd(-a.key_dr / 50.0, -1, 1);
    return e;
}

// sample lookup: an Akai sample file next to the program, or a WAV (MESA, exported programs)
struct SampleSource {
    SampleRefPtr ref;
    S1kSample hdr;
    bool from_wav = false;
    AudioInfo wav;
};

using SampleFinder = std::function<bool(const std::string &name, SampleSource &out)>;

void program_to_zones(const S1kProgram &prog, const SampleFinder &find, Instrument &inst, int group) {
    double gain = loudness_db(prog.volume) - FULL_LEVEL * DB_PER_STEP;
    double stereo = loudness_db(prog.stereo_level) - FULL_LEVEL * DB_PER_STEP;
    double stereo_pan = clampi(prog.mix_pan, -50, 50) / 50.0;
    int octave = clampi(prog.octave_shift, -2, 2) * 12;
    for (const auto &kg : prog.keygroups) {
        double kg_tune = tuning(kg.tune_semis, kg.tune_cents);
        Envelope amp = convert_env(kg.env1);
        amp.attack_slope = 1; amp.decay_slope = -1; amp.release_slope = -1;
        Envelope aux = convert_env(kg.env2);
        double cutoff_mod = kg.env2_filter / 50.0;
        bool has_filter = kg.filter < 99 || cutoff_mod != 0;
        double pitch_mod = kg.env2_pitch / 50.0;
        for (int i = 0; i < 4; i++) {
            const S1kZone &kz = kg.zones[i];
            if (kz.name.empty() || kz.hivel == 0) continue;
            SampleSource src;
            if (!find(kz.name, src)) { inst.warnings.push_back("missing sample " + kz.name); continue; }
            Zone z;
            z.name = kz.name;
            z.group = group;
            z.sample = src.ref;
            z.key_lo = kg.lokey; z.key_hi = kg.hikey;
            z.vel_lo = kz.lovel; z.vel_hi = kz.hivel;
            z.pan = clampi(kz.pan, -50, 50) / 50.0;
            z.gain_db = loudness_db(kz.loudness);
            z.amp_env = amp;
            int loop_mode;
            double sample_tune;
            if (src.from_wav) {
                z.root = src.wav.root >= 0 ? src.wav.root : 60;
                sample_tune = src.wav.fine / 100.0;
                loop_mode = kz.loop_mode == 0 ? (src.wav.loops.empty() ? 2 : 0) : kz.loop_mode - 1;
                if (loop_mode < 2 && !src.wav.loops.empty()) { Loop l = src.wav.loops[0]; l.until_release = loop_mode == 1; z.loops.push_back(l); }
            } else {
                const S1kSample &s = src.hdr;
                z.root = s.root;
                sample_tune = int16_t(s.tune_semis << 8 | s.tune_cents) / 256.0;
                z.start = s.start;
                z.stop = s.end > s.start ? int64_t(s.end) + 1 : -1;
                loop_mode = kz.loop_mode == 0 ? s.loop_mode : kz.loop_mode - 1;
                if (s.active_loops > 0 && loop_mode < 2 && s.first_loop >= 0 && s.first_loop < 8) {
                    const S1kLoop &l = s.loops[s.first_loop];
                    Loop lp;
                    lp.start = int64_t(l.marker) - int64_t(l.coarse);
                    lp.end = l.marker;
                    lp.until_release = loop_mode == 1;
                    if (lp.end > lp.start && lp.start >= 0) z.loops.push_back(lp);
                }
            }
            z.one_shot = loop_mode == 3;
            if (has_filter) {
                Filter &f = z.filter;
                f.type = FilterType::LowPass;
                f.poles = 3;
                f.cutoff = S1000_FILTER_CUTOFF[clampi(kg.filter, 0, 99)];
                f.env_depth = cutoff_mod;
                f.env = aux;
                f.vel_depth = kg.vel_filter / 50.0;
                f.key_tracking = clampd(kg.key_filter / 12.0, 0, 1);
            }
            z.key_tracking = kg.keytrack[i] ? 1 : 0;
            z.tune = kg_tune + tuning(kz.tune_semis, kz.tune_cents) + sample_tune;
            if (pitch_mod != 0) { z.pitch_env_depth = pitch_mod; z.pitch_env = aux; }
            // program level
            z.bend_up = prog.bend_to_pitch * 100;
            z.bend_down = -prog.bend_to_pitch * 100;
            z.gain_db += gain + stereo;
            if (stereo_pan != 0) z.pan = clampd(z.pan + stereo_pan, -1, 1);
            if (octave) { z.key_lo = std::max(0, z.key_lo - octave); z.key_hi = std::min(127, z.key_hi - octave); z.tune += octave; }
            z.amp_vel_depth = clampd(prog.vel_to_vol / 50.0, -1, 1);
            if (z.amp_vel_depth <= 0) z.amp_vel_depth = 0;
            z.amp_key_tracking = clampd(prog.key_to_vol / 50.0, -1, 1);
            inst.zones.push_back(std::move(z));
        }
    }
    if (prog.polyphony > 0) inst.polyphony = inst.polyphony ? std::min(inst.polyphony, clampi(prog.polyphony, 1, 16)) : clampi(prog.polyphony, 1, 16);
}

// Finds "<name>.s"/".s3" (Akai) or "<name>.wav" next to the program.
SampleFinder folder_finder(VolumePtr vol, const std::string &dir, bool s3000_hint) {
    auto cache = std::make_shared<std::map<std::string, SampleSource>>();
    auto listing = std::make_shared<std::vector<DirEntry>>(vol->list(dir));
    return [vol, dir, s3000_hint, cache, listing](const std::string &name, SampleSource &out) -> bool {
        auto it = cache->find(name);
        if (it != cache->end()) { out = it->second; return true; }
        std::string want = lower(trim(name));
        for (const auto &e : *listing) {
            if (e.dir) continue;
            std::string stem = lower(trim(path_stem(e.name))), ext = path_ext(e.name);
            if (stem != want) continue;
            std::string full = path_join(dir, e.name);
            SampleSource src;
            if (ext == "s" || ext == "s3" || ext == "s1") {
                BlobPtr b = vol->open(full);
                std::vector<uint8_t> h = b->head(192);
                try { src.hdr = parse_sample_header(h.data(), h.size(), b->size(), s3000_hint || ext == "s3"); } catch (const ParseError &) { continue; }
                src.ref = raw_sample_ref(b, src.hdr.data_offset, src.hdr.frames, 1, Enc::S16LE, src.hdr.rate, src.hdr.name);
            } else if (is_audio_ext(ext)) {
                src.from_wav = true;
                try { src.wav = probe_audio(*vol->open(full)); } catch (const std::exception &) { continue; }
                src.ref = file_sample_ref(vol, full);
            } else continue;
            (*cache)[name] = src;
            out = src;
            return true;
        }
        return false;
    };
}

std::vector<PresetInfo> list_single(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

Instrument load_s1000_program(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->all(1 << 20);
    bool s3 = path_ext(path) == "p3";
    S1kProgram prog = parse_program(d, s3);
    Instrument inst;
    inst.format = s3 ? "Akai S3000 program" : "Akai S1000 program";
    inst.name = prog.name.empty() ? path_stem(path) : prog.name;
    program_to_zones(prog, folder_finder(vol, path_dir(path), s3), inst, 0);
    finish_instrument(inst);
    return inst;
}

// A lone sample file plays over the whole keyboard.
Instrument load_s1000_sample(VolumePtr vol, const std::string &path, int) {
    BlobPtr b = vol->open(path);
    std::vector<uint8_t> h = b->head(192);
    S1kSample s = parse_sample_header(h.data(), h.size(), b->size(), path_ext(path) == "s3");
    Instrument inst;
    inst.format = "Akai S1000/S3000 sample";
    inst.name = s.name.empty() ? path_stem(path) : s.name;
    Zone z;
    z.name = inst.name;
    z.sample = raw_sample_ref(b, s.data_offset, s.frames, 1, Enc::S16LE, s.rate, s.name);
    z.root = s.root;
    z.tune = int16_t(s.tune_semis << 8 | s.tune_cents) / 256.0;
    z.start = s.start;
    if (s.end > s.start) z.stop = int64_t(s.end) + 1;
    if (s.active_loops > 0 && s.loop_mode < 2 && s.first_loop < 8) {
        Loop l;
        l.end = s.loops[s.first_loop].marker;
        l.start = int64_t(l.end) - int64_t(s.loops[s.first_loop].coarse);
        l.until_release = s.loop_mode == 1;
        if (l.start >= 0 && l.end > l.start) z.loops.push_back(l);
    }
    z.one_shot = s.loop_mode == 3;
    inst.zones.push_back(z);
    finish_instrument(inst);
    return inst;
}

bool probe_s1000_program(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 150 && h[0] == 1; }
bool probe_s1000_sample(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 150 && h[0] == 3 && (h[1] == 0 || h[1] == 1); }

// ---------------------------------------------------------------------------------------------------------------
// MESA .S3P: an S1000 program as SysEx (nibbles), samples as WAVs next to it

std::vector<uint8_t> nibbles(const uint8_t *p, size_t n) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < n; i += 2) out.push_back(uint8_t((p[i + 1] & 0x0F) << 4 | (p[i] & 0x0F)));
    return out;
}

Instrument load_s3p(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->all(16 << 20);
    Reader r(d);
    if (!r.starts("PSYSSS30")) throw ParseError("not a MESA S3P file");
    r.skip(8);
    r.u32be();
    S1kProgram prog;
    bool have_program = false;
    while (r.left() >= 4) {
        uint32_t len = r.u32be();
        if (len > r.left()) break;
        const uint8_t *m = r.take(len);
        if (len < 9 || m[0] != 0xF0 || m[1] != 0x47 || m[4] != 0x48 || m[len - 1] != 0xF7) continue;
        if (m[3] == 7) {
            std::vector<uint8_t> c = nibbles(m + 7, len - 8);
            Reader pr(c);
            read_program_header(pr, prog);
            have_program = true;
        } else if (m[3] == 9) {
            std::vector<uint8_t> c = nibbles(m + 8, len - 9);
            Reader kr(c);
            try { prog.keygroups.push_back(read_keygroup(kr)); } catch (const ParseError &) {}
        }
    }
    if (!have_program) throw ParseError("no program in S3P file");
    Instrument inst;
    inst.format = "Akai MESA program";
    inst.name = prog.name.empty() ? path_stem(path) : prog.name;
    program_to_zones(prog, folder_finder(vol, path_dir(path), false), inst, 0);
    finish_instrument(inst);
    return inst;
}

bool probe_s3p(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 8 && !std::memcmp(h, "PSYSSS30", 8); }

// ---------------------------------------------------------------------------------------------------------------
// S900 / S950 (AkaiS900*)

struct S900Layer { std::string sample; int fa = 0, fd = 0, fs = 0, fr = 0, tuning = 0, filter = 99, loudness = 0; };
struct S900Keygroup {
    int key_hi = 127, key_lo = 0, vel_switch = 0, attack = 0, decay = 0, sustain = 99, release = 0;
    int filter_vel = 0, filter_key = 50, attack_vel = 0, vel_release = 0, loud_vel = 0;
    int warp_vel = 0, warp_offset = 0, warp_recovery = 0, lfo_buildup = 0, lfo_rate = 0, lfo_depth = 0, flags = 0;
    int output = 0, midi_offset = 0, at_depth = 0, wheel_lfo = 0, env_filter = 0;
    S900Layer layers[2];
};

std::vector<S900Keygroup> parse_s900_program(const std::vector<uint8_t> &d, std::string &name, int &tilt, bool &xfade) {
    Reader r(d);
    name = trim(r.str(10));
    r.skip(6);
    tilt = r.s8();
    r.skip(1 + 2 + 1);
    xfade = r.u8() > 0;
    r.skip(1);
    int n = r.u8();
    r.skip(14);
    std::vector<S900Keygroup> kgs;
    for (int i = 0; i < n && r.left() >= 70; i++) {
        S900Keygroup k;
        k.key_hi = r.u8(); k.key_lo = r.u8(); k.vel_switch = r.u8();
        k.attack = r.u8(); k.decay = r.u8(); k.sustain = r.u8(); k.release = r.u8();
        k.filter_vel = r.u8(); k.filter_key = r.u8(); k.attack_vel = r.u8(); k.vel_release = r.s8(); k.loud_vel = r.u8();
        k.warp_vel = r.u8(); k.warp_offset = r.s8(); k.warp_recovery = r.u8();
        k.lfo_buildup = r.u8(); k.lfo_rate = r.u8(); k.lfo_depth = r.u8(); k.flags = r.u8();
        k.output = r.u8(); k.midi_offset = r.u8(); k.at_depth = r.u8(); k.wheel_lfo = r.u8(); k.env_filter = r.s8();
        for (auto &l : k.layers) {
            l.sample = trim(r.str(10));
            l.fa = r.u8(); l.fd = r.u8(); l.fs = r.u8(); l.fr = r.u8();
            r.skip(2);
            r.skip(2);
            l.tuning = r.u16le();
            l.filter = r.u8(); l.loudness = r.u8();
        }
        r.skip(2);
        kgs.push_back(k);
    }
    return kgs;
}

struct S900Sample {
    std::string name;
    uint32_t length = 0, end = 0, start = 0, loop_length = 0;
    int rate = 0, pitch = 960, loudness = 0, mode = 'O', direction = 'N';
};
S900Sample parse_s900_sample(const uint8_t *h, size_t n) {
    Reader r(h, n);
    S900Sample s;
    s.name = trim(r.str(10));
    r.skip(6);
    s.length = r.u32le(); s.rate = r.u16le(); s.pitch = r.u16le(); s.loudness = r.s16le();
    s.mode = r.u8(); r.skip(1);
    s.end = r.u32le(); s.start = r.u32le(); s.loop_length = r.u32le();
    r.skip(2);
    r.u8();
    s.direction = r.u8();
    return s;
}

// 12-bit S900 data to int16 (AkaiS900DiskImage.convertNonCompressedSampleToWav / decodeS900CompressedToWav)
PcmPtr decode_s900(const std::vector<uint8_t> &src, uint32_t count, int rate, bool compressed) {
    auto p = std::make_shared<Pcm>();
    p->rate = rate > 0 ? rate : 22050;
    p->channels = 1;
    uint32_t part = (count + 1) / 2;
    p->data.assign(size_t(part) * 2, 0);
    if (!compressed) {
        for (uint32_t i = 0; i < part && size_t(i) * 2 + 1 < src.size(); i++)
            p->data[i] = int16_t(src[2 * i + 1] << 8 | (src[2 * i] & 0xF0));
        for (uint32_t i = 0; i < part && size_t(part) * 2 + i < src.size(); i++)
            p->data[part + i] = int16_t(src[size_t(part) * 2 + i] << 8 | ((src[2 * i] << 4) & 0xF0));
    } else {
        auto bits = [&](size_t pos, int count2) {
            int v = 0;
            for (int i = 0; i < count2; i++, pos++) {
                v <<= 1;
                if (pos / 8 < src.size() && (src[pos / 8] & (1 << (7 - (pos & 7))))) v |= 1;
            }
            return v;
        };
        int16_t value = 0, inc = 0;
        size_t remaining = src.size() * 8, bitpos = 0, out = 0;
        while (remaining >= 4 && out < p->data.size()) {
            int code = bits(bitpos, 4);
            bitpos += 4; remaining -= 4;
            if (code == 0) {
                for (int i = 0; i < 10 && out < p->data.size(); i++) { value = int16_t(value + inc); p->data[out++] = int16_t(value << 4); }
                continue;
            }
            int bpv = 16 - code;
            size_t need = size_t(10 * (1 + bpv));
            if (remaining < need) break;
            for (int i = 0; i < 10 && out < p->data.size(); i++) {
                int sign = bits(bitpos + i, 1), mag = bits(bitpos + 10 + size_t(i) * bpv, bpv);
                inc = int16_t(sign ? inc - mag : inc + mag);
                value = int16_t(value + inc);
                p->data[out++] = int16_t(value << 4);
            }
            bitpos += need; remaining -= need;
        }
    }
    p->data.resize(std::min<size_t>(p->data.size(), count));
    return p;
}

Instrument load_s900_program(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->all(1 << 20);
    std::string name;
    int tilt = 0;
    bool kg_xfade = false;
    std::vector<S900Keygroup> kgs = parse_s900_program(d, name, tilt, kg_xfade);
    Instrument inst;
    inst.format = "Akai S900/S950 program";
    inst.name = name.empty() ? path_stem(path) : name;
    std::string dir = path_dir(path);
    std::vector<DirEntry> listing = vol->list(dir);
    std::map<std::string, std::pair<SampleRefPtr, S900Sample>> cache;
    auto find = [&](const std::string &sn, SampleRefPtr &ref, S900Sample &hdr) -> bool {
        auto it = cache.find(sn);
        if (it != cache.end()) { ref = it->second.first; hdr = it->second.second; return true; }
        for (auto &e : listing) {
            std::string ext = path_ext(e.name);
            if (e.dir || (ext != "s9" && ext != "s9c") || !equals_ci(trim(path_stem(e.name)), sn)) continue;
            BlobPtr b = vol->open(path_join(dir, e.name));
            std::vector<uint8_t> h = b->head(60);
            if (h.size() < 60) return false;
            hdr = parse_s900_sample(h.data(), h.size());
            bool compressed = ext == "s9c";
            auto r = std::make_shared<SampleRef>();
            r->name = hdr.name;
            r->rate = hdr.rate;
            r->channels = 1;
            r->frames = hdr.length;
            r->key = "s900:" + std::to_string(reinterpret_cast<uintptr_t>(b.get()));
            uint32_t count = hdr.length;
            int rate = hdr.rate;
            r->decode = [b, count, rate, compressed]() {
                std::vector<uint8_t> all = b->all(64 << 20);
                std::vector<uint8_t> data(all.begin() + 60, all.end());
                return decode_s900(data, count, rate, compressed);
            };
            ref = r;
            cache[sn] = {ref, hdr};
            return true;
        }
        return false;
    };
    auto env = [](int a, int dcy, int s, int r) {
        Envelope e;
        e.set = true;
        e.attack = a / 99.0 * 2.0; e.decay = dcy / 99.0 * 6.0; e.sustain = s / 99.0; e.release = r / 99.0 * 2.0;
        return e;
    };
    for (auto &k : kgs) {
        for (int layer = 0; layer < 2; layer++) {
            if (layer == 0 && k.vel_switch <= 0) continue;
            if (layer == 1 && k.vel_switch >= 128) continue;
            const S900Layer &l = k.layers[layer];
            SampleRefPtr ref;
            S900Sample s;
            if (!find(l.sample, ref, s)) { if (!l.sample.empty()) inst.warnings.push_back("missing sample " + l.sample); continue; }
            Zone z;
            z.name = l.sample;
            z.sample = ref;
            z.key_lo = k.key_lo; z.key_hi = k.key_hi;
            if (layer == 0) { z.vel_lo = 0; z.vel_hi = std::min(k.vel_switch, 127); if (!(k.flags & 2)) z.vel_xfade_hi = 127 - k.vel_switch; }
            else { z.vel_lo = std::min(k.vel_switch + 1, 127); z.vel_hi = 127; if (!(k.flags & 2)) z.vel_xfade_lo = k.vel_switch; }
            z.reverse = s.direction == 'R';
            z.one_shot = (k.flags & 8) != 0;
            double nominal = s.pitch / 16.0;
            z.root = int(std::lround(nominal));
            z.tune = z.root - nominal;
            z.key_tracking = (k.flags & 1) ? 0 : 1;
            if (k.lfo_depth > 0 && k.wheel_lfo == 0) {
                z.pitch_lfo_depth = k.lfo_depth / 99.0 * 300.0 / MAX_ENVELOPE_DEPTH;
                z.pitch_lfo.set = true;
                z.pitch_lfo.wave = LfoWave::Sine;
                double nv = clampd(k.lfo_rate / 99.0, 0, 1);
                z.pitch_lfo.rate_hz = 0.001953125 + 20.0 * (std::pow(4.0, nv) - 1.0) / 3.0;
                z.pitch_lfo.delay = k.lfo_buildup / 99.0 * 2.0;
            }
            if (k.warp_offset > 0) {
                z.pitch_env_depth = 1.0 / 40.0;
                z.pitch_env.set = true;
                z.pitch_env.start_level = k.warp_offset / 50.0 * 0.25;
                z.pitch_env.decay = k.warp_recovery / 99.0 * 2.0;
                z.pitch_env.sustain = 0;
            }
            if (k.output == 8) z.pan = -1; else if (k.output == 9) z.pan = 1;
            z.gain_db = std::min(24.0, (l.loudness + s.loudness) * 0.375);
            z.amp_env = env(k.attack, k.decay, k.sustain, k.release);
            z.amp_vel_depth = k.loud_vel / 99.0;
            z.start = 0;
            z.stop = s.end > 0 ? int64_t(s.end) : -1;
            if (s.loop_length > 0 && (s.mode == 'L' || s.mode == 'A')) {
                Loop lp;
                lp.end = s.end;
                lp.start = int64_t(s.end) - int64_t(s.loop_length);
                lp.type = s.mode == 'A' ? LoopType::Alternating : LoopType::Forward;
                if (lp.start >= 0) z.loops.push_back(lp);
            }
            if (l.filter < 99) {
                Filter &f = z.filter;
                f.type = FilterType::LowPass;
                f.poles = 3;
                f.cutoff = S900_FILTER_CUTOFF[clampi(l.filter, 0, 99)];
                f.env_depth = k.env_filter / 50.0;
                f.env = env(l.fa, l.fd, l.fs, l.fr);
                f.key_tracking = (k.filter_key - 50) / 50.0;
                f.vel_depth = k.filter_vel / 99.0;
            }
            z.amp_key_tracking = tilt / 50.0;
            inst.zones.push_back(z);
        }
    }
    finish_instrument(inst);
    return inst;
}

Instrument load_s900_sample(VolumePtr vol, const std::string &path, int) {
    BlobPtr b = vol->open(path);
    std::vector<uint8_t> h = b->head(60);
    if (h.size() < 60) throw ParseError("truncated S900 sample");
    S900Sample s = parse_s900_sample(h.data(), h.size());
    bool compressed = path_ext(path) == "s9c";
    Instrument inst;
    inst.format = "Akai S900/S950 sample";
    inst.name = s.name.empty() ? path_stem(path) : s.name;
    Zone z;
    z.name = inst.name;
    auto r = std::make_shared<SampleRef>();
    r->name = s.name; r->rate = s.rate; r->channels = 1; r->frames = s.length;
    r->key = "s900:" + std::to_string(reinterpret_cast<uintptr_t>(b.get()));
    uint32_t count = s.length;
    int rate = s.rate;
    r->decode = [b, count, rate, compressed]() {
        std::vector<uint8_t> all = b->all(64 << 20);
        std::vector<uint8_t> data(all.begin() + 60, all.end());
        return decode_s900(data, count, rate, compressed);
    };
    z.sample = r;
    double nominal = s.pitch / 16.0;
    z.root = int(std::lround(nominal));
    z.tune = z.root - nominal;
    z.reverse = s.direction == 'R';
    if (s.loop_length > 0 && (s.mode == 'L' || s.mode == 'A')) {
        Loop lp; lp.end = s.end; lp.start = int64_t(s.end) - int64_t(s.loop_length);
        if (lp.start >= 0) z.loops.push_back(lp);
    }
    inst.zones.push_back(z);
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_akai_s() {
    register_reader({"Akai S1000/S3000 program", "p p1 p3", probe_s1000_program, list_single, load_s1000_program});
    register_reader({"Akai S1000/S3000 sample", "s s1 s3", probe_s1000_sample, list_single, load_s1000_sample});
    register_reader({"Akai MESA program", "s3p", probe_s3p, list_single, load_s3p});
    register_reader({"Akai S900/S950 program", "p9", nullptr, list_single, load_s900_program});
    register_reader({"Akai S900/S950 sample", "s9 s9c", nullptr, list_single, load_s900_sample});
}

}  // namespace omni

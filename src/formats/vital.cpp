// Omni Sampler 2.4.11: Vital presets (.vital: JSON; a .vitalbank is a ZIP of them, mounted by fs/zip.cpp) on Omni's
// SYNTH engine (formats/synth_patch.hpp), and Vital's SAMPLE oscillator (a recording stored in the preset) on the sampler.
// Approximate by nature, like the other translators: two of Vital's three oscillators, one filter, envelopes, two LFOs and
// up to eight modulations; effects, the second filter and spectral morph are not read. 2.4.13: the wavetables and the LFOs'
// drawn shapes are played as they are (formats/vital_table.cpp).
//
// The file's units (written from the preset files themselves: 265 presets of two banks measured; [G] = a guess from the
// value ranges, check by ear):
//   osc_N_on 0/1, osc_N_level 0..1, osc_N_transpose semitones, osc_N_tune -1..1 semitone, osc_N_pan -1..1,
//   osc_N_unison_voices 1..16, osc_N_unison_detune 0..10 (cents = range x 100 x (d / 10)^2 [G]), osc_N_detune_range st,
//   osc_N_stereo_spread 0..1, osc_N_wave_frame 0..256, osc_N_distortion_type (0 none, 1 sync, 4 bend, 5 squeeze, 6 pulse
//   width, 7/8 FM from another osc, 10/11 RM from another osc [G]) and _amount 0..1.
//   filter_N_on, _cutoff = MIDI note (8..136), _resonance 0..1, _drive dB (0..20), _keytrack -1..1, _mix 0..1,
//   _blend 0..2 (0 low pass, 1 band pass, 2 high pass), _style (0 12 dB, 1 24 dB, 2.. notch styles [G]), _model (0 analog,
//   1 dirty, 2 ladder, 3 digital, 4 diode, 5 formant, 6 comb, 7 phaser [G]).
//   env_N_attack / _decay / _release: seconds = v^4 (0..2.378 = 0..32 s), _sustain 0..1. env_1 is the amp envelope.
//   lfo_N_frequency: Hz = 2^v; _sync 0 free, 1 tempo, 2 dotted, 3 triplet [G]; _tempo: 32/1 16/1 8/1 4/1 2/1 1/1 1/2 1/4
//   1/8 1/16 1/32 1/64 [G]; _sync_type 0 = retrigger [G]; the shape: settings.lfos[N-1] {points, powers, smooth, name}.
//   modulations[i] {source, destination}, the amount settings.modulation_<i+1>_amount: a fraction of the destination's
//   whole range (cutoff 128 st, transpose 96 st, tune 2 st, wave frame 0..256, level 0..1).
//   polyphony, legato, portamento_time (seconds = 2^v; -10 = off), portamento_force, pitch_bend_range st,
//   velocity_track 0..1, volume (5473 = 0 dB; dB = 40 log10(v / 5473) [G]).
//   wavetables[N-1]: groups of components (formats/vital_table.cpp has the details).
//   sample {name, sample_rate, length, samples: base64 int16 LE, samples_stereo: the right channel or ""},
//   sample_on, _level 0..1, _transpose st, _tune st, _keytrack 0/1, _loop 0/1, _pan -1..1.
// 2.4.13: an oscillator plays its own table ("vital:<osc>:{path}"), an LFO its own shape ("vlfo:<n>:{path}"): the SYNTH loads
// them from this file again (plugin.cpp), so a project keeps them.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include "../core/parse.hpp"
#include "format.hpp"
#include "synth_patch.hpp"
#include "vital_table.hpp"

namespace omni {
namespace {

constexpr size_t MAX_VITAL = size_t(64) << 20;   // the biggest measured: 8.3 MB (a long stereo recording)
constexpr int64_t MAX_SAMPLE_FRAMES = int64_t(48000) * 60;

double num(const Json &s, const std::string &k, double def) {
    const Json &v = s[k];
    if (v.type != Json::Number) return def;
    return std::isfinite(v.num) ? v.num : def;
}

// one of Vital's two filters as Omni's: 0 when off. Vital's resonance near 1 is tamed in Vital; on Omni it rings far
// louder (the sampler's 1 = +40 dB), so the synth's is scaled to 0.85 and a zone's to 0.5 by the callers
struct VFilter { int n = 0, type = SynthPatch::LP24; double cutoff = 20000, res = 0, drive = 0, keytrack = 0; };
VFilter filter_of(const Json &s, int n) {
    VFilter f;
    if (n < 1 || n > 2) return f;
    const std::string fk = "filter_" + std::to_string(n) + "_";
    if (num(s, fk + "on", 0) < 0.5 || num(s, fk + "mix", 1) <= 0.2) return f;
    f.n = n;
    const int model = int(num(s, fk + "model", 0)), style = int(num(s, fk + "style", 0));
    const double blend = num(s, fk + "blend", 0);
    const bool lp = blend < 0.5, bp = blend >= 0.5 && blend <= 1.5;
    if (model == 6) f.type = SynthPatch::COMB;
    else if (model == 7) f.type = SynthPatch::NOTCH;
    else if (model == 5) f.type = SynthPatch::BP;
    else if (bp) f.type = style >= 2 ? SynthPatch::NOTCH : SynthPatch::BP;
    else if (!lp) f.type = SynthPatch::HP;
    else if (model == 2 || model == 4) f.type = SynthPatch::LADDER;
    else f.type = style == 0 ? SynthPatch::LP12 : SynthPatch::LP24;
    f.cutoff = 440.0 * std::exp2((clampd(num(s, fk + "cutoff", 60), 8, 136) - 69) / 12.0);
    f.res = clampd(num(s, fk + "resonance", 0), 0, 1);
    f.drive = clampd(num(s, fk + "drive", 0) / 20.0 * 0.5 + (model == 1 ? 0.2 : 0), 0, 1);
    f.keytrack = clampd(num(s, fk + "keytrack", 0), 0, 1);
    return f;
}
// an oscillator's / the sample's destination: 0 filter 1, 1 filter 2, 2 both (filter 1 here), 3+ past the filters [G]
int filter_for_dest(int dest) { return dest == 0 || dest == 2 ? 1 : dest == 1 ? 2 : 0; }

double env_s(double v) { v = std::max(0.0, v); return v * v * v * v; }

SynthPatch::Env env_of(const Json &s, int n) {
    const std::string k = "env_" + std::to_string(n) + "_";
    SynthPatch::Env e;
    // the attack's curve (attack_power: Vital's (e^(p t) - 1) / (e^p - 1), negative = rises fast): the time to 70%, as a
    // linear attack would reach it; the hold (full level before the decay) lengthens the decay (Omni has no hold stage)
    const double ap = clampd(num(s, k + "attack_power", 0), -20, 20);
    const double t70 = std::fabs(ap) < 0.01 ? 0.7 : std::log(1 + 0.7 * (std::exp(ap) - 1)) / ap;
    e.attack = env_s(num(s, k + "attack", 0.1495)) * clampd(t70 / 0.7, 0.02, 1.43);
    e.decay = env_s(num(s, k + "decay", 1)) + env_s(num(s, k + "hold", 0));
    e.sustain = clampd(num(s, k + "sustain", 1), 0, 1);
    e.release = env_s(num(s, k + "release", 0.5476));
    return e;
}

Envelope zone_env(const SynthPatch::Env &e) {
    Envelope z;
    z.set = true;
    z.attack = e.attack; z.decay = e.decay; z.sustain = e.sustain; z.release = e.release;
    return z;
}

int lfo_shape(const std::string &name) {
    std::string n;
    for (char c : name) n += char(std::tolower(static_cast<unsigned char>(c)));
    if (n.find("sin") != std::string::npos) return SynthPatch::L_SINE;
    if (n.find("saw") != std::string::npos || n.find("ramp") != std::string::npos) return SynthPatch::L_SAW;
    if (n.find("square") != std::string::npos || n.find("gate") != std::string::npos) return SynthPatch::L_SQUARE;
    if (n.find("random") != std::string::npos || n.find("s&h") != std::string::npos) return SynthPatch::L_SH;
    return SynthPatch::L_TRI;
}

// Vital tempo index (32/1 .. 1/64) and sync mode -> design.py LFO_SYNC (0 Free, 1 4 Bars, 2 2 Bars, 3 1 Bar, 4 1/2, 5 1/4,
// 6 1/8, 7 1/16, 8 1/32, 9 1/4 T, 10 1/8 T, 11 1/16 T, 12 1/4 ., 13 1/8 ., 14 1/16 .)
int lfo_sync(int tempo, int mode) {
    static const int STRAIGHT[12] = {1, 1, 1, 1, 2, 3, 4, 5, 6, 7, 8, 8};
    const int base = STRAIGHT[clampi(tempo, 0, 11)];
    if (mode == 3) return base == 5 ? 9 : base == 6 ? 10 : base == 7 ? 11 : base;   // triplet
    if (mode == 2) return base == 5 ? 12 : base == 6 ? 13 : base == 7 ? 14 : base;  // dotted
    return base;
}

PcmPtr sample_pcm(const Json &smp) {
    const int rate = int(num(smp, "sample_rate", 44100));
    const int64_t len = int64_t(num(smp, "length", 0));
    if (rate < 4000 || rate > 192000 || len <= 0 || len > MAX_SAMPLE_FRAMES) return nullptr;
    const std::vector<uint8_t> l = vital_base64(smp["samples"].as_str(), size_t(len) * 2);
    const std::vector<uint8_t> r = vital_base64(smp["samples_stereo"].as_str(), size_t(len) * 2);
    const int64_t frames = int64_t(l.size() / 2);
    if (frames < 16) return nullptr;
    const bool stereo = int64_t(r.size() / 2) == frames;
    auto p = std::make_shared<Pcm>();
    p->rate = rate;
    p->channels = stereo ? 2 : 1;
    p->data.resize(size_t(frames) * size_t(p->channels));
    for (int64_t i = 0; i < frames; i++) {
        p->data[size_t(i) * size_t(p->channels)] = int16_t(uint16_t(l[size_t(i) * 2] | l[size_t(i) * 2 + 1] << 8));
        if (stereo) p->data[size_t(i) * 2 + 1] = int16_t(uint16_t(r[size_t(i) * 2] | r[size_t(i) * 2 + 1] << 8));
    }
    return p;
}

bool is_noise_name(const std::string &n, int &color) {
    if (n == "White Noise") { color = 0; return true; }
    if (n == "Pink Noise" || n == "Brown Noise") { color = 1; return true; }
    return false;
}

// 2.4.14: a modulation's amount with what other modulations add to it ("velocity -> modulation_1_amount": Vital Pro
// "Harpsichord" opens its oscillator only that way, amount 0 itself), each at its source's typical value: velocity 0.8
// (a firm hit), a macro at its knob, anything else halfway
double source_typical(const Json &s, const std::string &src) {
    if (src == "velocity") return 0.8;
    if (!src.compare(0, 14, "macro_control_")) return clampd(num(s, src, 0), 0, 1);
    return 0.5;
}
double eff_amount(const Json &s, size_t i) {
    const std::string mk = "modulation_" + std::to_string(i + 1) + "_";
    double a = num(s, mk + "amount", 0);
    const Json &mods = s["modulations"];
    if (mods.type != Json::Array) return a;
    const std::string target = mk + "amount";
    for (size_t j = 0; j < mods.arr.size(); j++) {
        if (j == i || mods.arr[j]["destination"].as_str() != target) continue;
        const std::string src = mods.arr[j]["source"].as_str();
        if (src.empty() || num(s, "modulation_" + std::to_string(j + 1) + "_bypass", 0) > 0.5) continue;
        a += num(s, "modulation_" + std::to_string(j + 1) + "_amount", 0) * source_typical(s, src);
    }
    return clampd(a, -1, 1);
}
// an LFO in one of Vital's envelope modes (sync_type 2 envelope, 3 sustain envelope): it rises once, like an envelope
bool lfo_is_envelope(const Json &s, const std::string &src) {
    if (src.compare(0, 4, "lfo_")) return false;
    const int t = int(num(s, src + "_sync_type", 0));
    return t == 2 || t == 3;
}

// a level (osc_N_level, sample_level) with what its modulations add: presets often set it to 0 and let velocity, an envelope
// or an LFO open it. `env` gets the envelope (2..6) that opens a silent level: that envelope is then the sound's shape.
double level_with_mods(const Json &s, const std::string &key, double base, int &env) {
    const Json &mods = s["modulations"];
    double add = 0;
    env = 0;
    if (mods.type != Json::Array) return base;
    for (size_t i = 0; i < mods.arr.size(); i++) {
        if (mods.arr[i]["destination"].as_str() != key) continue;
        const std::string mk = "modulation_" + std::to_string(i + 1) + "_", src = mods.arr[i]["source"].as_str();
        if (src.empty() || num(s, mk + "bypass", 0) > 0.5) continue;
        double a = eff_amount(s, i);   // 2.4.14: with what modulates the amount itself
        if (a <= 0) continue;
        // swings around the level (route below); an LFO in an envelope mode opens it fully, once
        if ((!src.compare(0, 4, "lfo_") && !lfo_is_envelope(s, src)) || !src.compare(0, 6, "random")) a *= 0.5;
        add = std::max(add, a);
        if (base < 0.01 && !env && !src.compare(0, 4, "env_") && std::atoi(src.c_str() + 4) >= 2) env = std::atoi(src.c_str() + 4);
    }
    return clampd(base + add, 0, 1);
}


// 2.4.14: Vital's effects on Omni's output effects. Vital Pro presets lean on them: of 794, the compressor is on in 647
// (a multiband upward / downward one whose band gains sit at +12..+16 dB: without it the translated presets played
// 12 to 16 dB quieter than in Vital), reverb 588, distortion 492, chorus 442, EQ 433, delay 343, phaser 139, flanger 107.
// Every key is named (an effect that is off as 0), so a Vital preset brings its whole effects section; plugin.cpp
// fx_follow() puts back the ones a later preset without effects does not name. Units: Omni's params.json; Vital's from
// the 794 presets (min / median / max per field, 2026-11-17).
// tempo-synced rates at 120 BPM (Omni's chorus / phaser / flanger run free): tempo index 5 = 1/1 .. 9 = 1/16
double vital_rate_hz(const Json &s, const std::string &k) {
    const int sync = int(num(s, k + "sync", 0));
    if (sync >= 1 && sync <= 3) {
        double hz = std::exp2(clampd(num(s, k + "tempo", 7), 0, 12) - 6.0);
        if (sync == 2) hz /= 1.5;          // dotted
        else if (sync == 3) hz *= 1.5;     // triplet
        return hz;
    }
    return std::exp2(clampd(num(s, k + "frequency", 0), -10, 10));
}
// Omni's 0..100 rate knob (chorus_hz: 0.05 Hz x 100^(v / 100)) for a rate
double omni_rate(double hz) { return clampd(100.0 * std::log(std::max(1e-3, hz) / 0.05) / std::log(100.0), 0, 100); }
// the Glue compressor's make-up for an amount (dsp.hpp BusComp: threshold -10 - 20a dB, ratio 2 + 2a, half the reduction
// at the threshold), inverted by bisection
double glue_makeup_db(double a) { const double thr = 10 + 20 * a, r = 2 + 2 * a; return thr * (1 - 1 / r) * 0.5; }
double glue_amount_for(double db) {
    double lo = 0, hi = 1;
    for (int i = 0; i < 30; i++) { const double m = (lo + hi) / 2; (glue_makeup_db(m) < db ? lo : hi) = m; }
    return (lo + hi) / 2;
}
// a tempo / time (Vital delay; its rate is the repeats per second) -> design.py DLY_TIME (in beats): 1/32, 1/16 T, 1/16, 1/16 ., 1/8 T, 1/8, 1/8 ., 1/4 T, 1/4, 1/4 ., 1/2, 1 Bar
int omni_delay_time(const Json &s) {
    const double beats = (1.0 / std::max(1e-3, vital_rate_hz(s, "delay_"))) / 0.5;   // its time in beats at 120 BPM (0.5 s each)
    static const double LEN[12] = {0.125, 1.0 / 6, 0.25, 0.375, 1.0 / 3, 0.5, 0.75, 2.0 / 3, 1, 1.5, 2, 4};
    int best = 5;
    double err = 1e9;
    for (int i = 0; i < 12; i++) { const double e = std::fabs(std::log(LEN[i] / beats)); if (e < err) { err = e; best = i; } }
    return best;
}

void vital_fx(const Json &s, std::vector<std::pair<std::string, float>> &out, std::vector<std::string> &notes) {
    auto put = [&](const char *k, double v) { out.push_back({k, float(v)}); };
    auto on = [&](const char *fx) { return num(s, std::string(fx) + "_on", 0) > 0.5; };
    // the compressor: Glue, its amount giving Vital's make-up (the enabled bands' output gains, by the mix)
    double comp = 0;
    if (on("compressor")) {
        const int bands = int(num(s, "compressor_enabled_bands", 0));   // 0 all three, 1 low + band, 2 band + high, 3 single
        const double lo = num(s, "compressor_low_gain", 16.3), mid = num(s, "compressor_band_gain", 11.7), hi = num(s, "compressor_high_gain", 16.3);
        double g = bands == 3 ? mid : bands == 1 ? (lo + mid) / 2 : bands == 2 ? (mid + hi) / 2 : (lo + mid + hi) / 3;
        g *= clampd(num(s, "compressor_mix", 1), 0, 1);
        if (g > 1) comp = glue_amount_for(std::min(g, glue_makeup_db(1)));
        if (g > 1 && comp < 0.02) comp = 0.02;
    }
    put("comp", comp * 100); put("comp_type", comp > 0 ? 2 : 0);
    // distortion: soft / hard clip on the drive (Classic), the folds on Fold, bit crush / down-sample on LO-FI
    double drive = 0, lofi = 0;
    int dtype = 0;
    if (on("distortion")) {
        const int t = int(num(s, "distortion_type", 0));
        const double mix = clampd(num(s, "distortion_mix", 1), 0, 1), db = clampd(num(s, "distortion_drive", 0), -30, 30);
        if (t <= 3) {
            drive = clampd((std::pow(10.0, db / 20) - 1) / 8, 0, 1) * mix;   // Omni: the shaper's input x (1 + 8 amount)
            dtype = drive > 0.005 ? (t >= 2 ? 4 : 1) : 0;
            if (!dtype) drive = 0;
        } else {
            lofi = mix;
            const double x = (db + 30) / 60;                                  // [G] Vital's drive as the crush's depth
            put("lofi_bits", t == 4 ? clampd(16 - 12 * x, 2, 16) : 16);
            put("lofi_rate", t == 5 ? clampd(100 * x, 0, 100) : 0);
        }
    }
    put("drive", drive * 100); put("drive_type", dtype); put("lofi_mix", lofi * 100);
    // reverb: the type whose decay range holds Vital's decay (2^decay_time s), its size giving that decay
    double rmix = 0;
    if (on("reverb")) {
        rmix = clampd(num(s, "reverb_dry_wet", 0.25), 0, 1);
        const double rt = std::exp2(clampd(num(s, "reverb_decay_time", 0), -6, 6));
        int type = 2; double lo = 0.5, hi = 7;                                // Plate 0.5 - 7 s
        if (rt < 0.5) { type = 4; lo = 0.25; hi = 2.5; }                      // Room
        else if (rt > 7) { type = 3; lo = 1.2; hi = 10; }                     // Hall
        put("rev_type", rmix > 0 ? type : 0);
        put("rev_size", clampd(100 * std::log(rt / lo) / std::log(hi / lo), 0, 100));
        put("rev_damp", clampd(20 + 60 * (-num(s, "reverb_high_shelf_gain", -1)) / 6, 0, 100));   // [G]
        put("rev_predelay", clampd(num(s, "reverb_delay", 0) * 1000, 0, 250));
        put("rev_mode", 0);
    } else put("rev_type", 0);
    put("rev_mix", rmix * 100);
    // chorus
    double cmix = 0;
    if (on("chorus")) {
        cmix = clampd(num(s, "chorus_dry_wet", 0.5), 0, 1);
        put("cho_rate", omni_rate(vital_rate_hz(s, "chorus_")));
        put("cho_depth", clampd(num(s, "chorus_mod_depth", 0.5) * 100, 0, 100));
    }
    put("cho_mix", cmix * 100); put("cho_type", cmix > 0 ? 1 : 0);
    // delay
    double dmix = 0;
    if (on("delay")) {
        dmix = clampd(num(s, "delay_dry_wet", 0.3), 0, 1);
        put("dly_time", omni_delay_time(s));
        put("dly_fb", clampd(std::fabs(num(s, "delay_feedback", 0.5)) * 95, 0, 95));
        put("dly_tone", clampd((num(s, "delay_filter_cutoff", 60) - 8) / 128 * 100, 0, 100));   // [G]
        put("dly_ping", num(s, "delay_style", 1) >= 2 ? 1 : 0);
    }
    put("dly_mix", dmix * 100); put("dly_type", dmix > 0 ? 1 : 0);
    // EQ: the shelves' and the band's gains (a low / high band in its cut mode, or a notch band: left flat)
    double el = 0, em = 0, eh = 0;
    if (on("eq")) {
        if (num(s, "eq_low_mode", 0) < 0.5) el = num(s, "eq_low_gain", 0);
        if (num(s, "eq_band_mode", 0) < 0.5) em = num(s, "eq_band_gain", 0);
        if (num(s, "eq_high_mode", 0) < 0.5) eh = num(s, "eq_high_gain", 0);
    }
    put("eq_low", clampd(el, -12, 12)); put("eq_mid", clampd(em, -12, 12)); put("eq_high", clampd(eh, -12, 12));
    // phaser and flanger
    double pmix = 0, fmix = 0;
    if (on("phaser")) {
        pmix = clampd(num(s, "phaser_dry_wet", 1), 0, 1);
        put("phs_rate", omni_rate(vital_rate_hz(s, "phaser_")));
        put("phs_depth", clampd(num(s, "phaser_mod_depth", 24) / 48 * 100, 0, 100));
        put("phs_fb", clampd(num(s, "phaser_feedback", 0.5) * 90, 0, 90));
    }
    put("phs_mix", pmix * 100);
    if (on("flanger")) {
        fmix = clampd(num(s, "flanger_dry_wet", 0.5), 0, 1);
        put("flg_rate", omni_rate(vital_rate_hz(s, "flanger_")));
        put("flg_depth", clampd(num(s, "flanger_mod_depth", 0.5) * 100, 0, 100));
        put("flg_fb", clampd(std::fabs(num(s, "flanger_feedback", 0.5)) * 90, 0, 90));
    }
    put("flg_mix", fmix * 100);
    if (on("filter_fx")) notes.push_back("Vital: the effects filter is not played");
    // Vital's compressor holds the peaks down (its upper ratio); on Omni the compressor's make-up and the drive (whose
    // output stands near full scale) reached 0 dBFS in 7 of 40 Vital Pro presets: the output limiter (ceiling -1 dBFS,
    // look-ahead) keeps them clean. Only ever switched on here (a user's own limiter is never switched off)
    if (comp > 0 || drive > 0.05) put("limiter", 1);
}

Instrument load_vital(VolumePtr vol, const std::string &path, int) {
    const std::vector<uint8_t> raw = vol->open(path)->all(MAX_VITAL + 1);
    if (raw.size() > MAX_VITAL) throw ParseError("Vital: preset too big");
    const Json root = parse_json(std::string(raw.begin(), raw.end()));
    const Json &s = root["settings"];
    if (s.type != Json::Object) throw ParseError("Vital: no settings in this file");

    Instrument inst;
    inst.format = "Vital preset";
    inst.name = root["preset_name"].as_str();
    if (inst.name.empty()) inst.name = path_stem(path);
    inst.category = root["preset_style"].as_str();

    SynthPatch p;
    p.source = "Vital";
    std::vector<std::string> table_notes;   // 2.4.13: what the wavetables have that Omni does not read
    int unread_tables = 0;
    int dropped_osc = 0;
    bool cross_mod_dropped = false;   // 2.4.13
    // the two loudest oscillators that play (in Vital's order)
    std::vector<int> oscs;
    int main_osc = 0;   // the loudest: its destination picks the synth's filter
    {
        std::vector<std::pair<double, int>> on;
        for (int n = 1; n <= 3; n++) {
            const std::string k = "osc_" + std::to_string(n) + "_";
            int env = 0;
            const double lv = level_with_mods(s, k + "level", num(s, k + "level", 0.7071), env);
            if (num(s, k + "on", n == 1) > 0.5 && lv > 0.001) on.push_back({lv, n});
        }
        std::stable_sort(on.begin(), on.end(), [](auto &a, auto &b) { return a.first > b.first; });
        if (!on.empty()) main_osc = on[0].second;
        for (size_t i = 0; i < on.size() && i < 2; i++) oscs.push_back(on[i].second);
        dropped_osc = int(on.size()) - int(oscs.size());
        std::sort(oscs.begin(), oscs.end());
    }
    std::map<int, int> slot_of;   // Vital osc -> Omni osc A/B
    int shape_env = 0;            // an envelope that opens a silent oscillator: the amp envelope's shape
    for (size_t i = 0; i < oscs.size(); i++) {
        const int n = oscs[i];
        slot_of[n] = int(i);
        const std::string k = "osc_" + std::to_string(n) + "_";
        SynthPatch::Osc &o = p.osc[i];
        o.on = true;
        int lv_env = 0;
        o.level = level_with_mods(s, k + "level", num(s, k + "level", 0.7071), lv_env);
        if (lv_env && !shape_env) shape_env = lv_env;
        o.pan = clampd(num(s, k + "pan", 0), -1, 1);
        const int tr = int(std::lround(num(s, k + "transpose", 0)));
        o.octave = tr / 12;
        o.semi = tr % 12;
        o.fine = num(s, k + "tune", 0) * 100;
        o.unison = clampi(int(num(s, k + "unison_voices", 1)), 1, 8);
        const double d = clampd(num(s, k + "unison_detune", 4.472), 0, 10) / 10.0;
        o.detune = clampd(num(s, k + "detune_range", 2) * d * d, 0, 1);
        o.spread = clampd(num(s, k + "stereo_spread", 1), 0, 1);
        // 2.4.13: the preset's own wavetable, played as it is (formats/vital_table.cpp; the oscillator loads it from the
        // file again: "{path}" = this preset, filled in by the loader); its position = Vital's wave frame
        const double frame = clampd(num(s, k + "wave_frame", 0), 0, 256);
        const Json &tables = s["wavetables"];
        {
            std::vector<float> one;
            int frames = 0;
            if (tables.type == Json::Array && size_t(n - 1) < tables.arr.size() &&
                vital_wavetable_render(tables.arr[size_t(n - 1)], 1, one, frames, &table_notes)) {
                o.wavetable = true;
                o.file = "vital:" + std::to_string(n) + ":{path}";
                o.position = frame / 256.0;
            } else {
                o.wave = SynthPatch::SAW;
                unread_tables++;
            }
        }
        const int dt = int(num(s, k + "distortion_type", 0));
        const double da = clampd(num(s, k + "distortion_amount", 0.5), 0, 1);
        switch (dt) {
        case 1: o.warp_mode = SynthPatch::W_SYNC; o.warp = da; break;
        case 4: case 5: o.warp_mode = SynthPatch::W_BEND; o.warp = da; break;
        case 6: o.warp_mode = SynthPatch::W_PWM; o.warp = da; break;
        // 2.4.13: FM / ring from another oscillator only when Omni plays two (Omni's FM and RING are A with B): with B off,
        // RING multiplied A by silence (Vital Pro "Ceramic" played nothing); then A plays plain
        case 7: case 8: if (oscs.size() > 1) p.fm = std::max(p.fm, da * 0.5); else cross_mod_dropped = true; break;
        case 10: case 11: if (oscs.size() > 1) p.ring = std::max(p.ring, da); else cross_mod_dropped = true; break;
        default: break;
        }
    }
    // Vital's SAMPLE oscillator: its noises on Omni's noise generator, a recording on the sampler
    const Json &smp = s["sample"];
    int sample_env = 0;
    const double sample_lv = level_with_mods(s, "sample_level", num(s, "sample_level", 0.7071), sample_env);
    const bool sample_on = num(s, "sample_on", 0) > 0.5 && sample_lv > 0.001;
    int noise_color = 0;
    bool sample_zone = false;
    if (sample_on && is_noise_name(smp["name"].as_str(), noise_color)) {
        p.noise_on = true;
        p.noise_color = noise_color;
        p.noise_level = sample_lv;
    } else if (sample_on) sample_zone = true;

    // the synth's filter: the one the loudest oscillator goes through (osc_N_destination)
    const VFilter vf = main_osc ? filter_of(s, filter_for_dest(int(num(s, "osc_" + std::to_string(main_osc) + "_destination", 0)))) : VFilter{};
    int fn = vf.n;
    const std::string fk = "filter_" + std::to_string(fn) + "_";
    if (fn) {
        p.filter_on = true;
        p.filter_type = vf.type;
        p.cutoff_hz = vf.cutoff;
        p.resonance = vf.res * 0.85;
        p.drive = vf.drive;
        p.keytrack = vf.keytrack;
    }

    p.amp = env_of(s, 1);
    // env 1 only gates (full sustain) while another envelope opens the oscillator: that one shapes the sound
    if (shape_env && p.amp.sustain > 0.99) {
        const double rel = p.amp.release;
        p.amp = env_of(s, shape_env);
        p.amp.release = std::min(p.amp.release, std::max(rel, 0.005));
    }
    p.amp_vel = clampd(num(s, "velocity_track", 0), 0, 1);
    // LFOs: Vital's first four that a modulation uses -> Omni LFO 1..4 (2.4.17: was two)
    const Json &mods = s["modulations"];
    std::vector<int> lfo_used;
    std::vector<int> env_used;
    if (mods.type == Json::Array)
        for (size_t i = 0; i < mods.arr.size(); i++) {
            const std::string src = mods.arr[i]["source"].as_str();
            if (src.empty() || num(s, "modulation_" + std::to_string(i + 1) + "_bypass", 0) > 0.5) continue;
            if (!src.compare(0, 4, "lfo_")) { const int n = std::atoi(src.c_str() + 4); if (std::find(lfo_used.begin(), lfo_used.end(), n) == lfo_used.end()) lfo_used.push_back(n); }
            if (!src.compare(0, 4, "env_")) { const int n = std::atoi(src.c_str() + 4); if (n >= 2 && std::find(env_used.begin(), env_used.end(), n) == env_used.end()) env_used.push_back(n); }
        }
    std::map<int, int> lfo_slot;
    for (size_t i = 0; i < lfo_used.size() && i < 4; i++) lfo_slot[lfo_used[i]] = int(i);
    static constexpr int LFO_SRC[4] = {SynthPatch::S_LFO1, SynthPatch::S_LFO2, SynthPatch::S_LFO3, SynthPatch::S_LFO4};
    auto is_lfo = [](int S) { return S == SynthPatch::S_LFO1 || S == SynthPatch::S_LFO2 || S == SynthPatch::S_LFO3 || S == SynthPatch::S_LFO4; };
    for (const auto &[n, slot] : lfo_slot) {
        const std::string k = "lfo_" + std::to_string(n) + "_";
        SynthPatch::Lfo &l = p.lfo[slot];
        const Json &lfos = s["lfos"];
        l.shape = lfos.type == Json::Array && size_t(n - 1) < lfos.arr.size() ? lfo_shape(lfos.arr[size_t(n - 1)]["name"].as_str()) : SynthPatch::L_TRI;
        VitalLine line;   // 2.4.13: the drawn shape itself (the SYNTH's CUSTOM shape, loaded from this file again)
        if (lfos.type == Json::Array && size_t(n - 1) < lfos.arr.size() && vital_line_parse(lfos.arr[size_t(n - 1)], line)) {
            l.shape = SynthPatch::L_CUSTOM;
            l.ref = "vlfo:" + std::to_string(n) + ":{path}";
        }
        l.hz = std::exp2(clampd(num(s, k + "frequency", 1), -7, 9));
        const int sync = int(num(s, k + "sync", 1));
        l.sync = sync >= 1 && sync <= 3 ? lfo_sync(int(num(s, k + "tempo", 7)), sync) : 0;
        l.retrig = int(num(s, k + "sync_type", 0)) != 1;   // 1 = runs free with the song [G]; trigger / envelope modes restart
    }
    // envelopes 2..6: the first that moves the filter's cutoff is the filter envelope; the next three MOD ENV 1..3 (2.4.17:
    // was one mod envelope)
    int flt_env = 0, mod_env = 0, x_env[2] = {0, 0};
    const std::string cut_dest = fn ? fk + "cutoff" : "";
    if (mods.type == Json::Array && fn)
        for (size_t i = 0; i < mods.arr.size() && !flt_env; i++) {
            const std::string src = mods.arr[i]["source"].as_str();
            if (!src.compare(0, 4, "env_") && std::atoi(src.c_str() + 4) >= 2 && mods.arr[i]["destination"].as_str() == cut_dest &&
                num(s, "modulation_" + std::to_string(i + 1) + "_bypass", 0) < 0.5) {
                flt_env = std::atoi(src.c_str() + 4);
                p.flt = env_of(s, flt_env);
                p.env_amount = clampd(eff_amount(s, i) * 128.0 / 72.0, -1, 1);
            }
        }
    {
        int k = 0;
        for (int n : env_used) {
            if (n == flt_env) continue;
            if (k == 0) mod_env = n; else if (k <= 2) x_env[k - 1] = n; else break;
            k++;
        }
    }
    if (mod_env) p.mod = env_of(s, mod_env);
    for (int k = 0; k < 2; k++) if (x_env[k]) p.xmod[k].env = env_of(s, x_env[k]);

    // the routes
    int unread_mods = 0;
    double cut_shift_st = 0;   // 2.4.14: unipolar LFO sweeps on the cutoff move the base up by half their range
    if (mods.type == Json::Array)
        for (size_t i = 0; i < mods.arr.size(); i++) {
            const std::string src = mods.arr[i]["source"].as_str(), dst = mods.arr[i]["destination"].as_str();
            const std::string mk = "modulation_" + std::to_string(i + 1) + "_";
            if (src.empty() || dst.empty() || num(s, mk + "bypass", 0) > 0.5) continue;
            const double amt = eff_amount(s, i);   // 2.4.14: with what modulates the amount itself
            if (amt == 0) continue;
            int S = SynthPatch::S_OFF;
            if (!src.compare(0, 4, "lfo_")) { auto it = lfo_slot.find(std::atoi(src.c_str() + 4)); if (it != lfo_slot.end()) S = LFO_SRC[it->second]; }
            else if (src == "env_1") S = SynthPatch::S_AMPENV;
            else if (!src.compare(0, 4, "env_")) { const int n = std::atoi(src.c_str() + 4); S = n == flt_env ? SynthPatch::S_FLTENV : n == mod_env ? SynthPatch::S_MODENV
                                                                                                                    : n == x_env[0] ? SynthPatch::S_MODENV2 : n == x_env[1] ? SynthPatch::S_MODENV3 : SynthPatch::S_OFF; }
            else if (src == "velocity") S = SynthPatch::S_VEL;
            else if (src == "note") S = SynthPatch::S_NOTE;
            else if (src == "mod_wheel") S = SynthPatch::S_WHEEL;
            else if (src == "aftertouch") S = SynthPatch::S_AT;
            else if (!src.compare(0, 14, "macro_control_")) S = SynthPatch::S_MACRO1 + clampi(std::atoi(src.c_str() + 14) - 1, 0, 3);
            else if (!src.compare(0, 6, "random")) S = SynthPatch::S_RANDOM;
            int D = SynthPatch::D_OFF;
            double a = amt;
            int on = -1;   // the Omni osc a per-oscillator destination belongs to
            if (!dst.compare(0, 4, "osc_")) { auto it = slot_of.find(std::atoi(dst.c_str() + 4)); if (it != slot_of.end()) on = it->second; }
            const std::string field = dst.size() > 6 && !dst.compare(0, 4, "osc_") ? dst.substr(6) : "";
            if (fn && dst == cut_dest) {
                if (S == SynthPatch::S_FLTENV) continue;   // the filter envelope's own amount (above)
                D = SynthPatch::D_CUTOFF; a = amt * 128.0 / 48.0;
                // 2.4.14: Vital's LFO / random sources are unipolar unless the modulation is bipolar: the cutoff sweeps from its
                // value up by amt x 128 st. Omni's LFOs swing both ways: the base goes up half the sweep, the depth is half
                // (Vital Pro "Lost Memories": cutoff 12 Hz opened by an LFO up to 1.7 kHz played nothing)
                const bool swinging = is_lfo(S) || S == SynthPatch::S_RANDOM;
                if (swinging && num(s, mk + "bipolar", 0) < 0.5) {
                    cut_shift_st += amt * 64.0;
                    a = amt * 64.0 / 48.0;
                }
            } else if (fn && dst == fk + "resonance") { D = SynthPatch::D_RES; a = amt * 2; }
            else if (dst == "volume") D = SynthPatch::D_LEVEL;
            else if (on >= 0 && field == "level") D = SynthPatch::D_LEVEL;
            else if (on >= 0 && field == "transpose") { D = SynthPatch::D_PITCH; a = amt * 96.0 / 12.0; }
            else if (on >= 0 && field == "tune") { D = SynthPatch::D_PITCH; a = amt * 2.0 / 12.0; }
            else if (on >= 0 && field == "wave_frame") D = on ? SynthPatch::D_POS_B : SynthPatch::D_POS_A;
            else if (on >= 0 && field == "pan") { D = SynthPatch::D_PAN; a = amt * 2; }
            else if (on >= 0 && field == "distortion_amount" && p.osc[on].warp_mode != SynthPatch::W_OFF) D = on ? SynthPatch::D_WARP_B : SynthPatch::D_WARP_A;
            if (D == SynthPatch::D_LEVEL && field == "level" && !is_lfo(S) && S != SynthPatch::S_VEL) continue;   // in the level (above)
            if (D == SynthPatch::D_LEVEL && field == "level" && is_lfo(S)) {
                // Omni's LEVEL target is the whole synth's: one route per LFO, only when it moves EVERY playing oscillator
                // (else the swing stays in that oscillator's level, above), at the smallest depth (stacked routes = 2x level)
                int moved = 0;
                double depth = 1;
                for (size_t j = 0; j < mods.arr.size(); j++) {
                    const std::string d2 = mods.arr[j]["destination"].as_str(), s2 = mods.arr[j]["source"].as_str();
                    if (s2 != src || d2.size() < 7 || d2.compare(0, 4, "osc_") || d2.substr(6) != "level") continue;
                    auto it = slot_of.find(std::atoi(d2.c_str() + 4));
                    if (it == slot_of.end() || num(s, "modulation_" + std::to_string(j + 1) + "_bypass", 0) > 0.5) continue;
                    const double lv = p.osc[it->second].level;
                    moved++;
                    depth = std::min(depth, lv > 0.001 ? eff_amount(s, j) * 0.5 / lv : 0);
                }
                if (moved < int(slot_of.size()) || on != slot_of.begin()->second) continue;   // once, from the first oscillator
                a = clampd(depth, 0, 1);
            }
            if (S == SynthPatch::S_VEL && D == SynthPatch::D_LEVEL) { p.amp_vel = clampd(std::max(p.amp_vel, std::fabs(amt)), 0, 1); continue; }
            if (S == SynthPatch::S_MODENV && D != SynthPatch::D_OFF && !p.mod_env_on) {   // the mod envelope's first target: its own
                p.mod_env_on = true; p.mod_env_dest = D; p.mod_env_amount = clampd(a, -1, 1);
                continue;
            }
            if ((S == SynthPatch::S_MODENV2 || S == SynthPatch::S_MODENV3) && D != SynthPatch::D_OFF) {   // 2.4.17: MOD ENV 2 / 3 alike
                SynthPatch::ModEnv &m = p.xmod[S == SynthPatch::S_MODENV3];
                if (!m.on) { m.on = true; m.dest = D; m.amount = clampd(a, -1, 1); continue; }
            }
            if (S == SynthPatch::S_OFF || D == SynthPatch::D_OFF) { unread_mods++; continue; }
            if (p.routes.size() >= size_t(SynthPatch::MAX_ROUTES)) { unread_mods++; continue; }   // 2.4.17: 16 (was 8)
            p.routes.push_back({S, D, clampd(a, -1, 1)});
        }

    if (cut_shift_st != 0) p.cutoff_hz = clampd(p.cutoff_hz * std::exp2(clampd(cut_shift_st, -96, 96) / 12.0), 20, 20000);   // 2.4.14

    const int poly = int(num(s, "polyphony", 8));
    p.polyphony = clampi(poly, 1, 12);
    p.voice_mode = poly <= 1 ? (num(s, "legato", 0) > 0.5 ? 2 : 1) : 0;
    const double port = num(s, "portamento_time", -10);
    p.glide = port > -9.9 ? std::min(2.0, std::exp2(port)) : 0;
    p.glide_legato = num(s, "portamento_force", 0) < 0.5;
    p.bend = clampi(int(std::lround(num(s, "pitch_bend_range", 2))), 0, 24);
    const double vol = num(s, "volume", 5473.0404);
    p.level_db = vol > 1 ? clampd(40.0 * std::log10(vol / 5473.0404), -24, 0) : -24;   // (Vital's +dB has headroom Omni lacks)
    // headroom for stacked voices: Omni's unison is louder than Vital's (gain / sqrt n), so 1.5 dB per doubling of the
    // widest unison, and 2 dB when both oscillators play (measured: 21 of 265 presets reached full scale on one note)
    {
        int uni = 1;
        for (size_t i = 0; i < oscs.size(); i++) uni = std::max(uni, p.osc[i].unison);
        p.level_db -= 1.5 * std::log2(double(uni)) + (oscs.size() > 1 ? 2.0 : 0.0);
        p.level_db = std::max(-24.0, p.level_db);
    }

    // the recording (Vital's SAMPLE oscillator) on the sampler, with the amp envelope and the filter
    if (sample_zone) {
        PcmPtr pcm = sample_pcm(smp);
        if (pcm) {
            auto ref = std::make_shared<SampleRef>();
            ref->key = path + "#vital-sample";
            ref->name = smp["name"].as_str();
            ref->rate = pcm->rate;
            ref->channels = pcm->channels;
            ref->frames = pcm->frames();
            ref->decode = [pcm] { return pcm; };
            Zone z;
            z.name = ref->name.empty() ? "Sample" : ref->name;
            z.sample = ref;
            z.root = 60;
            z.key_tracking = num(s, "sample_keytrack", 0) > 0.5 ? 1 : 0;
            z.tune = num(s, "sample_transpose", 0) + num(s, "sample_tune", 0);
            z.gain_db = 20 * std::log10(sample_lv) + p.level_db;
            z.pan = clampd(num(s, "sample_pan", 0), -1, 1);
            if (num(s, "sample_loop", 1) > 0.5) {
                Loop l;
                l.start = 0;
                l.end = pcm->frames() - 1;
                l.crossfade = std::min<int64_t>(pcm->frames() / 8, pcm->rate / 20);
                z.loops.push_back(l);
            }
            z.amp_env = zone_env(p.amp);
            if (sample_env && env_of(s, 1).sustain > 0.99) z.amp_env = zone_env(env_of(s, sample_env));
            z.amp_vel_depth = std::max(0.6, p.amp_vel);   // 2.4.12 (user): at least 60 % velocity response
            z.bend_up = p.bend * 100; z.bend_down = -p.bend * 100;
            const VFilter zf = filter_of(s, filter_for_dest(int(num(s, "sample_destination", 0))));   // the sample's own filter
            if (zf.n && zf.type != SynthPatch::COMB) {
                Filter &f = z.filter;
                f.type = zf.type == SynthPatch::HP ? FilterType::HighPass : zf.type == SynthPatch::BP ? FilterType::BandPass
                       : zf.type == SynthPatch::NOTCH ? FilterType::BandReject : FilterType::LowPass;
                f.poles = zf.type == SynthPatch::LP12 ? 2 : 4;
                f.cutoff = zf.cutoff;
                f.resonance = zf.res * 0.5;
                f.key_tracking = zf.keytrack;
                if (flt_env && zf.n == fn) { f.env = zone_env(p.flt); f.env_depth = clampd(p.env_amount * 7200.0 / MAX_ENVELOPE_DEPTH, -1, 1); }
            }
            inst.zones.push_back(std::move(z));
        } else inst.warnings.push_back("Vital: the SAMPLE oscillator's recording could not be read");
    }

    const bool synth = !oscs.empty() || p.noise_on;
    if (synth) inst.settings = synth_settings(p);
    std::string note = "Vital: ";
    if (synth) note += "synth played by Omni's SYNTH engine with its effects (approximate; filter 2, spectral morph not read)";
    if (!inst.zones.empty()) note += synth ? "; its recording on the sampler" : "the SAMPLE oscillator's recording on the sampler";
    inst.warnings.insert(inst.warnings.begin(), note);
    if (dropped_osc > 0) inst.warnings.push_back("Vital: " + std::to_string(dropped_osc) + " more oscillator(s) not played (Omni has two)");
    for (const std::string &tn : table_notes) inst.warnings.push_back(tn);   // 2.4.13
    if (cross_mod_dropped) inst.warnings.push_back("Vital: FM / ring modulation from an oscillator Omni does not play: left out");
    if (unread_tables > 0) inst.warnings.push_back("Vital: " + std::to_string(unread_tables) + " wavetable(s) could not be read: played as a saw");
    if (unread_mods > 0) inst.warnings.push_back("Vital: " + std::to_string(unread_mods) + " modulation(s) not translated");
    if (!synth && inst.zones.empty()) throw ParseError("Vital: the preset plays no oscillator");
    {   // 2.4.14: the effects (inst.settings: after the synth's; the loader applies both)
        std::vector<std::string> fx_notes;
        vital_fx(s, inst.settings, fx_notes);
        for (const std::string &n : fx_notes) inst.warnings.push_back(n);
    }
    finish_instrument(inst);
    return inst;
}

std::vector<PresetInfo> list_vital(VolumePtr, const std::string &path) { return {{path_stem(path), 0}}; }

bool probe_vital(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    for (size_t i = 0; i < n; i++) {
        if (h[i] == ' ' || h[i] == '\n' || h[i] == '\r' || h[i] == '\t') continue;
        return h[i] == '{';
    }
    return false;
}

}  // namespace

void register_vital() {
    register_reader({"Vital preset", "vital", probe_vital, list_vital, load_vital});
}

}  // namespace omni

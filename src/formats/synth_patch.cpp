// Omni Sampler 2.4.10: SynthPatch (real units) -> Omni's SYNTH engine settings. The units and ranges are design.py
// params() (and plugin.cpp: syn_cutoff_hz, syn_lfo_hz), checked by tests/test_translate.cpp.
#include "synth_patch.hpp"
#include <algorithm>
#include <cmath>

namespace omni {

namespace {
double clampd(double v, double lo, double hi) { return std::isfinite(v) ? std::max(lo, std::min(hi, v)) : lo; }
int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
}  // namespace

std::vector<std::pair<std::string, float>> synth_settings(const SynthPatch &p) {
    std::vector<std::pair<std::string, float>> s;
    auto put = [&](const std::string &k, double v) { s.emplace_back(k, float(v)); };
    auto ms = [](double sec, double lo) { return clampd(sec * 1000.0, lo, 10000.0); };
    put("syn_on", 1);
    put("syn_level", clampd(p.level_db, -24, 6));
    put("syn_pan", clampd(p.pan * 100, -100, 100));
    put("syn_poly", clampi(p.polyphony, 1, 12));
    put("syn_vmode", clampi(p.voice_mode, 0, 2));
    put("syn_glide", ms(p.glide, 0) > 2000 ? 2000 : ms(p.glide, 0));
    put("syn_glide_mode", p.glide_legato ? 1 : 0);
    put("syn_bend", clampi(p.bend, 0, 24));
    // 2.4.12 (user): every translated synth patch answers velocity at least 60 % (many have none: GENERATE's dynamics,
    // accents and ghost notes were not heard)
    put("syn_vel", clampd(std::max(0.6, p.amp_vel) * 100, 0, 100));
    for (int k = 0; k < 2; k++) {
        const SynthPatch::Osc &o = p.osc[k];
        const std::string x = k ? "sb_" : "sa_";
        put(x + "on", o.on ? 1 : 0);
        put(x + "type", o.sample ? 2 : o.wavetable ? 1 : 0);   // 2.4.33: 2 = SAMPLE
        if ((o.wavetable || o.sample) && !o.file.empty()) s.emplace_back(std::string("@syn_file_") + char('a' + k) + "=" + o.file, 0.0f);   // 2.4.13
        // (a SAMPLE file: an index past the 4 slot entries, so the oscillator plays its file, not a slot's instrument)
        put(x + "table", o.sample ? 4 : o.wavetable ? clampi(o.table, 0, 4095) : clampi(o.wave, 0, 7));
        put(x + "pos", clampd(o.position * 100, 0, 100));
        put(x + "oct", clampi(o.octave, -4, 4));
        put(x + "semi", clampi(o.semi, -12, 12));
        put(x + "fine", clampd(o.fine, -100, 100));
        put(x + "level", clampd(o.level * 100, 0, 100));
        put(x + "pan", clampd(o.pan * 100, -100, 100));
        put(x + "unison", clampi(o.unison, 1, 8));
        put(x + "detune", clampd(o.detune * 100, 0, 100));
        put(x + "spread", clampd(o.spread * 100, 0, 100));
        put(x + "wmode", clampi(o.warp_mode, 0, 5));
        put(x + "warp", clampd(o.warp * 100, 0, 100));
    }
    put("syn_sub_on", p.sub_on ? 1 : 0);
    put("syn_sub_shape", clampi(p.sub_shape, 0, 2));
    put("syn_sub_oct", p.sub_oct >= 2 ? 1 : 0);
    put("syn_sub_level", clampd(p.sub_level * 100, 0, 100));
    put("syn_noise_on", p.noise_on ? 1 : 0);
    put("syn_noise_color", clampi(p.noise_color, 0, 2));
    put("syn_noise_level", clampd(p.noise_level * 100, 0, 100));
    put("syn_noise_tone", clampd(p.noise_tone * 100, 0, 100));
    put("syn_fm", clampd(p.fm * 100, 0, 100));
    put("syn_ring", clampd(p.ring * 100, 0, 100));
    put("syn_feedback", clampd(p.feedback * 100, 0, 100));
    put("syn_balance", clampd(p.balance * 100, -100, 100));
    put("syn_sync", p.sync ? 1 : 0);
    put("syn_flt_on", p.filter_on ? 1 : 0);
    put("syn_flt_type", clampi(p.filter_type, 0, 6));
    // cutoff knob 0..100 = 20 Hz .. 20 kHz exponential (plugin.cpp syn_cutoff_hz)
    put("syn_cutoff", clampd(100.0 * std::log(clampd(p.cutoff_hz, 20, 20000) / 20.0) / std::log(1000.0), 0, 100));
    put("syn_res", clampd(p.resonance * 100, 0, 100));
    put("syn_drive", clampd(p.drive * 100, 0, 100));
    put("syn_keytrack", clampd(p.keytrack * 100, 0, 100));
    put("syn_fenv", clampd(p.env_amount * 100, -100, 100));
    put("syn_fvel", clampd(p.filter_vel * 100, 0, 100));
    put("syn_attack", ms(p.amp.attack, 0)); put("syn_decay", ms(p.amp.decay, 1));
    put("syn_sustain", clampd(p.amp.sustain * 100, 0, 100)); put("syn_release", ms(p.amp.release, 1));
    put("syn_fe_attack", ms(p.flt.attack, 0)); put("syn_fe_decay", ms(p.flt.decay, 1));
    put("syn_fe_sustain", clampd(p.flt.sustain * 100, 0, 100)); put("syn_fe_release", ms(p.flt.release, 1));
    put("syn_me_on", p.mod_env_on ? 1 : 0);
    put("syn_me_attack", ms(p.mod.attack, 0)); put("syn_me_decay", ms(p.mod.decay, 1));
    put("syn_me_sustain", clampd(p.mod.sustain * 100, 0, 100)); put("syn_me_release", ms(p.mod.release, 1));
    put("syn_me_dest", clampi(p.mod_env_dest, 0, SynthPatch::D_LEVEL_B));
    put("syn_me_amt", clampd(p.mod_env_amount * 100, -100, 100));
    for (int k = 0; k < 2; k++) {   // 2.4.17: MOD ENV 2 / 3
        const SynthPatch::ModEnv &m = p.xmod[k];
        const std::string x = std::string("syn_me") + char('2' + k) + "_";
        put(x + "on", m.on ? 1 : 0);
        put(x + "attack", ms(m.env.attack, 0)); put(x + "decay", ms(m.env.decay, 1));
        put(x + "sustain", clampd(m.env.sustain * 100, 0, 100)); put(x + "release", ms(m.env.release, 1));
        put(x + "dest", clampi(m.on ? m.dest : 0, 0, SynthPatch::D_LEVEL_B));
        put(x + "amt", clampd(m.amount * 100, -100, 100));
    }
    for (int k = 0; k < 4; k++) {   // 2.4.17: was 2
        const SynthPatch::Lfo &l = p.lfo[k];
        const std::string x = std::string("syn_lfo") + char('1' + k) + "_";
        put(x + "shape", clampi(l.shape, 0, SynthPatch::L_CUSTOM));
        if (l.shape == SynthPatch::L_CUSTOM && !l.ref.empty()) s.emplace_back("@" + x + "ref=" + l.ref, 0.0f);   // 2.4.13
        // rate knob 0..100 = 0.05 .. 30 Hz exponential (plugin.cpp syn_lfo_hz)
        put(x + "rate", clampd(100.0 * std::log(clampd(l.hz, 0.05, 30) / 0.05) / std::log(600.0), 0, 100));
        put(x + "sync", clampi(l.sync, 0, 14));
        put(x + "trig", l.one_shot ? 2 : l.retrig ? 1 : 0);   // 2.4.35: SYN_LFO_TRIGS Free Run / Retrig / One-Shot
        put(x + "route", 0);   // (LFOs reach their targets through the MATRIX)
        put(x + "depth", 100);
    }
    int n = 0;
    for (const SynthPatch::Route &r : p.routes) {
        if (n >= SynthPatch::MAX_ROUTES || r.src <= SynthPatch::S_OFF || r.src > SynthPatch::S_MODENV3 || r.dst <= SynthPatch::D_OFF || r.dst > SynthPatch::D_LEVEL_B ||
            !std::isfinite(r.amount) || r.amount == 0) continue;
        const std::string x = "syn_mx" + std::to_string(++n) + "_";
        put(x + "src", r.src); put(x + "dst", r.dst); put(x + "amt", clampd(r.amount * 100, -100, 100));
    }
    for (int k = n + 1; k <= SynthPatch::MAX_ROUTES; k++) {   // the rest cleared (no older patch's routes left over)
        const std::string x = "syn_mx" + std::to_string(k) + "_";
        put(x + "src", 0); put(x + "dst", 0); put(x + "amt", 0);
    }
    return s;
}

}  // namespace omni

// Omni Sampler 2.4.10: the synth patch translator. A reader that finds SYNTH settings in a preset (Omnisphere synth layers,
// Nexus .fxp, other synths' patches) describes them here in real units; synth_settings() turns that into Omni's own
// SYNTH engine settings (the syn_* / sa_* / sb_* keys, design.py params), which the loader applies like an .omni's
// (Instrument::settings). Approximate by nature: the readers fill what maps, the rest stays at Omni's defaults
// (plugin.cpp synth_follow resets every synth key a preset does not name).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace omni {

struct SynthPatch {
    // engine OscType / classic waves (synth.cpp CLASSIC) / WarpMode / FilterType / LFO shapes / matrix sources and
    // destinations: the orders of design.py SYN_TYPES, CLASSIC, SYN_WMODES, SYN_FTYPES, SYN_LFO_SHAPES, SYN_MX_SRCS,
    // SYN_ALL_DESTS
    enum Wave { SINE, TRIANGLE, SAW, SQUARE, PULSE25, PULSE10, SOFT_SAW, ORGAN };
    enum Warp { W_OFF, W_BEND, W_PWM, W_MIRROR, W_SYNC, W_FM };
    enum Filter { LP12, LP24, LADDER, BP, HP, NOTCH, COMB };
    enum LfoShape { L_SINE, L_TRI, L_SAW, L_SQUARE, L_SH, L_RANDOM, L_CUSTOM };   // 2.4.13: CUSTOM = Lfo::ref's shape
    enum Src { S_OFF, S_LFO1, S_LFO2, S_MODENV, S_AMPENV, S_FLTENV, S_VEL, S_NOTE, S_WHEEL, S_AT, S_MACRO1, S_MACRO2, S_MACRO3,
               S_MACRO4, S_RANDOM,
               S_LFO3, S_LFO4, S_MODENV2, S_MODENV3 };   // 2.4.17 (appended, as synth.hpp SynthSrc)
    enum Dest { D_OFF, D_PITCH, D_CUTOFF, D_RES, D_POS_A, D_POS_B, D_WARP_A, D_WARP_B, D_LEVEL, D_PAN, D_FM, D_BALANCE, D_LFO1_RATE,
                D_LFO2_RATE, D_DET_A, D_DET_B, D_SUB, D_NOISE, D_DRIVE, D_SPREAD_A, D_SPREAD_B, D_RING, D_FEEDBACK, D_NOISE_TONE,
                D_SUB_PAN, D_GLIDE,
                D_LFO3_RATE, D_LFO4_RATE,     // 2.4.17 (appended, as synth.hpp SynthDest)
                D_LEVEL_A, D_LEVEL_B };       // 2.4.35 (appended): one oscillator's level
    struct Osc {
        bool on = false;
        bool wavetable = false;   // false: a classic wave; true: factory wavetable `table` scanned at `position`
        int wave = SAW, table = 0;
        double position = 0;      // 0..1
        int octave = 0, semi = 0;
        double fine = 0;          // cents
        double level = 0.8, pan = 0;   // 0..1, -1..1
        int unison = 1;
        double detune = 0.2, spread = 0.5;   // cents / 100 (0..1 = 0..100 ct), 0..1
        int warp_mode = W_OFF;
        double warp = 0;          // 0..1
        // 2.4.13: a wavetable of the preset's own (wavetable = true): a reference the oscillator loads it from
        // (formats/vital_table.hpp: "vital:<osc>:{path}"; {path} = the preset's file, filled in by the loader)
        std::string file;
        // 2.4.33: a SAMPLE oscillator playing `file` (a sample / multisample reference: "serumsmp:<osc>:{path}")
        bool sample = false;
    } osc[2];
    bool sub_on = false;
    int sub_shape = 0, sub_oct = 1;      // Sine / Tri / Square; 1 or 2 octaves down
    double sub_level = 0.6;
    bool noise_on = false;
    int noise_color = 0;                 // White / Pink / Crackle
    double noise_level = 0.3, noise_tone = 0.7;
    double fm = 0, ring = 0, feedback = 0, balance = 0;   // 0..1 (balance -1 A .. 1 B)
    bool sync = false;
    bool filter_on = false;
    int filter_type = LP24;
    double cutoff_hz = 20000, resonance = 0, drive = 0, keytrack = 0, env_amount = 0, filter_vel = 0;   // env -1..1
    struct Env { double attack = 0.002, decay = 0.3, sustain = 1, release = 0.3; };   // seconds, 0..1
    Env amp, flt{0.001, 0.45, 0.2, 0.3}, mod{0.4, 0.5, 0, 0.5};
    bool mod_env_on = false;
    int mod_env_dest = D_OFF;
    double mod_env_amount = 0;           // -1..1
    // 2.4.17: MOD ENV 2 / 3 (MATRIX sources S_MODENV2 / S_MODENV3; their own destination as MOD ENV 1's)
    struct ModEnv { bool on = false; int dest = D_OFF; double amount = 0; Env env{0.4, 0.5, 0, 0.5}; };
    ModEnv xmod[2];
    // sync: SYN_LFO_SYNC index, 0 free; ref (2.4.13): shape L_CUSTOM's reference ("vlfo:<n>:{path}", as Osc::file)
    // one_shot (2.4.35): runs once from each note and holds at its end (SYN_LFO_TRIGS "One-Shot"; Serum's Envelope mode)
    struct Lfo { int shape = L_SINE; double hz = 2; int sync = 0; bool retrig = true; std::string ref; bool one_shot = false; };
    Lfo lfo[4];                          // 2.4.17: was 2
    struct Route { int src = S_OFF, dst = D_OFF; double amount = 0; };   // -1..1
    std::vector<Route> routes;           // at most 16 used (the MATRIX; 2.4.17: was 8)
    static constexpr int MAX_ROUTES = 16;
    int voice_mode = 0;                  // Poly / Mono / Legato
    int polyphony = 8;
    double glide = 0;                    // seconds
    bool glide_legato = false;
    int bend = 2;                        // semitones
    double level_db = 0, pan = 0;        // the synth's output
    double amp_vel = 1;                  // velocity -> level, 0..1
    std::string source;                  // e.g. "Omnisphere synth": for the info line
};

// the settings for the loader (Instrument::settings): SYNTH on, every value clamped to its parameter's range. 2.4.13: a
// text setting rides along as a key "@<name>=<text>" (its value unused): @syn_file_a / @syn_file_b (an oscillator's
// file), @syn_lfo1_ref .. @syn_lfo4_ref (a CUSTOM LFO's shape); plugin.cpp synth_follow() applies them
std::vector<std::pair<std::string, float>> synth_settings(const SynthPatch &p);

}  // namespace omni

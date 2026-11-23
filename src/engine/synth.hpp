// Omni Sampler 2.3: the SYNTH layer. Two wavetable oscillators per voice (classic waves, wavetables: the factory set
// or the user's own WAVs from <library>/Oscillators), unison, warp and FM, an amp envelope; it plays together with
// slots A-D (SYNTH ON) and goes through the same FX.
//
// Wavetables are mip-mapped (one copy of every frame per octave, with only the harmonics that fit under Nyquist at
// that pitch), so a high note does not alias; a read blends two frames and two mip levels (4 table reads). Tables are
// built off the audio thread (build_wavetable: an FFT per frame, an inverse FFT per mip) and handed over through an
// atomic pointer (SynthOsc::table); the plugin keeps every table it handed over alive until well after the swap.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "dsp.hpp"
#include "../core/model.hpp"

namespace omni {

struct Program;   // sampler.hpp

// 2.3.4: a SAMPLE oscillator's own sample (a file from the Oscillators folders): the audio, its root key, its loop
struct SynthSample {
    std::string name;
    PcmPtr pcm;
    int root = 60;
    double tune = 0;                  // semitones
    int64_t loop_start = 0, loop_end = 0;   // frames; loop_end <= loop_start: no loop
    LoopType loop_type = LoopType::Forward;   // 2.4.33 (Serum 2: ping-pong loops)
    // 2.4.33: a multisample (Serum 2's multisample oscillator): the first zone holding the note and velocity plays; a
    // note no zone holds is silent on this oscillator. Empty: `pcm` above for every note
    struct Zone {
        PcmPtr pcm;
        int lokey = 0, hikey = 127, lovel = 0, hivel = 127;
        double root = 60;             // + tune (semitones)
        int64_t start = 0, end = 0, loop_start = 0, loop_end = 0;   // end 0 = the whole sample; loop_end <= loop_start: none
        LoopType loop_type = LoopType::Forward;
    };
    std::vector<Zone> zones;
};
using SynthSamplePtr = std::shared_ptr<const SynthSample>;

constexpr int WT_SIZE = 2048;          // samples per single cycle
constexpr int WT_MIPS = 11;            // mip k keeps harmonics 1 .. 1024 >> k
constexpr int WT_MAX_FRAMES = 256;
constexpr int SYN_MAX_VOICES = 16;
constexpr int SYN_MAX_UNISON = 8;
constexpr int SYN_OSCS = 2;
constexpr int SYN_BARS = 32;           // the OSC screens' live bars

struct Wavetable {
    std::string name;                  // as listed ("Basic Shapes", "Glass Pad.wav")
    int frames = 1;
    std::vector<float> data;           // frames x WT_MIPS x (WT_SIZE + 1): one guard sample per cycle for interpolation
    const float *cycle(int frame, int mip) const { return data.data() + (size_t(frame) * WT_MIPS + size_t(mip)) * (WT_SIZE + 1); }
};
using WavetablePtr = std::shared_ptr<const Wavetable>;

// cycles: frames x WT_SIZE samples, one single cycle each. DC is removed and the table normalised to a peak of 1.
WavetablePtr build_wavetable(const std::string &name, const std::vector<float> &cycles, int frames);
// A WAV of single cycles: a Serum-style "clm " chunk gives the cycle length; else 2048 if the length is a multiple,
// else one cycle (the whole file, resampled to 2048). Throws ParseError on an unreadable file.
WavetablePtr load_wavetable_file(const std::string &path);
// 2.3.2: a folder of single-cycle WAVs as one wavetable: each file (its whole length, resampled to 2048) one frame, in
// natural order ("wt2" before "wt10"); at most WT_MAX_FRAMES. name: as listed. Throws ParseError when none can be read
WavetablePtr load_wavetable_frames(const std::string &name, const std::vector<std::string> &files);
// natural order: digit runs compare as numbers, the rest without case
bool natural_less(const std::string &a, const std::string &b);

// the phase warp the oscillators use (the skin's bars show the cycle through it)
double synth_warp_phase(int mode, double ph, float amount);
// 2.3.5: the MACRO engines (POSITION = harmonics, WARP = timbre, SPREAD = morph)
int macro_engine_count();
const char *macro_engine_name(int i);

// The factory oscillators: CLASSIC waves (one frame) and WAVETABLES (64 frames), generated on demand.
int factory_classic_count();
int factory_table_count();
const char *factory_classic_name(int i);
const char *factory_table_name(int i);
WavetablePtr factory_classic(int i);
WavetablePtr factory_table(int i);

// One oscillator's settings (UI thread writes, audio thread reads per block)
struct SynthOscSettings {
    std::atomic<int> on{0}, type{1}, unison{1}, phase_mode{0}, warp_mode{0}, octave{0}, semi{0};
    std::atomic<float> pos{0}, detune{0.2f}, spread{0.5f}, warp{0}, pan{0}, level{0.8f}, fine{0}, phase{0}, rand{0};
    std::atomic<const Wavetable *> table{nullptr};
    // 2.3.4: SAMPLE type: a slot's instrument (0..3, the zone the note picks) or a file (sample_slot -1)
    std::atomic<int> sample_slot{-1};
    std::atomic<const SynthSample *> sample{nullptr};
    std::atomic<int> macro_engine{0};   // 2.3.5: MACRO type: the engine (synth.cpp MACRO_NAMES)
};

// 2.3.1: the MIX / FILTER page
struct SynthMixSettings {
    // SUB: shape 0 sine 1 triangle 2 square (its table: the plugin hands the classic wave over), octave 1 or 2 down
    std::atomic<int> sub_on{0}, sub_oct{1};
    std::atomic<float> sub_level{0.6f}, sub_pan{0};
    std::atomic<const Wavetable *> sub_table{nullptr};
    // NOISE: colour 0 white 1 pink 2 crackle; key = pitched (a band-pass at the note); tone = a low-pass 0..1
    std::atomic<int> noise_on{0}, noise_color{1}, noise_key{0};
    std::atomic<float> noise_level{0.3f}, noise_pan{0}, noise_tone{0.7f};
    // OSC MOD: FM (B modulates A's phase), feedback (A its own), ring (A x B), balance -1 (A) .. 1 (B), sync B to A
    std::atomic<float> fm{0}, feedback{0}, ring{0}, balance{0};
    std::atomic<int> sync{0};
    // FILTER: type 0 LP12 1 LP24 2 Ladder 3 BP 4 HP 5 Notch 6 Comb; cutoff Hz; res, drive, key tracking, velocity, fat,
    // mix 0..1; env amount -1..1 (x 6 octaves)
    std::atomic<int> flt_on{0}, flt_type{1};
    std::atomic<float> cutoff{8000}, res{0}, drive{0}, keytrack{0}, env_amt{0}, vel{0}, fat{0}, mix{1};
    // what goes into the filter (bits 0 OSC A, 1 OSC B, 2 SUB, 3 NOISE); the rest goes round it
    std::atomic<int> route{0xB};
    std::atomic<float> pan{0};
    // the filter envelope (s, s, 0..1, s)
    std::atomic<float> fe_attack{0.001f}, fe_decay{0.45f}, fe_sustain{0.2f}, fe_release{0.3f};
};

// 2.3.2: the ENV / LFO page. Destinations (option order): 0 Off, 1 Pitch (+-12 st), 2 Cutoff (+-4 oct), 3 Resonance
// (+-0.5), 4 Position A, 5 Position B, 6 Warp A, 7 Warp B (+-1), 8 Level (+-100 %), 9 Pan (+-1), 10 FM (+1), 11 Balance (+-1)
// 2.3.3: the MATRIX adds 12 LFO 1 Rate, 13 LFO 2 Rate (+-3 oct), 14 Detune A, 15 Detune B (+-100 ct), 16 Sub Level,
// 17 Noise Level (+-100 %), 18 Drive (+-1) (its own option list: the LFOs' and macros' lists stay as they were)
enum SynthDest { SD_OFF, SD_PITCH, SD_CUTOFF, SD_RES, SD_POS_A, SD_POS_B, SD_WARP_A, SD_WARP_B, SD_LEVEL, SD_PAN, SD_FM, SD_BALANCE,
                 SD_LFO1_RATE, SD_LFO2_RATE, SD_DET_A, SD_DET_B, SD_SUB, SD_NOISE, SD_DRIVE,
                 // 2.4.9 (appended: design.py SYN_ALL_DESTS): unison spread, ring, feedback, noise tone, sub pan, glide time
                 SD_SPREAD_A, SD_SPREAD_B, SD_RING, SD_FEEDBACK, SD_NOISE_TONE, SD_SUB_PAN, SD_GLIDE,
                 // 2.4.17 (appended): LFO 3 / LFO 4 rate (+-3 oct)
                 SD_LFO3_RATE, SD_LFO4_RATE,
                 // 2.4.35 (appended): one oscillator's level, + absolute (0..1 of its level range; Serum's oscillator volume)
                 SD_LEVEL_A, SD_LEVEL_B, SD_COUNT };
// 2.3.3: the MATRIX's sources (option order). 2.4.17: LFO 3, LFO 4, Mod Env 2, Mod Env 3 appended (saved projects keep
// their indexes)
enum SynthSrc { SS_OFF, SS_LFO1, SS_LFO2, SS_MOD_ENV, SS_AMP_ENV, SS_FLT_ENV, SS_VEL, SS_NOTE, SS_WHEEL, SS_AT, SS_MACRO1,
                SS_MACRO2, SS_MACRO3, SS_MACRO4, SS_RANDOM, SS_LFO3, SS_LFO4, SS_MOD_ENV2, SS_MOD_ENV3, SS_COUNT };
constexpr int SYN_ROUTES = 16;   // 2.4.17: was 8 (a Vital preset uses 12 on median)
constexpr int SYN_LFOS = 4;      // 2.4.17: was 2
constexpr int SYN_XENVS = 2;     // 2.4.17: MOD ENV 2 and 3 (MOD ENV 1 = SynthModSettings::me_*)
struct SynthLfoSettings {
    // shape 0 sine 1 triangle 2 saw 3 square 4 sample & hold 5 smooth random; sync 0 free (rate_hz), else a tempo
    // division (synth.cpp SYNC_BEATS: the sampler's LFO list); trig 0 free run (all voices one phase), 1 retrig at
    // every note, 2 one-shot (one cycle from the note, then it holds)
    // 2.4.13: shape 6 CUSTOM plays `custom`: SYN_LFO_TAB + 1 values -1..1 over one cycle (a Vital shape; the plugin
    // keeps the table alive until well after a swap); none: a sine
    std::atomic<int> shape{0}, sync{0}, trig{1}, dest{0};
    std::atomic<float> rate_hz{2}, depth{0.5f};
    std::atomic<const float *> custom{nullptr};
};
constexpr int SYN_LFO_CUSTOM = 6;
constexpr int SYN_LFO_TAB = 256;   // = formats/vital_table.hpp VITAL_LFO_TAB
// 2.3.3: the VOICE page: mode 0 poly 1 mono 2 legato; glide seconds, only between overlapping notes (legato) or
// always; key range; transpose (st); drift (random detune per note, 0..1 = +-25 ct)
struct SynthVoiceSettings {
    std::atomic<int> mode{0}, glide_legato{0}, key_lo{0}, key_hi{127}, transpose{0};
    std::atomic<float> glide{0}, drift{0};
};

// 2.4.17: MOD ENV 2 / 3: as MOD ENV 1 (own destination + amount, and a MATRIX source)
struct SynthModEnvSettings {
    std::atomic<int> on{0}, dest{0};
    std::atomic<float> attack{0.4f}, decay{0.5f}, sustain{0}, release{0.5f}, amt{0.5f};
};
struct SynthModSettings {
    std::atomic<int> me_on{0}, me_dest{0};
    std::atomic<float> me_attack{0.4f}, me_decay{0.5f}, me_sustain{0}, me_release{0.5f}, me_amt{0.5f};
    SynthModEnvSettings mex[SYN_XENVS];   // 2.4.17
    SynthLfoSettings lfo[SYN_LFOS];
    std::atomic<float> macro[4]{};
    std::atomic<int> macro_dest[4]{};
    // 2.3.3: the MATRIX: source, destination, amount -1..1
    std::atomic<int> mx_src[SYN_ROUTES]{}, mx_dst[SYN_ROUTES]{};
    std::atomic<float> mx_amt[SYN_ROUTES]{};
};

struct SynthSettings {
    std::atomic<int> on{0};
    std::atomic<int> polyphony{8};
    std::atomic<float> level{1};                       // linear
    std::atomic<float> attack{0.002f}, decay{0.3f}, sustain{1.0f}, release{0.3f};   // s, s, 0..1, s
    std::atomic<float> bend_range{2};                  // semitones
    std::atomic<float> vel_sens{1};                    // 0..1
    SynthOscSettings osc[SYN_OSCS];
    SynthMixSettings mix;   // 2.3.1
    SynthModSettings mod;   // 2.3.2
    SynthVoiceSettings voice;   // 2.3.3
};

class Synth {
public:
    enum PhaseMode { PM_RESET, PM_FREE, PM_RANDOM, PM_SPREAD };
    enum WarpMode { WM_OFF, WM_BEND, WM_PWM, WM_MIRROR, WM_SYNC, WM_FM };
    enum OscType { OT_CLASSIC, OT_WAVETABLE, OT_SAMPLE, OT_MACRO };
    enum FilterType { FT_LP12, FT_LP24, FT_LADDER, FT_BP, FT_HP, FT_NOTCH, FT_COMB };
    static constexpr int COMB_LEN = 2400;   // the comb filter's line: down to about 20 Hz
    SynthSettings settings;

    // audio thread
    void note_on(int note, int vel);
    void note_off(int note);
    void sustain(bool down);
    void all_off(bool hard);
    // 2.3.4: the program SAMPLE oscillators read slots from (audio thread: set after a swap); fade_program() stops the
    // voices that read it (before a swap frees it); program_voices() counts those still sounding
    void set_program(const Program *p) { prog_ = p; }
    void fade_program();
    int program_voices() const;
    void set_bend(float b) { bend_ = b; }
    void set_tempo(double bpm) { if (bpm > 1) tempo_ = bpm; }   // 2.3.2: the LFOs' tempo sync
    void set_wheel(float v) { wheel_ = v; }   // 2.3.3: the MATRIX's mod wheel and aftertouch (0..1)
    void set_at(float v) { at_ = v; }
    // adds the synth into l/r (frames at most 64 per call is not needed: any count)
    void render(float *l, float *r, int frames);
    std::atomic<int> active{0};

private:
    struct Voice {
        bool on = false, released = false, held_by_pedal = false;
        int note = 0, age = 0;
        int key = 0;                // 2.3.3: the key that plays it (note = key + transpose; note-offs match the key)
        float vel = 1;
        Env amp;
        double ph[SYN_OSCS][SYN_MAX_UNISON] = {};
        float last[SYN_OSCS] = {};   // the oscillators' last outputs (FM from the other one)
        float fade = 1;             // a stolen voice fades out over 5 ms
        bool stealing = false;
        // 2.3.1
        Env fenv;                                   // the filter envelope
        double sub_ph = 0;
        double start_ph[SYN_OSCS][SYN_MAX_UNISON] = {};   // where each phase started (SYNC restarts B there)
        uint32_t noise = 0x12345678u;
        float pink[7] = {}, ntone[2] = {}, nbp1 = 0, nbp2 = 0, crack = 0;
        float svf[2][2][2] = {};                    // [stage][channel][ic1, ic2]
        float lad[2][4] = {};                       // the ladder's four stages per channel
        float fat_lp[2] = {};
        float comb[2][COMB_LEN] = {};
        int comb_w = 0;
        // 2.3.2: the mod envelope and the LFOs (per voice: retrig / one-shot)
        Env menv;
        Env xenv[SYN_XENVS];          // 2.4.17: MOD ENV 2 / 3 (ticked a chunk at a time: only the modulation reads them)
        double lfo_ph[SYN_LFOS] = {};
        float lfo_rnd[SYN_LFOS][2] = {};   // sample & hold / random: the value now and the next (random glides to it)
        // 2.3.3
        float glide = 0, glide_step = 0;   // semitones still to glide (to 0), and per sample
        float drift = 0;                   // this note's random detune, semitones
        // 2.4.5: analog wander (DRIFT): a slow random walk on pitch (semitones) and cutoff (octaves), and this voice's
        // own fixed cutoff offset (component tolerance), all scaled by DRIFT
        float wander[2] = {}, wander_to[2] = {}, cut_tol = 0;
        int wander_n = 0;
        float rnd = 0;                     // the MATRIX's RANDOM source for this note (-1..1)
        float lfo_rate_mod[SYN_LFOS] = {1, 1, 1, 1};    // LFO rate multipliers from the MATRIX (last chunk)
        // 2.4.2: the filter's coefficients now and their per-sample step (they glide to each chunk's target)
        enum { FC_K, FC_A1, FC_A2, FC_A3, FC_K2, FC_B1, FC_B2, FC_B3, FC_G, FC_KL, FC_INV, FC_CD, FC_CFB, FC_N };
        float fc[FC_N] = {}, fcd[FC_N] = {};
        bool fc_set = false;
        int fc_type = -1;
        // 2.3.4: SAMPLE oscillators: the audio each plays (a slot's zone, or a file), where in it, its root and loop
        struct SamplePlay {
            const Pcm *pcm = nullptr;
            const SynthSample *file = nullptr;   // the oscillator's file it came from (silenced once that is swapped out)
            bool from_program = false;
            double root = 60;              // + the zone's / file's tune
            int64_t start = 0, end = 0, loop_start = 0, loop_end = 0;
            // 2.4.30 (audit F9): the zone's loop type and release rule (a file's loop is forward); `complex_loop`: anything but a
            // plain forward loop, so the block's inner loop keeps its old, branch-light path for the usual one
            LoopType loop_type = LoopType::Forward;
            bool until_release = false, complex_loop = false;
            double pos[SYN_MAX_UNISON] = {};
            bool done[SYN_MAX_UNISON] = {};
            bool back[SYN_MAX_UNISON] = {};      // playing backwards (a Backward loop's loop leg, an Alternating loop's return)
        } smp[SYN_OSCS];
        // 2.3.5: MACRO oscillators' state (phases, filters, the string's line, the resonators)
        struct MacroPlay {
            static constexpr int KS = 2048;
            double ph[4] = {};
            float y[8] = {};
            float ks[KS] = {};
            int ks_w = 0, ks_len = 0;
            bool excited = false;
            float c1[4] = {}, c2[4] = {}, g[4] = {};   // Modal: the resonators' coefficients (kept while the pitch holds)
            double last_inc = -1;
            float last_h = -1, last_m = -1;
            // 2.4.5: per-chunk constants of the other engines (pow / exp kept out of the sample loop)
            double ck_inc = -1;
            float ck_h = -1, ck_t = -1, ck_m = -1;
            float cf[16] = {};
            int strike = 0;    // Modal: samples of the bright mallet's noise played (it used y[7], a resonator's state)
            int engine = -1;   // the engine this state belongs to (another one mid-note starts from fresh state)
        } mac[SYN_OSCS];
    };
    Voice voices_[SYN_MAX_VOICES];
    int age_ = 0;
    float bend_ = 0;
    bool pedal_ = false;
    uint32_t rnd_ = 0x9e3779b9u;
    double free_ph_[SYN_OSCS] = {};   // Free phase mode: a running phase per oscillator
    double tempo_ = 120;
    float wheel_ = 0, at_ = 0;        // 2.3.3
    const Program *prog_ = nullptr;   // 2.3.4
    void start_samples(Voice &v, int vel);
    float macro_tick(Voice &v, int o, int engine, double inc, float h, float t, float m);   // 2.3.5
    // 2.3.3: mono / legato: the keys held, oldest first; the last note started (glide's start)
    int held_[16] = {};
    int n_held_ = 0;
    int last_note_ = -1;
    void hold(int note) { drop(note); if (n_held_ < 16) held_[n_held_++] = note; }
    void drop(int note) { int k = 0; for (int i = 0; i < n_held_; i++) if (held_[i] != note) held_[k++] = held_[i]; n_held_ = k; }
    void glide_to(Voice &v, int from_note);
    double glfo_ph_[SYN_LFOS] = {};          // 2.3.2: the free-running LFOs' phase (all voices)
    float glfo_rate_mod_[SYN_LFOS] = {1, 1, 1, 1};  // 2.4.2: their MATRIX LFO RATE (the newest voice's, last block)
    static constexpr int MAX_BLOCK = 256;   // 2.4.2: render() works in passes of at most this
    float glfo_rnd_[SYN_LFOS][2] = {};
    float lfo_value(int shape, double ph, const float *rnd, const float *tab) const;
    double lfo_hz(const SynthLfoSettings &s) const;
    float rand01() { rnd_ = rnd_ * 1664525u + 1013904223u; return float(rnd_ >> 8) / float(1u << 24); }
    Voice *alloc();
    void start(Voice &v, int note, int vel);
};

}  // namespace omni

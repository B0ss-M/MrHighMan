// Omni Sampler: the playing engine. A Program (instrument + decoded audio) is built on the loader thread and handed
// to the audio thread through set_program(); the audio thread fades its voices out, swaps, and hands the old program
// back through take_retired() so it is freed off the audio thread.
#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include "../core/model.hpp"
#include "dsp.hpp"

namespace omni {

struct Program {
    uint32_t serial = 0;               // the loader's load counter, to match LoadedInfo
    Instrument inst;
    std::vector<PcmPtr> pcm;           // per zone (shared between zones of one sample)
    std::vector<uint32_t> rr;          // per zone round-robin counters (audio thread only)
    std::vector<std::vector<int>> by_key;   // zone indices per MIDI key
    int64_t bytes = 0;
    void index();                      // fill by_key and rr
};

// The modulation matrix: sources and destinations (the skin's MOD page; option indices in params.json).
enum ModSrc { MS_OFF, MS_LFO1, MS_LFO2, MS_WHEEL, MS_AT, MS_BEND, MS_VEL, MS_KEY, MS_RAND, MS_ENV, MS_COUNT };
enum ModDst { MD_OFF, MD_PITCH, MD_CUTOFF, MD_RES, MD_VOL, MD_PAN, MD_START, MD_DRIVE, MD_REVERB, MD_LFO1_RATE, MD_LFO2_RATE, MD_COUNT };
constexpr int MOD_SLOTS = 8;
// full-scale (amount 100%, source 1) ranges of the destinations
constexpr float MOD_PITCH_SEMIS = 12, MOD_CUTOFF_OCT = 4, MOD_RES = 0.5f, MOD_RATE_OCT = 3;   // volume: gain 1 + x (silent at -100%, +6 dB at +100%)

// Global controls, written by the UI thread, read per block by the audio thread.
struct Settings {
    // LFOs: wave 0 sine 1 triangle 2 saw up 3 saw down 4 square 5 sample & hold; sync 0 = free (rate_hz), else a
    // tempo division (Sampler::SYNC_BEATS); retrig 0 free-running, 1 restarts at every note
    std::atomic<int> lfo_wave[2]{}, lfo_sync[2]{}, lfo_retrig[2]{};
    std::atomic<float> lfo_rate[2]{};
    std::atomic<int> mod_src[MOD_SLOTS]{}, mod_dst[MOD_SLOTS]{};
    std::atomic<float> mod_amt[MOD_SLOTS]{};   // -1..1
    // instrument slots A-D: key range, gain (linear), transpose (semitones), mute; layer_mode 0 = layer (every
    // slot in its range), 1 = keyswitch (ks_base..ks_base+3 pick the one slot that plays; they make no sound)
    std::atomic<int> slot_lo[4]{}, slot_hi[4]{}, slot_mute[4]{};
    std::atomic<float> slot_gain[4]{}, slot_tune[4]{};
    std::atomic<int> layer_mode{0}, ks_base{24};
    std::atomic<float> volume_db{0}, pan{0}, transpose{0}, tune_cents{0};
    std::atomic<float> cutoff_hz{20000}, resonance{0}, filter_type{0}, filter_vel{0}, filter_env{0};
    std::atomic<float> attack_add{0}, decay_scale{1}, sustain_scale{1}, release_add{0};
    std::atomic<float> vel_sens{1}, bend_range{0};   // bend 0 = the instrument's own
    std::atomic<int> polyphony{48}, voice_mode{0}, interpolation{2};   // mode 0 poly 1 mono 2 legato; interp 0 none 1 linear 2 cubic
    std::atomic<float> glide_s{0};
    std::atomic<float> reverb_mix{0}, reverb_size{0.6f}, reverb_damp{0.4f};
    std::atomic<float> drive{0};
    std::atomic<int> zone_filter_on{1}, zone_env_on{1};
};

constexpr int MAX_VOICES = 64;

class Sampler {
public:
    Sampler();
    ~Sampler();
    Settings settings;

    // Loader thread: hand over a new program (takes ownership). Returns false while a previous hand-over is pending.
    bool set_program(Program *p);
    // Loader / UI thread: a program the audio thread no longer uses, or null.
    Program *take_retired();
    bool switching() const { return pending_.load() != nullptr; }

    // Audio thread
    void midi(const uint8_t *msg, int len);
    void render(int16_t *out, int frames);
    void transport(double ppq, double tempo, int playing, int ppq_valid);   // before render, host block start
    std::atomic<float> lfo_out[2]{};          // the LFOs' last values (-1..1), for the skin
    std::atomic<int> active_slot{0};          // keyswitch mode: the slot that plays (set by keyswitch notes)

    // Diagnostics (any thread)
    std::atomic<int> active_voices{0};
    std::atomic<float> peak{0};
    std::atomic<int> notes_played{0};
    std::atomic<int> last_note{-1};
    // the zone the last note-on started (first one when several), for the skin's "active layer" readout
    std::atomic<int> last_zone{-1}, last_vel{0};
    std::atomic<uint32_t> last_zone_prog{0}, layer_events{0};
    std::atomic<uint8_t> key_down[128]{};   // keys held (any channel): the skin's pad lights
    std::atomic<float> slowest_attack{0};   // the program's longest delay + attack + hold (s): how long a pad tap holds

private:
    struct Voice {
        bool on = false;
        const Zone *zone = nullptr;
        const Pcm *pcm = nullptr;
        int zi = 0, note = 0, vel = 0, chan = 0;
        uint32_t age = 0;
        double pos = 0, step_base = 0;
        int dir = 1;
        bool released = false, held_by_pedal = false, looping = false, fading = false, reverse_ = false;
        int64_t start = 0, stop = 0, loop_start = 0, loop_end = 0;   // loop_end exclusive
        LoopType loop_type = LoopType::Forward;
        bool loop_until_release = false, one_shot = false;
        double loop_tune = 0;                             // semitones while inside the loop
        float gain = 1, pan_l = 1, pan_r = 1, xfade = 1;
        Env amp, fenv, penv;
        LfoState alfo, plfo, flfo;
        VoiceFilter zf[2], mf[2];    // zone filter, master filter (left, right)
        float glide_from = 0, glide = 0, glide_inc = 0;   // semitones offset gliding to 0
        float last_l = 0, last_r = 0;
        float rnd = 0;                                    // per-note random source, -1..1
        float mod_cut_mul = 1, mod_res = 0, mod_gain = 1, mod_pan = 0;   // matrix results for this sub-block
    };

    std::atomic<Program *> pending_{nullptr}, retired_{nullptr};
    Program *prog_ = nullptr;
    Voice voices_[MAX_VOICES];
    uint32_t age_counter_ = 0;
    uint32_t rand_ = 12345;
    float bend_ = 0, modwheel_ = 0, cc7_ = 1, cc10_ = 0, cc11_ = 1;
    bool sustain_ = false;
    float at_ = 0;                                         // channel / poly aftertouch 0..1
    // modulation (audio thread): transport, LFO state, the global sources and this block's matrix snapshot
    double ppq_ = 0, tempo_ = 120;
    bool playing_ = false, ppq_valid_ = false;
    double lfo_phase_[2] = {};
    float lfo_sh_[2] = {};
    float gsrc_[MS_COUNT] = {};
    int slot_src_[MOD_SLOTS] = {}, slot_dst_[MOD_SLOTS] = {};
    float slot_amt_[MOD_SLOTS] = {};
    float g_drive_add_ = 0, g_rev_add_ = 0;
    bool cut_mod_ = false;
    void update_mod(int frames);
    float src_val(const Voice &v, int s) const;
    bool slot_plays(const Zone &z, int note) const;
    bool held_[16][128] = {};
    int held_count_ = 0;
    int last_mono_note_ = -1;
    float last_pitch_semi_ = 0;
    int swap_fade_ = 0;
    Reverb reverb_;
    std::vector<float> mixl_, mixr_;

    void note_on(int chan, int note, int vel);
    void note_off(int chan, int note);
    void start_voice(int zi, int chan, int note, int vel, bool release_trigger, float glide_semis);
    Voice *alloc_voice();
    void all_off(bool hard);
    void render_voice(Voice &v, float *l, float *r, int n);
    void block_mod(Voice &v, int n, double &step, float &cutoff_mul);
    void swap_program();
};

}  // namespace omni

// Omni Sampler: the playing engine. A Program (instrument + decoded audio) is built on the loader thread and handed
// to the audio thread through set_program(); the audio thread fades its voices out, swaps, and hands the old program
// back through take_retired() so it is freed off the audio thread.
#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include "../core/model.hpp"
#include "dsp.hpp"
#include "synth.hpp"
#include "arp.hpp"
#include "omni_fx_bank.hpp"   // 2.4.30: FX BANK (T6, MAI 2: OmniSampler FX Bank, FX1/FX2 fixed)
#include "fx_expansion.hpp"   // 2.4.33: the bay's expansion slots B / C / D

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
enum ModDst { MD_OFF, MD_PITCH, MD_CUTOFF, MD_RES, MD_VOL, MD_PAN, MD_START, MD_DRIVE, MD_REVERB, MD_LFO1_RATE, MD_LFO2_RATE,
              MD_WTPOS, MD_COUNT };   // (append only: option indices in params.json)
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
    // 1.8: the instrument slot a route acts on: 0 = all, 1..4 = A..D (its per-voice destinations only; Drive, Reverb
    // Mix and the LFO rates are shared by every slot)
    std::atomic<int> mod_layer[MOD_SLOTS]{};
    // instrument slots A-D: key range, gain (linear), transpose (semitones), mute; layer_mode 0 = layer (every
    // slot in its range), 1 = keyswitch (ks_base..ks_base+3 pick the one slot that plays; they make no sound)
    std::atomic<int> slot_lo[4]{}, slot_hi[4]{}, slot_mute[4]{};
    std::atomic<int> slot_shift[4]{};   // 1.7.3: semitones the slot's mapping moves on the keyboard (pitch unchanged)
    std::atomic<float> slot_gain[4]{}, slot_tune[4]{};
    std::atomic<float> slot_pan[4]{};   // 1.8: each slot's pan, -1..1 (a balance on top of the zones' own pan)
    std::atomic<int> layer_mode{0}, ks_base{24};
    std::atomic<float> volume_db{0}, pan{0}, transpose{0}, tune_cents{0};
    std::atomic<float> cutoff_hz{20000}, resonance{0}, filter_type{0}, filter_vel{0}, filter_env{0};
    std::atomic<float> attack_add{0}, decay_scale{1}, sustain_scale{1}, release_add{0};
    std::atomic<float> vel_sens{1}, bend_range{0};   // bend 0 = the instrument's own
    std::atomic<int> vel_curve{1};   // 2.4.6: the keyboard's velocity: 0 Soft, 1 Linear, 2 Hard, 3 Fixed (127)
    std::atomic<int> polyphony{48}, voice_mode{0}, interpolation{2};   // mode 0 poly 1 mono 2 legato; interp 0 none 1 linear 2 cubic 3 sinc (2.1.1)
    std::atomic<float> glide_s{0};
    std::atomic<float> reverb_mix{0}, reverb_size{0.6f}, reverb_damp{0.4f};
    std::atomic<int> reverb_type{0};                  // 2.2: 0 Off, 1 Classic (Freeverb), 2 Plate, 3 Hall, 4 Room (PlateReverb)
    std::atomic<float> reverb_predelay{0.01f};        // 2.2: seconds (Plate / Hall / Room)
    std::atomic<int> drive_type{0};                   // 2.2: 0 Off, 1 Classic, 2 Tape, 3 Tube, 4 Fold (Drive, 2x oversampled)
    std::atomic<float> drive{0};
    // 1.8: the FX page. EQ gains in dB; compressor amount, chorus / delay mix, depth, feedback and tone 0..1; chorus rate
    // in Hz; delay_div an index into DELAY_BEATS (sampler.cpp); delay_ping crosses the repeats left / right
    std::atomic<float> eq_low{0}, eq_mid{0}, eq_high{0}, comp{0};
    std::atomic<int> comp_type{0};                    // 2.2: 0 Off, 1 Classic, 2 Glue, 3 Punch (BusComp)
    std::atomic<int> limiter{0};                      // 2.2: the output's lookahead limiter, 0 off, 1 on
    std::atomic<float> chorus_mix{0}, chorus_rate{0.6f}, chorus_depth{0.5f};
    std::atomic<int> chorus_type{0};                  // 2.2: 0 Off, 1 Classic, 2 Ens I, 3 Ens II, 4 Ens I+II
    std::atomic<float> delay_mix{0}, delay_fb{0.35f}, delay_tone{0.3f};
    std::atomic<int> delay_div{5}, delay_ping{0};
    std::atomic<int> delay_type{0};                   // 2.2: 0 Off, 1 Classic, 2 Tape, 3 Analog
    // 1.8.2: the MOD FX page. Mix / depth / feedback 0..1, rates in Hz; trem_sync an index into SYNC_BEATS (0 = free);
    // trem_pan 0 tremolo 1 auto-pan; trem_square 0 sine 1 square; lofi_rate the held rate in Hz
    std::atomic<float> phaser_mix{0}, phaser_rate{0.2f}, phaser_depth{0.6f}, phaser_fb{0.4f};
    std::atomic<float> flanger_mix{0}, flanger_rate{0.13f}, flanger_depth{0.5f}, flanger_fb{0.5f};
    std::atomic<float> trem_depth{0}, trem_rate{2.4f};
    std::atomic<int> trem_sync{0}, trem_pan{0}, trem_square{0};
    std::atomic<float> lofi_mix{0}, lofi_rate{11025};
    std::atomic<int> lofi_bits{8};
    // 2.4.30: FX BANK (FX tab, third page): fxb_prog = index into omnisampler::fxbank::programs() (0 = NO EFFECT, off:
    // no work at all); the rest in the Rack's own units (mix / depth / tone 0..1, rate Hz, time ms, levels dB)
    std::atomic<int> fxb_prog{0}, fxb_bypass{0};
    std::atomic<float> fxb_mix{0.5f}, fxb_rate{0.7f}, fxb_depth{0.65f}, fxb_fb{0.35f}, fxb_time{320}, fxb_tone{0.55f},
        fxb_drive{12}, fxb_thr{-24}, fxb_ratio{4}, fxb_pitch{7}, fxb_low{0}, fxb_mid{0}, fxb_high{0};
    // 2.4.33: the bay's slot (0 A = the factory bank above, 1..3 = B..D) and the program chosen in each expansion slot
    // (0 = off, 1..256); the controls above are shared by every slot
    std::atomic<int> fxb_slot{0};
    std::atomic<int> fxb_xprog[3] = {{0}, {0}, {0}};
    std::atomic<float> wt_pos{0};                     // wavetables: the position through their cycles, 0..1
    // 1.7.1: the PLAY page's envelope faders show the preset's own envelope; what they are moved by is applied to every
    // zone on top of its own envelope (seconds; sustain 0..1)
    std::atomic<float> d_attack{0}, d_decay{0}, d_sustain{0}, d_release{0};
    std::atomic<int> zone_filter_on{1}, zone_env_on{1};
    // 1.8.2: each slot's playback: 0 forward (as the file plays), 1 reverse, 2 ping-pong (its loop, else the whole
    // sample), 3 ping-pong with the backward pass inverted (as Roland's LP chip plays it); slot_inv flips the polarity
    std::atomic<int> slot_play[4]{}, slot_inv[4]{};
    std::atomic<int> rev_gated{0};                    // 2.1.16: Rev: 0 Full (end to start whatever the note-off), 1 Gated (the note-off releases)
    // 1.8.2: note locks (PLAY's second page): what a note plays with, whichever slot it reaches. mode / inv 0 = the
    // slot's, else slot_play + 1 / 1 normal, 2 inverted; vel 0 = as played; semis; start (a share of the sample, forward
    // only); cut (octaves on the master cutoff); gain (linear); pan (-1..1)
    struct NoteLock {
        std::atomic<int> on{0}, mode{0}, inv{0}, vel{0};
        std::atomic<float> semis{0}, start{0}, cut{0}, gain{1}, pan{0};
    };
    NoteLock lock[128];
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
    // Any thread: PANIC. The audio thread then silences every voice (a 5 ms fade) and forgets held keys and the pedal.
    void panic() { panic_req_.store(true); }
    // 2.4.33: the bay's expansion slots (slot 1..3 = B..D): 0 programs = an empty slot. Banks are fixed after the
    // constructor and count / name / title are const, so the UI thread may read them
    int fx_expansion_count(int slot) const { return slot >= 1 && slot <= 3 && fxx_[slot - 1] ? fxx_[slot - 1]->count() : 0; }
    const char *fx_expansion_name(int slot, int program) const {
        return program >= 1 && program <= fx_expansion_count(slot) ? fxx_[slot - 1]->name(program) : "";
    }
    const char *fx_expansion_title(int slot) const { return fx_expansion_count(slot) > 0 ? fxx_[slot - 1]->title() : ""; }
    // Any thread: play p once on top of the voices (a browser preview: an NI kit's .previews/<file>.ogg); takes ownership.
    // Null stops the one playing. Its memory comes back through take_retired_preview() (freed off the audio thread).
    void preview(Pcm *p);
    Pcm *take_retired_preview() { return prev_retired_.exchange(nullptr); }
    std::atomic<int> previewing{0};

    // Audio thread
    void midi(const uint8_t *msg, int len);
    void render(int16_t *out, int frames);
    void transport(double ppq, double tempo, int playing, int ppq_valid);   // before render, host block start
    std::atomic<float> lfo_out[2]{};          // the LFOs' last values (-1..1), for the skin
    std::atomic<int> active_slot{0};          // keyswitch mode: the slot that plays (set by keyswitch notes)

    // Diagnostics (any thread)
    std::atomic<int> active_voices{0};
    std::atomic<float> peak{0};
    std::atomic<float> gain_red_db{0};
    std::atomic<int> nonfinite_heals{0};   // 2.4.32: blocks muted because the mix went inf / NaN (see heal_nonfinite)   // 2.2: the compressor's and the limiter's reduction together (FX GR meter), dB
    std::atomic<int> notes_played{0};
    Synth synth;   // 2.3: the SYNTH layer, played with the slots (its own settings: synth.settings)
    Arp arp;       // 2.4: the ARP tab: while on, the keys steer it and it plays the slots and the SYNTH
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
        int znote = 0;   // the note in the instrument's own mapping (note - the slot's key shift): pitch, crossfades
        uint32_t age = 0;
        double pos = 0, step_base = 0;
        int dir = 1;
        bool released = false, held_by_pedal = false, looping = false, fading = false, reverse_ = false;
        int64_t start = 0, stop = 0, loop_start = 0, loop_end = 0;   // loop_end exclusive
        LoopType loop_type = LoopType::Forward;
        bool loop_until_release = false, one_shot = false;
        bool rev_tail = false;                            // 1.8.2 Rev: one-shot that releases by itself before the start
        bool rev_faded = false;                           // 2.1.16 Rev: its fade to the start has begun
        double loop_tune = 0;                             // semitones while inside the loop
        float gain = 1, pan_l = 1, pan_r = 1, xfade = 1;
        Env amp, fenv, penv;
        LfoState alfo, plfo, flfo;
        VoiceFilter zf[2], mf[2];    // zone filter, master filter (left, right)
        float glide_from = 0, glide = 0, glide_inc = 0;   // semitones offset gliding to 0
        float last_l = 0, last_r = 0;
        float rnd = 0;                                    // per-note random source, -1..1
        float mod_cut_mul = 1, mod_res = 0, mod_gain = 1, mod_pan = 0;   // matrix results for this sub-block
        float mod_wt = 0;                                                // matrix: wavetable position offset (-1..1)
        // 1.8.2: playback mode, polarity and the note's lock
        float polarity = 1;
        float lock_semis = 0, lock_cut_mul = 1, lock_pan = 0;
        // ping-pong inverted: the backward pass is pp_c + pp_d * u - sample, u = 0 at the loop start .. 1 at its last
        // sample, so it meets the sample at both turns (2 x the sample there - it)
        bool pp_inv = false;
        float pp_c[2] = {}, pp_d[2] = {};
        double pp_lo = 0, pp_k = 0;
    };

    std::atomic<Program *> pending_{nullptr}, retired_{nullptr};
    std::atomic<bool> panic_req_{false};
    std::atomic<Pcm *> prev_req_{nullptr}, prev_retired_{nullptr};
    std::atomic<bool> prev_stop_{false};
    Pcm *prev_ = nullptr;          // the preview playing (audio thread)
    double prev_pos_ = 0;
    std::vector<float> pvl_, pvr_;
    void retire_preview();
    void do_panic();
    void heal_nonfinite();   // 2.4.32
    void render_block(int16_t *out, int frames);   // 2.4: render() between the ARP's events
    struct ArpSink : Arp::Out {   // 2.4: the ARP's notes, played as keys
        Sampler *s = nullptr;
        void arp_on(int note, int vel) override { arp_on_ch(0, note, vel); }
        void arp_off(int note) override { arp_off_ch(0, note); }
        // 2.4.30 (audit F2): the sampler hears the channel of the key the note came from (a multi's channel-bound zones);
        // the synth has no channels
        void arp_on_ch(int chan, int note, int vel) override { s->note_on(chan & 15, note, vel); s->synth.note_on(note, vel); }
        void arp_off_ch(int chan, int note) override { s->note_off(chan & 15, note); s->synth.note_off(note); }
    } arp_sink_;
    Program *prog_ = nullptr;
    Voice voices_[MAX_VOICES];
    uint32_t age_counter_ = 0;
    uint32_t note_age0_ = 0;                               // age_counter_ when the current note-on began
    uint32_t rand_ = 12345;
    float bend_ = 0, modwheel_ = 0, cc7_ = 1, cc10_ = 0, cc11_ = 1;
    bool sustain_ = false;
    uint8_t cc_val_[128] = {};   // each controller's last value, for zones enabled by a controller
    float at_ = 0;                                         // channel / poly aftertouch 0..1
    // modulation (audio thread): transport, LFO state, the global sources and this block's matrix snapshot
    double ppq_ = 0, tempo_ = 120;
    bool playing_ = false, ppq_valid_ = false;
    double lfo_phase_[2] = {};
    float lfo_sh_[2] = {};
    float gsrc_[MS_COUNT] = {};
    int slot_src_[MOD_SLOTS] = {}, slot_dst_[MOD_SLOTS] = {}, slot_layer_[MOD_SLOTS] = {};   // layer: -1 all, 0..3 A..D
    float slot_amt_[MOD_SLOTS] = {};
    float g_drive_add_ = 0, g_rev_add_ = 0;
    bool cut_mod_ = false;
    void update_mod(int frames);
    float src_val(const Voice &v, int s) const;
    bool slot_plays(const Zone &z, int note) const;
    bool held_[16][128] = {};
    uint8_t note_vel_[16][128] = {};   // 2.4.27: each held note's note-on velocity (release triggers, after its voice ended)
    int held_count_ = 0;
    int last_mono_note_ = -1;
    float last_pitch_semi_ = 0;
    int swap_fade_ = 0;
    Reverb reverb_;
    PlateReverb plate_;          // 2.2
    Drive drive_;                // 2.2
    int drive_type_ = 0;         // the type the drive last ran as; switching clears it
    int rev_type_ = -1;          // the type the reverb last ran as (-1: off); switching clears it
    Eq3 eq_;
    Compressor comp_;
    BusComp bus_;                // 2.2
    Limiter limiter_;            // 2.2
    int comp_type_ = 0;
    int64_t quiet_frames_ = 0;   // 2.3.1: how long nothing has played and the output stayed under -100 dB (idle skip)
    bool limiter_on_ = false;
    Chorus chorus_;
    Ensemble ensemble_;          // 2.2
    int chorus_type_ = 0;
    Delay delay_;
    bool chorus_on_ = false, delay_on_ = false;
    LoFi lofi_;            // 1.8.2: the MOD FX page
    Phaser phaser_;
    Flanger flanger_;
    Tremolo trem_;
    bool phaser_on_ = false, flanger_on_ = false;
    bool comp_on_ = false, lofi_on_ = false;
    std::vector<float> mixl_, mixr_;
    // 2.4.30: FX BANK: one stereo insert on the mix, before the per-sample FX. Prepared once in the constructor (its
    // ~0.76 MB of delay lines): render never allocates. fxb_sel_ the program it runs; fxb_c_ the controls last set
    omnisampler::fxbank::Rack fxb_;
    int fxb_sel_ = 0;
    omnisampler::fxbank::Controls fxb_c_{};
    bool fxb_set_ = false, fxb_byp_ = false;
    // 2.4.33: expansion banks B / C / D (nullptr = an empty slot), created and prepared in the constructor; the program
    // each runs, the controls it was last given, and the slot of the last block
    std::unique_ptr<FxExpansion> fxx_[3];
    int fxx_sel_[3] = {0, 0, 0};
    omnisampler::fxbank::Controls fxx_c_[3]{};
    bool fxx_set_[3] = {false, false, false};
    int fxx_slot_ = 0;

    void note_on(int chan, int note, int vel);
    int curve_vel(int v) const;   // 2.4.6: an incoming note-on's velocity through settings.vel_curve
    void note_off(int chan, int note);
    void start_voice(int zi, int chan, int note, int vel, bool release_trigger, float glide_semis, int znote = -1);
    int zone_slot(const Zone &z) const { return z.slot < 0 || z.slot > 3 ? 0 : z.slot; }
    Voice *alloc_voice();
    void all_off(bool hard);
    void render_voice(Voice &v, float *l, float *r, int n);
    void block_mod(Voice &v, int n, double &step, float &cutoff_mul);
    void swap_program();
};

}  // namespace omni

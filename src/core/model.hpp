// Omni Sampler: the one instrument model every format reader produces and the engine plays.
// Units follow ConvertWithMoss's model (de.mossgrabers.convertwithmoss.core.model), so readers translated from it
// keep their numbers: times in seconds, gains in dB, pan -1..1, tuning in semitones, modulation depths -1..1 where
// 1 = MAX_ENVELOPE_DEPTH cents (pitch/cutoff), velocity-to-cutoff 1 = 9600 cents, resonance 0..1 (= 0..40 dB).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace omni {

constexpr double MAX_ENVELOPE_DEPTH = 12000.0;   // cents for a modulation depth of 1
constexpr double MAX_VOLUME_DEPTH = 96.0;        // dB for an amplitude LFO depth of 1
constexpr double MAX_RESONANCE_DB = 40.0;
constexpr double MAX_CUTOFF_HZ = 20000.0;

// Decoded audio, interleaved int16. All formats decode to this; 24-bit and float sources are dithered down
// (the engine's own output is int16, and it halves memory against float).
struct Pcm {
    int rate = 44100;
    int channels = 1;
    std::vector<int16_t> data;
    int64_t frames() const { return channels ? int64_t(data.size()) / channels : 0; }
    size_t bytes() const { return data.size() * sizeof(int16_t); }
};
using PcmPtr = std::shared_ptr<const Pcm>;

// A sample the reader found but has not decoded: decoding runs on the loader thread, once per key.
struct SampleRef {
    std::string key;                   // cache identity (path + offset): zones sharing a key share one Pcm
    std::string name;
    int rate = 0;                      // 0 = unknown until decoded
    int channels = 0;
    int64_t frames = 0;                // 0 = unknown until decoded
    std::function<PcmPtr()> decode;    // throws ParseError on failure
};
using SampleRefPtr = std::shared_ptr<SampleRef>;

enum class LoopType { Forward, Backward, Alternating };
struct Loop {
    LoopType type = LoopType::Forward;
    int64_t start = 0, end = 0;        // frames, end inclusive as in WAV smpl / CWM
    bool until_release = false;        // loop only while the key is held, then play on to the end
    int64_t crossfade = 0;             // frames
    double tune = 0;                   // semitones, applied while playing the loop (Roland loop tune)
};

struct Envelope {
    bool set = false;
    double delay = 0, start_level = 0, attack = 0, hold = 0, decay = 0, sustain = 1, release = 0, end_level = 0;
    double attack_slope = 0, decay_slope = 0, release_slope = 0;   // -1..1: 0 linear, + exponential-ish
    double time_key_tracking = 0, time_vel_tracking = 0;           // -1..1
};

enum class LfoWave { Sine, Triangle, Square, SawUp, SawDown, Random };
struct Lfo {
    bool set = false;
    LfoWave wave = LfoWave::Triangle;
    double rate_hz = 5, delay = 0, fade_in = 0, start_phase = 0;
    bool key_sync = true;
};

enum class FilterType { None, LowPass, HighPass, BandPass, BandReject };
struct Filter {
    FilterType type = FilterType::None;
    int poles = 2;
    double cutoff = MAX_CUTOFF_HZ, resonance = 0;   // Hz, 0..1
    double key_tracking = 0;                        // 1 = 100 cents per key around the root
    double env_depth = 0;  Envelope env;            // depth -1..1 (x MAX_ENVELOPE_DEPTH cents)
    double vel_depth = 0;                           // -1..1 (x 9600 cents)
    double lfo_depth = 0;  Lfo lfo;
    double modwheel_depth = 0;
};

enum class Trigger { Attack, Release, First, Legato };
enum class PlayLogic { Always, RoundRobin, Random };

struct Zone {
    std::string name;
    SampleRefPtr sample;
    int key_lo = 0, key_hi = 127, root = 60;
    int key_xfade_lo = 0, key_xfade_hi = 0;          // keys of fade-in above key_lo / fade-out below key_hi
    int vel_lo = 0, vel_hi = 127;
    int vel_xfade_lo = 0, vel_xfade_hi = 0;
    int group = 0;                                   // index into Instrument::groups
    double gain_db = 0, pan = 0, tune = 0;           // tune in semitones (fractions = cents)
    double key_tracking = 1;                         // 0 = fixed pitch
    double amp_key_tracking = 0;                     // dB per key / 1 around root (CWM: 1 = 1 dB/key)
    double amp_vel_depth = 1, amp_vel_curve = 0;     // depth 0..1, curve: x^(3^curve)
    bool reverse = false, one_shot = false;
    int exclusive_group = 0;                         // >0: the zone's choke group
    int off_by = -1;                                 // choked by notes of this group; -1 = its own exclusive_group
    Trigger trigger = Trigger::Attack;
    PlayLogic play_logic = PlayLogic::Always;
    int seq_position = 1, seq_length = 1;            // round robin: plays on every seq_length-th hit
    double rand_lo = 0, rand_hi = 1;                 // random: plays when the note's random number is in [lo, hi)
    int64_t start = 0, stop = -1;                    // frames; stop -1 = end of sample (exclusive)
    std::vector<Loop> loops;
    Envelope amp_env;
    double amp_lfo_depth = 0; Lfo amp_lfo;
    double pitch_env_depth = 0; Envelope pitch_env;
    double pitch_lfo_depth = 0; Lfo pitch_lfo;
    int bend_up = 200, bend_down = -200;             // cents
    Filter filter;
    int midi_channel = -1;                           // -1 = any (multis that map parts to channels)
    int slot = 0;                                    // the plugin's instrument slot (A-D) the zone was loaded into
};

struct Group {
    std::string name;
    Trigger trigger = Trigger::Attack;
};

struct Instrument {
    std::string name;
    std::string format;                              // reader name, for the display
    std::vector<Group> groups{Group{}};
    std::vector<Zone> zones;
    int polyphony = 0;                               // 0 = engine default
    double gain_db = 0;
    std::vector<std::string> warnings;               // shown on the info line, logged
};

// A preset inside a file (an SF2 preset, an E4 bank preset, an Akai program...): enough to list it.
struct PresetInfo {
    std::string name;
    int index = 0;
};

}  // namespace omni

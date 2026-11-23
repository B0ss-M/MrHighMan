// Omni Sampler 2.4: the ARP tab's step sequencer / arpeggiator.
//
// A pattern of up to 64 steps (four pages of 16); each step holds any of 16 rows (a bit mask) and a velocity.
//   SEQ  rows are notes of the chosen scale. The pattern is written in the key: the root at C3 plays it as written,
//        any other key moves it up or down the scale (an off-scale key: by semitones). `base` is row 1's scale degree
//        above the root at C3 (a MIDI file's lowest note), 0 for a drawn pattern.
//   ARP  rows are the keys held (or latched): ORDER Pattern plays row r as the r-th lowest key (past the last, an octave
//        up); the other orders take the notes in turn (Up, Down, Up/Down, Down/Up, Random, As Played) over OCTAVES, or
//        all of them (Chord), one for each dot that starts on the step.
// TIE: a run of dots on a row is one note held to its last dot (GATE shortens that last one). STRUM: the notes a step
// starts together are spread over up to one step, low to high, high to low or alternating.
// The clock follows MPC's song position while the transport plays (steps land on the grid) and runs by itself from the
// first key otherwise (and when MPC says "playing" but its position does not move). Audio thread except where marked;
// the pattern and settings are atomics the UI thread writes.
#pragma once
#include <atomic>
#include <cstdint>

namespace omni {

struct ArpSettings {
    std::atomic<int> on{0}, mode{0}, rate{2}, length{16}, latch{0}, scale{1}, root{0};
    std::atomic<float> swing{0}, gate{0.6f};
    std::atomic<int> order{0}, octaves{1}, tie{0}, strum_dir{0};   // 2.4.1
    std::atomic<float> strum{0};                                   // 0..1 of a step
    std::atomic<int> base{0};                                      // SEQ: row 1's degree above the root at C3
    std::atomic<int> octave{0};                                    // OCTAVE: everything it plays, -3..+3 octaves
    // 2.4.2 GROOVE: play direction (0 forward, 1 back, 2 ping-pong, 3 random walk); HUMANIZE time (0..1: up to a quarter
    // step late) and velocity (0..1: up to +-40 %); CHAIN: up to 4 pages played in turn (-1 = unused; none = LENGTH)
    std::atomic<int> direction{0};
    std::atomic<float> human_time{0}, human_vel{0};
    std::atomic<int> chain[4] = {{-1}, {-1}, {-1}, {-1}};
};

class Arp {
public:
    static constexpr int STEPS = 64, ROWS = 16, SCALES = 16, RATES = 7, ORDERS = 8;
    enum Order { O_PATTERN, O_UP, O_DOWN, O_UPDOWN, O_DOWNUP, O_RANDOM, O_PLAYED, O_CHORD };
    static const char *const SCALE_NAMES[SCALES];
    static const char *const RATE_NAMES[RATES];
    static const double RATE_BEATS[RATES];

    ArpSettings settings;
    std::atomic<uint16_t> mask[STEPS];   // rows on at each step (bit r = row r + 1)
    std::atomic<uint8_t> vel[STEPS];     // each step's velocity, 1..127 (100 = as played)
    std::atomic<uint8_t> ngate[STEPS * ROWS];   // a note's own GATE % (at its first step and row), 0 = the pattern's
    // 2.4.2: a note's own expression (at its first step and row; 0 = as the pattern plays it): velocity % (1-200),
    // the chance it skips (100 - CHANCE %), ratchet hits in its first step (2-4), octave (-2..+2), SLIDE (held into the
    // next note: legato, glides with GLIDE)
    std::atomic<uint8_t> nvel[STEPS * ROWS], nskip[STEPS * ROWS], nratch[STEPS * ROWS], nslide[STEPS * ROWS];
    std::atomic<int8_t> noct[STEPS * ROWS];
    std::atomic<int> step_now{-1};       // the step playing (0..63), -1 stopped: the skin's playhead
    std::atomic<int> ref_note{-1};       // the key row 1 follows now (SEQ: the latest; ARP: the lowest held), -1 none
    std::atomic<uint32_t> keys_rev{0};   // bumped when the keys change (the skin's row names)

    struct Out {   // where the notes go (the Sampler)
        virtual void arp_on(int note, int vel) = 0;
        virtual void arp_off(int note) = 0;
        // 2.4.30 (audit F2): with the MIDI channel of the key the note came from (a sink that ignores channels need not override)
        virtual void arp_on_ch(int chan, int note, int vel) { (void)chan; arp_on(note, vel); }
        virtual void arp_off_ch(int chan, int note) { (void)chan; arp_off(note); }
        virtual ~Out() = default;
    };

    Arp();
    // SEQ: the note row `row` plays for `key` (base: row 1's degree above the root at C3); 0..127 or -1
    static int seq_note(int key, int row, int scale, int root, int base = 0);
    static int arp_note(const int *pool, int n, int row);
    static int scale_size(int scale);
    // the scale degree of a key (rounded down onto the scale; degree 0 = the root in octave -1), and back
    static int degree_of(int key, int scale, int root);
    static int note_of(int degree, int scale, int root);
    static bool in_scale(int key, int scale, int root);

    // audio thread
    void key_on(int note, int velocity, int chan = 0);
    void key_off(int note, int chan = -1);   // chan -1 (or one that does not hold it): the key up on every channel
    void reset(Out &out);                // all notes off, keys forgotten (PANIC, ARP turned off)
    void transport(double ppq, double bpm, bool playing, bool valid);   // once per host block, before render
    // plays what is due now, then returns how many of `frames` to render before the next event (>= 1)
    int process(Out &out, int frames);
    void advance(int frames);            // after rendering that many frames
    bool running() const { return running_; }
    bool busy() const { return running_ || snd_n_ > 0 || pend_n_ > 0 || any_held_ || latched_any_; }

private:
    double beats_per_step() const;
    double step_time(long k) const;      // the start of absolute step k, in beats (swing delays the odd steps)
    void start_free();
    void stop(Out &out);
    void play_step(Out &out, long k);
    int step_of(long k);                 // 2.4.2: the pattern step absolute step k plays (DIRECTION, CHAIN)
    uint32_t rnd();
    void sound(Out &out, int note, int chan, int v, int64_t dur);
    void note_off_all(Out &out);
    int pool(int *out) const;            // the keys in play (held, or latched), lowest first
    int next_ordered(const int *pl, int pn, int *out);   // ORDER: the next note(s) (Chord: all)

    // keys
    uint8_t held_[128] = {};             // velocity of each key held (0 = up)
    int order_[128] = {};                // key press order (SEQ: the latest wins; ARP As Played)
    int order_n_ = 0;
    uint8_t latched_[128] = {};          // LATCH: the last chord, kept after the keys go up
    // 2.4.30 (audit F2): the MIDI channel each key plays on. A pitch held on two channels is one key (the pattern is
    // built from pitches) that plays on the channel of its latest press still held; it is up when the last channel lets go
    uint16_t chmask_[128] = {};          // channels holding each key (bit c = channel c)
    uint8_t chan_[128] = {}, lchan_[128] = {};   // the channel it plays on (held, latched)
    bool any_held_ = false, latched_any_ = false;
    // clock
    double bpm_ = 120, pos_ = 0;         // pos_: beats; the host's song position while it plays, else our own
    bool host_ = false;                  // following the host's song position
    double host_last_ = -1;              // the host's last position (a frozen one is not playing)
    int host_still_ = 0;
    bool running_ = false;
    long next_k_ = 0;                    // the next absolute step to play
    // ORDER
    int ord_i_ = 0;
    uint32_t rng_ = 0x9E3779B9u;
    int last_rand_ = -1;
    bool strum_flip_ = false;
    int walk_ = 0;                       // random walk: the step it is on
    long walk_k_ = -1;
    int last_step_ = -1;                 // 2.4.14: the step played before (TIE: a run continues only from its neighbour)
    // sounding notes, and strummed ones waiting to start
    struct Sounding { int note, chan; int64_t left; };
    Sounding snd_[64];
    int snd_n_ = 0;
    struct Pending { int note, chan, vel; int64_t wait, dur; };
    Pending pend_[64];
    int pend_n_ = 0;
};

}  // namespace omni

// Omni Sampler: mpc_engine() glue: parameters, the file browser, project state. The sampler and loader do the work.
//
// Threads: set_param/get_param come from the host (UI thread; get_param of "<key>_on" and of numeric params also from
// the audio thread inside the wrapper's housekeeping), midi/render from the audio thread, loading on the loader
// thread. Browser state is guarded by ui_mutex; everything the audio thread may read is atomic.
#define _FILE_OFFSET_BITS 64   // 2.4.14: stat() of a file of 2 GiB or more fails on the 32-bit MPCs otherwise (see core/vfs.cpp)
#include <algorithm>
#include <unordered_map>
#include <chrono>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>   // 2.4.14: UINTPTR_MAX (MEM_MAX_MB)
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
extern "C" {
#include "engine.h"
}
#include "engine/loader.hpp"
#include "core/browser_queue.hpp"
#include "engine/sampler.hpp"
#include "engine/smf.hpp"   // 2.4
#include "engine/chord.hpp"     // 2.4.14: the GRID's chord names
#include "engine/voicing.hpp"   // 2.4.14: the GRID's VOICE (inversions, spread) in key and scale
#include <random>
#include "formats/format.hpp"
#include "formats/omni_native.hpp"
#include "formats/vital_table.hpp"   // 2.4.13
#include "formats/arturia_table.hpp"   // 2.4.24: Arturia AGSS Wavetable ("artwav:<preset path>")
#include "formats/serum_table.hpp"   // 2.4.32: Serum 2 preset tables ("serum:<osc>:<preset path>")
#include "formats/serum_sample.hpp"   // 2.4.33: Serum 2 sample / multisample oscillators ("serumsmp:<osc>:<preset path>")

using namespace omni;

namespace {

constexpr int ROWS = 10;
constexpr int ENV_BARS = 24;   // 2.3.2: the ENV page's envelope screens
constexpr int ENV_SCREENS = 5;   // 2.4.17: AMP, FILTER, MOD ENV 1, MOD ENV 2, MOD ENV 3 (bar keys syn_<a|f|m|n|o>e_b<n>)

enum class RowKind { Up, Drive, Folder, Image, File, Preset, Group, Sounds };   // 2.4.11 Sounds: a drum ROM kit's ONE SOUND list
struct Row {
    std::string label, path;
    RowKind kind;
    int preset = 0;
    std::string group{};   // Group: the type of file it holds (path = the folder)
};

// Numeric parameters: key, default (in parameter units). Order is irrelevant here; params.json fixes VST indices.
struct NumParam { const char *key; float def; };
const NumParam NUM_PARAMS[] = {
    {"volume", 0}, {"pan", 0}, {"transpose", 0}, {"tune", 0},
    {"cutoff", 100}, {"resonance", 0}, {"filter_type", 0}, {"filter_vel", 0},
    {"attack", 0}, {"decay", 100}, {"sustain", 100}, {"release", 0},
    {"vel_sens", 100}, {"vel_curve", 1}, {"bend", 0}, {"polyphony", 48}, {"voice_mode", 0}, {"glide", 0}, {"interp", 2}, {"sinc", 0},
    {"rev_mix", 0}, {"rev_size", 60}, {"rev_damp", 40}, {"drive", 0},
    {"mem_limit", 512}, {"prog_change", 1}, {"pad_vel", 100}, {"pad_base", 36}, {"auto_extract", 1},
    {"target_slot", 0}, {"layer_mode", 0}, {"ks_base", 24}, {"auto_loop", 0}, {"info_page", 0}, {"wt_pos", 0}, {"skin", 1},
    // 1.7.1: the envelope as it plays (the preset's own, then as moved): times as fader units 0..1000 (ms = 60000 x
    // (v/1000)^3), sustain %
    {"env_attack", 0}, {"env_decay", 0}, {"env_sustain", 100}, {"env_release", 69},
    {"rev_type", 0},     // 2.2: Reverb type: 0 Off (no key lit), 1 Classic, 2 Plate, 3 Hall, 4 Room
    {"rev_predelay", 10},   // 2.2: ms
    // 2.3: the SYNTH layer: on/off, level (dB), polyphony, its amp envelope (ms, ms, %, ms), and two oscillators
    // (sa_ = OSC A, sb_ = OSC B): type 0 Classic 1 Wavetable 2 Sample 3 Macro, table = the index in that type's list,
    // position / detune (ct) / spread / warp / level %, pan -100..100, octave, semi, fine ct, phase deg, random %,
    // unison 1..8, phase mode (Reset Free Random Spread), warp mode (Off Bend PWM Mirror Sync FM)
    {"syn_on", 0}, {"syn_level", 0}, {"syn_poly", 8}, {"syn_attack", 2}, {"syn_decay", 300}, {"syn_sustain", 100}, {"syn_release", 300},
    {"sa_on", 1}, {"sa_type", 1}, {"sa_table", 0}, {"sa_pos", 0}, {"sa_detune", 20}, {"sa_spread", 50}, {"sa_warp", 0},
    {"sa_pan", 0}, {"sa_level", 80}, {"sa_oct", 0}, {"sa_semi", 0}, {"sa_fine", 0}, {"sa_phase", 0}, {"sa_rand", 0},
    {"sa_unison", 1}, {"sa_pmode", 0}, {"sa_wmode", 0},
    {"sb_on", 0}, {"sb_type", 0}, {"sb_table", 2}, {"sb_pos", 0}, {"sb_detune", 20}, {"sb_spread", 50}, {"sb_warp", 0},
    {"sb_pan", 0}, {"sb_level", 80}, {"sb_oct", 0}, {"sb_semi", 0}, {"sb_fine", 0}, {"sb_phase", 0}, {"sb_rand", 0},
    {"sb_unison", 1}, {"sb_pmode", 0}, {"sb_wmode", 0},
    // 2.3.1: the MIX / FILTER page: SUB (shape 0 sine 1 triangle 2 square; octave 0 = -1, 1 = -2), NOISE (colour 0 white
    // 1 pink 2 crackle; key 1 = pitched), OSC MOD (FM, feedback, ring, balance -100 A..100 B, sync B to A), FILTER (type
    // LP12 LP24 Ladder BP HP Notch Comb; cutoff 0..100 = 20 Hz..20 kHz; env -100..100), ROUTING (each source into the
    // filter: 1), OUTPUT pan; and the filter envelope (ms, ms, %, ms: its knobs on the ENV page)
    {"syn_sub_on", 0}, {"syn_sub_shape", 0}, {"syn_sub_oct", 0}, {"syn_sub_level", 60}, {"syn_sub_pan", 0},
    {"syn_noise_on", 0}, {"syn_noise_color", 1}, {"syn_noise_key", 0}, {"syn_noise_level", 30}, {"syn_noise_pan", 0}, {"syn_noise_tone", 70},
    {"syn_fm", 0}, {"syn_feedback", 0}, {"syn_ring", 0}, {"syn_balance", 0}, {"syn_sync", 0},
    {"syn_flt_on", 0}, {"syn_flt_type", 1}, {"syn_cutoff", 75}, {"syn_res", 0}, {"syn_drive", 0}, {"syn_keytrack", 0},
    {"syn_fenv", 0}, {"syn_fvel", 0}, {"syn_fat", 0}, {"syn_fmix", 100},
    {"syn_fr_a", 1}, {"syn_fr_b", 1}, {"syn_fr_sub", 0}, {"syn_fr_noise", 1}, {"syn_pan", 0},
    {"syn_fe_attack", 1}, {"syn_fe_decay", 450}, {"syn_fe_sustain", 20}, {"syn_fe_release", 300},
    // 2.3.2: the ENV / LFO page: the mod envelope (on, ms, ms, %, ms, destination, amount -100..100), two LFOs (shape
    // Sine Tri Saw Square S&H Random; rate 0..100 = 0.05..30 Hz; sync: the LFO list; trig Free Run / Retrig / One-Shot;
    // destination; depth %), four macros (value %, destination). Destinations: Off Pitch Cutoff Resonance Position A
    // Position B Warp A Warp B Level Pan FM Balance
    {"syn_me_on", 0}, {"syn_me_attack", 400}, {"syn_me_decay", 500}, {"syn_me_sustain", 0}, {"syn_me_release", 500},
    {"syn_me_dest", 0}, {"syn_me_amt", 50},
    {"syn_lfo1_shape", 1}, {"syn_lfo1_rate", 50}, {"syn_lfo1_sync", 0}, {"syn_lfo1_trig", 1}, {"syn_lfo1_dest", 0}, {"syn_lfo1_depth", 50},
    {"syn_lfo2_shape", 4}, {"syn_lfo2_rate", 50}, {"syn_lfo2_sync", 0}, {"syn_lfo2_trig", 1}, {"syn_lfo2_dest", 0}, {"syn_lfo2_depth", 50},
    {"syn_macro1", 0}, {"syn_macro2", 0}, {"syn_macro3", 0}, {"syn_macro4", 0},
    {"syn_macro1_dest", 0}, {"syn_macro2_dest", 0}, {"syn_macro3_dest", 0}, {"syn_macro4_dest", 0},
    // 2.3.3: the VOICE page (mode Poly / Mono / Legato; glide ms; glide only between held notes; the synth's own bend
    // range st and velocity %; key range; transpose st; drift %) and the MATRIX (8 routes: source, destination, amount
    // -100..100; synth.hpp SynthSrc / SynthDest)
    {"syn_vmode", 0}, {"syn_glide", 0}, {"syn_glide_mode", 0}, {"syn_bend", 2}, {"syn_vel", 100}, {"syn_key_lo", 0},
    {"syn_key_hi", 127}, {"syn_transpose", 0}, {"syn_drift", 0},
    {"syn_mx1_src", 0}, {"syn_mx1_dst", 0}, {"syn_mx1_amt", 0}, {"syn_mx2_src", 0}, {"syn_mx2_dst", 0}, {"syn_mx2_amt", 0},
    {"syn_mx3_src", 0}, {"syn_mx3_dst", 0}, {"syn_mx3_amt", 0}, {"syn_mx4_src", 0}, {"syn_mx4_dst", 0}, {"syn_mx4_amt", 0},
    {"syn_mx5_src", 0}, {"syn_mx5_dst", 0}, {"syn_mx5_amt", 0}, {"syn_mx6_src", 0}, {"syn_mx6_dst", 0}, {"syn_mx6_amt", 0},
    {"syn_preset", 0},
    {"syn_lfo1_route", 0}, {"syn_lfo2_route", 0},
    // 2.4: the ARP tab (design.py ARP_*: mode Seq/Arp, rate 1/4..1/32 T, length 1-64 steps, swing / gate %, latch,
    // scale, root C..B, the page A-D the grid shows). The pattern itself is "arp_pattern" in the state
    {"arp_on", 0}, {"arp_mode", 0}, {"arp_rate", 2}, {"arp_length", 16}, {"arp_swing", 0}, {"arp_gate", 60},
    {"arp_latch", 0}, {"arp_scale", 1}, {"arp_root", 0}, {"arp_page", 0},
    // 2.4.1: ORDER, OCTAVES (0 = 1), MIDI SNAP, TIE, STRUM %, its direction, the grid's touch (Tap / Draw), page FOLLOW
    {"arp_order", 0}, {"arp_oct", 0}, {"arp_snap", 0}, {"arp_tie", 0}, {"arp_strum", 0}, {"arp_strum_dir", 0},
    {"arp_draw", 0}, {"arp_follow", 1},
    {"arp_octave", 0}, {"arp_note_gate", 60},
    // 2.4.2: the selected note's VEL %, CHANCE %, RATCHET, NOTE OCT, SLIDE; arp_sel: a note is selected (the skin shows
    // these instead of the pattern's knobs; the plugin sets it)
    {"arp_n_vel", 100}, {"arp_n_chance", 100}, {"arp_n_ratchet", 1}, {"arp_n_oct", 0}, {"arp_n_slide", 0}, {"arp_sel", 0},
    // 2.4.2 GEN page: GROOVE (direction, humanize time / velocity %, chain slots 0 none 1-4 = A-D, Euclid hits / rotate)
    // and MELODY (contour, density %, range rows, syncopation %, mutate %)
    {"arp_dir", 0}, {"arp_h_time", 0}, {"arp_h_vel", 0}, {"arp_chain1", 0}, {"arp_chain2", 0}, {"arp_chain3", 0}, {"arp_chain4", 0},
    {"arp_e_hits", 5}, {"arp_e_rotate", 0}, {"arp_g_contour", 2}, {"arp_g_density", 55}, {"arp_g_range", 8}, {"arp_g_sync", 25},
    {"arp_g_mutate", 30}, {"arp_g_artic", 50}, {"arp_g_expr", 50}, {"arp_g_ghost", 50}, {"mut_amount", 1},   // 2.4.1: OCTAVE -3..+3; NOTE GATE: the selected note's gate %   // 2.3.6: Off, Amp (level), Filter (cutoff), Mod (the mod env's destination)   // 2.3.5: the synth's factory preset last chosen (SYN_PRESETS)
    {"syn_mx7_src", 0}, {"syn_mx7_dst", 0}, {"syn_mx7_amt", 0}, {"syn_mx8_src", 0}, {"syn_mx8_dst", 0}, {"syn_mx8_amt", 0},
    {"comp_type", 0},    // 2.2: Compressor type: 0 Off, 1 Classic, 2 Glue, 3 Punch
    {"limiter", 0},      // 2.2: the output limiter: 0 Off, 1 On
    {"cho_type", 0},     // 2.2: Chorus type: 0 Off, 1 Classic, 2 Ens I, 3 Ens II, 4 Ens I+II
    {"dly_type", 0},     // 2.2: Delay type: 0 Off, 1 Classic, 2 Tape, 3 Analog
    {"drive_type", 0},   // 2.2: Drive type: 0 Off, 1 Classic, 2 Tape, 3 Tube, 4 Fold
    {"rev_mode", 0},     // 2.1.16: Rev: 0 Full (the whole sample back to its start), 1 Gated (the note-off releases it)
    {"br_folders", 0},   // 2.1.3: SAMPLE FOLDERS: 0 Hide (an instrument's own sample folder is not listed), 1 Show
    {"br_preview", 1},   // 1.7.2: play a kit's preview (NI .previews/<file>.ogg) when it is tapped
    // 1.7.3: each slot's key shift (its whole mapping moves up or down the keyboard; the sound keeps its pitch)
    {"slot1_shift", 0}, {"slot2_shift", 0}, {"slot3_shift", 0}, {"slot4_shift", 0},
    // 1.8: each slot's pan (-100..100)
    {"slot1_pan", 0}, {"slot2_pan", 0}, {"slot3_pan", 0}, {"slot4_pan", 0},
    // 1.8: the FX page
    {"eq_low", 0}, {"eq_mid", 0}, {"eq_high", 0}, {"comp", 0}, {"cho_mix", 0}, {"cho_rate", 50}, {"cho_depth", 50},
    {"dly_mix", 0}, {"dly_time", 5}, {"dly_fb", 35}, {"dly_tone", 30}, {"dly_ping", 0},
    {"slot1_lo", 0}, {"slot1_hi", 127}, {"slot1_vol", 0}, {"slot1_tune", 0}, {"slot1_mute", 0},
    {"slot2_lo", 0}, {"slot2_hi", 127}, {"slot2_vol", 0}, {"slot2_tune", 0}, {"slot2_mute", 0},
    {"slot3_lo", 0}, {"slot3_hi", 127}, {"slot3_vol", 0}, {"slot3_tune", 0}, {"slot3_mute", 0},
    {"slot4_lo", 0}, {"slot4_hi", 127}, {"slot4_vol", 0}, {"slot4_tune", 0}, {"slot4_mute", 0},

    {"lfo1_wave", 0}, {"lfo1_rate", 50}, {"lfo1_sync", 0}, {"lfo1_retrig", 0},
    {"lfo2_wave", 1}, {"lfo2_rate", 35}, {"lfo2_sync", 0}, {"lfo2_retrig", 0},
    {"mod1_src", 1}, {"mod1_dst", 1}, {"mod1_amt", 0},
    {"mod2_src", 2}, {"mod2_dst", 2}, {"mod2_amt", 0},
    {"mod3_src", 3}, {"mod3_dst", 2}, {"mod3_amt", 0},
    {"mod4_src", 6}, {"mod4_dst", 2}, {"mod4_amt", 0},
    {"mod5_src", 8}, {"mod5_dst", 5}, {"mod5_amt", 0},
    {"mod6_src", 7}, {"mod6_dst", 2}, {"mod6_amt", 0},
    {"mod7_src", 4}, {"mod7_dst", 4}, {"mod7_amt", 0},
    {"mod8_src", 1}, {"mod8_dst", 4}, {"mod8_amt", 0},
    // 1.8: the slot each route acts on: 0 = all (as before), 1..4 = A..D
    {"mod1_slot", 0}, {"mod2_slot", 0}, {"mod3_slot", 0}, {"mod4_slot", 0},
    {"mod5_slot", 0}, {"mod6_slot", 0}, {"mod7_slot", 0}, {"mod8_slot", 0},
    // 1.8.2: each slot's playback mode (Fwd, Rev, Ping-Pong, PP Inv) and polarity
    {"slot1_play", 0}, {"slot2_play", 0}, {"slot3_play", 0}, {"slot4_play", 0},
    {"slot1_inv", 0}, {"slot2_inv", 0}, {"slot3_inv", 0}, {"slot4_inv", 0},
    // 1.8.2: note locks: the note being edited, and its lock as shown (LOCK_KEYS: views of Omni::lock_ui); the pads
    // follow the notes played
    {"lock_note", 60}, {"lock_mode", 0}, {"lock_inv", 0}, {"lock_tune", 0}, {"lock_fine", 0}, {"lock_start", 0},
    {"lock_cut", 0}, {"lock_level", 0}, {"lock_pan", 0}, {"lock_vel", 0}, {"pad_follow", 1},
    // 1.8.2: the MOD FX page
    {"phs_mix", 0}, {"phs_rate", 30}, {"phs_depth", 60}, {"phs_fb", 40},
    {"flg_mix", 0}, {"flg_rate", 20}, {"flg_depth", 50}, {"flg_fb", 50},
    {"trm_depth", 0}, {"trm_rate", 60}, {"trm_sync", 0}, {"trm_mode", 0}, {"trm_shape", 0},
    {"lofi_mix", 0}, {"lofi_bits", 8}, {"lofi_rate", 40},
    // 2.4.14: the GRID's VOICE: the voicing (VOICE_MODE_NAMES) and the widest DROP / OPEN result (semitones)
    {"arp_v_mode", 0}, {"arp_v_span", 24},
    // 2.4.17: the SYNTH's MOD 2 page (MOD ENV 2 / 3, LFO 3 / 4; no hidden _dest: the route keys and the MATRIX place them)
    // and MATRIX 2 (routes 9-16). Defaults = design.py's
    {"syn_me2_on", 0}, {"syn_me2_attack", 400}, {"syn_me2_decay", 500}, {"syn_me2_sustain", 0}, {"syn_me2_release", 500},
    {"syn_me2_dest", 0}, {"syn_me2_amt", 50},
    {"syn_me3_on", 0}, {"syn_me3_attack", 400}, {"syn_me3_decay", 500}, {"syn_me3_sustain", 0}, {"syn_me3_release", 500},
    {"syn_me3_dest", 0}, {"syn_me3_amt", 50},
    {"syn_lfo3_shape", 0}, {"syn_lfo3_rate", 50}, {"syn_lfo3_sync", 0}, {"syn_lfo3_trig", 1}, {"syn_lfo3_depth", 50}, {"syn_lfo3_route", 0},
    {"syn_lfo4_shape", 2}, {"syn_lfo4_rate", 50}, {"syn_lfo4_sync", 0}, {"syn_lfo4_trig", 1}, {"syn_lfo4_depth", 50}, {"syn_lfo4_route", 0},
    {"syn_mx9_src", 0}, {"syn_mx9_dst", 0}, {"syn_mx9_amt", 0}, {"syn_mx10_src", 0}, {"syn_mx10_dst", 0}, {"syn_mx10_amt", 0},
    {"syn_mx11_src", 0}, {"syn_mx11_dst", 0}, {"syn_mx11_amt", 0}, {"syn_mx12_src", 0}, {"syn_mx12_dst", 0}, {"syn_mx12_amt", 0},
    {"syn_mx13_src", 0}, {"syn_mx13_dst", 0}, {"syn_mx13_amt", 0}, {"syn_mx14_src", 0}, {"syn_mx14_dst", 0}, {"syn_mx14_amt", 0},
    {"syn_mx15_src", 0}, {"syn_mx15_dst", 0}, {"syn_mx15_amt", 0}, {"syn_mx16_src", 0}, {"syn_mx16_dst", 0}, {"syn_mx16_amt", 0},
    // 2.4.30: FX BANK (FX tab, third page). fxb_prog: 0 Off, then the bank's programs without its NO EFFECT / THRU
    // entries (fxb_catalog); fxb_rate 0..100 -> 0.05..40 Hz (fxb_hz). Defaults = design.py's
    {"fxb_prog", 0}, {"fxb_mix", 50}, {"fxb_rate", 39}, {"fxb_depth", 65}, {"fxb_fb", 35}, {"fxb_time", 320}, {"fxb_tone", 55},
    {"fxb_drive", 12}, {"fxb_thr", -24}, {"fxb_ratio", 4}, {"fxb_pitch", 7}, {"fxb_low", 0}, {"fxb_mid", 0}, {"fxb_high", 0},
    {"fxb_bypass", 0},   // 0 Active, 1 Bypass (the page's ON | BYPASS keys)
    {"fxb_pad", 0},      // 2.4.32: the program keypad shown (1) or not; the project's own, not a sound setting
    // 2.4.33 (appended): the bay's slot (0 A factory, 1..3 B..D expansions) and each expansion slot's program (0 Off, 1..256); 2.4.35: 1..999
    {"fxb_slot", 0}, {"fxb_prog_b", 0}, {"fxb_prog_c", 0}, {"fxb_prog_d", 0},
};
constexpr int NUM_COUNT = int(sizeof NUM_PARAMS / sizeof NUM_PARAMS[0]);
#if UINTPTR_MAX > 0xFFFFFFFFu
constexpr int MEM_MAX_MB = 2048;   // 2.4.14: a 64-bit MPC (Live III, 8 GB)
#else
constexpr int MEM_MAX_MB = 1024;   // 2.4.14: a 32-bit MPC (Live II, 2 GB)
#endif

// 1.8.2: a note lock as the NOTE LOCK page edits it (parameter units); all at 0 = no lock. mode 0 = the slot's, 1..4 =
// Fwd .. PP Inv; inv 0 = the slot's, 1 normal, 2 inverted; tune st; fine ct; start %; cut -100..100 (= -4..+4 octaves on
// the cutoff); level dB; pan -100..100; vel 0 = as played
constexpr int LOCK_N = 9;
const char *const LOCK_KEYS[LOCK_N] = {"lock_mode", "lock_inv", "lock_tune", "lock_fine", "lock_start", "lock_cut",
                                       "lock_level", "lock_pan", "lock_vel"};

struct Omni {
    Sampler sampler;
    Vfs vfs;
    Loader loader{sampler, vfs};
    std::string data_dir;
    std::string last_browse;   // 1.8.13: the folder last written to <data_dir>/last_browse.txt
    std::string last_pos;      // 2.4.27: what last_browse.txt last held (folder, highlighted row, scroll): no rewrite when equal
    std::atomic<float> values[NUM_COUNT];

    std::mutex ui_mutex;
    int drum_set = -1;                    // 2.4.11: the drum ROM kit whose ONE SOUND list is shown (-1: none) (ui_mutex)
    std::vector<PresetInfo> drum_presets; // its file's presets
    uint64_t disp_sig = 0;        // 2.4.11: update_displays' inputs last time (diag thread only)
    std::string state_warning;   // 2.4.11: get_state left something out (shown in the status line)
    std::string mut_msg;         // 2.4.12: what MUTATE / BREED did (the status line, 4 s; gp_cache_mutex)
    long long mut_msg_at = 0;
    std::mutex gp_cache_mutex;                                  // 2.4.11: get_param's last texts (shown while ui_mutex is busy)
    std::unordered_map<std::string, std::string> gp_cache;
    std::string dir;                 // full path of the folder or image shown; "" = the drives list
    std::string group;               // 1.8: the type group of dir shown ("Drum Programs"...); "" = the whole folder
    std::map<std::string, std::string> xpm_kinds;   // MPC program path -> its group, read once (ui_mutex)
    std::map<std::string, std::string> heat_folders;   // 2.1.3: Heat Up preset path -> the sample folder it names (ui_mutex)
    std::map<std::string, bool> sample_only;           // 2.1.4: folder -> it holds only Heat Up samples (ui_mutex)
    std::string preset_file;
    std::string sel_path;            // the row last tapped (file or preset): highlighted until another is tapped
    int cur = -1;                    // the data wheel / Q-Link 1 highlight (an index into rows); -1 until one is turned
    // one Q-Link / data wheel turn: when its last event came, when it started, its direction and its last step (one row
    // per turn; a turn held past 300 ms scrolls fast)
    std::chrono::steady_clock::time_point turn_last{}, turn_start{}, turn_step{};
    int turn_dir = 0;
    // the Plugin Library's location: the user's choice (an SD card or SSD folder, saved in <data_dir>/library_path.txt
    // for every project), else <data_dir>/library on the internal storage
    std::mutex lib_mutex;
    std::string lib_choice;
    int sel_preset = -1;
    // diagnostics: with /sdcard/omni-debug present, the state is written to /tmp/omni-debug.txt every second
    std::thread diag_thread;
    std::atomic<bool> diag_quit{false};
    // the skin's displays, computed by the diag/display thread (30 Hz), read anywhere (audio thread included)
    std::atomic<uint8_t> disp_wave[48]{}, disp_ptile[32]{}, disp_ltile[32]{};         // non-empty: listing the presets of this file
    std::vector<Row> rows;
    int top = 0;                     // the first row shown (the list scrolls by rows: page buttons, the scroll fader)
    // the scroll fader's position (read lock-free): 1 = the top of the list (a vertical fader's top), 0 = the end
    std::atomic<float> scroll_pos{1};
    std::string info_text;           // the last tapped item
    std::string browse_msg;
    std::atomic<int> row_on[ROWS];
    std::atomic<int> trig_on[40];   // one per TRIGGERS entry (TRIGGER_COUNT, at most 40)
    // the skin's 16 pads: a tap queues a note (set_param), the audio thread plays it with a fixed gate (render)
    std::atomic<int> pad_req[16];
    int pad_gate[16] = {}, pad_note[16] = {};   // audio thread only
    std::atomic<int> pad_base_rev{0};
    std::atomic<int> pad_panic{0};
    std::atomic<int> logo_frame{-1};   // 2.1.5: the logo spark's frame last reported to the host
    // LFO ROUTE keys (lfo_route_tap): the keys to report (bit lfo * 3 + key), each LFO's route they last showed (audio
    // thread), and when a project came back (its keys' restore is not a tap)
    std::atomic<uint16_t> rk_dirty{0};   // 2.4.17: 4 LFOs x 3 keys (was uint8_t, 2 LFOs)
    int rk_shown[SYN_LFOS] = {-1, -1, -1, -1};
    std::atomic<long long> rk_hold_ms{0};
    int gr_frame = -1;                 // 2.2: the GR meter's segment count last reported (audio thread)
    long long gr_ms = 0;               // 2.2.3: when it was (at most every 80 ms: MPC's screen thread pays for each)
    // 1.8: each slot's key range as it is before its key shift (UI thread), so shifting is reversible: a closed end
    // shifted off the keyboard is clamped there and comes back when shifted back (it used to stay at the edge and then
    // count as open)
    struct RangeBase { bool set = false, open = false; int base = 0; } range_base[4][2];       // PANIC: the audio thread drops the pads' pending notes and gates
    // the envelope faders: how far they are moved from the target slot's preset envelope (s; sustain 0..1), and whether
    // their values came from a project / patch / saved instrument (then the next load keeps them) or follow the preset
    std::atomic<float> d_env[4] = {};
    std::atomic<bool> env_pending{false};     // trigger buttons held (the wrapper releases them): their highlight
    // browser actions run on their own thread: listing folders, mounting images and reading a file's presets can
    // take a while, and set_param must return at once whichever thread the host calls it from
    std::thread br_thread;
    std::mutex br_qm;
    std::condition_variable br_cv;
    std::deque<std::pair<std::string, std::string>> br_q;
    bool br_quit = false;
    std::atomic<int> br_busy{0};
    std::atomic<bool> br_queue_dropped{false};
    std::atomic<uint32_t> ui_rev{0};
    std::atomic<long long> restore_ms{-100000};
    struct FxSet { int i; float set, before; };
    std::vector<FxSet> fx_by_preset;   // 2.4.14: the output effects presets set, and their values before (loader thread)
    std::string miss_path[4];   // 2.4.14: the missing entry miss_seen describes (a new failure is checked once first)
    bool miss_seen[4] = {};   // 2.4.9: retry_missing (the diag thread's): each missing slot's file was there last check   // 2.4.9: when a project's state was last restored (grown_opt_replay)
    // 1.8.2: every note's lock in parameter units (lock_mutex); the engine gets them converted (commit_lock)
    std::mutex lock_mutex;
    float lock_ui[128][LOCK_N] = {};
    int follow_seen = 0;   // the sampler's notes_played the pads last followed (display thread)
    // 2.1.16: parameters the engine changed by itself (auto split, reset) and the key-map tiles, for mpc_engine_changed to
    // report so MPC redraws them at once (they used to show only after a page change). Index: NUM_PARAMS, then ptile_1..32,
    // ltile_1..32. Set from any thread; read and cleared on the audio thread
    static constexpr int ARP_REP0 = NUM_COUNT + 64 + SYN_OSCS * SYN_BARS + ENV_SCREENS * ENV_BARS;   // 2.4: then the grid's cells, its lights
    static constexpr int REP_COUNT = ARP_REP0 + 256 + 16;   // 2.3: + the OSC screens' bars, 2.3.2 the envelopes'
    std::atomic<uint8_t> rep_dirty[REP_COUNT]{};
    std::atomic<int> rep_pending{0};
    int rep_cur = 0;   // audio thread only
    // 2.3: the SYNTH's oscillators (syn_mutex): what each has loaded, the user's own (<library>/Oscillators), tables
    // swapped out (kept 2 s, until the audio thread surely let go), and a loader thread of its own (building a
    // 64-frame table takes a moment on a Live II; the browser's worker holds ui_mutex)
    std::mutex syn_mutex;
    struct SynOsc { WavetablePtr cur; SynthSamplePtr smp; std::string name, err; int loading = 0;
                    int fold_i = 0, fold_n = 0; } syn[SYN_OSCS];   // 2.4.9: its folder's number / how many (the loader's)
    std::vector<int> syn_user_files;   // 2.3.4: the entries of syn_user that are files (SAMPLE's list)
    std::vector<std::pair<SynthSamplePtr, long long>> syn_retired_s;   // 2.3.4: samples swapped out (freed 2 s later)
    std::vector<std::string> syn_user;
    WavetablePtr syn_sub;             // 2.3.1: the SUB's wave
    std::vector<std::pair<WavetablePtr, long long>> syn_retired;
    std::string syn_file[SYN_OSCS];   // a project's own oscillator file (its name), found again after a rescan
    bool syn_pending[SYN_OSCS] = {};          // 2.4.11: syn_file not found yet (library not mounted): kept, retried (syn_mutex)
    long long syn_pending_since[SYN_OSCS] = {};
    // 2.4.13: the LFOs' CUSTOM shapes (syn_mutex): what each plays (ref: a Vital preset's "vlfo:<n>:<path>", or a .vitallfo
    // under <library>/LFOs by its relative path), its name, a load's error, its table (the engine reads it through
    // SynthLfoSettings::custom; tables swapped out are kept 2 s), a < > step asked of the loader thread; the LFOs folder
    struct SynLfoShape { std::string ref, name, err; std::shared_ptr<const std::vector<float>> tab; int step = 0; } syn_lfo[SYN_LFOS];   // 2.4.17: 4
    std::vector<std::pair<std::shared_ptr<const std::vector<float>>, long long>> syn_lfo_retired;
    std::vector<std::string> syn_lfo_files;
    std::thread syn_thread;
    std::condition_variable syn_cv;
    int syn_req = 0;                  // bit o: load oscillator o (syn_mutex); 2.4: bit 3 scan <library>/MIDI, bit 4 load arp_want
    // 2.4: the ARP tab: the MIDI files under <library>/MIDI (relative paths) and the one picked (syn_mutex); the grid as
    // shown (the page's 16 steps x 16 rows) and the playhead column shown (-1 none; audio thread)
    std::vector<std::string> arp_files;
    // 2.4.1: UJAM style phrases in the list ("Styles/<library> ~ <style> ~ <phrase>.ujs"): what each names, and each
    // blob's styles once read (the worker's)
    struct ArpStyleRef { std::string blob; int style = 0, phrase = 0; };
    std::map<std::string, ArpStyleRef> arp_style_refs;
    std::map<std::string, std::vector<UjamMidi>> arp_ujam;
    int arp_sel = -1;
    std::string arp_loaded, arp_msg, arp_want;
    std::atomic<uint8_t> arp_view[256]{};
    int arp_led_shown = -1;
    // 2.4.1: a DRAW stroke on the grid (UI thread): what it does, the note it holds (row, first and last step), where
    // the finger was, and the pad's latest position
    struct ArpStroke { bool active = false, moved = false; int kind = 0, row = 0, s0 = 0, s1 = 0, grab = 0, at_r = -1, at_s = -1; uint8_t vel = 100; };
    ArpStroke arp_stroke;
    float arp_xy[2] = {0, 0};
    bool arp_sel_on = false;
    // 2.4.2: the pattern before the last GEN edit (UNDO swaps back and forth)
    // 2.4.18: + the step (RATE) and LENGTH, put back by UNDO only when the edit changed them (timing)
    struct ArpSnap { bool set = false, timing = false; uint16_t mask[64]; uint8_t vel[64]; uint8_t nx[64 * 16][6]; float rate = 2, length = 16; };
    ArpSnap arp_undo;
    // 2.4.12 MUTATE / BREED: the synth's two parents and the step before (UNDO), the GRID's two parent patterns (in memory)
    struct SynSnap { bool set = false; std::vector<float> v; };
    SynSnap mut_a, mut_b, mut_undo;
    ArpSnap brd_a, brd_b;
    // 2.4.14 VOICE: per step, the voicing it made last (a hash of its pitches) and the mean pitch it keeps near, so
    // INVERT pressed again cycles instead of climbing (UI thread only; not saved)
    uint32_t voice_sig[64] = {};
    double voice_anchor[64] = {};
    // 2.4.9: the MIDI file loaded last, as it came in: GENERATE's source (syn_mutex). Cleared by RESET
    struct ArpSrc { bool set = false; uint16_t mask[64]; uint8_t vel[64]; int length = 0, rate = 2; } arp_src;   // 2.4.1: the note selected (amber): its row and first step
    int arp_sel_r = 0, arp_sel_s0 = 0;
    bool syn_quit = false;
    std::atomic<uint8_t> syn_bar[SYN_OSCS][SYN_BARS]{};
    std::atomic<uint8_t> env_bar[ENV_SCREENS][ENV_BARS]{};   // 2.3.2: the AMP / FILTER / MOD envelope screens (2.4.17: + MOD ENV 2 / 3)
    float env_key[ENV_SCREENS] = {-1, -1, -1, -1, -1};
    const Wavetable *bar_t[SYN_OSCS] = {};
    float bar_key[SYN_OSCS] = {-1, -1};
    // 2.1.16: RESET (SETUP): the first tap arms it for RESET_ARM_MS, a second tap inside that resets; reset_info says so
    std::atomic<long long> reset_armed_ms{0}, reset_done_ms{0};
    // 2.4.32: FX BANK's program number keypad: the digits typed so far, packed (count * 1000 + value); FXB_ERR shows ERR
    std::atomic<int> fxb_entry{0};

    Omni() {
        for (int i = 0; i < NUM_COUNT; i++) values[i].store(NUM_PARAMS[i].def);
        for (auto &r : row_on) r.store(0);
        for (auto &t : trig_on) t.store(0);
        for (auto &p : pad_req) p.store(0);
    }
};

int num_index(const char *key) {
    for (int i = 0; i < NUM_COUNT; i++) if (!std::strcmp(NUM_PARAMS[i].key, key)) return i;
    return -1;
}

// 2.2: the FX page's GR meter: 0..24 dB of reduction in its 12 segments (2 dB each), as the strip's frame 0..127.
// 2.2.3: whole segments only, so the value (and MPC's repaint) moves only when a segment lights or goes out
constexpr int GR_SEGS = 12;
int gr_meter_seg(Omni *o) {
    return std::max(0, std::min(GR_SEGS, int(std::lround(o->sampler.gain_red_db.load() / 24.0f * GR_SEGS))));
}
int gr_meter_frame(Omni *o) { return (gr_meter_seg(o) * 127 + GR_SEGS / 2) / GR_SEGS; }

// 2.1.16: queue a parameter for mpc_engine_changed (rep_dirty's index)
void mark_changed(Omni *o, int i) {
    if (i < 0 || i >= Omni::REP_COUNT) return;
    if (!o->rep_dirty[i].exchange(1)) o->rep_pending.fetch_add(1);
}
void mark_changed(Omni *o, const char *key) { mark_changed(o, num_index(key)); }

float num(Omni *o, const char *key) { int i = num_index(key); return i < 0 ? 0 : o->values[i].load(); }

struct Omni;
void save_last_browse(Omni *o, const std::string &dir);   // 1.8.13, below
void save_browse_pos(Omni *o);   // 2.4.27, below
bool host_dir(const std::string &p) { struct stat st; return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
// 1.8: the Plugin Library is only ever on an SD card / USB drive / SSD (sample libraries are large and the internal
// storage is small). MPC OS mounts its own storage under /media too: az01-internal (= /data), az01-internal-sd
// (= /sdcard) and acvs-content (read-only), so a folder counts as external when it is under /media/<drive>, the drive
// is not one of those, and (when it is mounted) it is not on the same device as /, /data or /sdcard.
#ifdef OMNI_OFFICIAL
// official package only (build_aarch64.sh / package_official.sh): MPC OS 3.10's Live III keeps the user's internal drive
// ("MPC User", what MPC's browser calls Internal) on /nvme; its /sdcard is an empty folder on the system. Used only while
// /nvme is a mounted drive, so other MPCs running this package keep /sdcard. OMNI_SAMPLER_USER_DRIVE: tests (any folder)
std::string user_drive() {
    const char *t = std::getenv("OMNI_SAMPLER_USER_DRIVE");
    std::string p = t && *t ? t : "/nvme";
    struct stat st, sys;
    if (stat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return "";
    if (!(t && *t) && stat("/", &sys) == 0 && sys.st_dev == st.st_dev) return "";   // not mounted
    return p;
}
#else
std::string user_drive() { return ""; }
#endif
bool on_user_drive(const std::string &p) {
    std::string u = user_drive();
    return !u.empty() && p.compare(0, u.size(), u) == 0 && (p.size() == u.size() || p[u.size()] == '/');
}
bool external_path(const std::string &p) {
    if (on_user_drive(p)) return true;   // official: the Live III's internal drive is large and the user's own
    if (p.compare(0, 7, "/media/") != 0) return false;
    std::string drive = p.substr(7, p.find('/', 7) == std::string::npos ? std::string::npos : p.find('/', 7) - 7);
    if (drive.empty() || drive.compare(0, 4, "az01") == 0 || drive == "acvs-content") return false;
    struct stat st, in;
    if (stat(p.c_str(), &st) != 0) return true;   // not connected now: judged by its name
    for (std::string i : {std::string("/"), std::string("/data"), std::string("/sdcard"), user_drive()})
        if (!i.empty() && stat(i.c_str(), &in) == 0 && in.st_dev == st.st_dev) return false;
    return true;
}
// the chosen library when its drive is there, else "" (no library: never the internal storage)
std::string library_dir(Omni *o) {
    std::lock_guard<std::mutex> lock(o->lib_mutex);
    return host_dir(o->lib_choice) ? o->lib_choice : std::string();
}
// where SAVE TO LIBRARY / Auto Extract / SAVE PATCH write: "" (they say so) while there is no library
void update_extract(Omni *o) {
    std::string lib = library_dir(o);
    o->loader.set_extract(int(num(o, "auto_extract") + 0.5f), lib.empty() ? std::string() : path_join(lib, "Extracted"));
}
std::string library_choice(Omni *o) {
    std::lock_guard<std::mutex> lock(o->lib_mutex);
    return o->lib_choice;
}

// envelope fader units 0..1000 <-> milliseconds (cubic: fine at short times, 60 s at the top)
constexpr float ENV_MAX_MS = 60000.0f;
float env_ms(float v) { float x = std::max(0.0f, std::min(1000.0f, v)) / 1000.0f; return ENV_MAX_MS * x * x * x; }
float env_units(float ms) { return 1000.0f * std::cbrt(std::max(0.0f, std::min(ENV_MAX_MS, ms)) / ENV_MAX_MS); }

// LFO rate knob 0..100 -> 0.02..20 Hz (logarithmic)
float lfo_hz(float v) { return 0.02f * std::pow(1000.0f, std::max(0.0f, std::min(100.0f, v)) / 100.0f); }
// chorus rate knob 0..100 -> 0.05..5 Hz (logarithmic)
float chorus_hz(float v) { return 0.05f * std::pow(100.0f, std::max(0.0f, std::min(100.0f, v)) / 100.0f); }   // also phaser, flanger
// 2.4.30: FX BANK. The RATE knob 0..100 -> 0.05..40 Hz; the program popup's option -> the bank's catalog index (option 0
// = Off = catalog 0, NO EFFECT; the popup leaves out the catalog's other NO EFFECT / THRU entries, as design.py does)
float fxb_hz(float v) { return 0.05f * std::pow(800.0f, std::max(0.0f, std::min(100.0f, v)) / 100.0f); }
int fxb_catalog(int option) {
    static const std::vector<int> table = [] {
        std::vector<int> t{0};
        std::size_t n = 0;
        const omnisampler::fxbank::Program *list = omnisampler::fxbank::programs(n);
        for (std::size_t i = 0; i < n; i++)
            if (list[i].engine != omnisampler::fxbank::Engine::Bypass) t.push_back(int(i));
        return t;
    }();
    return option >= 0 ? (option < int(table.size()) ? table[size_t(option)] : 0) : int(table.size()) - 1;
}
int fxb_catalog_count() { return fxb_catalog(-1); }   // the programs the popup lists (Off not counted)
// 2.4.33: the bay's expansion slots B..D: each keeps its own program (0 Off, 1..256); 2.4.35: 1..999
const char *const FXB_XPROG[3] = {"fxb_prog_b", "fxb_prog_c", "fxb_prog_d"};
// slot A's program as design.py lists it (FXB_OPTIONS): Off, or "<card> <name>" with " / " written as " "
std::string fxb_label(int option) {
    const int cat = fxb_catalog(option);
    if (option <= 0 || cat <= 0) return "Off";
    std::size_t n = 0;
    const omnisampler::fxbank::Program *list = omnisampler::fxbank::programs(n);
    const std::string bank = list[cat].bank;
    std::string name = list[cat].name;
    for (std::size_t at; (at = name.find(" / ")) != std::string::npos;) name.replace(at, 3, " ");
    const char *card = bank == "700/reverb" ? "REV" : bank == "700/chorus" ? "MOD" : bank == "700/variation" ? "VAR"
                     : bank == "Z-FX" ? "MULTI" : "FX";
    return std::string(card) + " " + name;
}
float trem_hz(float v) { return 0.1f * std::pow(200.0f, std::max(0.0f, std::min(100.0f, v)) / 100.0f); }       // 1.8.2: 0.1 - 20 Hz
float lofi_hz(float v) { return 44100.0f * std::pow(1000.0f / 44100.0f, std::max(0.0f, std::min(100.0f, v)) / 100.0f); }   // 44.1 - 1 kHz

// 2.3.2: an LFO's RATE 0..100 as Hz (0.05 .. 30, exponential); the sync divisions' names (design.py LFO_SYNC)
float syn_lfo_hz(float v) { return 0.05f * std::pow(600.0f, std::max(0.0f, std::min(100.0f, v)) / 100.0f); }
const char *const SYN_LFO_SYNC[] = {"Free", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4 T", "1/8 T",
                                    "1/16 T", "1/4 .", "1/8 .", "1/16 ."};
// 2.3.1: the synth filter's CUTOFF 0..100 as Hz (20 Hz .. 20 kHz, exponential)
float syn_cutoff_hz(float v) { return 20.0f * std::pow(1000.0f, std::max(0.0f, std::min(100.0f, v)) / 100.0f); }

void apply_settings(Omni *o) {
    {   // 2.4: the ARP
        ArpSettings &a = o->sampler.arp.settings;
        a.mode.store(int(num(o, "arp_mode") + 0.5f));
        a.rate.store(int(num(o, "arp_rate") + 0.5f));
        a.length.store(std::max(1, std::min(64, int(num(o, "arp_length") + 0.5f))));
        a.swing.store(num(o, "arp_swing") / 100.0f);
        a.gate.store(num(o, "arp_gate") / 100.0f);
        a.latch.store(num(o, "arp_latch") > 0.5f);
        a.scale.store(int(num(o, "arp_scale") + 0.5f));
        a.root.store(int(num(o, "arp_root") + 0.5f));
        a.on.store(num(o, "arp_on") > 0.5f);
        a.order.store(int(num(o, "arp_order") + 0.5f));   // 2.4.1
        a.octaves.store(int(num(o, "arp_oct") + 0.5f) + 1);
        a.tie.store(num(o, "arp_tie") > 0.5f);
        a.strum.store(num(o, "arp_strum") / 100.0f);
        a.strum_dir.store(int(num(o, "arp_strum_dir") + 0.5f));
        a.octave.store(int(std::lround(num(o, "arp_octave"))));
        a.direction.store(int(num(o, "arp_dir") + 0.5f));   // 2.4.2
        a.human_time.store(num(o, "arp_h_time") / 100.0f);
        a.human_vel.store(num(o, "arp_h_vel") / 100.0f);
        for (int c = 0; c < 4; c++) { char k[24]; std::snprintf(k, sizeof k, "arp_chain%d", c + 1); a.chain[c].store(int(num(o, k) + 0.5f) - 1); }
    }
    Settings &s = o->sampler.settings;
    s.volume_db.store(num(o, "volume"));
    s.pan.store(num(o, "pan") / 100.0f);
    s.transpose.store(num(o, "transpose"));
    s.tune_cents.store(num(o, "tune"));
    s.cutoff_hz.store(20.0f * std::pow(1000.0f, num(o, "cutoff") / 100.0f));
    s.resonance.store(num(o, "resonance") / 100.0f);
    s.filter_type.store(num(o, "filter_type"));
    s.filter_vel.store(num(o, "filter_vel") / 100.0f);
    s.attack_add.store(num(o, "attack") / 1000.0f);
    s.decay_scale.store(num(o, "decay") / 100.0f);
    s.sustain_scale.store(num(o, "sustain") / 100.0f);
    s.release_add.store(num(o, "release") / 1000.0f);
    s.vel_sens.store(num(o, "vel_sens") / 100.0f);
    s.vel_curve.store(std::max(0, std::min(3, int(num(o, "vel_curve") + 0.5f))));   // 2.4.6
    s.bend_range.store(num(o, "bend"));
    s.polyphony.store(int(num(o, "polyphony")));
    s.voice_mode.store(int(num(o, "voice_mode")));
    s.glide_s.store(num(o, "glide") / 1000.0f);
    s.interpolation.store(num(o, "sinc") > 0.5f ? 3 : int(num(o, "interp")));   // 2.1.1: HQ SINC overrides
    s.reverb_mix.store(num(o, "rev_mix") / 100.0f);
    s.reverb_size.store(num(o, "rev_size") / 100.0f);
    s.reverb_damp.store(num(o, "rev_damp") / 100.0f);
    s.reverb_type.store(std::max(0, std::min(4, int(num(o, "rev_type") + 0.5f))));   // 2.2
    s.drive_type.store(std::max(0, std::min(4, int(num(o, "drive_type") + 0.5f))));   // 2.2
    s.reverb_predelay.store(std::max(0.0f, std::min(250.0f, num(o, "rev_predelay"))) / 1000.0f);
    s.drive.store(num(o, "drive") / 100.0f);
    s.wt_pos.store(num(o, "wt_pos") / 100.0f);
    s.eq_low.store(num(o, "eq_low")); s.eq_mid.store(num(o, "eq_mid")); s.eq_high.store(num(o, "eq_high"));
    s.comp.store(num(o, "comp") / 100.0f);
    s.chorus_mix.store(num(o, "cho_mix") / 100.0f);
    s.chorus_rate.store(chorus_hz(num(o, "cho_rate")));
    s.chorus_depth.store(num(o, "cho_depth") / 100.0f);
    s.comp_type.store(std::max(0, std::min(3, int(num(o, "comp_type") + 0.5f))));   // 2.2
    s.limiter.store(num(o, "limiter") > 0.5f ? 1 : 0);
    s.chorus_type.store(std::max(0, std::min(4, int(num(o, "cho_type") + 0.5f))));   // 2.2
    s.delay_mix.store(num(o, "dly_mix") / 100.0f);
    s.delay_div.store(int(num(o, "dly_time") + 0.5f));
    s.delay_fb.store(num(o, "dly_fb") / 100.0f);
    s.delay_tone.store(num(o, "dly_tone") / 100.0f);
    s.delay_ping.store(num(o, "dly_ping") > 0.5f ? 1 : 0);
    s.delay_type.store(std::max(0, std::min(3, int(num(o, "dly_type") + 0.5f))));   // 2.2
    s.phaser_mix.store(num(o, "phs_mix") / 100.0f);
    s.phaser_rate.store(chorus_hz(num(o, "phs_rate")));
    s.phaser_depth.store(num(o, "phs_depth") / 100.0f);
    s.phaser_fb.store(num(o, "phs_fb") / 100.0f);
    s.flanger_mix.store(num(o, "flg_mix") / 100.0f);
    s.flanger_rate.store(chorus_hz(num(o, "flg_rate")));
    s.flanger_depth.store(num(o, "flg_depth") / 100.0f);
    s.flanger_fb.store(num(o, "flg_fb") / 100.0f);
    s.trem_depth.store(num(o, "trm_depth") / 100.0f);
    s.trem_rate.store(trem_hz(num(o, "trm_rate")));
    s.trem_sync.store(int(num(o, "trm_sync") + 0.5f));
    s.trem_pan.store(num(o, "trm_mode") > 0.5f ? 1 : 0);
    s.trem_square.store(num(o, "trm_shape") > 0.5f ? 1 : 0);
    s.lofi_mix.store(num(o, "lofi_mix") / 100.0f);
    s.lofi_bits.store(int(num(o, "lofi_bits") + 0.5f));
    s.lofi_rate.store(lofi_hz(num(o, "lofi_rate")));
    // 2.4.30: FX BANK
    s.fxb_prog.store(fxb_catalog(int(num(o, "fxb_prog") + 0.5f)));
    s.fxb_mix.store(num(o, "fxb_mix") / 100.0f);
    s.fxb_rate.store(fxb_hz(num(o, "fxb_rate")));
    s.fxb_depth.store(num(o, "fxb_depth") / 100.0f);
    s.fxb_fb.store(num(o, "fxb_fb") / 100.0f);
    s.fxb_time.store(num(o, "fxb_time"));
    s.fxb_tone.store(num(o, "fxb_tone") / 100.0f);
    s.fxb_drive.store(num(o, "fxb_drive"));
    s.fxb_thr.store(num(o, "fxb_thr"));
    s.fxb_ratio.store(num(o, "fxb_ratio"));
    s.fxb_pitch.store(num(o, "fxb_pitch"));
    s.fxb_low.store(num(o, "fxb_low")); s.fxb_mid.store(num(o, "fxb_mid")); s.fxb_high.store(num(o, "fxb_high"));
    s.fxb_bypass.store(num(o, "fxb_bypass") > 0.5f ? 1 : 0);
    s.fxb_slot.store(std::max(0, std::min(3, int(num(o, "fxb_slot") + 0.5f))));   // 2.4.33
    for (int k = 0; k < 3; k++) s.fxb_xprog[k].store(int(num(o, FXB_XPROG[k]) + 0.5f));
    s.d_attack.store(o->d_env[0].load()); s.d_decay.store(o->d_env[1].load());
    s.d_sustain.store(o->d_env[2].load()); s.d_release.store(o->d_env[3].load());
    for (int i = 0; i < 2; i++) {
        char k[24];
        std::snprintf(k, sizeof k, "lfo%d_wave", i + 1); s.lfo_wave[i].store(int(num(o, k) + 0.5f));
        std::snprintf(k, sizeof k, "lfo%d_sync", i + 1); s.lfo_sync[i].store(int(num(o, k) + 0.5f));
        std::snprintf(k, sizeof k, "lfo%d_retrig", i + 1); s.lfo_retrig[i].store(int(num(o, k) + 0.5f));
        std::snprintf(k, sizeof k, "lfo%d_rate", i + 1); s.lfo_rate[i].store(lfo_hz(num(o, k)));
    }
    for (int i = 0; i < MOD_SLOTS; i++) {
        char k[24];
        std::snprintf(k, sizeof k, "mod%d_src", i + 1); s.mod_src[i].store(int(num(o, k) + 0.5f));
        std::snprintf(k, sizeof k, "mod%d_dst", i + 1); s.mod_dst[i].store(int(num(o, k) + 0.5f));
        std::snprintf(k, sizeof k, "mod%d_amt", i + 1); s.mod_amt[i].store(num(o, k) / 100.0f);
        std::snprintf(k, sizeof k, "mod%d_slot", i + 1); s.mod_layer[i].store(int(num(o, k) + 0.5f));
    }
    for (int i = 0; i < 4; i++) {
        char k[24];
        std::snprintf(k, sizeof k, "slot%d_lo", i + 1); int lo = int(num(o, k) + 0.5f);
        std::snprintf(k, sizeof k, "slot%d_hi", i + 1); int hi = int(num(o, k) + 0.5f);
        s.slot_lo[i].store(std::min(lo, hi)); s.slot_hi[i].store(std::max(lo, hi));
        std::snprintf(k, sizeof k, "slot%d_vol", i + 1); s.slot_gain[i].store(std::pow(10.0f, num(o, k) / 20.0f));
        std::snprintf(k, sizeof k, "slot%d_tune", i + 1); s.slot_tune[i].store(num(o, k));
        std::snprintf(k, sizeof k, "slot%d_mute", i + 1); s.slot_mute[i].store(num(o, k) > 0.5f ? 1 : 0);
        std::snprintf(k, sizeof k, "slot%d_shift", i + 1); s.slot_shift[i].store(int(std::lround(num(o, k))));
        std::snprintf(k, sizeof k, "slot%d_pan", i + 1); s.slot_pan[i].store(std::max(-1.0f, std::min(1.0f, num(o, k) / 100.0f)));
        std::snprintf(k, sizeof k, "slot%d_play", i + 1); s.slot_play[i].store(std::max(0, std::min(3, int(num(o, k) + 0.5f))));
        std::snprintf(k, sizeof k, "slot%d_inv", i + 1); s.slot_inv[i].store(num(o, k) > 0.5f ? 1 : 0);
    }
    s.rev_gated.store(num(o, "rev_mode") > 0.5f ? 1 : 0);   // 2.1.16
    s.layer_mode.store(int(num(o, "layer_mode") + 0.5f));
    s.ks_base.store(int(num(o, "ks_base") + 0.5f));
    {   // 2.3: the SYNTH layer
        SynthSettings &y = o->sampler.synth.settings;
        y.on.store(num(o, "syn_on") > 0.5f ? 1 : 0);
        y.level.store(std::pow(10.0f, num(o, "syn_level") / 20.0f));
        y.polyphony.store(std::max(1, std::min(12, int(num(o, "syn_poly") + 0.5f))));
        y.attack.store(num(o, "syn_attack") / 1000.0f);
        y.decay.store(num(o, "syn_decay") / 1000.0f);
        y.sustain.store(num(o, "syn_sustain") / 100.0f);
        y.release.store(num(o, "syn_release") / 1000.0f);
        y.bend_range.store(num(o, "syn_bend"));   // 2.3.3: the synth's own (it followed SETUP's)
        y.vel_sens.store(num(o, "syn_vel") / 100.0f);
        {   // 2.3.3: VOICE
            SynthVoiceSettings &vc = y.voice;
            vc.mode.store(int(num(o, "syn_vmode") + 0.5f));
            vc.glide.store(num(o, "syn_glide") / 1000.0f);
            vc.glide_legato.store(num(o, "syn_glide_mode") > 0.5f ? 1 : 0);
            vc.key_lo.store(int(num(o, "syn_key_lo") + 0.5f));
            vc.key_hi.store(int(num(o, "syn_key_hi") + 0.5f));
            vc.transpose.store(int(std::lround(num(o, "syn_transpose"))));
            vc.drift.store(num(o, "syn_drift") / 100.0f);
        }
        {   // 2.3.1: MIX / FILTER
            SynthMixSettings &mx = y.mix;
            mx.sub_on.store(num(o, "syn_sub_on") > 0.5f ? 1 : 0);
            mx.sub_oct.store(num(o, "syn_sub_oct") > 0.5f ? 2 : 1);
            mx.sub_level.store(num(o, "syn_sub_level") / 100.0f);
            mx.sub_pan.store(num(o, "syn_sub_pan") / 100.0f);
            mx.noise_on.store(num(o, "syn_noise_on") > 0.5f ? 1 : 0);
            mx.noise_color.store(int(num(o, "syn_noise_color") + 0.5f));
            mx.noise_key.store(num(o, "syn_noise_key") > 0.5f ? 1 : 0);
            mx.noise_level.store(num(o, "syn_noise_level") / 100.0f);
            mx.noise_pan.store(num(o, "syn_noise_pan") / 100.0f);
            mx.noise_tone.store(num(o, "syn_noise_tone") / 100.0f);
            mx.fm.store(num(o, "syn_fm") / 100.0f);
            mx.feedback.store(num(o, "syn_feedback") / 100.0f);
            mx.ring.store(num(o, "syn_ring") / 100.0f);
            mx.balance.store(num(o, "syn_balance") / 100.0f);
            mx.sync.store(num(o, "syn_sync") > 0.5f ? 1 : 0);
            mx.flt_on.store(num(o, "syn_flt_on") > 0.5f ? 1 : 0);
            mx.flt_type.store(int(num(o, "syn_flt_type") + 0.5f));
            mx.cutoff.store(syn_cutoff_hz(num(o, "syn_cutoff")));
            mx.res.store(num(o, "syn_res") / 100.0f);
            mx.drive.store(num(o, "syn_drive") / 100.0f);
            mx.keytrack.store(num(o, "syn_keytrack") / 100.0f);
            mx.env_amt.store(num(o, "syn_fenv") / 100.0f);
            mx.vel.store(num(o, "syn_fvel") / 100.0f);
            mx.fat.store(num(o, "syn_fat") / 100.0f);
            mx.mix.store(num(o, "syn_fmix") / 100.0f);
            mx.route.store((num(o, "syn_fr_a") > 0.5f ? 1 : 0) | (num(o, "syn_fr_b") > 0.5f ? 2 : 0) |
                           (num(o, "syn_fr_sub") > 0.5f ? 4 : 0) | (num(o, "syn_fr_noise") > 0.5f ? 8 : 0));
            mx.pan.store(num(o, "syn_pan") / 100.0f);
            mx.fe_attack.store(num(o, "syn_fe_attack") / 1000.0f);
            mx.fe_decay.store(num(o, "syn_fe_decay") / 1000.0f);
            mx.fe_sustain.store(num(o, "syn_fe_sustain") / 100.0f);
            mx.fe_release.store(num(o, "syn_fe_release") / 1000.0f);
        }
        {   // 2.3.2: ENV / LFO
            SynthModSettings &md = y.mod;
            md.me_on.store(num(o, "syn_me_on") > 0.5f ? 1 : 0);
            md.me_attack.store(num(o, "syn_me_attack") / 1000.0f);
            md.me_decay.store(num(o, "syn_me_decay") / 1000.0f);
            md.me_sustain.store(num(o, "syn_me_sustain") / 100.0f);
            md.me_release.store(num(o, "syn_me_release") / 1000.0f);
            md.me_dest.store(int(num(o, "syn_me_dest") + 0.5f));
            md.me_amt.store(num(o, "syn_me_amt") / 100.0f);
            for (int k = 0; k < SYN_XENVS; k++) {   // 2.4.17: MOD ENV 2 / 3 (syn_me2_*, syn_me3_*)
                char key[32];
                auto ev = [&](const char *n) { std::snprintf(key, sizeof key, "syn_me%d_%s", k + 2, n); return num(o, key); };
                SynthModEnvSettings &xs = md.mex[k];
                xs.on.store(ev("on") > 0.5f ? 1 : 0);
                xs.attack.store(ev("attack") / 1000.0f);
                xs.decay.store(ev("decay") / 1000.0f);
                xs.sustain.store(ev("sustain") / 100.0f);
                xs.release.store(ev("release") / 1000.0f);
                xs.dest.store(int(ev("dest") + 0.5f));
                xs.amt.store(ev("amt") / 100.0f);
            }
            for (int k = 0; k < SYN_LFOS; k++) {   // 2.4.17: 4 (was 2)
                char key[32];
                auto lv = [&](const char *n) { std::snprintf(key, sizeof key, "syn_lfo%d_%s", k + 1, n); return num(o, key); };
                SynthLfoSettings &ls = md.lfo[k];
                ls.shape.store(int(lv("shape") + 0.5f));
                ls.rate_hz.store(syn_lfo_hz(lv("rate")));
                ls.sync.store(int(lv("sync") + 0.5f));
                ls.trig.store(int(lv("trig") + 0.5f));
                {   // 2.3.6: a route (AMP / FILTER / MOD key) wins over the older destination
                    const int route = int(lv("route") + 0.5f), me = int(num(o, "syn_me_dest") + 0.5f);
                    ls.dest.store(route == 1 ? int(SD_LEVEL) : route == 2 ? int(SD_CUTOFF) : route == 3 ? me : int(lv("dest") + 0.5f));
                }
                ls.depth.store(lv("depth") / 100.0f);
            }
            for (int k = 0; k < SYN_ROUTES; k++) {   // 2.3.3: the MATRIX
                char key[32];
                std::snprintf(key, sizeof key, "syn_mx%d_src", k + 1); md.mx_src[k].store(int(num(o, key) + 0.5f));
                std::snprintf(key, sizeof key, "syn_mx%d_dst", k + 1); md.mx_dst[k].store(int(num(o, key) + 0.5f));
                std::snprintf(key, sizeof key, "syn_mx%d_amt", k + 1); md.mx_amt[k].store(num(o, key) / 100.0f);
            }
            for (int k = 0; k < 4; k++) {
                char key[32];
                std::snprintf(key, sizeof key, "syn_macro%d", k + 1); md.macro[k].store(num(o, key) / 100.0f);
                std::snprintf(key, sizeof key, "syn_macro%d_dest", k + 1); md.macro_dest[k].store(int(num(o, key) + 0.5f));
            }
        }
        for (int k = 0; k < SYN_OSCS; k++) {
            SynthOscSettings &os = y.osc[k];
            auto v = [&](const char *name) { char key[24]; std::snprintf(key, sizeof key, "s%c_%s", 'a' + k, name); return num(o, key); };
            os.on.store(v("on") > 0.5f ? 1 : 0);
            os.type.store(int(v("type") + 0.5f));
            os.unison.store(std::max(1, std::min(SYN_MAX_UNISON, int(v("unison") + 0.5f))));
            os.phase_mode.store(int(v("pmode") + 0.5f));
            os.warp_mode.store(int(v("wmode") + 0.5f));
            os.octave.store(int(std::lround(v("oct"))));
            os.semi.store(int(std::lround(v("semi"))));
            os.pos.store(v("pos") / 100.0f);
            os.detune.store(v("detune") / 100.0f);
            os.spread.store(v("spread") / 100.0f);
            os.warp.store(v("warp") / 100.0f);
            os.pan.store(v("pan") / 100.0f);
            os.level.store(v("level") / 100.0f);
            os.fine.store(v("fine"));
            os.phase.store(v("phase"));
            os.rand.store(v("rand") / 100.0f);
        }
    }
    o->loader.set_target(int(num(o, "target_slot") + 0.5f));
    o->loader.set_auto_loop(num(o, "auto_loop") > 0.5f);
    // 2.4.14 (user): Omni's samples in total 512 MB by default; at most 1024 MB on a 32-bit MPC (Live II: 2 GB, about 1 GB
    // free with MPC running), 2048 MB on a 64-bit one (Live III: 8 GB). The knob shows the value in force.
    {
        const int mi = num_index("mem_limit");
        const float mb = std::max(64.0f, std::min(float(MEM_MAX_MB), num(o, "mem_limit")));
        if (mi >= 0 && o->values[mi].load() != mb) { o->values[mi].store(mb); mark_changed(o, mi); }
        o->loader.set_budget_mb(int(mb));
    }
    update_extract(o);
}

// 1.8.2: note locks. A note's lock into the engine (lock_mutex held); the audio thread reads it at note-on
void commit_lock(Omni *o, int note) {
    const float *r = o->lock_ui[note];
    Settings::NoteLock &L = o->sampler.settings.lock[note];
    bool on = false;
    for (int k = 0; k < LOCK_N; k++) on = on || r[k] != 0;
    L.on.store(0);
    L.mode.store(std::max(0, std::min(4, int(std::lround(r[0])))));
    L.inv.store(std::max(0, std::min(2, int(std::lround(r[1])))));
    L.semis.store(r[2] + r[3] / 100.0f);
    L.start.store(std::max(0.0f, std::min(95.0f, r[4])) / 100.0f);
    L.cut.store(std::max(-100.0f, std::min(100.0f, r[5])) * 0.04f);
    L.gain.store(std::pow(10.0f, r[6] / 20.0f));
    L.pan.store(std::max(-1.0f, std::min(1.0f, r[7] / 100.0f)));
    L.vel.store(std::max(0, std::min(127, int(std::lround(r[8])))));
    L.on.store(on ? 1 : 0);
}
int lock_note(Omni *o) { return std::max(0, std::min(127, int(std::lround(num(o, "lock_note"))))); }
bool lock_key(const char *key) {
    for (const char *k : LOCK_KEYS) if (!std::strcmp(key, k)) return true;
    return false;
}
std::atomic<uint32_t> lock_rev{0};   // NOTE LOCK's text: changes with the note shown and its lock
// the NOTE LOCK controls show the selected note's lock
void show_lock(Omni *o) {
    std::lock_guard<std::mutex> lock(o->lock_mutex);
    int n = lock_note(o);
    for (int k = 0; k < LOCK_N; k++) o->values[num_index(LOCK_KEYS[k])].store(o->lock_ui[n][k]);
    lock_rev.fetch_add(1);
}
// a NOTE LOCK control moved: the selected note's lock
void take_lock(Omni *o) {
    std::lock_guard<std::mutex> lock(o->lock_mutex);
    int n = lock_note(o);
    for (int k = 0; k < LOCK_N; k++) o->lock_ui[n][k] = num(o, LOCK_KEYS[k]);
    commit_lock(o, n);
    lock_rev.fetch_add(1);
}
// a note played (a key, a pad, the sequencer): the pads move to show it and NOTE LOCK edits it, while FOLLOW is on
// (the display thread)
void follow_notes(Omni *o) {
    int np = o->sampler.notes_played.load();
    if (np == o->follow_seen) return;
    o->follow_seen = np;
    int note = o->sampler.last_note.load();
    if (num(o, "pad_follow") < 0.5f || note < 0 || note > 127) return;
    int pb = num_index("pad_base"), base = int(std::lround(o->values[pb].load()));
    if (note < base || note > base + 15) {   // whole banks of 16 from where the pads are, so pad 1 stays on its row
        int nb = base + 16 * int(std::floor((note - base) / 16.0));
        o->values[pb].store(float(std::max(0, std::min(112, nb))));
        o->pad_base_rev.fetch_add(1);
    }
    if (lock_note(o) != note) {
        o->values[num_index("lock_note")].store(float(note));
        show_lock(o);
    }
}

// The target slot's preset envelope with the older offset settings (attack +, decay %, sustain %, release +) applied:
// what the envelope faders show when they are not moved (s; sustain 0..1)
void env_reference(Omni *o, float ref[4]) {
    LoadedInfo li = o->loader.info();
    ref[0] = float(li.env_attack) + num(o, "attack") / 1000.0f;
    ref[1] = float(li.env_decay) * num(o, "decay") / 100.0f;
    ref[2] = std::max(0.0f, std::min(1.0f, float(li.env_sustain) * num(o, "sustain") / 100.0f));
    ref[3] = float(li.env_release) + num(o, "release") / 1000.0f;
}
const char *const ENV_KEYS[4] = {"env_attack", "env_decay", "env_sustain", "env_release"};
// the faders show the reference plus how far they are moved
void env_show(Omni *o) {
    float ref[4];
    env_reference(o, ref);
    for (int k = 0; k < 4; k++) {
        float v = ref[k] + o->d_env[k].load();
        o->values[num_index(ENV_KEYS[k])].store(k == 2 ? std::max(0.0f, std::min(100.0f, v * 100.0f)) : env_units(v * 1000.0f));
    }
}
// the faders were moved (or restored): how far that is from the reference
void env_take(Omni *o) {
    float ref[4];
    env_reference(o, ref);
    for (int k = 0; k < 4; k++) {
        float v = num(o, ENV_KEYS[k]);
        // a fader at its top with a preset time beyond its range (a 90 s decay): the preset's time, not the fader's
        if (k != 2 && v >= 999.5f && ref[k] * 1000.0f >= ENV_MAX_MS) { o->d_env[k].store(0); continue; }
        o->d_env[k].store((k == 2 ? v / 100.0f : env_ms(v) / 1000.0f) - ref[k]);
    }
    apply_settings(o);
}
// a load is playing (loader thread): a fresh preset shows its own envelope (and leaves the older offsets neutral unless it
// brought settings); a project, patch or saved instrument keeps the envelope it stored
void env_loaded(Omni *o, int slot, bool fresh, bool had_settings) {
    if (slot != o->loader.target()) return;
    if (!fresh || had_settings) {
        if (o->env_pending.exchange(false)) { env_take(o); return; }
    } else {
        o->env_pending.store(false);
        o->values[num_index("attack")].store(0); o->values[num_index("decay")].store(100);
        o->values[num_index("sustain")].store(100); o->values[num_index("release")].store(0);
    }
    for (auto &d : o->d_env) d.store(0);
    env_show(o);
    apply_settings(o);
}

// ---------------------------------------------------------------------------------------------------------------
// browser


bool is_dir_host(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// a ROM chip dump named after its board position (MAME: "x.ic30", "x.u5"): the ROM readers know them by content
bool chip_ext(const std::string &ext) {
    size_t n = ext[0] == 'u' ? 1 : ext.compare(0, 2, "ic") == 0 ? 2 : 0;
    if (!n || ext.size() == n || ext.size() > n + 3) return false;
    for (size_t i = n; i < ext.size(); i++) if (!std::isdigit(static_cast<unsigned char>(ext[i]))) return false;
    return true;
}

bool listable_ext(const std::string &ext) {
    if (ext.empty()) return false;
    if (chip_ext(ext) || ext == "rom") return true;   // .rom: Munt's / MAME's whole-ROM dumps (MT-32, CM-32L)
    // every reader's extensions, gathered once (asked for each file of a folder: expansions hold thousands)
    static const std::set<std::string> all = [] {
        std::set<std::string> s;
        for (auto &f : formats()) {
            std::istringstream in(f.exts);
            std::string e;
            while (in >> e) s.insert(e);
        }
        return s;
    }();
    return all.count(ext) > 0;
}

bool image_ext(const std::string &ext) {
    static const char *exts[] = {"iso", "img", "bin", "cdr", "hda", "hdf", "hds", "out", "sdk", "hfe", "imd", "dsk",
                                 "ima", "raw", "toc", "cue", "e3", "eiv", "e4", "ei", "em", "emu", "akai", "dmg", "nrg", "mdf",
                                 "omnisphere", "zip", "vitalbank", "dslibrary"};   // 2.4.30: Decent Sampler libraries (ZIP)   // 2.4.11: Vital banks (ZIP)   // 2.4.10: library packages + ZIP mounted fine but were never listed
    for (auto *e : exts) if (ext == e) return true;
    return false;
}

std::string short_path(Omni *o, const std::string &p) {
    std::string lib = library_dir(o);
    if (!lib.empty() && p.compare(0, lib.size(), lib) == 0) return "Library" + p.substr(lib.size());
    if (on_user_drive(p)) return "Internal" + p.substr(user_drive().size());
    if (p.compare(0, 7, "/sdcard") == 0) return "Internal" + p.substr(7);
    if (p.compare(0, 6, "/media") == 0) return p.substr(6);
    return p;
}

int max_top(const Omni *o) { return std::max(0, int(o->rows.size()) - ROWS); }
void rebuild_on(Omni *o);
// scroll to row t (clamped); the fader follows
void set_top(Omni *o, int t) {
    int m = max_top(o);
    o->top = std::max(0, std::min(m, t));
    rebuild_on(o);
}

void rebuild_on(Omni *o) {
    int m = max_top(o);
    if (o->top > m) o->top = m;
    if (o->cur >= int(o->rows.size())) o->cur = o->rows.empty() ? -1 : int(o->rows.size()) - 1;
    o->scroll_pos.store(m ? 1.0f - float(o->top) / float(m) : 1.0f);
    LoadedInfo li = o->loader.info();
    for (int r = 0; r < ROWS; r++) {
        size_t i = size_t(o->top + r);
        int on = 0;
        if (i < o->rows.size()) {
            const Row &row = o->rows[i];
            // the tapped row, or what is loaded (an extracted instrument: its source)
            const std::string &lp = li.source.empty() ? li.path : li.source;
            int lpre = li.source.empty() ? li.preset : li.source_preset;
            if (row.kind == RowKind::Preset) on = (row.path == o->sel_path && row.preset == o->sel_preset) || (row.path == lp && row.preset == lpre);
            else if (row.kind == RowKind::File) on = row.path == o->sel_path || row.path == lp;
            if (o->cur >= 0) on = int(i) == o->cur;   // one box: once the highlight is in use, only it is lit
        }
        o->row_on[r].store(on);
    }
    o->ui_rev.fetch_add(1);
}

// A row's text: names longer than a row holds (about 64 characters at the list's text size) lose their middle, so
// both ends show ("Ensoniq ASR10 - Essential ... & EDA_ 1"): series of folders often differ only at the end.
// what the SELECTED box says: a tapped name (in full), a load error, or a browser message (get_param: ui_mutex held)
std::string browse_info(Omni *o) {
    if (o->br_queue_dropped.load()) return "Browser busy: action not queued - try again";
    if (o->br_busy.load()) return "Opening...";
    std::string st = o->loader.status();   // a failed load is shown here, where the tap was made
    if (st.compare(0, 6, "Error:") == 0 || st.compare(0, 7, "Extract") == 0 || st.compare(0, 4, "Save") == 0 ||
        st.compare(0, 7, "Nothing") == 0 || st.compare(0, 9, "No Plugin") == 0)   // (a kit whose samples are missing still plays its preview)
        return st + (st.compare(0, 6, "Error:") == 0 && o->sampler.previewing.load() ? " - preview playing" : "");
    return o->browse_msg.empty() ? o->info_text : o->browse_msg;
}

// 1.7.3: text wrapped into at most `lines` lines of `width` characters (UTF-8), breaking after spaces, "_", "-" or "/"
// where it can; what still doesn't fit loses the middle of the last line, so the end shows
constexpr size_t SEL_CHARS = 54;
std::vector<std::string> wrap_lines(const std::string &s, size_t width, size_t lines) {
    std::vector<size_t> st;   // UTF-8 character starts
    for (size_t i = 0; i < s.size(); i++) if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) st.push_back(i);
    st.push_back(s.size());
    size_t n = st.size() - 1, c = 0;
    std::vector<std::string> out;
    while (c < n && out.size() + 1 < lines) {
        if (n - c <= width) break;
        size_t e = c + width;   // the break: after the last separator in the line, else a hard break
        for (size_t j = c + width; j > c + width / 2; j--) {
            char ch = s[st[j - 1]];
            if (ch == ' ' || ch == '_' || ch == '-' || ch == '/') { e = j; break; }
        }
        std::string line = s.substr(st[c], st[e] - st[c]);
        while (line.size() > 1 && line.back() == ' ') line.pop_back();
        out.push_back(line);
        c = e;
        while (c < n && s[st[c]] == ' ') c++;
    }
    if (c < n) {
        if (n - c <= width) out.push_back(s.substr(st[c]));
        else {
            size_t head = width / 2 - 2, tail = width - head - 3;
            out.push_back(s.substr(st[c], st[c + head] - st[c]) + "..." + s.substr(st[n - tail]));
        }
    }
    if (out.empty()) out.push_back(" ");
    return out;
}

// the folder line: long paths keep their start and, mostly, their end (the folder shown)
std::string fit_loc(const std::string &s) {
    constexpr size_t MAX = 72, HEAD = 20;
    std::vector<size_t> starts;   // UTF-8 character starts
    for (size_t i = 0; i < s.size(); i++) if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) starts.push_back(i);
    if (starts.size() <= MAX) return s;
    size_t tail = MAX - HEAD - 3;
    return s.substr(0, starts[HEAD]) + "..." + s.substr(starts[starts.size() - tail]);
}

std::string fit_row(const std::string &s) {
    constexpr size_t MAX = 64, HEAD = 36;
    std::vector<size_t> starts;   // UTF-8 character starts
    for (size_t i = 0; i < s.size(); i++) if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) starts.push_back(i);
    if (starts.size() <= MAX) return s;
    size_t tail = MAX - HEAD - 3;
    return s.substr(0, starts[HEAD]) + "..." + s.substr(starts[starts.size() - tail]);
}

void list_drives(Omni *o) {
    o->rows.clear();
    o->dir.clear();
    o->group.clear();
    o->preset_file.clear();
    o->top = 0;
    if (o->cur >= 0) o->cur = 0;
    std::string lib = library_dir(o);   // listed only while its drive is connected
    if (!lib.empty()) o->rows.push_back({"Plugin Library", lib, RowKind::Drive});
    std::string user = user_drive();   // official package: the Live III's internal drive in place of its empty /sdcard
    if (!user.empty()) o->rows.push_back({"Internal", user, RowKind::Drive});
    else if (is_dir_host("/sdcard")) o->rows.push_back({"Internal", "/sdcard", RowKind::Drive});
    std::vector<DirEntry> media;
    try { media = o->vfs.list("/media"); } catch (const std::exception &) {}
    // MPC OS's own mounts (the internal storage again, the system data, read-only content) are not drives to browse
    for (auto &m : media) if (m.dir && external_path("/media/" + m.name)) o->rows.push_back({m.name, "/media/" + m.name, RowKind::Drive});
    const char *extra = std::getenv("OMNI_SAMPLER_ROOT");   // tests and desktop builds
    if (extra && *extra) o->rows.push_back({extra, extra, RowKind::Drive});
    rebuild_on(o);
}

// 1.8: the type group of a listable file, for folders that mix instruments with loose samples (MPC expansions:
// hundreds of programs among thousands of WAVs). MPC programs are told apart by their Program type, read once.
const char SAMPLES_GROUP[] = "Samples";
std::string file_group(Omni *o, const std::string &full, const std::string &ext) {
    static const char *const AUDIO[] = {"wav", "wave", "aif", "aiff", "aifc", "flac", "ogg", "ncw"};
    for (const char *a : AUDIO) if (ext == a) return SAMPLES_GROUP;
    if (ext == "omnipatch") return "Patches";
    if (ext == "xpm") {
        auto it = o->xpm_kinds.find(full);
        if (it != o->xpm_kinds.end()) return it->second;
        std::string kind = "Programs";
        try {
            std::vector<uint8_t> h = o->vfs.open(full)->head(4096);
            std::string s(h.begin(), h.end());
            size_t at = s.find("Program type=\"");
            if (at != std::string::npos) {
                std::string t = lower(s.substr(at + 14, 8));
                if (!t.compare(0, 8, "keygroup")) kind = "Keygroup Programs";
                else if (!t.compare(0, 4, "drum")) kind = "Drum Programs";
            }
        } catch (const std::exception &) {}
        if (o->xpm_kinds.size() > 20000) o->xpm_kinds.clear();
        return o->xpm_kinds[full] = kind;
    }
    std::string up = ext;
    for (char &c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
    return up + " Files";
}

// A folder's listable files by type group, in the order the groups are shown; grouped only when the folder mixes
// instruments with samples (a plain sample folder or instrument folder is listed as it is)
struct FolderGroups {
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;   // group -> file names
    bool mixed = false;
};
FolderGroups folder_groups(Omni *o, const std::string &dir, const std::vector<DirEntry> &entries) {
    FolderGroups fg;
    std::map<std::string, size_t> at;
    for (auto &e : entries) {
        if (e.dir || e.name.empty() || e.name[0] == '.') continue;
        std::string full = path_join(dir, e.name), ext = path_ext(e.name);
        if (!(listable_ext(ext) || ext == "omnipatch") || o->vfs.view_name(full)) continue;
        std::string g = file_group(o, full, ext);
        auto it = at.find(g);
        if (it == at.end()) { it = at.emplace(g, fg.groups.size()).first; fg.groups.push_back({g, {}}); }
        fg.groups[it->second].second.push_back(e.name);
    }
    bool samples = at.count(SAMPLES_GROUP) > 0;
    fg.mixed = samples && fg.groups.size() > 1;
    // programs first (keygroups, drums, others), other instrument types, patches, the samples last
    auto rank = [](const std::string &g) {
        if (g == "Keygroup Programs") return 0;
        if (g == "Drum Programs") return 1;
        if (g == "Programs") return 2;
        if (g == "Patches") return 4;
        if (g == SAMPLES_GROUP) return 5;
        return 3;
    };
    std::stable_sort(fg.groups.begin(), fg.groups.end(), [&](const auto &a, const auto &b) { return rank(a.first) < rank(b.first); });
    return fg;
}

// 2.1.3: the folders of dir that belong to an instrument beside them, by lower-case name: a Heat Up preset's sample folder
// (the name its intr= gives), and for any other instrument file "<name>", "<name> Samples", "<name>_Samples" (Kontakt
// .nki + "<name> Samples", EXS, SFZ, Reason ReFills...). SAMPLE FOLDERS Hide leaves them out of the list: an expansion's
// presets then stand alone, and the loaders still find the samples. Samples, disk images and folders never hide one.
std::set<std::string> companion_folders(Omni *o, const std::string &dir, const std::vector<DirEntry> &entries) {
    std::set<std::string> names, out;
    for (auto &e : entries) {
        if (e.dir || e.name.empty() || e.name[0] == '.') continue;
        std::string ext = path_ext(e.name), full = path_join(dir, e.name);
        if (!listable_ext(ext) || file_group(o, full, ext) == SAMPLES_GROUP) continue;
        std::string stem = lower(path_stem(e.name));
        names.insert(stem);
        names.insert(stem + " samples");
        names.insert(stem + "_samples");
        if (ext == "heatuppreset") {
            auto it = o->heat_folders.find(full);
            if (it == o->heat_folders.end()) {
                std::string f;
                try { f = heatup_sample_folder(o->vfs.open(full)->head(16384)); } catch (const std::exception &) {}
                if (o->heat_folders.size() > 20000) { o->heat_folders.clear(); o->sample_only.clear(); }   // 2.3.6: was clearing sample_only every time
                it = o->heat_folders.emplace(full, lower(f)).first;
            }
            if (!it->second.empty()) names.insert(it->second);
        }
    }
    // 2.1.4: Heat Up's own expansion layout keeps the presets in category folders (SAUCE3/LEADS/) and each preset's
    // samples in a folder beside those (SAUCE3/Lead_21/): a folder holding only .ignitex samples is hidden wherever it is
    // (nothing in it loads by itself; the presets still find it). Each folder is looked into once (refresh forgets)
    size_t dirs = 0;
    for (auto &e : entries) if (e.dir) dirs++;
    for (auto &e : entries) {
        if (!e.dir || e.name.empty() || e.name[0] == '.') continue;
        if (names.count(lower(e.name))) { out.insert(e.name); continue; }
        if (dirs > 4000) continue;
        std::string full = path_join(dir, e.name);
        auto it = o->sample_only.find(full);
        if (it == o->sample_only.end()) {
            bool only = false;
            try {
                std::vector<DirEntry> in = o->vfs.list(full);
                for (auto &f : in) {
                    if (f.name.empty() || f.name[0] == '.') continue;
                    if (f.dir || path_ext(f.name) != "ignitex") { only = false; break; }
                    only = true;
                }
            } catch (const std::exception &) {}
            if (o->sample_only.size() > 20000) o->sample_only.clear();
            it = o->sample_only.emplace(full, only).first;
        }
        if (it->second) out.insert(e.name);
    }
    return out;
}

// 2.4.11: an Omnisphere patch the sampler cannot play says so on its row ("  [factory]" / "  [no patch]"); each one is
// parsed (patch XML only, ~45 KB), so at most 400 per folder (budget)
std::string file_label(Omni *o, const std::string &full, const std::string &name, int &budget) {
    const std::string ext = path_ext(name);
    if ((ext != "prt_omn" && ext != "mlt_omn") || budget <= 0) return name;
    budget--;
    try {
        Location l = o->vfs.resolve(full);
        const std::string note = omnisphere_note(l.volume, l.inner);
        if (!note.empty()) return name + "  [" + note + "]";
    } catch (const std::exception &) {}
    return name;
}

void list_folder(Omni *o, const std::string &dir, const std::string &group = "") {
    std::vector<DirEntry> entries;
    try {
        entries = o->vfs.list(dir);
    } catch (const std::exception &e) {
        o->browse_msg = e.what();
        o->ui_rev.fetch_add(1);
        return;
    }
    FolderGroups fg = folder_groups(o, dir, entries);
    std::string show = fg.mixed ? group : "";
    o->dir = dir;
    save_last_browse(o, dir);   // 1.8.13: every instance starts here next time
    o->group = show;
    o->preset_file.clear();
    o->drum_set = -1;   // 2.4.11
    o->top = 0;
    if (o->cur >= 0) o->cur = 0;
    o->rows.clear();
    o->rows.push_back({"..", show.empty() ? path_dir(dir) : dir, RowKind::Up});
    int note_budget = 400;
    if (!show.empty()) {   // one type group of a mixed folder
        for (auto &g : fg.groups)
            if (g.first == show)
                for (auto &n : g.second) o->rows.push_back({file_label(o, path_join(dir, n), n, note_budget), path_join(dir, n), RowKind::File});
        o->browse_msg.clear();
        rebuild_on(o);
        return;
    }
    std::set<std::string> hide;
    if (num(o, "br_folders") < 0.5f) hide = companion_folders(o, dir, entries);
    for (auto &e : entries) {
        if (e.name.empty() || e.name[0] == '.') continue;   // hidden files, macOS "._" resource forks
#ifdef OMNI_OFFICIAL
        if (e.dir && (e.name == "$RECYCLE.BIN" || e.name == "System Volume Information")) continue;   // Windows' own, on exFAT drives
#endif
        std::string full = path_join(dir, e.name), ext = path_ext(e.name);
        if (e.dir && hide.count(e.name)) continue;          // 2.1.3: an instrument's own sample folder
        if (e.dir) o->rows.push_back({"[" + e.name + "]", full, RowKind::Folder});
        else if (const char *view = o->vfs.view_name(full))   // a file shown as folders: TRITON's manifest by its view's name
            o->rows.push_back({"[" + (ext == "csv" ? std::string(view) : e.name) + "]", full, RowKind::Folder});
        else if (listable_ext(ext) || ext == "omnipatch") { if (!fg.mixed) o->rows.push_back({file_label(o, full, e.name, note_budget), full, RowKind::File}); }
        else if (image_ext(ext)) {
            // 1.8.2: a ROM-sized .bin a reader knows by content (the SC-55mkII's chips, the SC-88Pro's 8 MB wave ROMs)
            // is a file, not a CD image
            bool rom = false;
            if (ext == "bin" && e.size <= (uint64_t(8) << 20)) {
                try { Location loc = o->vfs.resolve(full); rom = find_reader(*loc.volume, loc.inner) != nullptr; } catch (const std::exception &) {}
            }
            if (rom) { if (!fg.mixed) o->rows.push_back({e.name, full, RowKind::File}); }
            else o->rows.push_back({"<" + e.name + ">", full, RowKind::Image});
        }
    }
    if (fg.mixed)
        for (auto &g : fg.groups)
            o->rows.push_back({"[" + g.first + " (" + std::to_string(g.second.size()) + ")]", dir, RowKind::Group, 0, g.first});
    o->browse_msg.clear();
    rebuild_on(o);
}

void list_presets(Omni *o, const std::string &file, const std::vector<PresetInfo> &presets) {
    o->preset_file = file;
    o->top = 0;
    if (o->cur >= 0) o->cur = 0;
    o->rows.clear();
    o->rows.push_back({".. " + path_name(file), o->dir, RowKind::Up});
    for (size_t i = 0; i < presets.size(); i++) o->rows.push_back({presets[i].name, file, RowKind::Preset, int(i)});
    rebuild_on(o);
}

// 2.4.11 (user): a drum machine ROM asks first: per kit, WHOLE KIT (every sound on its own key from C1, one layer) or
// ONE SOUND (its sounds listed; one across the keyboard). The kits are the reader's presets whose index ends in 000
// (drum_rom.cpp: set * 1000 + sound), its sounds the rest of that thousand.
void list_drum_choice(Omni *o, const std::string &file, const std::vector<PresetInfo> &presets) {
    o->preset_file = file;
    o->drum_set = -1;
    o->drum_presets = presets;
    o->top = 0;
    if (o->cur >= 0) o->cur = 0;
    o->rows.clear();
    o->rows.push_back({".. " + path_name(file), o->dir, RowKind::Up});
    for (size_t i = 0; i < presets.size(); i++) {
        if (presets[i].index % 1000) continue;
        std::string kit = presets[i].name, n;
        const size_t p = kit.rfind(" kit (");
        if (p != std::string::npos) { n = kit.substr(p + 6, kit.size() - p - 7); kit = kit.substr(0, p); }
        o->rows.push_back({"WHOLE KIT: " + kit + (n.empty() ? "" : " - " + n + " sounds, one per key from C1"), file, RowKind::Preset, int(i)});
        Row r{"[ONE SOUND: " + kit + (n.empty() ? "" : " (" + n + ")") + "]", file, RowKind::Sounds, presets[i].index / 1000};
        o->rows.push_back(r);
    }
    o->info_text = path_name(file) + ": WHOLE KIT (one layer) or ONE SOUND";
    rebuild_on(o);
}
void list_drum_sounds(Omni *o, const std::string &file, int set) {
    o->preset_file = file;
    o->drum_set = set;
    o->top = 0;
    if (o->cur >= 0) o->cur = 0;
    o->rows.clear();
    o->rows.push_back({".. WHOLE KIT / ONE SOUND", file, RowKind::Up});
    for (size_t i = 0; i < o->drum_presets.size(); i++) {
        const PresetInfo &p = o->drum_presets[i];
        if (p.index / 1000 != set || p.index % 1000 == 0) continue;
        const size_t c = p.name.find(": ");
        o->rows.push_back({c == std::string::npos ? p.name : p.name.substr(c + 2), file, RowKind::Preset, int(i)});
    }
    o->info_text = "One sound: it plays across the keyboard, C4 at its own pitch";
    rebuild_on(o);
}

void load_patch(Omni *o, const std::string &path);
void synth_follow(Omni *o, const Settings_list &kv);   // 2.4.6, below

// A file's preview, as Native Instruments lays them out: <folder>/.previews/<file>.ogg (Maschine, Komplete libraries) or
// previews/<file>.ogg (Battery kits). Host folders only. "" = none.
std::string find_preview(const std::string &file) {
    std::string dir = path_dir(file), name = path_name(file), stem = path_stem(file);
    struct stat st;
    for (const char *sub : {".previews", "previews", ".Previews", "Previews"})
        for (const std::string &n : {name + ".ogg", stem + ".ogg"}) {
            std::string p = path_join(path_join(dir, sub), n);
            if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return p;
        }
    return "";
}
// play the tapped file's preview if it has one (stops the one playing otherwise); true if one plays
bool play_preview(Omni *o, const std::string &file) {
    std::string pv = num(o, "br_preview") > 0.5f ? find_preview(file) : std::string();
    if (pv.empty()) { o->sampler.preview(nullptr); return false; }
    try {
        PcmPtr p = decode_audio(*open_host_file(pv));
        if (!p || p->frames() < 2) { o->sampler.preview(nullptr); return false; }
        o->sampler.preview(new Pcm(*p));
        return true;
    } catch (const std::exception &) { o->sampler.preview(nullptr); return false; }
}

void tap_row(Omni *o, int index) {
    if (index < 0 || index >= int(o->rows.size())) return;
    o->cur = index;   // a tap moves the highlight there: taps and the wheel share one position
    save_browse_pos(o);   // 2.4.27
    Row row = o->rows[size_t(index)];
    switch (row.kind) {
    case RowKind::Up:
        if (!o->preset_file.empty() && o->drum_set >= 0) { list_drum_choice(o, o->preset_file, o->drum_presets); break; }   // 2.4.11
        if (!o->preset_file.empty()) { list_folder(o, o->dir, o->group); break; }
        if (!o->group.empty()) { list_folder(o, o->dir); break; }   // a type group: back to its folder
        if (o->dir.empty() || o->dir == "/" || row.path.empty()) list_drives(o);
        else {
            // leaving a drive root goes back to the drives list
            bool root = false;
            for (const char *r : {"/sdcard", "/media"}) if (o->dir == r) root = true;
            if (!o->dir.empty() && o->dir == user_drive()) root = true;   // official: not up into the system
            if (o->dir == library_dir(o) || root || (o->dir.compare(0, 7, "/media/") == 0 && std::count(o->dir.begin(), o->dir.end(), '/') == 2))
                list_drives(o);
            else list_folder(o, row.path);
        }
        break;
    case RowKind::Drive:
        if (!row.path.empty() && row.path == library_dir(o)) mkdir(row.path.c_str(), 0755);
        list_folder(o, row.path);
        break;
    case RowKind::Sounds:   // 2.4.11: a drum ROM kit's single sounds
        list_drum_sounds(o, row.path, row.preset);
        break;
    case RowKind::Group:
        list_folder(o, row.path, row.group);
        o->info_text = path_name(row.path) + ": " + row.label.substr(1, row.label.size() - 2);
        break;
    case RowKind::Folder:
    case RowKind::Image:
        list_folder(o, row.path);
        // the full name (the row may show it shortened); a disk image this sampler can't open says so
        o->info_text = row.kind == RowKind::Image && o->dir != row.path ? "Not a disk image this sampler reads" : path_name(row.path);
        break;
    case RowKind::File: {
        // a kit's preview plays at once: before it loads (and even when its samples are missing)
        bool previewing = play_preview(o, row.path);
        struct PreviewNote { Omni *o; bool on; ~PreviewNote() { if (on) o->info_text += " - preview playing"; } } note{o, previewing};
        if (path_ext(row.path) == "omnipatch") {
            o->sel_path = row.path;
            o->sel_preset = 0;
            load_patch(o, row.path);
            rebuild_on(o);
            break;
        }
        std::vector<PresetInfo> presets;
        const char *r_name = "";
        try {
            Location loc = o->vfs.resolve(row.path);
            const FormatReader *r = find_reader(*loc.volume, loc.inner);
            if (!r) { o->info_text = "Not readable: " + path_name(row.path); break; }
            r_name = r->name;
            presets = r->list(loc.volume, loc.inner);
            o->info_text = path_name(row.path) + " - " + r->name + ": " + std::to_string(presets.size()) +
                           (presets.size() == 1 ? " instrument" : " instruments");
        } catch (const std::exception &e) {
            o->info_text = e.what();
            break;
        }
        o->sel_path = row.path;
        o->sel_preset = 0;
        if (std::string(r_name) == "Drum machine ROM" && !presets.empty()) list_drum_choice(o, row.path, presets);   // 2.4.11
        else if (presets.size() > 1) list_presets(o, row.path, presets);
        else if (presets.size() == 1) o->loader.request(row.path, 0);
        rebuild_on(o);
        break;
    }
    case RowKind::Preset:
        o->sel_path = row.path;
        o->sel_preset = row.preset;
        o->loader.request(row.path, row.preset);
        rebuild_on(o);
        break;
    }
    o->ui_rev.fetch_add(1);
}

// Step through the presets of the loaded file, then on through the loadable files of its folder.
// 1.8.x: the slot's < > arrows move the highlight onto the row they load (a preset of the file listed, or a file of the
// folder shown), so what is highlighted and what is loaded stay the same; the list scrolls to show it (ui_mutex held)
void follow_highlight(Omni *o, const std::string &path, int preset) {
    for (int i = 0; i < int(o->rows.size()); i++) {
        const Row &r = o->rows[size_t(i)];
        bool hit = preset >= 0 ? r.kind == RowKind::Preset && r.path == path && r.preset == preset
                               : r.kind == RowKind::File && r.path == path;
        if (!hit) continue;
        o->cur = i;
        if (i < o->top) set_top(o, i);
        else if (i >= o->top + ROWS) set_top(o, i - ROWS + 1);
        else rebuild_on(o);
        save_browse_pos(o);   // 2.4.27
        return;
    }
}

void step_program(Omni *o, int delta) {
    LoadedInfo li = o->loader.info();
    if (li.path.empty()) return;
    int n = int(li.presets.size());
    // an extracted instrument steps through its source (the disk image's presets / folder) when that is still there
    if (!li.source.empty()) {
        bool reachable = false;
        try { reachable = o->vfs.resolve(li.source).volume->exists(o->vfs.resolve(li.source).inner); } catch (const std::exception &) {}
        if (reachable) { li.path = li.source; li.preset = li.source_preset; n = std::max(1, li.source_presets); }
    }
    if (n > 1) {
        int p = (li.preset + delta + n) % n;
        o->loader.request(li.path, p);
        follow_highlight(o, li.path, p);
        return;
    }
    std::vector<DirEntry> entries;
    try { entries = o->vfs.list(path_dir(li.path)); } catch (const std::exception &) { return; }
    std::vector<std::string> files;
    FolderGroups fg = folder_groups(o, path_dir(li.path), entries);
    if (fg.mixed) {   // 1.8: a folder of programs and samples steps through the loaded file's own type
        std::string g = file_group(o, li.path, path_ext(li.path));
        for (auto &gr : fg.groups)
            if (gr.first == g)
                for (auto &n : gr.second) if (listable_ext(path_ext(n))) files.push_back(path_join(path_dir(li.path), n));
    } else
        for (auto &e : entries) if (!e.dir && listable_ext(path_ext(e.name))) files.push_back(path_join(path_dir(li.path), e.name));
    if (files.empty()) return;
    auto it = std::find(files.begin(), files.end(), li.path);
    int i = it == files.end() ? 0 : int(it - files.begin());
    i = (i + delta + int(files.size())) % int(files.size());
    o->loader.request(files[size_t(i)], 0);
    follow_highlight(o, files[size_t(i)], -1);
}

// ---------------------------------------------------------------------------------------------------------------
// instrument settings: the sound parameters an extracted instrument remembers (not the slots, pads or system)

// 1.8: mod1_slot .. mod8_slot
bool mod_route_slot(const char *key) { return !std::strncmp(key, "mod", 3) && std::isdigit(static_cast<unsigned char>(key[3])) && !std::strcmp(key + 4, "_slot"); }

bool sound_param(const char *key) {
    static const char *const NOT[] = {"mem_limit", "prog_change", "auto_extract", "pad_vel", "vel_curve", "pad_base", "target_slot", "layer_mode",
                                      "ks_base", "auto_loop", "info_page", "br_preview", "skin", "br_folders", "mut_amount",
                                      "arp_v_mode", "arp_v_span", "fxb_pad"};   // 2.4.32: fxb_pad
    for (const char *n : NOT) if (!std::strcmp(key, n)) return false;
    if (!std::strncmp(key, "lock_", 5) || !std::strcmp(key, "pad_follow")) return false;   // 1.8.2: the project's own
    // 2.4.29 (GRID audit F5): the GRID belongs to the project, not to a sound: an instrument / patch saved while it ran
    // carried arp_on, rate, scale, key, page ... and loading it started or re-keyed the sequencer (no reader sets arp_*)
    if (!std::strncmp(key, "arp_", 4)) return false;
    if (mod_route_slot(key)) return false;   // which slot a route acts on: a patch (all four slots) keeps it
    return std::strncmp(key, "slot", 4) != 0;
}

// a patch also keeps the slots' key ranges, levels and tuning and the layer mode
bool patch_param(const char *key) {
    if (sound_param(key)) return true;
    if (!std::strcmp(key, "layer_mode") || !std::strcmp(key, "ks_base") || mod_route_slot(key)) return true;
    return !std::strncmp(key, "slot", 4) && std::isdigit(static_cast<unsigned char>(key[4]));
}

bool is_vital_ref(const std::string &s);   // 2.4.13, below
char syn_pre(int k);
// (syn_mutex held) no project / preset file on the oscillators, no CUSTOM shapes: the caller requests the loads
void syn_clear_refs(Omni *o) {
    for (int k = 0; k < SYN_OSCS; k++) { o->syn_file[k].clear(); o->syn_pending[k] = false; }
    for (auto &L : o->syn_lfo) { L.ref.clear(); L.step = 0; }
}
void syn_request_bits(Omni *o, int bits) {
    std::lock_guard<std::mutex> lock(o->syn_mutex);
    o->syn_req |= bits;
    o->syn_cv.notify_one();
}
constexpr int SYN_REQ_LFO1 = 32, SYN_REQ_LFO2 = 64, SYN_REQ_LFO_STEP = 128;   // syn_req bits (0-4: syn_request)
constexpr int SYN_REQ_LFO3 = 256, SYN_REQ_LFO4 = 512;   // 2.4.17
constexpr int SYN_REQ_LFO_ALL = SYN_REQ_LFO1 | SYN_REQ_LFO2 | SYN_REQ_LFO3 | SYN_REQ_LFO4;
constexpr int SYN_REQ_LFO_N[SYN_LFOS] = {SYN_REQ_LFO1, SYN_REQ_LFO2, SYN_REQ_LFO3, SYN_REQ_LFO4};
std::string settings_json(Omni *o, bool (*want)(const char *) = sound_param) {
    std::string s = "{";
    char buf[96];
    for (int i = 0; i < NUM_COUNT; i++) {
        if (!want(NUM_PARAMS[i].key)) continue;
        std::snprintf(buf, sizeof buf, "%s\"%s\":%g", s.size() > 1 ? "," : "", NUM_PARAMS[i].key, o->values[i].load());
        s += buf;
    }
    if (want("syn_lfo1_shape")) {   // 2.4.13: the text settings (formats/synth_patch.hpp "@key=value"): a Vital preset's
        // tables and CUSTOM LFO shapes, so an extracted .omni or a patch plays them too
        auto esc = [](const std::string &t) {
            std::string e;
            for (unsigned char c : t) {
                if (c == '"' || c == '\\') { e += '\\'; e += char(c); }
                else if (c < 0x20) { char u[8]; std::snprintf(u, sizeof u, "\\u%04x", c); e += u; }
                else e += char(c);
            }
            return e;
        };
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        for (int k = 0; k < SYN_OSCS; k++)
            if (is_vital_ref(o->syn_file[k])) s += std::string(s.size() > 1 ? "," : "") + "\"" + esc(std::string("@syn_file_") + syn_pre(k) + "=" + o->syn_file[k]) + "\":0";
        for (int n = 0; n < SYN_LFOS; n++) {   // 2.4.17: 4
            char key[24];
            std::snprintf(key, sizeof key, "syn_lfo%d_shape", n + 1);
            if (int(num(o, key) + 0.5f) == SYN_LFO_CUSTOM && !o->syn_lfo[n].ref.empty())
                s += std::string(s.size() > 1 ? "," : "") + "\"" + esc("@syn_lfo" + std::to_string(n + 1) + "_ref=" + o->syn_lfo[n].ref) + "\":0";
        }
    }
    return s + "}";
}

// loader thread, after an instrument carrying settings loaded: values are atomics; MPC picks them up through the
// "live" parameter polling
// 2.2: a project / patch / instrument saved before the reverb types: its reverb was Classic (Freeverb). The type's
// default is Off, so one with the reverb up gets Classic and sounds as it was saved
// (the same for the drive's and the delay's types: one with the drive / delay up gets Classic, as it was)
void type_compat(Omni *o, bool has_type, const char *amount, const char *type) {
    if (has_type || num(o, amount) <= 0) return;
    o->values[num_index(type)].store(1);
    mark_changed(o, type);
}
const char *const TYPE_COMPAT[5][2] = {{"rev_mix", "rev_type"}, {"drive", "drive_type"}, {"dly_mix", "dly_type"}, {"cho_mix", "cho_type"},
                                      {"comp", "comp_type"}};
// after a saved state's / patch's settings are stored: kv says which keys it had
template <class KV, class Key> void types_compat(Omni *o, const KV &kv, Key key) {
    for (auto &tc : TYPE_COMPAT) {
        bool has_amount = false, has_type = false;
        for (auto &p : kv) { has_amount = has_amount || key(p) == tc[0]; has_type = has_type || key(p) == tc[1]; }
        if (has_amount) type_compat(o, has_type, tc[0], tc[1]);
    }
}

// 2.4.14: a preset that names output effects (a Vital preset's, an .omni's) brings them; one that names none (a sampler
// instrument, an Omnisphere / DX7 patch) puts back the values from before the first preset that set them, where they
// still have the value a preset set (a knob the user moved since stays). Without this a Vital preset's reverb went on
// into the next instrument. `before`: the values as they were before this preset's settings were stored.
const char *const FX_KEYS[] = {"comp", "comp_type", "drive", "drive_type", "lofi_mix", "lofi_bits", "lofi_rate", "rev_mix", "rev_type",
                               "rev_size", "rev_damp", "rev_predelay", "rev_mode", "cho_mix", "cho_type", "cho_rate", "cho_depth",
                               "dly_mix", "dly_type", "dly_time", "dly_fb", "dly_tone", "dly_ping", "eq_low", "eq_mid", "eq_high",
                               "phs_mix", "phs_rate", "phs_depth", "phs_fb", "flg_mix", "flg_rate", "flg_depth", "flg_fb", "limiter",
                               "fxb_prog", "fxb_mix", "fxb_rate", "fxb_depth", "fxb_fb", "fxb_time", "fxb_tone", "fxb_drive",   // 2.4.30
                               "fxb_thr", "fxb_ratio", "fxb_pitch", "fxb_low", "fxb_mid", "fxb_high", "fxb_bypass",
                               "fxb_slot", "fxb_prog_b", "fxb_prog_c", "fxb_prog_d"};   // 2.4.33
constexpr int FX_COUNT = int(sizeof FX_KEYS / sizeof FX_KEYS[0]);
void fx_snapshot(Omni *o, float *before) {
    for (int k = 0; k < FX_COUNT; k++) { const int i = num_index(FX_KEYS[k]); before[k] = i >= 0 ? o->values[i].load() : 0; }
}
void fx_follow(Omni *o, const Settings_kv &kv, const float *before) {
    bool names = false;
    for (int k = 0; k < FX_COUNT && !names; k++) for (auto &p : kv) if (p.first == FX_KEYS[k]) { names = true; break; }
    if (names) {
        for (int k = 0; k < FX_COUNT; k++) {
            const int i = num_index(FX_KEYS[k]);
            if (i < 0) continue;
            bool named = false;
            for (auto &p : kv) if (p.first == FX_KEYS[k]) { named = true; break; }
            if (!named) continue;
            auto it = std::find_if(o->fx_by_preset.begin(), o->fx_by_preset.end(), [i](const Omni::FxSet &f) { return f.i == i; });
            if (it != o->fx_by_preset.end()) it->set = o->values[i].load();               // the first value before stays
            else o->fx_by_preset.push_back({i, o->values[i].load(), before[k]});
        }
        return;
    }
    for (auto &f : o->fx_by_preset)
        if (o->values[f.i].load() == f.set && f.set != f.before) {
            o->values[f.i].store(f.before);
            mark_changed(o, f.i);
        }
    o->fx_by_preset.clear();
}

void apply_saved_settings(Omni *o, const Settings_kv &kv) {
    float fx_before[FX_COUNT];
    fx_snapshot(o, fx_before);   // 2.4.14
    for (auto &p : kv) {
        if (!sound_param(p.first.c_str())) continue;
        if (!p.first.compare(0, 4, "env_")) o->env_pending.store(true);
        int i = num_index(p.first.c_str());
        if (i >= 0) o->values[i].store(p.second);
    }
    types_compat(o, kv, [](const std::pair<std::string, float> &p) { return p.first; });   // 2.2
    synth_follow(o, kv);   // 2.4.6
    fx_follow(o, kv, fx_before);   // 2.4.14
    apply_settings(o);
}

// browser worker (ui_mutex held): a patch's settings, then its instruments into their slots (without the settings
// their own .omni files carry: the patch's win); slots the patch does not use are cleared
void load_patch(Omni *o, const std::string &path) {
    Patch p;
    try { p = read_patch(path); } catch (const std::exception &e) { o->info_text = path_name(path) + ": " + e.what(); return; }
    if (p.layers.empty()) { o->info_text = "This patch has no instruments"; return; }
    for (auto &kv : p.settings) {
        if (!patch_param(kv.first.c_str())) continue;
        if (!kv.first.compare(0, 4, "env_")) o->env_pending.store(true);
        int i = num_index(kv.first.c_str());
        if (i >= 0) o->values[i].store(kv.second);
    }
    types_compat(o, p.settings, [](const std::pair<std::string, float> &q) { return q.first; });   // 2.2
    synth_follow(o, p.settings);   // 2.4.6
    apply_settings(o);
    bool used[SLOTS] = {};
    int missing = 0;
    for (auto &l : p.layers) {
        if (used[l.slot]) continue;
        used[l.slot] = true;
        struct stat st;
        if (stat(l.file.c_str(), &st) != 0) missing++;
        o->loader.request(l.file, l.preset, l.slot, false);
    }
    for (int s = 0; s < SLOTS; s++) if (!used[s]) o->loader.request("", 0, s, false);
    int n = int(p.layers.size());
    o->info_text = "Patch " + p.name + ": " + std::to_string(n) + (n == 1 ? " instrument" : " instruments");
    if (missing) o->info_text += ", " + std::to_string(missing) + " missing";
}

// key ranges for the loaded slots: the keyboard divided evenly, in slot order
void auto_split(Omni *o) {
    std::vector<int> loaded;
    for (int i = 0; i < SLOTS; i++) if (!o->loader.slot_info(i).path.empty()) loaded.push_back(i);
    if (loaded.size() < 2) {
        std::lock_guard<std::mutex> lock(o->ui_mutex);
        o->browse_msg = "Auto split: load sounds into two or more slots first";
        o->ui_rev.fetch_add(1);
        return;
    }
    size_t n = loaded.size();
    for (size_t k = 0; k < n; k++) {
        char key[24];
        std::snprintf(key, sizeof key, "slot%d_lo", loaded[k] + 1);
        o->values[num_index(key)].store(float(128 * k / n));
        mark_changed(o, key);   // 2.1.16: the knobs move at once
        std::snprintf(key, sizeof key, "slot%d_hi", loaded[k] + 1);
        o->values[num_index(key)].store(float(128 * (k + 1) / n - 1));
        mark_changed(o, key);
    }
    o->values[num_index("layer_mode")].store(0);
    mark_changed(o, "layer_mode");
    apply_settings(o);
}

// 2.1.16: RESET (SETUP): Omni as a new instance has it. Every slot empty, every sound, layer, mod, FX and note-lock
// setting at its default, the voices silenced. Kept: the user's system choices (skin, memory limit, auto extract,
// sample folders, kit previews, the INFO page) and the library location / browser folder (not parameters)
constexpr long long RESET_ARM_MS = 3000;
long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void show_lock(Omni *o);
void commit_lock(Omni *o, int note);
void syn_request(Omni *o, int k);   // 2.3, below
// ---- 2.4: the ARP tab ----

std::string note_name(int n);
std::string arp_dir(Omni *o) {   // <library>/MIDI (made, for the user to fill)
    std::string lib = library_dir(o);
    if (lib.empty()) return "";
    std::string d = path_join(lib, "MIDI");
    mkdir(d.c_str(), 0755);
    return d;
}

bool is_midi_file(const std::string &n) {
    std::string e = lower(n.substr(n.find_last_of('.') == std::string::npos ? n.size() : n.find_last_of('.')));
    return e == ".mid" || e == ".midi" || e == ".smf" || e == ".kar" || e == ".rmi";
}

void arp_scan_dir(Omni *o, const std::string &root, const std::string &rel, int depth, std::vector<std::string> &out) {
    std::vector<DirEntry> es;
    try { es = o->vfs.list(rel.empty() ? root : path_join(root, rel.substr(0, rel.size() - 1))); } catch (...) { return; }
    for (const DirEntry &e : es) {
        if (e.name.empty() || e.name[0] == '.') continue;
        if (e.dir) { if (depth < 6 && out.size() < 8000) arp_scan_dir(o, root, rel + e.name + "/", depth + 1, out); }
        else if (is_midi_file(e.name) && out.size() < 8000) out.push_back(rel + e.name);
    }
}

// 2.4.1: UJAM styles. A split BLOB library (*_part1.blob) in <library>/MIDI or one folder under the library (e.g.
// "UJAM SE-CHOOIR") adds its styles' phrases to the list: each phrase (the top byte of an event: one per trigger key in
// UJAM's player) with more than one note, as "Styles/<library> ~ <style> ~ <n>.ujs". Worker thread.
bool is_blob_part1(const std::string &name) {
    std::string l = name;
    for (auto &ch : l) ch = char(std::tolower((unsigned char)ch));
    return l.size() > 11 && l.compare(l.size() - 11, 11, "_part1.blob") == 0;
}
void arp_scan_styles(Omni *o, std::vector<std::string> &files, std::map<std::string, Omni::ArpStyleRef> &refs) {
    const std::string lib = library_dir(o);
    if (lib.empty()) return;
    std::vector<std::string> blobs;
    auto look = [&](const std::string &dir) {
        std::vector<DirEntry> es;
        try { es = o->vfs.list(dir); } catch (...) { return; }
        for (const DirEntry &e : es) if (!e.dir && is_blob_part1(e.name)) blobs.push_back(path_join(dir, e.name));
    };
    look(lib);
    std::vector<DirEntry> top;
    try { top = o->vfs.list(lib); } catch (...) {}
    for (const DirEntry &e : top) if (e.dir && !e.name.empty() && e.name[0] != '.') look(path_join(lib, e.name));
    for (const std::string &b : blobs) {
        auto it = o->arp_ujam.find(b);
        if (it == o->arp_ujam.end()) {
            std::vector<UjamMidi> mf;
            try { Location loc = o->vfs.resolve(b); mf = ujam_midi_files(loc.volume, loc.inner); } catch (...) {}
            it = o->arp_ujam.emplace(b, std::move(mf)).first;
        }
        std::string libname = path_name(b);
        libname = libname.substr(0, libname.size() - 11);
        for (size_t si = 0; si < it->second.size(); si++) {
            const UjamMidi &m = it->second[si];
            std::map<int, int> notes;   // phrase -> its notes
            for (size_t i = 0; i < m.events.size(); i++) {
                const uint32_t x = m.events[i];
                if (((x >> 16) & 0xF0) == 0x90 && (x & 0x7F) && ((x >> 8) & 0x7F) >= 12) notes[int(x >> 24)]++;
            }
            std::string sname = m.name;
            const size_t v = sname.find(" v0");
            if (v != std::string::npos) sname = sname.substr(0, v);
            int n = 0;
            for (const auto &pn : notes) {
                if (pn.second < 2) continue;
                const std::string rel = "Styles/" + libname + " ~ " + sname + " ~ " + std::to_string(++n) + ".ujs";
                files.push_back(rel);
                refs[rel] = {b, int(si), pn.first};
            }
        }
    }
}
// a style phrase as a MIDI song (960 ticks a beat, from its first tick)
SmfSong arp_style_song(Omni *o, const Omni::ArpStyleRef &ref) {
    auto it = o->arp_ujam.find(ref.blob);
    if (it == o->arp_ujam.end() || ref.style < 0 || size_t(ref.style) >= it->second.size()) throw ParseError("UJAM style: not found (rescan)");
    const UjamMidi &m = it->second[size_t(ref.style)];
    SmfSong song;
    song.division = 960;
    int64_t t0 = -1, last = 0;
    std::map<int, std::pair<int64_t, int>> open;   // (channel << 8 | note) -> start, velocity
    for (size_t i = 0; i < m.events.size(); i++) {
        const uint32_t x = m.events[i];
        if (int(x >> 24) != ref.phrase) continue;
        const int64_t t = m.ticks[i];
        if (t0 < 0) t0 = t;
        last = std::max(last, t);
        const int type = (x >> 16) & 0xF0, ch = (x >> 16) & 15, a = (x >> 8) & 0x7F, v = x & 0x7F;
        if (a < 12 || (type != 0x90 && type != 0x80)) continue;
        const int k = ch << 8 | a;
        auto op = open.find(k);
        if (op != open.end()) { song.notes.push_back({op->second.first - t0, t - t0, a, op->second.second, ch}); open.erase(op); }
        if (type == 0x90 && v) open[k] = {t, v};
    }
    for (const auto &op : open) song.notes.push_back({op.second.first - t0, std::max(op.second.first + 240, last) - t0, op.first & 0x7F, op.second.second, op.first >> 8});
    std::stable_sort(song.notes.begin(), song.notes.end(), [](const SmfNote &x, const SmfNote &y) { return x.on < y.on; });
    if (song.notes.empty()) throw ParseError("UJAM style: no notes in this phrase");
    return song;
}

// 2.4.1: a note on the grid: with TIE, the run of dots on its row; without, one dot
bool arp_lit(Omni *o, int r, int s) { return s >= 0 && s < Arp::STEPS && (o->sampler.arp.mask[s].load() >> r & 1); }
// 2.4.14 (audit B3): a step's velocity (one per step) belongs to every note on it: a note taken off resets it only when
// the step is left empty; a note put on keeps it when other notes are there (deleting, moving or shifting one note of a
// chord set the whole chord to 100)
void arp_vel_after_take(Omni *o, int s) {
    if (s >= 0 && s < Arp::STEPS && !o->sampler.arp.mask[s].load()) o->sampler.arp.vel[s].store(100);
}
void arp_vel_for_put(Omni *o, int s, uint16_t mask_before, uint8_t v) {   // mask_before: the step's notes before this one
    if (s >= 0 && s < Arp::STEPS && !mask_before) o->sampler.arp.vel[s].store(v);
}
bool arp_note_span(Omni *o, int r, int s, int &s0, int &s1) {
    if (!arp_lit(o, r, s)) return false;
    s0 = s1 = s;
    if (num(o, "arp_tie") > 0.5f) {
        while (arp_lit(o, r, s0 - 1)) s0--;
        while (arp_lit(o, r, s1 + 1)) s1++;
    }
    return true;
}
float arp_gate_of(Omni *o, int r, int s0) {   // %: the note's own, else the pattern's
    const int g = o->sampler.arp.ngate[s0 * 16 + r].load();
    return g ? float(g) : num(o, "arp_gate");
}

// a cell's picture (design.py draw_arp_cell): 0 empty; else 1 + 40 past LENGTH + 20 selected + 0 a note's first step,
// 1 its middle, 2 + g its last (g 0-7: its gate in eighths), 10 + g a one-step note
int arp_cell_code(Omni *o, int r, int s) {
    int s0, s1;
    if (!arp_note_span(o, r, s, s0, s1)) return 0;
    const int len = int(num(o, "arp_length") + 0.5f);
    const bool sel = o->arp_sel_on && o->arp_sel_r == r && o->arp_sel_s0 == s0;
    const int g8 = std::max(1, std::min(8, int(std::lround(arp_gate_of(o, r, s0) / 12.5f)))) - 1;
    const int kind = s0 == s1 ? 10 + g8 : s == s0 ? 0 : s == s1 ? 2 + g8 : 1;
    return 1 + (s >= len ? 40 : 0) + (sel ? 20 : 0) + kind;
}

// the grid as shown: page `arp_page`'s 16 steps; report = tell MPC about the cells that changed
void arp_refresh_view(Omni *o, bool report = true) {
    const int page = std::max(0, std::min(3, int(num(o, "arp_page") + 0.5f)));
    for (int c = 0; c < 16; c++)
        for (int r = 0; r < 16; r++) {
            const uint8_t v = uint8_t(arp_cell_code(o, r, page * 16 + c));
            if (o->arp_view[r * 16 + c].exchange(v) != v && report) mark_changed(o, Omni::ARP_REP0 + r * 16 + c);
        }
}

// the selection: NOTE GATE shows (and sets) that note's gate
void arp_select(Omni *o, bool on, int r = 0, int s0 = 0) {
    { std::lock_guard<std::mutex> lock(o->syn_mutex); o->arp_msg.clear(); }   // (the info line shows the selection)
    o->arp_sel_on = on;
    o->arp_sel_r = r;
    o->arp_sel_s0 = s0;
    const int i = num_index("arp_note_gate");
    const float g = on ? arp_gate_of(o, r, s0) : num(o, "arp_gate");
    if (i >= 0 && o->values[i].exchange(g) != g) mark_changed(o, i);
    // 2.4.2: its expression, and the flag that swaps the panel's knobs
    const Arp &a = o->sampler.arp;
    const int ni = s0 * 16 + r;
    auto put = [&](const char *k, float v) { const int j = num_index(k); if (j >= 0 && o->values[j].exchange(v) != v) mark_changed(o, j); };
    put("arp_n_vel", on && a.nvel[ni].load() ? float(a.nvel[ni].load()) : 100.0f);
    put("arp_n_chance", on ? float(100 - a.nskip[ni].load()) : 100.0f);
    put("arp_n_ratchet", on ? float(std::max(1, int(a.nratch[ni].load()))) : 1.0f);
    put("arp_n_oct", on ? float(a.noct[ni].load()) : 0.0f);
    put("arp_n_slide", on ? float(a.nslide[ni].load() ? 1 : 0) : 0.0f);
    put("arp_sel", on ? 1.0f : 0.0f);
}
// a note's own settings (gate, 2.4.2 expression) go where its first step goes (moved, shifted), or away with it
struct ArpNoteX { uint8_t gate, vel, skip, ratch, slide; int8_t oct; };
ArpNoteX arp_note_take(Omni *o, int i) {
    Arp &a = o->sampler.arp;
    return {a.ngate[i].exchange(0), a.nvel[i].exchange(0), a.nskip[i].exchange(0), a.nratch[i].exchange(0), a.nslide[i].exchange(0), a.noct[i].exchange(0)};
}
void arp_note_put(Omni *o, int i, const ArpNoteX &x) {
    Arp &a = o->sampler.arp;
    a.ngate[i].store(x.gate); a.nvel[i].store(x.vel); a.nskip[i].store(x.skip); a.nratch[i].store(x.ratch); a.nslide[i].store(x.slide); a.noct[i].store(x.oct);
}
void arp_move_gate(Omni *o, int r, int s0, int r2, int s2) {
    const ArpNoteX x = arp_note_take(o, s0 * 16 + r);
    if (s2 >= 0 && s2 < Arp::STEPS) arp_note_put(o, s2 * 16 + r2, x);
}
void arp_notes_clear(Omni *o) { for (int i = 0; i < Arp::STEPS * 16; i++) arp_note_take(o, i); }

void arp_clear_steps(Omni *o, int from, int n) {
    for (int s = from; s < from + n && s < Arp::STEPS; s++) {
        o->sampler.arp.mask[s].store(0);
        o->sampler.arp.vel[s].store(100);
        for (int r = 0; r < 16; r++) arp_note_take(o, s * 16 + r);
    }
    if (o->arp_sel_on && o->arp_sel_s0 >= from && o->arp_sel_s0 < from + n) arp_select(o, false);
    arp_refresh_view(o);
}

std::string arp_pattern_text(Omni *o) {
    std::string t;
    char b[8];
    for (int s = 0; s < Arp::STEPS; s++) {
        std::snprintf(b, sizeof b, "%04x%02x", o->sampler.arp.mask[s].load(), o->sampler.arp.vel[s].load());
        t += b;
    }
    return t;
}
void arp_pattern_set(Omni *o, const std::string &t) {
    for (int s = 0; s < Arp::STEPS; s++) {
        uint16_t m = 0; uint8_t v = 100;
        if (t.size() >= size_t(s + 1) * 6) {
            m = uint16_t(std::strtoul(t.substr(size_t(s) * 6, 4).c_str(), nullptr, 16));
            v = uint8_t(std::max(1, std::min(127, int(std::strtoul(t.substr(size_t(s) * 6 + 4, 2).c_str(), nullptr, 16)))));
        }
        o->sampler.arp.mask[s].store(m);
        o->sampler.arp.vel[s].store(v);
    }
    arp_notes_clear(o);   // 2.4.1: the notes' own gates come after ("arp_gates"), 2.4.2 their expression ("arp_nx")
    // 2.4.14 (audit B1): the selection's knobs too, not only the flag: a project saved with a note selected came back with
    // the NOTE knobs shown (arp_sel 1) and nothing selected, the knobs dead and LENGTH / SWING / GATE hidden
    arp_select(o, false);
    // 2.4.14 (audit B2): UNDO and the BREED parents belong to the pattern that was here: another project's UNDO
    // overwrote this one
    o->arp_undo.set = o->brd_a.set = o->brd_b.set = false;
    arp_refresh_view(o);
}
// 2.4.1: the notes' own gates in the state: "<step hex 2><row hex 1><gate % hex 2>" each
[[maybe_unused]] std::string arp_gates_text(Omni *o) {   // (2.4.29: kept for the tests; arp_x replaced it in the state)
    std::string t;
    char b[8];
    for (int i = 0; i < Arp::STEPS * 16; i++)
        if (int g = o->sampler.arp.ngate[i].load()) { std::snprintf(b, sizeof b, "%02x%x%02x", i / 16, i % 16, g); t += b; }
    return t;
}
// 2.4.2: the notes' expression: "<step 2><row 1><vel 2><skip 2><ratchet 1><oct+2 1><slide 1>" (hex) each
[[maybe_unused]] std::string arp_nx_text(Omni *o) {
    const Arp &a = o->sampler.arp;
    std::string t;
    char b[16];
    for (int i = 0; i < Arp::STEPS * 16; i++) {
        const int v = a.nvel[i].load(), k = a.nskip[i].load(), rt = a.nratch[i].load(), oc = a.noct[i].load(), sl = a.nslide[i].load();
        if (v || k || rt > 1 || oc || sl) {
            std::snprintf(b, sizeof b, "%02x%x%02x%02x%x%x%x", i / 16, i % 16, v, k, std::max(1, std::min(4, rt)), std::max(0, std::min(4, oc + 2)), sl ? 1 : 0);
            t += b;
        }
    }
    return t;
}
void arp_nx_set(Omni *o, const std::string &t) {
    Arp &a = o->sampler.arp;
    auto hx = [&](size_t at, size_t n) { return int(std::strtoul(t.substr(at, n).c_str(), nullptr, 16)); };
    for (size_t i = 0; i + 10 <= t.size(); i += 10) {
        const int st = hx(i, 2), r = hx(i + 2, 1);
        if (st >= Arp::STEPS || r >= 16) continue;
        const int ni = st * 16 + r;
        a.nvel[ni].store(uint8_t(std::min(200, hx(i + 3, 2))));
        a.nskip[ni].store(uint8_t(std::min(100, hx(i + 5, 2))));
        a.nratch[ni].store(uint8_t(std::max(1, std::min(4, hx(i + 7, 1)))));
        a.noct[ni].store(int8_t(std::max(-2, std::min(2, hx(i + 8, 1) - 2))));
        a.nslide[ni].store(uint8_t(hx(i + 9, 1) ? 1 : 0));
    }
}
void arp_gates_set(Omni *o, const std::string &t) {
    for (size_t i = 0; i + 5 <= t.size(); i += 5) {
        const int st = int(std::strtoul(t.substr(i, 2).c_str(), nullptr, 16)), r = int(std::strtoul(t.substr(i + 2, 1).c_str(), nullptr, 16));
        const int g = int(std::strtoul(t.substr(i + 3, 2).c_str(), nullptr, 16));
        if (st < Arp::STEPS && r < 16) o->sampler.arp.ngate[st * 16 + r].store(uint8_t(std::max(0, std::min(100, g))));
    }
    arp_refresh_view(o);
}
// 2.4.29 (audit F7): "arp_x", the notes' gates and expression in one compact text (the old "arp_gates" + "arp_nx" took 15
// characters a note with everything set, so a dense pattern did not fit MPC's 8 KB state and its expression was dropped).
// A stream of tokens for the cells that are not at their defaults, in cell order (step * 16 + row), base 64 (A64 below):
//   header (1 char, bits)  1 gate, 2 velocity %, 4 skip %, 8 extras, 16 "same values as the token before" (no payload),
//                          32 long gap
//   gap    (1 char: cells since the token before - 1; the first: the cell number)  or, with the long-gap bit, 2 chars
//   payload gate (2 chars), velocity (2), skip (2), extras (1 char: ratchet - 1 | octave + 2 << 2 | slide << 5)
// Lossless. A dense pattern with a gate on every cell is 2 characters a cell (same as the one before) to 4. If even this
// does not fit the room given, the stream stops at a whole token (the last steps lose their expression) and the cells
// left out are counted.
constexpr char A64[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_";
int a64_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 36;
    return c == '-' ? 62 : c == '_' ? 63 : -1;
}
struct ArpX { int g, v, k, x; };
std::string arp_x_text(Omni *o, size_t room, int *omitted = nullptr) {
    const Arp &a = o->sampler.arp;
    std::string t;
    int prev = -1, left = 0;
    bool full = false;
    ArpX last = {0, 0, 0, 0};
    for (int i = 0; i < Arp::STEPS * 16; i++) {
        const int rt = a.nratch[i].load(), oc = a.noct[i].load();
        // extras: bits 0-1 ratchet - 1, 2-4 octave + 2, 5 slide; stored XOR 8 so that the defaults (1, 0, off) are 0
        ArpX c = {a.ngate[i].load(), a.nvel[i].load(), a.nskip[i].load(),
                  ((std::max(1, std::min(4, rt)) - 1) | (std::max(0, std::min(4, oc + 2)) << 2) | (a.nslide[i].load() ? 32 : 0)) ^ 8};
        if (!(c.g || c.v || c.k || c.x)) continue;
        if (full) { left++; continue; }
        const bool same = prev >= 0 && c.g == last.g && c.v == last.v && c.k == last.k && c.x == last.x;
        const int gap = prev < 0 ? i : i - prev - 1;
        std::string tok;
        const bool lng = gap > 63;
        tok += A64[(c.g ? 1 : 0) | (c.v ? 2 : 0) | (c.k ? 4 : 0) | (c.x ? 8 : 0) | (same ? 16 : 0) | (lng ? 32 : 0)];
        if (lng) { tok += A64[gap >> 6 & 63]; tok += A64[gap & 63]; } else tok += A64[gap];
        if (!same) {
            if (c.g) { tok += A64[c.g >> 6 & 63]; tok += A64[c.g & 63]; }
            if (c.v) { tok += A64[c.v >> 6 & 63]; tok += A64[c.v & 63]; }
            if (c.k) { tok += A64[c.k >> 6 & 63]; tok += A64[c.k & 63]; }
            if (c.x) tok += A64[c.x & 63];
        }
        if (t.size() + tok.size() > room) { left++; full = true; continue; }   // (nothing more fits: count the rest)
        t += tok;
        prev = i;
        last = c;
    }
    if (omitted) *omitted = left;
    return t;
}
void arp_x_set(Omni *o, const std::string &t) {
    Arp &a = o->sampler.arp;
    size_t i = 0;
    int prev = -1;
    ArpX last = {0, 0, 0, 0};
    auto rd = [&](int n) { int v = 0; for (; n > 0; n--) { const int d = i < t.size() ? a64_val(t[i]) : -1; if (d < 0) return -1; v = v << 6 | d; i++; } return v; };
    while (i < t.size()) {
        const int h = rd(1);
        if (h < 0) break;
        const int gap = rd(h & 32 ? 2 : 1);
        if (gap < 0) break;
        ArpX c = last;
        if (!(h & 16)) {
            c = {0, 0, 0, 0};
            if (h & 1) { c.g = rd(2); if (c.g < 0) break; }
            if (h & 2) { c.v = rd(2); if (c.v < 0) break; }
            if (h & 4) { c.k = rd(2); if (c.k < 0) break; }
            if (h & 8) { c.x = rd(1); if (c.x < 0) break; }
        }
        const int ci = prev < 0 ? gap : prev + 1 + gap;
        prev = ci;
        last = c;
        if (ci >= Arp::STEPS * 16) break;
        a.ngate[ci].store(uint8_t(std::max(0, std::min(100, c.g))));
        a.nvel[ci].store(uint8_t(std::min(200, c.v)));
        a.nskip[ci].store(uint8_t(std::min(100, c.k)));
        const int xr = c.x ^ 8;
        a.nratch[ci].store(uint8_t((xr & 3) ? (xr & 3) + 1 : 0));   // (1 hit = unset, as in a pattern that never had one)
        a.noct[ci].store(int8_t(std::max(-2, std::min(2, (xr >> 2 & 7) - 2))));
        a.nslide[ci].store(uint8_t(xr & 32 ? 1 : 0));
    }
    arp_refresh_view(o);
}

// worker (syn_worker): read and convert the picked file
// the playhead lights: each step of the page shown, 0 past LENGTH, 64 played, 127 playing now (design.py strip_led)
// 2.4.1: the lights also show the timing at the RATE: a beat's first step brighter, a bar's first step accented
// (design.py strip_led: 0-31 past LENGTH, 32-63 a step, 64-79 a beat, 80-95 a bar, 96-127 playing)
int arp_led_value(Omni *o, int col) {
    const int page = std::max(0, std::min(3, int(num(o, "arp_page") + 0.5f))), len = int(num(o, "arp_length") + 0.5f);
    if (col == o->arp_led_shown) return 127;
    const int s = page * 16 + col;
    if (s >= len) return 0;
    static const int PER_BEAT[Arp::RATES] = {1, 2, 4, 8, 3, 6, 12};   // steps a beat: 1/4 1/8 1/16 1/32 1/8T 1/16T 1/32T
    const int spb = PER_BEAT[std::max(0, std::min(Arp::RATES - 1, int(num(o, "arp_rate") + 0.5f)))];
    return s % (spb * 4) == 0 ? 88 : s % spb == 0 ? 72 : 48;
}
void arp_leds_dirty(Omni *o) { for (int c = 0; c < 16; c++) mark_changed(o, Omni::ARP_REP0 + 256 + c); }
void arp_set_num(Omni *o, const char *key, float v) {
    const int i = num_index(key);
    if (i >= 0 && o->values[i].exchange(v) != v) mark_changed(o, i);
}

void arp_load_now(Omni *o) {
    std::string rel;
    {
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        rel = o->arp_want;
    }
    const std::string dir = arp_dir(o);
    if (rel.empty() || dir.empty()) return;
    std::string msg;
    try {
        SmfSong song;
        bool style = false;
        if (rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".ujs") == 0) {   // 2.4.1: a UJAM style phrase
            Omni::ArpStyleRef ref;
            {
                std::lock_guard<std::mutex> lock(o->syn_mutex);
                auto it = o->arp_style_refs.find(rel);
                if (it == o->arp_style_refs.end()) throw ParseError("UJAM style: not found (rescan)");
                ref = it->second;
            }
            song = arp_style_song(o, ref);
            style = true;
        } else {
            std::vector<uint8_t> data = o->vfs.open(path_join(dir, rel))->all(4 << 20);
            song = smf_parse(data);
        }
        ArpImport how;
        // 2.4.29 (user): in ARP mode a MIDI file keeps its shape: rows = each pitch's rank in the file (lowest = row 1),
        // as for UJAM styles; the chord ranking (rows = voice of each chord) folded a file onto 2-5 rows
        how.rank_all = style || int(num(o, "arp_mode") + 0.5f) != 0;
        how.snap = int(num(o, "arp_snap") + 0.5f);
        how.mode = int(num(o, "arp_mode") + 0.5f);
        how.scale = int(num(o, "arp_scale") + 0.5f);
        how.root = int(num(o, "arp_root") + 0.5f);
        ArpPattern p = smf_to_pattern(song, how);
        if (!p.length) throw ParseError("no notes in this MIDI file");
        Arp &arp = o->sampler.arp;
        for (int s = 0; s < Arp::STEPS; s++) { arp.mask[s].store(p.mask[s]); arp.vel[s].store(p.vel[s] ? p.vel[s] : 100); }
        arp_notes_clear(o);
        if (how.mode == 0)   // 2.4.18: notes above the rows keep their octave (NOTE OCT)
            for (int i = 0; i < Arp::STEPS * Arp::ROWS; i++) if (p.oct[i]) arp.noct[i].store(p.oct[i]);
        arp_select(o, false);
        arp.settings.base.store(how.mode == 0 ? p.base : 0);
        // the file sets the step, the length, TIE (its notes' lengths) and, when it is not in the key chosen, the key
        arp_set_num(o, "arp_length", float(p.length));
        arp_set_num(o, "arp_rate", float(p.rate));
        arp_set_num(o, "arp_tie", 1);
        arp_set_num(o, "arp_page", 0);
        if (p.key_found) { arp_set_num(o, "arp_scale", float(p.scale)); arp_set_num(o, "arp_root", float(p.root)); }
        apply_settings(o);
        arp_refresh_view(o);
        arp_leds_dirty(o);
        static const char *const ROOTS[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        char t[200];
        std::snprintf(t, sizeof t, "%d notes  ·  %d of %d bar%s, %d steps of %s  ·  %s %s%s", p.notes, p.bars_used, p.bars,
                      p.bars == 1 ? "" : "s", p.length, Arp::RATE_NAMES[p.rate], ROOTS[p.root % 12], Arp::SCALE_NAMES[p.scale],
                      p.key_found ? " (from the file)" : "");
        msg = t;
        if (p.off_scale) msg += "  ·  " + std::to_string(p.off_scale) + " off the scale";
        if (p.raised) msg += "  ·  " + std::to_string(p.raised) + " with NOTE OCT";
        if (p.lost) msg += "  ·  " + std::to_string(p.lost) + " octave" + (p.lost > 1 ? "s" : "") + " merged (too wide for the grid)";   // 2.4.29 (audit F4)
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        o->arp_loaded = rel;
        o->arp_src.set = true;   // 2.4.9: GENERATE's source
        std::copy(p.mask, p.mask + 64, o->arp_src.mask); std::copy(p.vel, p.vel + 64, o->arp_src.vel);
        o->arp_src.length = p.length;
        o->arp_src.rate = p.rate;   // 2.4.18: the step the file came in at (GENERATE reads it at the grid's step now)
    } catch (const std::exception &e) { msg = e.what(); }
    std::lock_guard<std::mutex> lock(o->syn_mutex);
    o->arp_msg = msg;
}

// a row's name: SEQ, the note row r plays from the key held now (or the root at C3, before any); ARP, which held key
std::string arp_row_name(Omni *o, int r) {
    const ArpSettings &a = o->sampler.arp.settings;
    if (a.mode.load() == 1) {
        if (a.order.load() != Arp::O_PATTERN) return r == 0 ? "NOTE" : "";   // the other orders: any row plays the next
        static const char *const ORD[] = {"1st", "2nd", "3rd"};
        return r < 3 ? std::string(ORD[r]) : std::to_string(r + 1) + "th";
    }
    // the notes as written: the scale up from the root at C3 (the key that plays them so; other keys move them along
    // the scale), OCTAVE included
    const int root = a.root.load() % 12;
    int n = Arp::seq_note(60 + root, r, a.scale.load(), root, a.base.load());
    if (n >= 0) n += 12 * a.octave.load();
    if (n > 127) n = -1;
    return n < 0 ? "-" : note_name(n);
}

// SHIFT: the pattern (its first LENGTH steps) a step later (+1) or earlier (-1), round
void arp_shift(Omni *o, int d) {
    Arp &arp = o->sampler.arp;
    const int len = std::max(1, std::min(Arp::STEPS, int(num(o, "arp_length") + 0.5f)));
    int s0, s1;
    if (o->arp_sel_on && arp_note_span(o, o->arp_sel_r, o->arp_sel_s0, s0, s1)) {   // 2.4.1: the selected note only
        const int r = o->arp_sel_r, n = s1 - s0;
        const int to = std::max(0, std::min(std::max(len, s1 + 1) - 1 - n, s0 + d));   // (it stays in the pattern)
        if (to == s0) return;
        const uint8_t vel = arp.vel[s0].load();
        for (int s = s0; s <= s1; s++) arp.mask[s].store(uint16_t(arp.mask[s].load() & ~(1u << r)));
        arp_vel_after_take(o, s0);   // 2.4.14 (B3)
        arp_move_gate(o, r, s0, r, to);
        const uint16_t before = arp.mask[to].load();
        for (int s = to; s <= to + n; s++) arp.mask[s].store(uint16_t(arp.mask[s].load() | (1u << r)));
        arp_vel_for_put(o, to, before, vel);
        o->arp_sel_s0 = to;
        arp_refresh_view(o);
        return;
    }
    uint16_t m[Arp::STEPS];
    uint8_t v[Arp::STEPS];
    ArpNoteX g[Arp::STEPS][16];   // 2.4.29 (GRID audit F8): per call (was static: shared by every plugin instance)
    for (int s = 0; s < len; s++) {
        m[s] = arp.mask[s].load(); v[s] = arp.vel[s].load();
        for (int r = 0; r < 16; r++) g[s][r] = arp_note_take(o, s * 16 + r);
    }
    for (int s = 0; s < len; s++) {
        const int f = ((s - d) % len + len) % len;
        arp.mask[s].store(m[f]); arp.vel[s].store(v[f]);
        for (int r = 0; r < 16; r++) arp_note_put(o, s * 16 + r, g[f][r]);
    }
    if (o->arp_sel_on && o->arp_sel_s0 < len) o->arp_sel_s0 = ((o->arp_sel_s0 + d) % len + len) % len;
    arp_refresh_view(o);
}


// 2.4.9: SHIFT up / down: the selected note a row higher (+1) or lower (-1): the pitch, one scale step. Rows where it
// would land on another note (or, with TIE, run into one) are jumped over; at the edge it stays. Nothing selected: the
// whole pattern (its first LENGTH steps) a row, unless a note would fall off the grid (nothing is ever lost)
void arp_shift_row(Omni *o, int d) {
    Arp &arp = o->sampler.arp;
    auto say = [o](const char *m) { std::lock_guard<std::mutex> lock(o->syn_mutex); o->arp_msg = m; };
    const bool tie = num(o, "arp_tie") > 0.5f;
    int s0, s1;
    if (o->arp_sel_on && arp_note_span(o, o->arp_sel_r, o->arp_sel_s0, s0, s1)) {
        const int r = o->arp_sel_r;
        auto clash = [&](int t) {
            for (int s = s0; s <= s1; s++) if (arp_lit(o, t, s)) return true;
            return tie && (arp_lit(o, t, s0 - 1) || arp_lit(o, t, s1 + 1));
        };
        int t = r + d;
        while (t >= 0 && t < 16 && clash(t)) t += d;
        if (t < 0 || t >= 16) { say(d > 0 ? "No free row above this note" : "No free row below this note"); return; }
        for (int s = s0; s <= s1; s++) arp.mask[s].store(uint16_t((arp.mask[s].load() & ~(1u << r)) | (1u << t)));
        arp_move_gate(o, r, s0, t, s0);
        o->arp_sel_r = t;
        say("");   // (an earlier "no free row" goes: the note's own line shows)
        arp_refresh_view(o);
        return;
    }
    const int len = std::max(1, std::min(Arp::STEPS, int(num(o, "arp_length") + 0.5f)));
    const uint16_t edge = d > 0 ? uint16_t(1u << 15) : uint16_t(1u);
    for (int s = 0; s < len; s++)
        if (arp.mask[s].load() & edge) { say(d > 0 ? "A note is on the top row: can't go higher" : "A note is on the bottom row: can't go lower"); return; }
    for (int s = 0; s < len; s++) {
        const uint16_t m = arp.mask[s].load();
        arp.mask[s].store(uint16_t(d > 0 ? m << 1 : m >> 1));
        ArpNoteX g[16];
        for (int r = 0; r < 16; r++) g[r] = arp_note_take(o, s * 16 + r);
        for (int r = 0; r < 16; r++) if (r + d >= 0 && r + d < 16) arp_note_put(o, s * 16 + r + d, g[r]);
    }
    if (o->arp_sel_on) o->arp_sel_r = std::max(0, std::min(15, o->arp_sel_r + d));
    say("");
    arp_refresh_view(o);
}

// ---------------------------------------------------------------------------------------------------------------
// 2.4.2 GEN: the melody generator and motif tools, EUCLID (UI thread). Each saves the pattern first (UNDO).
void arp_snapshot(Omni *o, Omni::ArpSnap &sn) {
    Arp &a = o->sampler.arp;
    for (int s = 0; s < Arp::STEPS; s++) { sn.mask[s] = a.mask[s].load(); sn.vel[s] = a.vel[s].load(); }
    for (int i = 0; i < Arp::STEPS * 16; i++)
        sn.nx[i][0] = a.ngate[i].load(), sn.nx[i][1] = a.nvel[i].load(), sn.nx[i][2] = a.nskip[i].load(),
        sn.nx[i][3] = a.nratch[i].load(), sn.nx[i][4] = a.nslide[i].load(), sn.nx[i][5] = uint8_t(a.noct[i].load());
    sn.rate = num(o, "arp_rate"); sn.length = num(o, "arp_length");
    sn.timing = false;
    sn.set = true;
}
void arp_restore(Omni *o, const Omni::ArpSnap &sn) {
    Arp &a = o->sampler.arp;
    for (int s = 0; s < Arp::STEPS; s++) { a.mask[s].store(sn.mask[s]); a.vel[s].store(sn.vel[s]); }
    for (int i = 0; i < Arp::STEPS * 16; i++)
        a.ngate[i].store(sn.nx[i][0]), a.nvel[i].store(sn.nx[i][1]), a.nskip[i].store(sn.nx[i][2]),
        a.nratch[i].store(sn.nx[i][3]), a.nslide[i].store(sn.nx[i][4]), a.noct[i].store(int8_t(sn.nx[i][5]));
    if (sn.timing) { arp_set_num(o, "arp_rate", sn.rate); arp_set_num(o, "arp_length", sn.length); apply_settings(o); }
}
// (2.4.12: OMNI_ARP_SEED fixes the seed for the tests, so a failing run can be repeated; on the MPC: the clock)
std::mt19937 &arp_rng() {
    thread_local std::mt19937 g{[] { const char *e = std::getenv("OMNI_ARP_SEED"); return e ? uint32_t(std::strtoul(e, nullptr, 10)) : uint32_t(now_ms()); }()};
    return g;
}
double arp_u() { return std::uniform_real_distribution<double>(0, 1)(arp_rng()); }
int arp_len(Omni *o) { return std::max(1, std::min(Arp::STEPS, int(num(o, "arp_length") + 0.5f))); }
int arp_per_beat(Omni *o) { static const int P[Arp::RATES] = {1, 2, 4, 8, 3, 6, 12}; return P[std::max(0, std::min(Arp::RATES - 1, int(num(o, "arp_rate") + 0.5f)))]; }
void arp_gen_msg(Omni *o, const std::string &m) { std::lock_guard<std::mutex> lock(o->syn_mutex); o->arp_msg = m; }

// a melody over steps [from, len): rhythm from DENSITY (stronger on beats; SYNCOPATION moves weight off them), rows
// following CONTOUR over RANGE rows with small steps (the occasional leap); accents on the beats. Returns notes made.
int arp_generate_into(Omni *o, int from, int len, double density, int ratchet_pct) {
    Arp &a = o->sampler.arp;
    const int spb = arp_per_beat(o), range = std::max(2, std::min(16, int(num(o, "arp_g_range") + 0.5f)));
    const int contour = int(num(o, "arp_g_contour") + 0.5f);
    const double sync = num(o, "arp_g_sync") / 100.0;
    const bool tie = num(o, "arp_tie") > 0.5f;
    for (int st = from; st < len; st++) {
        a.mask[st].store(0); a.vel[st].store(100);
        for (int r = 0; r < 16; r++) arp_note_take(o, st * 16 + r);
    }
    int row = int(arp_u() * range * 0.5), made = 0, prev_row = -1, prev_step = -9;
    double walk = arp_u();
    for (int st = from; st < len; st++) {
        const int pos = st % spb;
        const double strong = st % (spb * 4) == 0 ? 1.0 : pos == 0 ? 0.75 : (spb > 1 && pos == spb / 2) ? 0.5 : 0.3;
        const double p = std::min(1.0, density * 1.4 * ((1 - sync) * strong + sync * (1.2 - strong) + 0.2));
        if (!(st == from || arp_u() < p)) continue;
        const double t = len > from + 1 ? double(st - from) / double(len - from - 1) : 0.0;
        double f;
        switch (contour) {
            case 0: f = t; break;                                          // Rise
            case 1: f = 1 - t; break;                                      // Fall
            case 2: f = std::sin(M_PI * t); break;                         // Arch
            case 3: f = 1 - std::sin(M_PI * t); break;                     // Valley
            case 4: f = 0.5 + 0.5 * std::sin(4 * M_PI * t); break;         // Wave
            default: walk = std::max(0.0, std::min(1.0, walk + (arp_u() - 0.5) * 0.35)); f = walk; break;   // Random
        }
        const int target = int(std::lround(f * (range - 1)));
        int step = std::max(-2, std::min(2, target - row));                 // towards the contour, a step or two at a time
        if (arp_u() < 0.25) step += arp_u() < 0.5 ? -1 : 1;                  // a little wander
        if (arp_u() < 0.08) step = (arp_u() < 0.5 ? -1 : 1) * (3 + int(arp_u() * 2));   // the occasional leap
        row = std::max(0, std::min(range - 1, row + step));
        if (tie && row == prev_row && prev_step == st - 1) row = row + 1 < range ? row + 1 : row - 1;   // (not joined to the last)
        a.mask[st].store(uint16_t(a.mask[st].load() | (1u << row)));
        a.vel[st].store(uint8_t(std::max(40, std::min(127, int(strong >= 0.75 ? 110 : 85) + int((arp_u() - 0.5) * 20)))));
        if (ratchet_pct && arp_u() * 100 < ratchet_pct) a.nratch[st * 16 + row].store(uint8_t(2 + (arp_u() < 0.3)));
        prev_row = row; prev_step = st;
        made++;
    }
    return made;
}

// ---------------------------------------------------------------------------------------------------------------
// 2.4.9 GENERATE (the user: "much more musical: scales, phrasing, call and response, tension / release"; and MOTIF modes
// ostinato, orchestral riffs, high-to-low, low-to-high). It thinks in the grid's rows = scale degrees: chord tones
// (root, third, fifth of the key) on the strong beats, steps between them, a leap followed by a step back, one rhythm
// motif reused bar after bar, two-bar phrases (a call that ends open on the fifth, an answer that starts like it and
// comes home to the root), the highest note once near the climax, the last note the root. With a MIDI file loaded, the
// file is the material: its rhythm, its notes and its opening cell, developed by the mode; without one, its own.
// One melody line (one note at a time). Velocities: accents on the beats, a swell to the climax.
enum ArpMode { AM_RISE, AM_FALL, AM_ARCH, AM_VALLEY, AM_WAVE, AM_RANDOM, AM_OSTINATO, AM_RIFF, AM_DOWN, AM_UP, AM_CALL,
               AM_PEDAL, AM_ARPEGGIO, AM_COUNT };   // design.py ARP_CONTOURS
const char *const ARP_MODE_NAMES[AM_COUNT] = {"Rise", "Fall", "Arch", "Valley", "Wave", "Random", "Ostinato", "Orch Riff",
                                               "High to Low", "Low to High", "Call + Answer", "Pedal", "Arpeggio"};
struct GNote {
    int s, len, row, vel;
    // 2.4.11: the note's own expression, as the grid stores it (0 = the pattern's): GATE %, velocity %, CHANCE skip %,
    // ratchet hits, SLIDE into the next
    int gate = 0, nvel = 0, skip = 0, ratch = 0, slide = 0;
    bool chord = false;   // 2.4.12: a note of the file's chords (kept whole), at its own grid row; the line's are offset
};

struct ArpKey {   // the rows as notes of the key
    int scale, root, base, size;
    int iv(int row) const { return ((Arp::note_of(base + row, scale, root) - root) % 12 + 12) % 12; }
    bool chord(int row) const { const int i = iv(row); return i == 0 || i == 3 || i == 4 || i == 7; }
    bool tonic(int row) const { return iv(row) == 0; }
    bool fifth(int row) const { return iv(row) == 7; }
};
int arp_nearest(int row, int lo, int hi, const std::function<bool(int)> &ok) {   // the nearest row ok() likes (row if none)
    for (int d = 0; d <= hi - lo; d++) {
        if (row - d >= lo && ok(row - d)) return row - d;
        if (row + d <= hi && ok(row + d)) return row + d;
    }
    return std::max(lo, std::min(hi, row));
}
double arp_shape(int mode, double t, double &walk) {   // the contour, 0..1, at t 0..1
    switch (mode) {
        case AM_RISE: return t;
        case AM_FALL: return 1 - t;
        case AM_VALLEY: return 1 - std::sin(M_PI * t);
        case AM_WAVE: return 0.5 + 0.5 * std::sin(4 * M_PI * t);
        case AM_RANDOM: walk = std::max(0.0, std::min(1.0, walk + (arp_u() - 0.5) * 0.35)); return walk;
        default: return 0.2 + 0.75 * std::pow(std::sin(M_PI * std::min(1.0, t / 1.3)), 1.5);   // Arch, Call: the top near 65 %
    }
}
// one bar's onsets: always the downbeat; DENSITY, stronger beats likelier, SYNCOPATION weight off them
std::vector<int> arp_rhythm(int bar, int spb, double density, double sync) {
    std::vector<int> on{0};
    for (int st = 1; st < bar; st++) {
        const int pos = st % spb;
        const double strong = pos == 0 ? (st % (spb * 2) == 0 ? 0.9 : 0.75) : (spb > 1 && pos * 2 == spb) ? 0.5 : 0.3;
        if (arp_u() < std::min(1.0, density * 1.3 * ((1 - sync) * strong + sync * (1.15 - strong) + 0.15))) on.push_back(st);
    }
    // 2.4.12: a floor (by chance every step but the first could miss: a one-note GENERATE, ~1 in 15000 at 55 %): at least
    // DENSITY x the bar's beats, 2 at the least, the strongest free places first (half bar, beats, eighths, the rest)
    const int want = std::min(bar, std::max(2, int(std::lround(density * double(bar) / std::max(1, spb)))));
    for (int pass = 0; pass < 4 && int(on.size()) < want; pass++)
        for (int st = 1; st < bar && int(on.size()) < want; st++) {
            const bool fit = pass == 0 ? st * 2 == bar : pass == 1 ? st % spb == 0 : pass == 2 ? (spb > 1 && (st % spb) * 2 == spb) : true;
            if (fit && std::find(on.begin(), on.end(), st) == on.end()) on.push_back(st);
        }
    std::sort(on.begin(), on.end());
    return on;
}

// the melody for steps [0, len) (into `out`); the source's notes (start step, row) sorted, or none
// 2.4.11 (user: "flat, no expression or articulation"): how each note is played, from where it sits in the phrase
// and how the line moves. Melodic modes (the contours, CALL & ANSWER, the cascades): steps legato, now and then a SLIDE
// into the next, leaps detached and accented, repeated notes shorter. Rhythmic ones (OSTINATO, RIFF, PEDAL): downbeats
// held, off-beats staccato, weak 16ths ghost notes (soft, sometimes skipped), a ratchet fill at every fourth bar's end.
// ARPEGGIO: the chord detached, its top held. Every phrase: a swell to its highest note, its last note held (tenuto),
// an answer a little softer than its call. Values are the grid's own per-note settings, so they show and can be edited.
// 2.4.11 (user: "a composer's tool"): GEN's ARTIC / EXPR / GHOST knobs, 0..1 each, 0.5 = the phrasing below as it is.
// ARTIC 0 = every note legato (held to the next) .. 1 = short, staccato; EXPR 0 = one level .. 1 = twice the dynamics;
// GHOST 0 = no ghost notes, fills or slides .. 1 = twice as many. The random draws are the same at any setting.
void arp_articulate(int mode, int len, int spb, int bar, double density, double artic, double expr, double ghost, std::vector<GNote> &v) {
    artic = std::max(0.0, std::min(1.0, artic)); expr = std::max(0.0, std::min(1.0, expr)); ghost = std::max(0.0, std::min(1.0, ghost));
    const bool rhythmic = mode == AM_OSTINATO || mode == AM_RIFF || mode == AM_PEDAL;
    const int unit = mode == AM_CALL ? bar : std::max(1, std::min(len, bar * 2));
    const int half = std::max(1, spb / 2);
    // each phrase's lowest and highest row (the swell)
    std::map<int, std::pair<int, int>> span;
    for (auto &n : v) {
        auto it = span.find(n.s / unit);
        if (it == span.end()) span[n.s / unit] = {n.row, n.row};
        else { it->second.first = std::min(it->second.first, n.row); it->second.second = std::max(it->second.second, n.row); }
    }
    int fill_bar = -1;
    for (size_t i = 0; i < v.size(); i++) {
        GNote &n = v[i];
        const GNote *nx = i + 1 < v.size() ? &v[i + 1] : nullptr;
        const int u = n.s / unit, d = nx ? nx->row - n.row : 0;
        const bool phrase_end = !nx || nx->s / unit != u;
        const bool beat = n.s % spb == 0, eighth = n.s % half == 0;
        const std::pair<int, int> sp = span[u];
        // dynamics: louder towards the phrase's top note; an answer (odd phrase of CALL) softer
        double dyn = 92 + 20.0 * (n.row - sp.first) / std::max(1, sp.second - sp.first);
        if (mode == AM_CALL && u % 2 == 1) dyn *= 0.9;
        int gate = 0, skip = 0, ratch = 0, slide = 0;
        if (phrase_end) { gate = 100; dyn *= 0.94; }   // tenuto: held, eased off
        else if (rhythmic) {
            if (beat) gate = 85;
            else if (eighth) gate = 45;   // off-beat: staccato
            else { gate = 35; dyn *= 1 - 0.38 * std::min(1.0, 2 * ghost); if (arp_u() < 0.8 * ghost) skip = 20 + int(arp_u() * 15); }   // a ghost note
        } else if (mode == AM_ARPEGGIO) {
            gate = n.row == sp.second ? 100 : 70;
            if (n.row == sp.second) dyn *= 1.08;
        } else {   // melodic
            if (d == 0) gate = 55;                                 // a repeated note: shorter, it speaks again
            else if (std::abs(d) <= 1) {                           // a step: legato, sometimes slid
                gate = 100;
                if (n.len <= spb && arp_u() < 0.6 * ghost) slide = 1;
            } else if (std::abs(d) >= 3) { gate = 70; dyn *= 1.12; }   // a leap: detached, accented
            else gate = 90;
            if (!beat && !eighth) dyn *= 0.85;                     // 16ths in passing: lighter
        }
        // ARTIC: below the middle towards legato (100), above it shorter (down to 30% of the natural length)
        gate = artic < 0.5 ? int(std::lround(gate + (100 - gate) * (0.5 - artic) * 2)) : int(std::lround(gate * (1 - (artic - 0.5) * 1.4)));
        // the fill at the end of every fourth bar: an off-beat note of its last beat (2.4.12: the phrase's last note too -
        // in ORCH RIFF it is the only one there, so the fill never came)
        if ((mode == AM_RIFF || mode == AM_OSTINATO) && n.s / bar % 4 == 3 && n.s / bar != fill_bar && n.s % bar >= bar - spb && !beat &&
            arp_u() < std::min(1.0, 1.4 * ghost)) {
            ratch = density > 0.6 ? 3 : 2;
            fill_bar = n.s / bar;
        }
        n.gate = std::max(5, std::min(100, gate));
        n.skip = skip;
        n.ratch = ratch;
        n.slide = slide;
        n.nvel = std::max(30, std::min(160, int(std::lround(100 + (dyn - 100 + (arp_u() - 0.5) * 6) * 2 * expr))));   // EXPR scales the shape around 100
    }
}

// 2.4.18: a pattern's steps at another step length (RATE): finer, each step repeats the one it falls in; coarser, a step
// holds every note of the steps it covers. Returns the steps (64 at most; 0 for none).
int arp_resample(const uint16_t *m, int n, int from, int to, uint16_t *out) {
    from = std::max(0, std::min(Arp::RATES - 1, from));
    to = std::max(0, std::min(Arp::RATES - 1, to));
    n = std::max(0, std::min(Arp::STEPS, n));
    if (n == 0) return 0;
    if (from == to) { std::copy(m, m + n, out); return n; }
    const double fb = Arp::RATE_BEATS[from], tb = Arp::RATE_BEATS[to];
    const int outn = std::max(1, std::min(Arp::STEPS, int(std::floor(n * fb / tb + 1e-6))));
    for (int t = 0; t < outn; t++) {
        const int s0 = int(std::floor(t * tb / fb + 1e-6)), s1 = std::max(s0 + 1, int(std::ceil((t + 1) * tb / fb - 1e-6)));
        uint16_t v = 0;
        for (int s = s0; s < s1 && s < n; s++) v = uint16_t(v | m[s]);
        out[t] = v;
    }
    return outn;
}

// ---------------------------------------------------------------------------------------------------------------
// 2.4.18 GENERATE over a file of chords (the user: "the motif doesn't spread the notes and generate, it just adds on top
// of the notes rather than using those notes as a baseline to generate a new sequence, riff"). Until 2.4.17 the file's
// chords stayed on the grid as they were and a one-note line was squeezed into the rows above them (with the high
// voicings of real chord files: one to five rows, at times on the chords). Now the chords are the material, and they
// are replaced: each step's chord (its pitch classes and its bass, the file read at the grid's step) is spread over
// the whole range as a ladder (every row of its pitch classes), and the mode builds a new part on that ladder:
//   OSTINATO      one cell of chord tones (bass, fifth, octave, third), again and again, through the chords
//   ORCH RIFF     a one-bar riff (bass or its octave on the beats, chord tones between, rests by DENSITY, beats pushed
//                 an eighth early by SYNCOPATION), the same figure through the progression, a fill every fourth bar
//   HIGH TO LOW / LOW TO HIGH   the chords broken, cascading down (up) the ladder a beat at a time
//   PEDAL         a steady pulse: the chord's bass on every other note, a melody of chord tones above it
//   ARPEGGIO      the chords broken over the range: Alberti below 40 % DENSITY, else up and down (16ths above 70 %)
//   the contours, CALL + ANSWER   a melody (chord tones on the beats and at every change, passing notes between: with
//                 a Chromatic key only the file's own notes) over the chord's bass, struck at each change and each bar
// RANGE here: at least an octave and a half (a ladder needs room). No note goes below the bass it plays over.
// Returns the chords (changes of harmony) found.
int arp_compose_harmony(Omni *o, int mode, int len, const uint16_t *smask, int src_len, std::vector<GNote> &out) {
    out.clear();
    len = std::max(1, std::min(Arp::STEPS, len));
    src_len = std::max(1, src_len);
    const int spb = arp_per_beat(o), bar = std::max(1, std::min(len, spb * 4));
    const int scale_k = int(num(o, "arp_scale") + 0.5f), root_k = int(num(o, "arp_root") + 0.5f), base_k = o->sampler.arp.settings.base.load();
    const ArpKey key{scale_k, root_k, base_k, Arp::scale_size(scale_k)};
    const int oct = std::max(1, key.size);   // rows an octave
    const int top = std::max(2, std::min(15, std::max(int(num(o, "arp_g_range") + 0.5f), (3 * oct + 1) / 2 + 1) - 1));
    const double density = num(o, "arp_g_density") / 100.0, sync = num(o, "arp_g_sync") / 100.0;
    const bool tie = num(o, "arp_tie") > 0.5f;
    // the harmony at each step: every note sounding where a note last started, as pitch classes over the key's root,
    // and its bass. A chord changes only when a note starts: notes that end (a held tone the import left a step short
    // where it starts again, a gap) leave the chord as it was
    std::vector<uint16_t> pcs(size_t(len), 0);
    std::vector<int> bass(size_t(len), 0);
    uint16_t coll = 0;   // every pitch class the file plays
    {
        std::vector<uint16_t> hm(size_t(len), 0);
        uint16_t last = 0, first = 0;
        for (int st = 0; st < len; st++) {
            const uint16_t m = smask[st % src_len], before = st % src_len ? smask[st % src_len - 1] : 0;
            if (m & ~before) { last = m; if (!first) first = m; }   // a note starts here
            hm[size_t(st)] = last;
        }
        for (int st = 0; st < len && !hm[size_t(st)]; st++) hm[size_t(st)] = first;
        for (int st = 0; st < len; st++) {
            const uint16_t m = hm[size_t(st)];
            for (int r = 0; r < 16; r++) if (m >> r & 1) pcs[size_t(st)] = uint16_t(pcs[size_t(st)] | 1u << key.iv(r));
            bass[size_t(st)] = m ? key.iv(__builtin_ctz(uint32_t(m))) : 0;
            if (!pcs[size_t(st)]) pcs[size_t(st)] = 1;   // (no notes at all: the key's root)
            coll = uint16_t(coll | pcs[size_t(st)]);
        }
    }
    auto at = [&](int st) { return size_t(std::max(0, std::min(len - 1, st))); };
    auto tone = [&](int st, int r) { return r >= 0 && r <= top && (pcs[at(st)] >> key.iv(r) & 1) != 0; };
    auto bass_row = [&](int st) {
        for (int r = 0; r <= top; r++) if (key.iv(r) == bass[at(st)]) return r;
        for (int r = 0; r <= top; r++) if (tone(st, r)) return r;
        return 0;
    };
    auto ladder = [&](int st, int from) {   // the chord's rows from `from` up to the top of the range (none: from the bottom)
        std::vector<int> v;
        for (int r = std::max(0, from); r <= top; r++) if (tone(st, r)) v.push_back(r);
        for (int r = 0; v.empty() && r <= top; r++) if (tone(st, r)) v.push_back(r);
        if (v.empty()) v.push_back(std::max(0, std::min(top, from)));
        return v;
    };
    auto sig = [&](int st) { return uint32_t(pcs[at(st)]) << 4 | uint32_t(bass[at(st)]); };
    auto change = [&](int st) { return st <= 0 || sig(st) != sig(st - 1); };
    auto passing = [&](int r) { return scale_k != 0 || (coll >> key.iv(r) & 1) != 0; };
    int chords = 0;
    for (int st = 0; st < len; st++) chords += change(st);
    // the rhythm (the contours, CALL, PEDAL, OSTINATO's cell): one bar's motif, kept; the third bar of four varies; the
    // last bar settles on its first half
    const std::vector<int> motif = arp_rhythm(bar, spb, density, sync), var = arp_rhythm(bar, spb, density, sync);
    std::vector<int> onsets;
    for (int b0 = 0, b = 0; b0 < len; b0 += bar, b++) {
        const bool last = b0 + bar >= len && b > 0;
        const std::vector<int> &r = b % 4 == 2 ? var : motif;
        for (int st : r) {
            if (b0 + st >= len) break;
            if (last && st > bar / 2) break;
            onsets.push_back(b0 + st);
        }
    }
    if (onsets.empty()) onsets.push_back(0);
    std::vector<GNote> v, bs;   // the new part (one note a step); the bass under a melody (held)
    auto add = [&](int st, int row) { if (st >= 0 && st < len) v.push_back({st, 1, std::max(0, std::min(top, row)), 0}); };
    const int half = std::max(1, spb / 2);   // an eighth (a step at 1/4)
    const bool line = !(mode == AM_OSTINATO || mode == AM_RIFF || mode == AM_DOWN || mode == AM_UP || mode == AM_PEDAL || mode == AM_ARPEGGIO);
    switch (mode) {
        case AM_OSTINATO: {
            const int cell_len = std::max(1, std::min(bar, spb * (density > 0.5 ? 1 : 2)));
            std::vector<int> cell;
            for (int st : motif) if (st < cell_len) cell.push_back(st);
            for (int st = 0; st < cell_len && cell.size() < 2; st += half)   // two notes a cell at least
                if (std::find(cell.begin(), cell.end(), st) == cell.end()) cell.push_back(st);
            std::sort(cell.begin(), cell.end());
            // ladder tones from the bass (root fifth octave third); every 4th bar from the octave down. The figure runs on
            // over the cells of a bar (a two-note cell plays root-fifth, then octave-third: every tone of the chord)
            static const int PAT[2][4] = {{0, 2, 3, 1}, {3, 2, 1, 2}};
            int k = 0;
            for (int c0 = 0; c0 < len; c0 += cell_len)
                for (size_t ci = 0; ci < cell.size(); ci++, k++) {
                    const int st = c0 + cell[ci];
                    if (st >= len) break;
                    if (c0 % bar == 0 && ci == 0) k = 0;
                    const std::vector<int> L = ladder(st, bass_row(st));
                    const int idx = PAT[(st / bar) % 4 == 3 ? 1 : 0][k % 4];
                    add(st, L[size_t(std::min(idx, int(L.size()) - 1))]);
                }
            break;
        }
        case AM_RIFF: {
            enum { R_REST, R_BASS, R_OCT, R_TONE };
            const int slots = std::max(1, bar / half);
            std::vector<std::pair<int, int>> fig(size_t(slots), {R_REST, 0});   // the bar's figure: (role, ladder tone)
            int prev_k = 0;
            for (int j = 0; j < slots; j++) {
                if (j == 0) { fig[0] = {R_BASS, 0}; continue; }
                if ((j * half) % spb == 0) fig[size_t(j)] = {arp_u() < 0.3 ? R_OCT : R_BASS, 0};
                else if (arp_u() < std::min(0.95, density + 0.2)) {
                    int k = 1 + int(arp_u() * 3) % 3;
                    if (k == prev_k) k = k % 3 + 1;
                    prev_k = k;
                    fig[size_t(j)] = {R_TONE, k};
                }
            }
            for (int j = 2; j < slots; j++)   // SYNCOPATION: a beat pushed an eighth early
                if ((j * half) % spb == 0 && half < spb && fig[size_t(j)].first != R_REST && arp_u() < sync * 0.6) {
                    fig[size_t(j - 1)] = fig[size_t(j)];
                    fig[size_t(j)] = {R_REST, 0};
                }
            {   // (after SYNCOPATION, which moves beats onto off-beats) the chord's other tones (third, fifth) in every bar: an off-beat rest (else its octave tone, else a beat SYNCOPATION emptied) takes a missing one
                bool has[4] = {};
                for (auto &f : fig) if (f.first == R_TONE) has[f.second] = true;
                for (int want = 1; want <= 2; want++) {
                    if (has[want]) continue;
                    for (int pass = 0; pass < 3 && !has[want]; pass++)   // an off-beat rest, an off-beat octave tone, a beat left empty
                        for (int j = 1; j < slots; j++) {
                            const bool beat = (j * half) % spb == 0;
                            const auto &f = fig[size_t(j)];
                            if (pass < 2 && beat) continue;
                            if (pass == 0 ? f.first != R_REST : pass == 1 ? !(f.first == R_TONE && f.second == 3) : f.first != R_REST) continue;
                            fig[size_t(j)] = {R_TONE, want};
                            has[want] = true;
                            break;
                        }
                }
            }
            for (int b0 = 0, b = 0; b0 < len; b0 += bar, b++)
                for (int j = 0; j < slots; j++) {
                    const int st = b0 + j * half;
                    if (st >= len) break;
                    std::pair<int, int> role = fig[size_t(j)];
                    if (b % 4 == 3 && j * half >= bar - spb && role.first == R_REST) role = {R_TONE, 3 - j % 3};   // the fill
                    if (role.first == R_REST) continue;
                    const int br = bass_row(st);
                    const std::vector<int> L = ladder(st, br);
                    int r = br;
                    if (role.first == R_OCT) r = br + oct <= top ? br + oct : L[std::min(L.size() - 1, size_t(3))];
                    else if (role.first == R_TONE) r = L[size_t(role.second) % L.size()];
                    add(st, r);
                }
            break;
        }
        case AM_DOWN: case AM_UP: {
            const bool down = mode == AM_DOWN;
            const int n = std::max(1, std::min(4, spb)), stp = std::max(1, spb / n);
            int prev = down ? top : 0;
            for (int b0 = 0; b0 < len; b0 += spb) {
                const std::vector<int> L = ladder(b0, bass_row(b0));
                const int N = int(L.size());
                int i = 0;   // a tone on from where the last beat started; past the ladder's end: back to its other end
                for (int q = 1, best = std::abs(L[0] - prev); q < N; q++) if (std::abs(L[size_t(q)] - prev) < best) { best = std::abs(L[size_t(q)] - prev); i = q; }
                i += down ? -1 : 1;
                if (b0 == 0 || N < n || (down ? i - (n - 1) < 0 : i + (n - 1) > N - 1)) i = down ? N - 1 : 0;
                prev = L[size_t(i)];
                for (int k = 0; k < n; k++) {
                    const int st = b0 + k * stp;
                    if (st >= len) break;
                    const int q = (((down ? i - k : i + k) % N) + N) % N;
                    const int r = L[size_t(q)];
                    add(st, tone(st, r) ? r : arp_nearest(r, bass_row(st), top, [&](int x) { return tone(st, x); }));   // (a change inside the beat)
                }
            }
            break;
        }
        case AM_PEDAL: {
            int mel = -1;
            double walk = arp_u();
            uint16_t used = 0;   // the chord's tones the melody has played since it came (prefers one not heard yet)
            uint32_t used_sig = ~0u;
            // a steady pulse, the bass and the melody in turn (eighths; sixteenths above 70 % DENSITY)
            const int pulse = std::max(1, density > 0.7 ? spb / 4 : spb / 2);
            onsets.clear();
            for (int st = 0; st < len; st += pulse) onsets.push_back(st);
            for (size_t i = 0; i < onsets.size(); i++) {
                const int st = onsets[i], br = bass_row(st);
                if (sig(st) != used_sig) { used_sig = sig(st); used = uint16_t(1u << bass[at(st)]); }
                if (i % 2 == 0) { add(st, br); continue; }
                const int lo = std::min(top, br + 2);
                const int target = lo + int(std::lround(arp_shape(AM_ARCH, double(st) / std::max(1, len - 1), walk) * (top - lo)));
                if (mel < 0) mel = target;
                mel = std::max(lo, std::min(top, mel + std::max(-2, std::min(2, target - mel))));
                // the chord tone nearest where the arch goes, one this chord has not played yet first (a chord's tones all
                // sound over its notes: the pedal melody outlines the harmony)
                int best = -1, bd = 1 << 30;
                for (int r = lo; r <= top; r++) {
                    if (!tone(st, r)) continue;
                    const int d = std::abs(r - mel) + ((used >> key.iv(r) & 1) ? 8 : 0);
                    if (d < bd) { bd = d; best = r; }
                }
                mel = best >= 0 ? best : arp_nearest(mel, 0, top, [&](int x) { return tone(st, x); });
                used = uint16_t(used | 1u << key.iv(mel));
                add(st, mel);
            }
            break;
        }
        case AM_ARPEGGIO: {
            const int pulse = std::max(1, density > 0.7 ? spb / 4 : spb / 2);
            int k = 0;
            uint32_t prev = ~0u;
            for (int st = 0; st < len; st += pulse) {
                if (sig(st) != prev || st % bar == 0) k = 0;   // anew at each chord and each bar
                prev = sig(st);
                const std::vector<int> L = ladder(st, bass_row(st));
                const int N = int(L.size());
                int q;
                if (density < 0.4) { static const int AL[4] = {0, 2, 1, 2}; q = std::min(N - 1, AL[k % 4]); }   // Alberti
                else { const int per = std::max(1, 2 * (N - 1)), p = k % per; q = N <= 1 ? 0 : p < N ? p : per - p; }   // up and down
                add(st, L[size_t(q)]);
                k++;
            }
            break;
        }
        default: {   // the contours and CALL + ANSWER: a melody over the chords' bass
            for (int st = 1; st < len; st++) if (change(st)) onsets.push_back(st);   // every chord change is heard
            std::sort(onsets.begin(), onsets.end());
            onsets.erase(std::unique(onsets.begin(), onsets.end()), onsets.end());
            const int unit = mode == AM_CALL ? bar : std::min(len, bar * 2);
            double walk = arp_u();
            int row = -1, last_leap = 0, cur = -1, head_k = 0;
            std::vector<int> head;   // the call's first beat: the answer starts with it
            uint16_t used = 0;       // the chord's pitch classes the melody (or the bass) has played since it came
            uint32_t used_sig = ~0u;
            for (size_t i = 0; i < onsets.size(); i++) {
                const int st = onsets[i], u = st / unit, in_u = st - u * unit;
                int lo = std::min(top, bass_row(st) + 1);   // above the bass (no chord tone there: the whole range)
                { bool any = false; for (int r = lo; r <= top && !any; r++) any = tone(st, r); if (!any) lo = 0; }
                auto ct = [&](int r) { return r >= lo && tone(st, r); };
                auto ok = [&](int r) { return r >= lo && passing(r); };
                const bool strong = st % spb == 0 || change(st), answer = mode == AM_CALL && u % 2 == 1;
                if (u != cur) { cur = u; head_k = 0; if (!answer) head.clear(); }
                const bool last = i + 1 == onsets.size(), unit_end = last || onsets[i + 1] / unit != u;
                const int target = lo + int(std::lround(arp_shape(mode, double(st) / std::max(1, len - 1), walk) * (top - lo)));
                if (row < 0) row = arp_nearest(target, lo, top, ct);
                else if (answer && in_u < spb && head_k < int(head.size())) row = head[size_t(head_k)];
                else {
                    int step = std::max(-2, std::min(2, target - row));
                    if (step == 0 && arp_u() < 0.5) step = arp_u() < 0.5 ? -1 : 1;   // a line moves
                    if (last_leap) step = last_leap > 0 ? -1 : 1;                     // a leap, then a step back
                    else if (strong && arp_u() < 0.18) step = (target >= row ? 1 : -1) * (3 + int(arp_u() * 2));
                    last_leap = std::abs(step) >= 3 ? step : 0;
                    row = std::max(lo, std::min(top, row + step));
                }
                row = arp_nearest(row, lo, top, strong ? std::function<bool(int)>(ct) : std::function<bool(int)>(ok));
                if (sig(st) != used_sig) { used_sig = sig(st); used = uint16_t(1u << bass[at(st)]); }   // (the bass sounds its own)
                if (strong && (used >> key.iv(row) & 1)) {   // a chord tone not heard yet in this chord, if it is near (a step
                    // or two costs less than a tone heard again: the melody outlines each chord without leaping about)
                    int best = row, bd = 1 << 30;
                    for (int r = lo; r <= top; r++) {
                        if (!ct(r)) continue;
                        const int d = std::abs(r - row) + ((used >> key.iv(r) & 1) ? (change(st) ? 5 : 3) : 0);
                        if (d < bd) { bd = d; best = r; }
                    }
                    row = best;
                }
                if (unit_end) {   // a call ends open (a chord tone but the bass's); an answer, a contour's phrase, the end: on the bass's
                    const bool home = mode != AM_CALL || answer || last;
                    const int pc = bass[at(st)];
                    row = arp_nearest(row, lo, top, [&](int r) { return ct(r) && (home ? key.iv(r) == pc : key.iv(r) != pc); });
                }
                if (in_u < spb) { if (answer) head_k++; else head.push_back(row); }
                used = uint16_t(used | 1u << key.iv(row));
                add(st, row);
            }
            // the bass: the chord's lowest note at each change and each bar it is held over, held to the next
            std::vector<int> hits;
            for (int st = 0; st < len; st++) if (change(st) || st % bar == 0) hits.push_back(st);
            for (size_t i = 0; i < hits.size(); i++) {
                GNote n{hits[i], (i + 1 < hits.size() ? hits[i + 1] : len) - hits[i], bass_row(hits[i]), 84};
                n.chord = true;
                bs.push_back(n);
            }
        }
    }
    std::stable_sort(v.begin(), v.end(), [](const GNote &a, const GNote &b) { return a.s < b.s; });
    std::vector<GNote> w;   // one note a step (the first)
    for (auto &n : v) if (w.empty() || w.back().s != n.s) w.push_back(n);
    // TIE joins a note to one on its row on the very next step: the later one goes to the nearest other chord tone there
    // (a melody's note before a bass on its row: the melody's moves)
    auto bass_starts = [&](int st, int r) { for (const GNote &b : bs) if (b.s == st && b.row == r) return true; return false; };
    if (tie)
        for (size_t i = 0; i < w.size(); i++) {   // (i-- after an erase: never at i 0, which has no note before it)
            const bool after = i > 0 && w[i].s == w[i - 1].s + 1 && w[i].row == w[i - 1].row;
            if (!after && !bass_starts(w[i].s + 1, w[i].row)) continue;
            const int st = w[i].s, was = w[i].row, lo = line ? std::min(top, bass_row(st) + 1) : 0;
            const int prev_row = i > 0 && w[i - 1].s == st - 1 ? w[i - 1].row : -1;
            const int alt = arp_nearest(was, lo, top, [&](int r) { return r != was && r != prev_row && tone(st, r) && !bass_starts(st + 1, r); });
            if (alt != was && tone(st, alt)) { w[i].row = alt; continue; }
            // no other chord tone (a narrow range: Chromatic's 16 rows are 16 semitones): the note before moves instead, to
            // a row of its own step the line may use (it would otherwise be moved a row off the chord when written)
            if (!after) continue;
            const int ps = w[i - 1].s, plo = line ? std::min(top, bass_row(ps) + 1) : 0;
            const int pprev = i > 1 && w[i - 2].s == ps - 1 ? w[i - 2].row : -1;
            const bool pstrong = ps % spb == 0 || change(ps);
            const int palt = arp_nearest(was, plo, top, [&](int r) {
                return r != was && r != pprev && (pstrong || !line ? tone(ps, r) : passing(r)) && !bass_starts(ps + 1, r);
            });
            if (palt != was) { w[i - 1].row = palt; continue; }
            // neither can move (one chord tone in range): the repeat goes, the note before is held over it instead
            w.erase(w.begin() + std::ptrdiff_t(i));
            i--;
        }
    int hi = 0;
    for (auto &n : w) hi = std::max(hi, n.row);
    for (size_t i = 0; i < w.size(); i++) {
        const int next = i + 1 < w.size() ? w[i + 1].s : len;
        w[i].len = std::max(1, next - w[i].s);
        const bool beat = w[i].s % spb == 0, bar1 = w[i].s % bar == 0;
        w[i].vel = std::max(40, std::min(127, (bar1 ? 112 : beat ? 100 : 82) + int(10.0 * w[i].row / std::max(1, hi)) + int((arp_u() - 0.5) * 12)));
    }
    const double artic = num(o, "arp_g_artic") / 100.0, expr = num(o, "arp_g_expr") / 100.0;
    arp_articulate(mode, len, spb, bar, density, artic, expr, num(o, "arp_g_ghost") / 100.0, w);
    // EXPR: each two-bar phrase swells to its middle, the chord changes accented (as over the 2.4.12 chords)
    const int phrase = std::max(1, std::min(len, bar * 2));
    auto swell = [&](int st) {
        const double arc = std::sin(M_PI * double(st % phrase) / double(phrase));
        return (1 + (arc - 0.5) * expr) * (st > 0 && change(st) ? 1 + 0.3 * expr : 1.0);
    };
    for (GNote &n : w) n.nvel = std::max(30, std::min(160, int(std::lround((n.nvel ? n.nvel : 100) * swell(n.s)))));
    for (GNote &n : bs) {   // the bass under a melody: softer, held (ARTIC shortens it)
        n.nvel = std::max(30, std::min(160, int(std::lround((100 + (84 - 100) * 2 * expr) * swell(n.s)))));
        n.gate = std::max(5, std::min(100, artic < 0.5 ? 100 : int(std::lround(100 * (1 - (artic - 0.5) * 1.4)))));
    }
    out = bs;
    out.insert(out.end(), w.begin(), w.end());
    std::stable_sort(out.begin(), out.end(), [](const GNote &a, const GNote &b) { return a.s < b.s; });
    return chords;
}

void arp_compose(Omni *o, int mode, int len, const std::vector<std::pair<int, int>> &src, int src_len, const uint16_t *smask,
                 std::vector<GNote> &out) {
    const int spb = arp_per_beat(o), bar = std::max(1, std::min(len, spb * 4));
    const int scale_k = int(num(o, "arp_scale") + 0.5f), root_k = int(num(o, "arp_root") + 0.5f), base_k = o->sampler.arp.settings.base.load();
    const ArpKey key0{scale_k, root_k, base_k, Arp::scale_size(scale_k)};   // the grid's own rows
    // 2.4.12 (user: "if the MIDI has a chord, all those notes are included in the generation"): the file's harmony = every
    // note it sounds on each step (smask: the whole grid of the file, not its top note); a step without notes keeps the
    // chord before it. With chords, they stay on the grid as they are and the line is built above them, on their tones.
    std::vector<uint16_t> hmask(size_t(len), 0), hpc(size_t(len), 0);
    std::vector<int> hroot(size_t(len), 0);
    bool chordal = false;
    int ch_hi = 0;
    if (smask && src_len > 0) {
        for (int st = 0; st < src_len; st++) {
            if (__builtin_popcount(smask[st]) >= 2) chordal = true;
            if (smask[st]) ch_hi = std::max(ch_hi, 31 - __builtin_clz(uint32_t(smask[st])));
        }
        uint16_t last = 0;
        for (int st = 0; st < len; st++) { const uint16_t m = smask[st % src_len]; if (m) last = m; hmask[size_t(st)] = last; }
        uint16_t first = 0;
        for (int st = 0; st < len && !first; st++) first = hmask[size_t(st)];
        for (int st = 0; st < len && !hmask[size_t(st)]; st++) hmask[size_t(st)] = first;
        for (int st = 0; st < len; st++) {
            const uint16_t m = hmask[size_t(st)];
            for (int r = 0; r < 16; r++) if (m >> r & 1) hpc[size_t(st)] = uint16_t(hpc[size_t(st)] | 1u << key0.iv(r));
            hroot[size_t(st)] = m ? key0.iv(__builtin_ctz(uint32_t(m))) : 0;
        }
    }
    const int line_off = chordal ? std::max(0, std::min(ch_hi + 1, 11)) : 0;   // the line's row 0 on the grid: above the chords
    const int range = std::max(2, std::min(16 - line_off, int(num(o, "arp_g_range") + 0.5f))), top = range - 1;
    const double density = num(o, "arp_g_density") / 100.0, sync = num(o, "arp_g_sync") / 100.0;
    const ArpKey key{scale_k, root_k, base_k + line_off, key0.size};   // the line's rows
    const int oct = key.size;   // rows an octave
    auto chord = [&](int r) { return key.chord(r); };
    auto tonic = [&](int r) { return key.tonic(r); };
    auto at = [&](int st) { return size_t(std::max(0, std::min(len - 1, st))); };
    // the chord sounding at step st (the key's triad without a file's chords) and its root
    auto chord_at = [&](int st) -> std::function<bool(int)> {
        if (!chordal) return [&key](int r) { return key.chord(r); };
        const uint16_t pcs = hpc[at(st)];
        return [&key, pcs](int r) { return (pcs >> key.iv(r) & 1) != 0; };
    };
    auto root_at = [&](int st) -> std::function<bool(int)> {
        if (!chordal) return [&key](int r) { return key.tonic(r); };
        const int pc = hroot[at(st)];
        return [&key, pc](int r) { return key.iv(r) == pc; };
    };
    std::vector<GNote> ch;   // the chords' notes (grid rows)
    auto add_chord = [&](int st, int l, int r) {
        if (st < 0 || st >= len || r < 0 || r > 15) return;
        GNote n{st, std::max(1, std::min(l, len - st)), r, 92};
        n.chord = true;
        ch.push_back(n);
    };
    const bool have = !src.empty();
    // the source's material: its rows (vocabulary, folded into the range), its onsets, its first cell
    std::vector<int> vocab;
    int s_lo = 15;
    for (auto &n : src) s_lo = std::min(s_lo, n.second);
    auto fold = [&](int r) { while (r > top && r - oct >= 0) r -= oct; return std::max(0, std::min(top, r)); };
    for (auto &n : src) { const int r = fold(n.second - s_lo); if (std::find(vocab.begin(), vocab.end(), r) == vocab.end()) vocab.push_back(r); }
    std::sort(vocab.begin(), vocab.end());
    // (with chords the line may use the whole scale: its strong beats go to the chord's tones instead)
    auto in_vocab = [&](int r) { return !have || chordal || vocab.empty() || std::find(vocab.begin(), vocab.end(), r) != vocab.end(); };
    // a cell: (step from the cell's start, row offset from its first note)
    std::vector<std::pair<int, int>> cell;
    int cell_len = spb;
    if (have) {
        cell_len = spb * (src.size() > 1 && src[1].first < spb ? 1 : 2);
        if (mode == AM_RIFF) cell_len = bar;
        for (auto &n : src) if (n.first < cell_len) cell.push_back({n.first, n.second - src[0].second});
        if (chordal) { cell.clear(); cell_len = spb; }   // the line is new, over the chords
    }
    std::vector<int> onsets;   // the rhythm over [0, len)
    if (chordal && mode == AM_ARPEGGIO) {   // 2.4.12: broken chords run on a steady pulse (eighths; DENSITY above 70 %: sixteenths)
        const int step = std::max(1, density > 0.7 ? spb / 4 : spb / 2);
        for (int st = 0; st < len; st += step) onsets.push_back(st);
    } else if (have && !chordal) {   // the file's rhythm, round again to fill the pattern; DENSITY far from 50 % drops or adds weak steps
        // (a file of chords gives the line no rhythm of its own - its chords are held - so the line gets the own rhythm below)
        const int sl = std::max(1, src_len);
        for (int st = 0; st < len; st++) {
            bool hit = false;
            for (auto &n : src) if (n.first == st % sl) { hit = true; break; }
            const bool beat = st % spb == 0;
            if (hit && !beat && density < 0.4 && arp_u() < (0.4 - density) * 2) hit = false;
            if (!hit && density > 0.6 && arp_u() < (density - 0.6) * (beat ? 1.5 : 0.6)) hit = true;
            if (hit) onsets.push_back(st);
        }
    } else {
        const std::vector<int> motif = arp_rhythm(bar, spb, density, sync), var = arp_rhythm(bar, spb, density, sync);
        for (int b0 = 0, b = 0; b0 < len; b0 += bar, b++) {
            const bool last = b0 + bar >= len && b > 0;
            const std::vector<int> &r = b % 4 == 2 ? var : motif;   // bar 3 varies; the others keep the motif
            for (int st : r) {
                if (b0 + st >= len) break;
                if (last && st > bar / 2) break;   // the cadence: the last bar settles on its second half
                onsets.push_back(b0 + st);
            }
        }
    }
    if (onsets.empty()) onsets.push_back(0);
    out.clear();
    auto add = [&](int st, int row) { if (st >= 0 && st < len) out.push_back({st, 1, std::max(0, std::min(top, row)), 0}); };
    const int prog[4] = {0, 5, 3, 4};   // i - vi - iv - v (degrees): the riff's and arpeggio's bars
    auto prog_root = [&](int b) { const int d = prog[b % 4] % oct; return d > top ? d - oct : d; };
    switch (mode) {
        case AM_OSTINATO: {   // one cell, again and again (from the file: its opening cell)
            if (!have || chordal) {   // root, fifth, octave or a neighbour, in a 1- or 2-beat cell on the rhythm's first notes
                cell_len = spb * (density > 0.5 ? 1 : 2);
                const int r5 = arp_nearest(std::min(top, oct / 2 + 1), 0, top, [&](int r) { return key.fifth(r); });
                const int pat[4] = {0, r5, std::min(top, oct), r5};
                int k = 0;
                for (int st : onsets) if (st < cell_len) cell.push_back({st, pat[k++ % 4]});
            }
            for (int c0 = 0; c0 < len; c0 += cell_len) {
                const int base_r = chordal ? arp_nearest(0, 0, top, root_at(c0)) : have ? fold(src[0].second - s_lo) : 0;   // follows the chords
                for (auto &c : cell) add(c0 + c.first, base_r + c.second);
            }
            break;
        }
        case AM_RIFF: {   // a driving bar (root and its octave / fifth accents) moved through i - vi - iv - v
            if (chordal) {   // 2.4.12: the file's chords as driving stabs (eighths; DENSITY keeps more of the off-beats)
                const int step = std::max(1, spb / 2);
                for (int st = 0; st < len; st += step) {
                    const bool beat = st % spb == 0;
                    if (!beat && arp_u() > density + 0.25) continue;
                    for (int r = 0; r < 16; r++) if (hmask[size_t(st)] >> r & 1) add_chord(st, step, r);
                }
                break;
            }
            if (!have) {
                const int r5 = arp_nearest(std::min(top, oct / 2 + 1), 0, top, [&](int r) { return key.fifth(r); });
                const int step = std::max(1, spb / 2);   // eighth notes
                const int pat[8] = {0, 0, r5, 0, std::min(top, oct), 0, r5, 0};
                for (int st = 0, k = 0; st < bar; st += step, k++) cell.push_back({st, pat[k % 8]});
            }
            for (int b0 = 0, b = 0; b0 < len; b0 += bar, b++)
                for (auto &c : cell) {
                    int r = (have ? fold(src[0].second - s_lo) : 0) + prog_root(b) + c.second;
                    while (r > top && r - oct >= 0) r -= oct;
                    add(b0 + c.first, r);
                }
            break;
        }
        case AM_DOWN: case AM_UP: {   // a cell a beat long, a scale step lower (higher) each beat: a cascade
            const bool down = mode == AM_DOWN;
            if (!have) {
                const int n = std::max(1, std::min(4, spb)), step = std::max(1, spb / n);
                const int shape[4] = {0, down ? 2 : -2, down ? 1 : -1, down ? -1 : 1};
                for (int k = 0; k < n; k++) cell.push_back({k * step, shape[k]});
                cell_len = spb;
            }
            int lo_c = 0, hi_c = 0;
            for (auto &c : cell) lo_c = std::min(lo_c, c.second), hi_c = std::max(hi_c, c.second);
            const int beats = (len + cell_len - 1) / cell_len;
            const int start = down ? top - hi_c : -lo_c;
            const int span = std::max(1, top - (hi_c - lo_c));
            for (int b = 0; b < beats; b++) {
                int base = down ? start - (b % (span + 1)) : start + (b % (span + 1));
                if (chordal) base = arp_nearest(base, std::max(0, -lo_c), std::max(0, top - hi_c), chord_at(b * cell_len));   // on the chord
                for (auto &c : cell) add(b * cell_len + c.first, base + c.second);
            }
            break;
        }
        case AM_PEDAL: {   // the root (the file's lowest note) on every other note; a melody above it
            const int ped = 0;
            int mel = arp_nearest(std::min(top, oct / 2 + 2), 1, top, chord);
            double walk = arp_u();
            for (size_t i = 0; i < onsets.size(); i++) {
                if (i % 2 == 0) { add(onsets[i], chordal ? arp_nearest(0, 0, top, root_at(onsets[i])) : ped); continue; }   // (the chord's root)
                const double t = double(onsets[i]) / std::max(1, len - 1);
                const int target = 1 + int(std::lround(arp_shape(AM_ARCH, t, walk) * (top - 1)));
                mel += std::max(-2, std::min(2, target - mel));
                mel = std::max(1, std::min(top, mel));
                if (!in_vocab(mel)) mel = arp_nearest(mel, 1, top, in_vocab);
                if (chordal && onsets[i] % spb == 0) mel = arp_nearest(mel, 1, top, chord_at(onsets[i]));
                add(onsets[i], mel);
            }
            break;
        }
        case AM_ARPEGGIO: {   // each bar's chord broken upwards (from the file: its notes in order of pitch)
            if (chordal) {   // 2.4.12: the file's own chords broken upwards (every note, then the octave above), anew at each change
                int k = 0;
                uint16_t prev = 0;
                for (size_t i = 0; i < onsets.size(); i++) {
                    const int st = onsets[i];
                    const uint16_t m = hmask[size_t(st)];
                    if (m != prev || st % bar == 0) k = 0;
                    prev = m;
                    std::vector<int> tones;
                    for (int r = 0; r < 16; r++) if (m >> r & 1) tones.push_back(r);
                    const size_t n0 = tones.size();
                    for (size_t j = 0; j < n0; j++) if (tones[j] + oct <= 15) tones.push_back(tones[j] + oct);
                    if (tones.empty()) continue;
                    const int next = i + 1 < onsets.size() ? onsets[i + 1] : len;
                    add_chord(st, next - st, tones[size_t(k++) % tones.size()]);
                }
                break;
            }
            std::vector<int> tones;
            if (have) tones = vocab;
            else for (int r = 0; r <= top; r++) if (chord(r)) tones.push_back(r);
            if (tones.empty()) tones.push_back(0);
            int k = 0;
            for (int st : onsets) {
                const int b = st / bar;
                if (st % bar == 0) k = 0;
                int r = tones[size_t(k % int(tones.size()))] + (have ? 0 : prog_root(b));
                while (r > top && r - oct >= 0) r -= oct;
                add(st, r);
                k++;
            }
            break;
        }
        default: {   // the contours and CALL & ANSWER: a line over the rhythm, in units (a call, then an answer)
            const int unit = mode == AM_CALL ? bar : std::min(len, bar * 2);
            const std::function<bool(int)> home_ok = tonic, open_ok = [&](int r) { return key.fifth(r); };
            double walk = arp_u();
            int row = arp_nearest(int(std::lround(arp_shape(mode, 0, walk) * top)), 0, top, chord_at(0)), last_leap = 0;
            std::vector<int> head;   // the call's first beat (rows): the answer starts with it
            int head_k = 0, cur = -1;
            for (size_t i = 0; i < onsets.size(); i++) {
                const int st = onsets[i], u = st / unit, in_u = st - u * unit;
                const bool strong = st % spb == 0, answer = u % 2 == 1;
                if (u != cur) { cur = u; head_k = 0; if (!answer) head.clear(); }
                const bool last = i + 1 == onsets.size(), unit_end = last || onsets[i + 1] / unit != u;
                const int target = int(std::lround(arp_shape(mode, double(st) / std::max(1, len - 1), walk) * top));
                if (have && !chordal && st < cell_len && i < cell.size()) {   // the file's opening cell, as it is
                    row = fold(src[0].second - s_lo + cell[i].second);
                } else if (answer && in_u < spb && head_k < int(head.size())) {
                    row = head[size_t(head_k)];
                    if (chordal && strong) row = arp_nearest(row, 0, top, chord_at(st));   // the call's head, on this chord
                } else {
                    int step = std::max(-2, std::min(2, target - row));
                    if (step == 0 && arp_u() < 0.5) step = arp_u() < 0.5 ? -1 : 1;   // a line moves
                    if (last_leap) step = last_leap > 0 ? -1 : 1;   // a leap is answered by a step back
                    else if (strong && arp_u() < 0.18) step = (target >= row ? 1 : -1) * (3 + int(arp_u() * 2));   // now and then a leap
                    last_leap = std::abs(step) >= 3 ? step : 0;
                    row = std::max(0, std::min(top, row + step));
                    if (strong) row = arp_nearest(row, 0, top, chord_at(st));   // beats on chord tones (the file's chord there)
                }
                if (!in_vocab(row)) row = arp_nearest(row, 0, top, in_vocab);
                if (unit_end) {   // a call ends open (the fifth); an answer, any phrase of the contours and the end: the root
                    // (with the file's chords: the chord's root, a call ends on any of its tones)
                    row = arp_nearest(row, 0, top, answer || last || mode != AM_CALL ? (chordal ? root_at(st) : home_ok) : (chordal ? chord_at(st) : open_ok));
                    if (!in_vocab(row)) row = arp_nearest(row, 0, top, in_vocab);
                }
                if (in_u < spb) { if (answer) head_k++; else head.push_back(row); }
                add(st, row);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const GNote &a, const GNote &b) { return a.s < b.s; });
    // one note a step (the first kept), lengths to the next note, velocities: beats accented, a swell to the top note
    std::vector<GNote> v;
    for (auto &n : out) if (v.empty() || v.back().s != n.s) v.push_back(n);
    int hi = 0;
    for (auto &n : v) hi = std::max(hi, n.row);
    for (size_t i = 0; i < v.size(); i++) {
        const int next = i + 1 < v.size() ? v[i + 1].s : len;
        v[i].len = std::max(1, next - v[i].s);
        const bool beat = v[i].s % spb == 0, bar1 = v[i].s % bar == 0;
        v[i].vel = std::max(40, std::min(127, (bar1 ? 112 : beat ? 100 : 82) + int(10.0 * v[i].row / std::max(1, hi)) + int((arp_u() - 0.5) * 12)));
    }
    // over chords, a note repeated on the very next step (TIE would join the two; the grid would move the second off the
    // chord) becomes the nearest other tone of the chord there
    if (chordal)
        for (size_t i = 1; i < v.size(); i++)
            if (v[i].s == v[i - 1].s + 1 && v[i].row == v[i - 1].row) {
                const std::function<bool(int)> ct = chord_at(v[i].s);
                const int was = v[i].row;
                v[i].row = arp_nearest(was, 0, top, [&](int r) { return r != was && ct(r); });
            }
    const double artic = num(o, "arp_g_artic") / 100.0, expr = num(o, "arp_g_expr") / 100.0;
    arp_articulate(mode, len, spb, bar, density, artic, expr, num(o, "arp_g_ghost") / 100.0, v);
    // 2.4.12 (user: "greater expression"): EXPR also shapes each two-bar phrase (a swell to its middle) and accents the
    // chord changes; 0 = one level as before
    const int phrase = std::max(1, std::min(len, bar * 2));
    auto swell = [&](int st, bool change) {
        const double arc = std::sin(M_PI * double(st % phrase) / double(phrase));
        return (1 + (arc - 0.5) * 0.5 * 2 * expr) * (change ? 1 + 0.15 * 2 * expr : 1.0);
    };
    auto changes = [&](int st) { return chordal && st > 0 && hmask[size_t(st)] != hmask[size_t(st - 1)]; };
    for (GNote &n : v) {
        n.nvel = std::max(30, std::min(160, int(std::lround((n.nvel ? n.nvel : 100) * swell(n.s, changes(n.s))))));
        n.row += line_off;
    }
    if (chordal && mode != AM_RIFF && mode != AM_ARPEGGIO) {   // the file's chords, kept as they are (every note, its length)
        for (int st = 0; st < len; st++) {
            const uint16_t m = smask[st % src_len];
            for (int r = 0; r < 16; r++) {
                if (!(m >> r & 1)) continue;
                if (st % src_len && (smask[(st - 1) % src_len] >> r & 1) && st > 0) continue;   // held from the step before
                int l = 1;
                while (st + l < len && (st + l) % src_len && (smask[(st + l) % src_len] >> r & 1)) l++;
                add_chord(st, l, r);
            }
        }
    }
    // the chords' expression: under the line (softer), held or detached by ARTIC, the same swell; stabs accent the beats
    const bool stabs = mode == AM_RIFF;
    for (GNote &n : ch) {
        const bool beat = n.s % spb == 0, bar1 = n.s % bar == 0;
        double d = stabs ? (bar1 ? 105 : beat ? 92 : 72) : mode == AM_ARPEGGIO ? (bar1 ? 100 : 88) : 84;
        d = 100 + (d - 100) * 2 * expr;
        n.nvel = std::max(30, std::min(160, int(std::lround(d * swell(n.s, changes(n.s))))));
        int g = stabs ? 60 : 100;
        g = artic < 0.5 ? int(std::lround(g + (100 - g) * (0.5 - artic) * 2)) : int(std::lround(g * (1 - (artic - 0.5) * 1.4)));
        n.gate = std::max(5, std::min(100, g));
    }
    out = ch;
    out.insert(out.end(), v.begin(), v.end());
    std::stable_sort(out.begin(), out.end(), [](const GNote &a, const GNote &b) { return a.s < b.s; });   // (chords first on a step)
}

// write notes into [0, len): TIE on: each note held to the next (a step short when the next is on the same row, or
// that row's next note would join it); TIE off: one step each
int arp_write(Omni *o, int len, std::vector<GNote> &notes) {
    Arp &a = o->sampler.arp;
    const bool tie = num(o, "arp_tie") > 0.5f;
    for (int st = 0; st < len; st++) {
        a.mask[st].store(0); a.vel[st].store(100);
        for (int r = 0; r < 16; r++) arp_note_take(o, st * 16 + r);
    }
    // (adjacent notes of the line on one row would join with TIE: the second moves; 2.4.12: a chord's notes keep their rows)
    for (size_t i = 0, prev = SIZE_MAX; i < notes.size(); i++) {
        if (notes[i].chord) continue;
        if (tie && prev != SIZE_MAX && notes[i].row == notes[prev].row && notes[prev].s + 1 == notes[i].s)
            notes[i].row = notes[i].row + 1 < 16 ? notes[i].row + 1 : notes[i].row - 1;
        prev = i;
    }
    int made = 0;
    for (size_t i = 0; i < notes.size(); i++) {
        const GNote &n = notes[i];
        if (n.s < 0 || n.s >= len) continue;
        int l = tie ? n.len : 1;
        if (tie) {
            for (size_t j = i + 1; j < notes.size(); j++)
                if (notes[j].row == n.row) { l = std::min(l, notes[j].s - n.s - 1); break; }
            l = std::max(1, std::min(l, len - n.s));
        }
        for (int st = n.s; st < n.s + l; st++) a.mask[st].store(uint16_t(a.mask[st].load() | (1u << n.row)));
        a.vel[n.s].store(uint8_t(n.vel));
        // 2.4.11: the note's own expression (arp_articulate), where the grid keeps it (its first step and row)
        const int ni = n.s * 16 + n.row;
        a.ngate[ni].store(uint8_t(std::max(0, std::min(100, n.gate))));
        a.nvel[ni].store(uint8_t(std::max(0, std::min(200, n.nvel))));
        a.nskip[ni].store(uint8_t(std::max(0, std::min(99, n.skip))));
        a.nratch[ni].store(uint8_t(n.ratch >= 2 ? std::min(4, n.ratch) : 0));
        a.nslide[ni].store(uint8_t(n.slide ? 1 : 0));
        made++;
    }
    return made;
}

// the notes of [0, len) as runs: (row, first, last)
struct ArpRun { int r, a, b; };
std::vector<ArpRun> arp_runs(Omni *o, int len) {
    std::vector<ArpRun> out;
    const bool tie = num(o, "arp_tie") > 0.5f;
    for (int r = 0; r < 16; r++)
        for (int st = 0; st < len; st++) {
            if (!arp_lit(o, r, st)) continue;
            int e = st;
            if (tie) while (e + 1 < len && arp_lit(o, r, e + 1)) e++;
            out.push_back({r, st, e});
            st = e;
        }
    return out;
}
// rewrite [0, len) from runs moved by `fn` (notes keep their expression and velocity)
template <typename F> void arp_remap(Omni *o, int len, F fn) {
    Arp &a = o->sampler.arp;
    std::vector<ArpRun> runs = arp_runs(o, len);
    std::vector<std::pair<ArpNoteX, uint8_t>> data;
    for (const ArpRun &rn : runs) data.push_back({arp_note_take(o, rn.a * 16 + rn.r), a.vel[rn.a].load()});
    for (int st = 0; st < len; st++) { a.mask[st].store(0); a.vel[st].store(100); }
    for (size_t i = 0; i < runs.size(); i++) {
        ArpRun n = fn(runs[i]);
        if (n.a < 0 || n.a >= len || n.r < 0 || n.r > 15) continue;
        n.b = std::max(n.a, std::min(len - 1, n.b));
        for (int st = n.a; st <= n.b; st++) a.mask[st].store(uint16_t(a.mask[st].load() | (1u << n.r)));
        arp_note_put(o, n.a * 16 + n.r, data[i].first);
        a.vel[n.a].store(data[i].second);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 2.4.14 the GRID's chords (the user: "the grid needs an algorithm that can detect chords"; GRID_AUDIT.md sections 3-4)
// The pitches step s plays as written (SEQ: the root at C3 the reference key, as the pattern is drawn; while a key is
// held and the pattern plays, that key: the chord heard). -1 pitches (outside 0..127) are left out.
int arp_step_pitches(Omni *o, int s, int *out, bool heard) {
    const Arp &a = o->sampler.arp;
    if (s < 0 || s >= Arp::STEPS) return 0;
    const int scale = a.settings.scale.load(), root = ((a.settings.root.load() % 12) + 12) % 12, base = a.settings.base.load();
    const int octave = std::max(-3, std::min(3, a.settings.octave.load()));
    const int ref = heard && a.ref_note.load() >= 0 ? a.ref_note.load() : 60 + root;
    const uint16_t m = a.mask[s].load();
    int n = 0;
    for (int r = 0; r < Arp::ROWS; r++) {
        if (!(m >> r & 1)) continue;
        const int q = Arp::seq_note(ref, r, scale, root, base);
        if (q < 0) continue;
        const int p = q + 12 * (octave + std::max(-2, std::min(2, int(a.noct[s * 16 + r].load()))));
        if (p >= 0 && p <= 127) out[n++] = p;
    }
    return n;
}
// the chord readout: the selected note's step, else the step playing, else the first step of the page shown with notes
std::string arp_chord_text(Omni *o) {
    const Arp &a = o->sampler.arp;
    if (a.settings.mode.load() != 0) return "ARP";   // rows are the held keys: no chord written
    int s = o->arp_sel_on ? o->arp_sel_s0 : -1;
    bool heard = false;
    if (s < 0 && a.settings.on.load() && a.step_now.load() >= 0) { s = a.step_now.load(); heard = true; }
    if (s < 0) {
        const int page = std::max(0, std::min(3, int(num(o, "arp_page") + 0.5f)));
        for (int st = page * 16; st < page * 16 + 16 && s < 0; st++) if (a.mask[st].load()) s = st;
    }
    int p[16];
    const int n = arp_step_pitches(o, s, p, heard);
    if (n == 0) return "-";
    return chord_detect(p, n).name;
}
// VOICE: the chords (notes that start on one step: TIE runs whose first dot is that step) re-voiced by arp_v_mode, each
// note to the GRID cell (row + NOTE OCT) that plays its new pitch; a note keeps its length, gate, velocity, chance,
// ratchet and slide; the step's velocity is not touched. A selected note: its chord only; else every chord in LENGTH.
// A chord with no room (a row taken by another note, out of the rows even with NOTE OCT, out of 0..127) is left as it is.
int arp_voice(Omni *o, int len, int only_step, char *msg, size_t msz) {
    Arp &a = o->sampler.arp;
    if (a.settings.mode.load() != 0) { std::snprintf(msg, msz, "VOICE works on SEQ grids (rows are scale steps): set MODE to SEQ"); return 0; }
    const int scale = a.settings.scale.load(), root = ((a.settings.root.load() % 12) + 12) % 12, base = a.settings.base.load();
    const int octave = std::max(-3, std::min(3, a.settings.octave.load()));
    const VoiceMode mode = VoiceMode(std::max(0, std::min(int(VM_COUNT) - 1, int(num(o, "arp_v_mode") + 0.5f))));
    const int span = std::max(12, std::min(48, int(num(o, "arp_v_span") + 0.5f)));
    const bool tie = num(o, "arp_tie") > 0.5f;
    const std::vector<ArpRun> runs = arp_runs(o, len);
    int done = 0, skipped = 0, chords = 0;
    const char *why = "";
    std::string first_name;
    auto sig_of = [](int *p, int n) {   // order-insensitive
        std::sort(p, p + n);
        uint32_t h = 2166136261u;
        for (int i = 0; i < n; i++) h = (h ^ uint32_t(p[i] + 1)) * 16777619u;
        return h;
    };
    for (int s = 0; s < len; s++) {
        if (only_step >= 0 && s != only_step) continue;
        ArpRun g[16];
        int k = 0;
        for (const ArpRun &r : runs) if (r.a == s && k < 16) g[k++] = r;
        if (k < 2) continue;
        chords++;
        int in[16];
        bool bad = false;
        for (int i = 0; i < k; i++) {
            const int q = Arp::seq_note(60 + root, g[i].r, scale, root, base);
            if (q < 0) { bad = true; break; }
            in[i] = q + 12 * (octave + std::max(-2, std::min(2, int(a.noct[s * 16 + g[i].r].load()))));
            if (in[i] < 0 || in[i] > 127) { bad = true; break; }
        }
        if (bad) { skipped++; why = "a note outside 0..127"; continue; }
        int sorted_in[16];
        std::copy(in, in + k, sorted_in);
        const uint32_t sig_in = sig_of(sorted_in, k);
        double mean = 0;
        for (int i = 0; i < k; i++) mean += in[i];
        mean /= k;
        const double anchor = o->voice_sig[s] == sig_in && o->voice_sig[s] != 0 ? o->voice_anchor[s] : -1;
        const VoiceResult res = voice_apply(in, k, mode, scale, root, 0, 127, span, anchor);
        if (!res.ok) { skipped++; why = res.why; continue; }
        // a cell for each new pitch: not a row another note uses over the note's steps (nor right next to them with TIE:
        // the two would join), not a row another note of this chord takes
        int nrow[16], nnoct[16];
        bool fit = true;
        for (int j = 0; j < res.n && fit; j++) {
            const ArpRun &src = g[res.v[j].src];
            auto row_free = [&](int r) {
                for (int q = 0; q < j; q++) if (nrow[q] == r) return false;
                const int t0 = std::max(0, src.a - (tie ? 1 : 0)), t1 = std::min(len - 1, src.b + (tie ? 1 : 0));
                for (int t = t0; t <= t1; t++) {
                    if (!(a.mask[t].load() >> r & 1)) continue;
                    bool mine = false;   // lit by this chord's own notes: they move too
                    for (int i = 0; i < k; i++) if (g[i].r == r && g[i].a <= t && t <= g[i].b) mine = true;
                    if (!mine) return false;
                }
                return true;
            };
            int r = -1, nc = 0;
            if (!pitch_to_cell(res.v[j].pitch, scale, root, base, octave, r, nc, row_free)) fit = false;
            else { nrow[j] = r; nnoct[j] = nc; }
        }
        if (!fit) { skipped++; why = "no room on the grid"; continue; }
        // commit: the notes' extras first, then the masks (one store per step: a step is never half a chord)
        ArpNoteX x[16];
        for (int i = 0; i < k; i++) x[i] = arp_note_take(o, s * 16 + g[i].r);
        int last = s;
        for (int i = 0; i < k; i++) last = std::max(last, g[i].b);
        for (int t = s; t <= last; t++) {
            uint16_t m = a.mask[t].load();
            for (int i = 0; i < k; i++) if (g[i].a <= t && t <= g[i].b) m = uint16_t(m & ~(1u << g[i].r));
            for (int j = 0; j < res.n; j++) {
                const ArpRun &src = g[res.v[j].src];
                if (src.a <= t && t <= src.b) m = uint16_t(m | (1u << nrow[j]));
            }
            a.mask[t].store(m);
        }
        for (int j = 0; j < res.n; j++) {
            ArpNoteX nx = x[res.v[j].src];
            nx.oct = int8_t(nnoct[j]);
            arp_note_put(o, s * 16 + nrow[j], nx);
        }
        int outp[16];
        for (int j = 0; j < res.n; j++) outp[j] = res.v[j].pitch;
        if (first_name.empty()) first_name = chord_detect(outp, res.n).name;
        o->voice_sig[s] = sig_of(outp, res.n);
        o->voice_anchor[s] = anchor >= 0 ? anchor : mean;
        done++;
    }
    if (!chords) {
        std::snprintf(msg, msz, "VOICE: %s", only_step >= 0 ? "the selected note's step has no chord (2+ notes starting there)"
                                                           : "no chords in the pattern (2+ notes starting on one step)");
        return 0;
    }
    std::snprintf(msg, msz, "VOICE %s  ·  %d of %d chord%s%s%s%s%s  ·  UNDO to go back", VOICE_MODE_NAMES[mode], done, chords,
                  chords == 1 ? "" : "s", first_name.empty() ? "" : "  ·  ", first_name.c_str(), skipped ? "  ·  left as they were: " : "",
                  skipped ? why : "");
    return done;
}

void arp_gen(Omni *o, const char *what) {
    Arp &a = o->sampler.arp;
    const int len = arp_len(o);
    const int sel_row = o->arp_sel_on ? o->arp_sel_r : 0;   // (EUCLID's row: taken before the selection goes)
    const int sel_step = o->arp_sel_on ? o->arp_sel_s0 : -1;   // 2.4.14 (VOICE: that chord only)
    if (!std::strcmp(what, "undo")) {
        if (!o->arp_undo.set) { arp_gen_msg(o, "Nothing to undo"); return; }
        Omni::ArpSnap now;
        arp_snapshot(o, now);
        now.timing = o->arp_undo.timing;   // 2.4.18 (redo puts GENERATE's step and length back too)
        arp_restore(o, o->arp_undo);
        o->arp_undo = now;   // (UNDO again: redo)
        arp_select(o, false);
        arp_refresh_view(o);
        arp_gen_msg(o, "UNDO (again: redo)");
        return;
    }
    arp_snapshot(o, o->arp_undo);
    arp_select(o, false);
    char msg[160] = "";
    if (!std::strcmp(what, "generate")) {   // 2.4.9: arp_compose, from the MIDI file loaded when there is one
        const int mode = std::max(0, std::min(AM_COUNT - 1, int(num(o, "arp_g_contour") + 0.5f)));
        uint16_t raw[64] = {};
        int raw_len = 0, raw_rate = 2;
        std::string file;
        {
            std::lock_guard<std::mutex> lock(o->syn_mutex);
            if (o->arp_src.set) {
                raw_len = std::max(1, std::min(Arp::STEPS, o->arp_src.length));
                raw_rate = o->arp_src.rate;
                std::copy(o->arp_src.mask, o->arp_src.mask + 64, raw);   // 2.4.12: the file's whole grid (its chords)
                file = o->arp_loaded.substr(o->arp_loaded.find_last_of('/') == std::string::npos ? 0 : o->arp_loaded.find_last_of('/') + 1);
            }
        }
        bool chordal = false;
        for (int st = 0; st < raw_len; st++) if (__builtin_popcount(raw[st]) >= 2) chordal = true;
        // 2.4.18: chords at 1/4 (a step a beat) leave a riff or a broken chord no room: the grid goes to 1/8 (1/16 above
        // 75 % DENSITY), as many bars as fit; UNDO puts the step and LENGTH back
        int glen = len;
        const char *step_note = "";
        if (chordal && int(num(o, "arp_rate") + 0.5f) == 0) {
            const int to = num(o, "arp_g_density") > 75.0f ? 2 : 1;
            glen = std::min(Arp::STEPS, len * (to == 2 ? 4 : 2));
            arp_set_num(o, "arp_rate", float(to));
            arp_set_num(o, "arp_length", float(glen));
            apply_settings(o);
            o->arp_undo.timing = true;
            step_note = to == 2 ? "  ·  1/16" : "  ·  1/8";
        }
        // the file read at the grid's step now (2.4.18: it was read step for step even after RATE changed)
        uint16_t smask[64] = {};
        const int src_len = raw_len ? arp_resample(raw, raw_len, raw_rate, int(num(o, "arp_rate") + 0.5f), smask) : 0;
        std::vector<std::pair<int, int>> src;   // (step, row): the file's melody (its top note on each step)
        for (int st = 0; st < src_len; st++) {   // (its top row; a held note counts where it starts)
            const uint16_t m = smask[st];
            if (!m) continue;
            const int r = 31 - __builtin_clz(uint32_t(m));
            if (st == 0 || !(smask[st - 1] >> r & 1)) src.push_back({st, r});
        }
        std::vector<GNote> notes;
        int chords = 0;
        if (chordal && src_len) chords = arp_compose_harmony(o, mode, glen, smask, src_len, notes);
        else arp_compose(o, mode, glen, src, src_len, src_len ? smask : nullptr, notes);
        const int n = arp_write(o, glen, notes);
        const std::string from = src.empty() ? "own melody" : ("from " + file.substr(0, file.find_last_of('.'))).substr(0, 36);
        if (chordal) std::snprintf(msg, sizeof msg, "GENERATED  ·  %s  ·  %s  ·  %d chord%s spread%s  ·  %d notes  ·  UNDO to go back",
                                   ARP_MODE_NAMES[mode], from.c_str(), chords, chords == 1 ? "" : "s", step_note, n);
        else std::snprintf(msg, sizeof msg, "GENERATED  ·  %s  ·  %s  ·  %d notes  ·  UNDO to go back", ARP_MODE_NAMES[mode], from.c_str(), n);
    } else if (!std::strcmp(what, "fill")) {   // the last bar again, busier, with ratchets
        const int bar = std::min(len, arp_per_beat(o) * 4), from = len - bar;
        const int n = arp_generate_into(o, from, len, std::min(1.0, num(o, "arp_g_density") / 100.0 + 0.35), 35);
        std::snprintf(msg, sizeof msg, "FILL  ·  steps %d-%d  ·  %d notes", from + 1, len, n);
    } else if (!std::strcmp(what, "invert")) {   // upside down within the rows used
        int lo = 15, hi = 0;
        for (const ArpRun &rn : arp_runs(o, len)) lo = std::min(lo, rn.r), hi = std::max(hi, rn.r);
        arp_remap(o, len, [&](ArpRun r) { r.r = lo + hi - r.r; return r; });
        std::snprintf(msg, sizeof msg, "INVERTED");
    } else if (!std::strcmp(what, "reverse")) {
        arp_remap(o, len, [&](ArpRun r) { const int a0 = len - 1 - r.b, b0 = len - 1 - r.a; r.a = a0; r.b = b0; return r; });
        std::snprintf(msg, sizeof msg, "REVERSED");
    } else if (!std::strcmp(what, "double")) {   // twice as fast: the pattern in half the steps, twice
        const int half = std::max(1, len / 2);
        arp_remap(o, len, [&](ArpRun r) { r.a /= 2; r.b = std::max(r.a, r.b / 2); return r; });
        // the copy: the second half, with each step's velocity and each note's own expression (2.4.14, audit B5: it was
        // dots only, so the second half played flat at 100)
        for (const ArpRun &rn : arp_runs(o, half)) {
            if (rn.a + half >= len) continue;
            for (int st = rn.a; st <= rn.b && st + half < len; st++) a.mask[st + half].store(uint16_t(a.mask[st + half].load() | (1u << rn.r)));
            const int from = rn.a * 16 + rn.r, to = (rn.a + half) * 16 + rn.r;
            arp_note_put(o, to, {a.ngate[from].load(), a.nvel[from].load(), a.nskip[from].load(), a.nratch[from].load(), a.nslide[from].load(), a.noct[from].load()});
            a.vel[rn.a + half].store(a.vel[rn.a].load());
        }
        std::snprintf(msg, sizeof msg, "DOUBLE TIME");
    } else if (!std::strcmp(what, "half")) {   // half as fast: the first half of the pattern, each note twice as long
        arp_remap(o, len, [&](ArpRun r) { r.a *= 2; r.b = r.b * 2 + 1; return r; });
        std::snprintf(msg, sizeof msg, "HALF TIME%s", num(o, "arp_tie") > 0.5f ? "" : "  ·  (TIE on to hold the longer notes)");
    } else if (!std::strcmp(what, "mutate")) {   // by MUTATE %: notes move a row or two, now and then a step, some go, some come
        const double m = num(o, "arp_g_mutate") / 100.0;
        int changed = 0;
        // 2.4.14 (audit B9): a chord (2+ notes starting on one step) is left as it is: its notes moved a row each on their
        // own broke the triad
        int starts[64] = {};
        for (const ArpRun &rn : arp_runs(o, len)) starts[rn.a]++;
        arp_remap(o, len, [&](ArpRun r) {
            if (starts[r.a] >= 2) return r;
            if (arp_u() < m * 0.15) { changed++; r.a = -1; return r; }
            if (arp_u() < m * 0.5) { r.r = std::max(0, std::min(15, r.r + (arp_u() < 0.5 ? -1 : 1) * (1 + (arp_u() < 0.3)))); changed++; }
            if (arp_u() < m * 0.25) { const int d = arp_u() < 0.5 ? -1 : 1; r.a += d; r.b += d; changed++; }
            return r;
        });
        const int add = int(m * len / 6 + 0.5);
        for (int i = 0; i < add; i++) {
            const int st = int(arp_u() * len), rw = int(arp_u() * std::max(2, std::min(16, int(num(o, "arp_g_range") + 0.5f))));
            a.mask[st].store(uint16_t(a.mask[st].load() | (1u << rw)));
            changed++;
        }
        std::snprintf(msg, sizeof msg, "MUTATED  ·  %d changes  ·  UNDO to go back", changed);
    } else if (!std::strcmp(what, "voice")) {   // 2.4.14
        arp_voice(o, len, sel_step, msg, sizeof msg);
    } else if (!std::strcmp(what, "euclid")) {   // HITS spread evenly over the steps, ROTATE, on the selected note's row (else row 1)
        const int hits = std::max(0, std::min(len, int(num(o, "arp_e_hits") + 0.5f))), rot = int(num(o, "arp_e_rotate") + 0.5f);
        const int row = sel_row;
        for (int st = 0; st < len; st++) {
            a.mask[st].store(uint16_t(a.mask[st].load() & ~(1u << row)));
            arp_note_take(o, st * 16 + row);
            if (hits && (((st + rot) % len) * hits) % len < hits) a.mask[st].store(uint16_t(a.mask[st].load() | (1u << row)));
        }
        std::snprintf(msg, sizeof msg, "EUCLID  ·  %d hits over %d steps, rotated %d, on row %d", hits, len, rot, row + 1);
    } else if (!std::strcmp(what, "breed")) {   // 2.4.12 BREED: each bar of the child from pattern A or B (both used)
        if (!o->brd_a.set || !o->brd_b.set) {
            std::snprintf(msg, sizeof msg, "BREED: STORE A and STORE B first (%s missing)", o->brd_a.set ? "B" : o->brd_b.set ? "A" : "A and B");
        } else {
            const int bar = std::max(1, std::min(len, arp_per_beat(o) * 4));
            const int nbars = (len + bar - 1) / bar, unit = nbars >= 2 ? bar : std::max(1, bar / 2);   // (one bar: halves)
            const int nunits = (len + unit - 1) / unit;
            std::vector<bool> from_b(static_cast<size_t>(nunits), false);
            int nb = 0;
            for (int u = 0; u < nunits; u++) { from_b[size_t(u)] = arp_u() < 0.5; nb += from_b[size_t(u)]; }
            if (nunits >= 2 && (nb == 0 || nb == nunits)) { const int u = int(arp_u() * nunits) % nunits; from_b[size_t(u)] = !from_b[size_t(u)]; nb += from_b[size_t(u)] ? 1 : -1; }
            for (int st = 0; st < len; st++) {
                const Omni::ArpSnap &p = from_b[size_t(st / unit)] ? o->brd_b : o->brd_a;
                a.mask[st].store(p.mask[st]); a.vel[st].store(p.vel[st]);
                for (int r = 0; r < 16; r++) {
                    const int i = st * 16 + r;
                    a.ngate[i].store(p.nx[i][0]); a.nvel[i].store(p.nx[i][1]); a.nskip[i].store(p.nx[i][2]);
                    a.nratch[i].store(p.nx[i][3]); a.nslide[i].store(p.nx[i][4]); a.noct[i].store(int8_t(p.nx[i][5]));
                }
            }
            std::string plan;
            for (int u = 0; u < nunits && plan.size() < 40; u++) plan += from_b[size_t(u)] ? 'B' : 'A';
            std::snprintf(msg, sizeof msg, "BREED  ·  %s %s  ·  UNDO to go back", nbars >= 2 ? "bars" : "halves", plan.c_str());
        }
    }
    arp_refresh_view(o);
    arp_gen_msg(o, msg);
}

// DRAW: one finger on the pad over the grid. Down on an empty cell draws a note there and the drag draws it along the
// row (a held note: TIE goes on); down on a note's last step drags its end (shorter, longer); down on any other step of
// a note drags the note (along its row and to other rows); a tap on a note, without moving, deletes it. A note is a
// run of dots on a row. The pad rests at (0, 0), off the cells, between touches (ResetOnGestureEnd).
constexpr int XY_COLS = 16, XY_ROWS = 16, XY_ML = 52, XY_MB = 14, XY_DX = 52, XY_DY = 27;   // design.py XY_*, AG_*
constexpr int XY_W = XY_ML + XY_COLS * XY_DX, XY_H = XY_MB + XY_ROWS * XY_DY;
enum { ST_PAINT, ST_RESIZE, ST_MOVE };

void arp_put(Omni *o, int r, int s, bool on) {
    if (s < 0 || s >= Arp::STEPS) return;
    uint16_t m = o->sampler.arp.mask[s].load();
    o->sampler.arp.mask[s].store(on ? uint16_t(m | (1u << r)) : uint16_t(m & ~(1u << r)));
}

void arp_tie_on(Omni *o) {   // a drawn line is a held note
    if (num(o, "arp_tie") < 0.5f) { arp_set_num(o, "arp_tie", 1); apply_settings(o); }
}

void arp_stroke(Omni *o) {
    Omni::ArpStroke &st = o->arp_stroke;
    Arp &arp = o->sampler.arp;
    const float xf = o->arp_xy[0], yf = o->arp_xy[1];
    if (xf <= 0 && yf <= 0) {   // the finger is up
        if (st.active && !st.moved && st.kind != ST_PAINT) {   // a tap on a note: select it; on the selected one: delete it
            if (o->arp_sel_on && o->arp_sel_r == st.row && o->arp_sel_s0 == st.s0) {
                for (int s = st.s0; s <= st.s1; s++) arp_put(o, st.row, s, false);
                arp_vel_after_take(o, st.s0);   // 2.4.14 (B3)
                arp_move_gate(o, st.row, st.s0, st.row, -1);
                arp_select(o, false);
            } else arp_select(o, true, st.row, st.s0);
            arp_refresh_view(o);
        }
        st.active = false;
        return;
    }
    const double px = xf * XY_W - XY_ML, py = yf * XY_H - XY_MB;   // (y counts up from the bottom row)
    if (px < 0 || py < 0) return;
    const int col = std::min(XY_COLS - 1, int(px / XY_DX)), row = std::min(XY_ROWS - 1, int(py / XY_DY));
    const int page = std::max(0, std::min(3, int(num(o, "arp_page") + 0.5f)));
    const int step = page * 16 + col;
    if (!st.active) {
        st = Omni::ArpStroke();
        st.active = true;
        st.at_r = row;
        st.at_s = step;
        st.row = row;
        if (!arp_note_span(o, row, step, st.s0, st.s1)) {   // a new note, selected
            st.kind = ST_PAINT;
            st.s0 = st.s1 = st.grab = step;
            const uint16_t before = arp.mask[step].load();
            arp_put(o, row, step, true);
            arp_vel_for_put(o, step, before, 100);   // 2.4.14 (B3): drawn into a chord, the chord's velocity
            arp_select(o, true, row, step);
        } else {
            st.kind = step == st.s1 ? ST_RESIZE : ST_MOVE;
            st.grab = step - st.s0;
            st.vel = arp.vel[st.s0].load();
        }
        arp_refresh_view(o);
        return;
    }
    if (row == st.at_r && step == st.at_s) return;
    st.moved = true;
    st.at_r = row;
    st.at_s = step;
    if (st.kind == ST_PAINT) {   // along the note's row, either way from the cell it started on (st.grab)
        const int first = std::min(st.grab, step), last = std::max(st.grab, step);
        for (int s = std::min(first, st.s0); s <= std::max(last, st.s1); s++) arp_put(o, st.row, s, s >= first && s <= last);
        if (first != st.s0) {   // 2.4.14 (B3)
            arp_vel_after_take(o, st.s0);
            arp_vel_for_put(o, first, uint16_t(arp.mask[first].load() & ~(1u << st.row)), 100);
        }
        st.s0 = first;
        st.s1 = last;
        if (last > first) arp_tie_on(o);
        arp_select(o, true, st.row, first);
    } else if (st.kind == ST_RESIZE) {   // its end: shorter or longer (at least its first step)
        const int end = std::max(st.s0, step);
        for (int s = st.s0; s <= std::max(end, st.s1); s++) arp_put(o, st.row, s, s <= end);
        st.s1 = end;
        if (end > st.s0) arp_tie_on(o);
        arp_select(o, true, st.row, st.s0);
    } else {   // the whole note, to where the finger is
        const int n = st.s1 - st.s0, s0 = std::max(0, std::min(Arp::STEPS - 1 - n, step - st.grab));
        // 2.4.14 (audit B7): not onto another note (nor, with TIE, touching one: they would join): it stays where it was
        // (the two merged, the other note's gate / velocity / ratchet lost)
        {
            const bool tie = num(o, "arp_tie") > 0.5f;
            for (int t = s0 - (tie ? 1 : 0); t <= s0 + n + (tie ? 1 : 0); t++)
                if (arp_lit(o, row, t) && !(row == st.row && t >= st.s0 && t <= st.s1)) { arp_refresh_view(o); return; }
        }
        for (int s = st.s0; s <= st.s1; s++) arp_put(o, st.row, s, false);
        arp_vel_after_take(o, st.s0);   // 2.4.14 (B3)
        arp_move_gate(o, st.row, st.s0, row, s0);
        const uint16_t before = arp.mask[s0].load();
        for (int s = s0; s <= s0 + n; s++) arp_put(o, row, s, true);
        arp_vel_for_put(o, s0, before, st.vel);
        st.row = row;
        st.s0 = s0;
        st.s1 = s0 + n;
        arp_select(o, true, row, s0);
    }
    arp_refresh_view(o);
}

std::string arp_status(Omni *o) {
    const ArpSettings &a = o->sampler.arp.settings;
    char t[160];
    int s0, s1;
    if (o->arp_sel_on && arp_note_span(o, o->arp_sel_r, o->arp_sel_s0, s0, s1)) {   // the note selected
        const Arp &a = o->sampler.arp;
        const int ni = s0 * 16 + o->arp_sel_r;
        std::snprintf(t, sizeof t, "NOTE %s  ·  STEP %d  ·  %d STEP%s  ·  GATE %d%%", arp_row_name(o, o->arp_sel_r).c_str(), s0 + 1,
                      s1 - s0 + 1, s1 > s0 ? "S" : "", int(std::lround(arp_gate_of(o, o->arp_sel_r, s0))));
        std::string x = t;   // 2.4.2: what else it does
        if (a.nvel[ni].load()) x += "  ·  VEL " + std::to_string(a.nvel[ni].load()) + "%";
        if (a.nskip[ni].load()) x += "  ·  " + std::to_string(100 - a.nskip[ni].load()) + "% CHANCE";
        if (a.nratch[ni].load() > 1) x += "  ·  x" + std::to_string(a.nratch[ni].load());
        if (a.noct[ni].load()) x += std::string("  ·  OCT ") + (a.noct[ni].load() > 0 ? "+" : "") + std::to_string(a.noct[ni].load());
        if (a.nslide[ni].load()) x += "  ·  SLIDE";
        { const std::string c = arp_chord_text(o); if (c != "-" && c != "ARP" && c.find(' ') == std::string::npos && c.size() > 0) x += "  ·  CHORD " + c; }   // 2.4.14
        return x;
    }
    if (!a.on.load()) return "ARP is OFF: turn ARP ON, then hold a key";
    std::snprintf(t, sizeof t, "LEN %d  ·  %s  ·  GATE %d%%%s", a.length.load(), Arp::RATE_NAMES[std::max(0, std::min(6, a.rate.load()))],
                  int(a.gate.load() * 100 + 0.5f), o->sampler.arp.running() ? "" : "  ·  HOLD A KEY");
    std::string x = t;   // 2.4.14: the chord on the step playing (else the page's first chord)
    { const std::string c = arp_chord_text(o); if (c != "-" && c != "ARP" && c.find(' ') == std::string::npos && !c.empty()) x += "  ·  CHORD " + c; }
    return x;
}

void reset_all(Omni *o) {
    static const char *const KEEP[] = {"skin", "mem_limit", "auto_extract", "br_folders", "br_preview", "info_page"};
    o->pad_panic.store(1);
    o->sampler.panic();
    for (int i = 0; i < NUM_COUNT; i++) {
        bool keep = false;
        for (const char *k : KEEP) keep = keep || !std::strcmp(k, NUM_PARAMS[i].key);
        if (!keep && o->values[i].exchange(NUM_PARAMS[i].def) != NUM_PARAMS[i].def) mark_changed(o, i);
    }
    for (auto &rb : o->range_base) for (auto &b : rb) b = Omni::RangeBase();
    for (auto &d : o->d_env) d.store(0);
    o->env_pending.store(false);
    apply_settings(o);
    {
        std::lock_guard<std::mutex> lock(o->lock_mutex);
        std::memset(o->lock_ui, 0, sizeof o->lock_ui);
        for (int n = 0; n < 128; n++) commit_lock(o, n);
    }
    show_lock(o);
    for (int i = 0; i < SLOTS; i++) o->loader.request("", 0, i, false);
    { std::lock_guard<std::mutex> lock(o->syn_mutex); syn_clear_refs(o); }   // 2.4.13 (a project's / preset's files: gone too)
    for (int k = 0; k < 3; k++) syn_request(o, k);   // 2.3: the default oscillators (and SUB wave) again
    syn_request_bits(o, SYN_REQ_LFO_ALL);
    arp_clear_steps(o, 0, Arp::STEPS);   // 2.4: an empty pattern
    o->sampler.arp.settings.base.store(0);
    o->arp_sel_on = false;
    { std::lock_guard<std::mutex> lock(o->syn_mutex); o->arp_loaded.clear(); o->arp_msg.clear(); o->arp_src.set = false; }
    o->arp_undo.set = o->brd_a.set = o->brd_b.set = false;   // 2.4.14 (audit B2)
    o->reset_done_ms.store(now_ms());
    o->ui_rev.fetch_add(1);
}

// ---------------------------------------------------------------------------------------------------------------
// 2.3: the SYNTH's oscillators. Each type has a list: CLASSIC the factory waves, WAVETABLE the factory tables then the
// user's own (<library>/Oscillators/*.wav, made when the library is set); SAMPLE and MACRO come in a later build.

std::string syn_user_dir(Omni *o) {
    std::string lib = library_dir(o);
    if (lib.empty()) return "";
    std::string d = path_join(lib, "Oscillators");
    mkdir(d.c_str(), 0755);   // there for the user to fill
    return d;
}

// the user's oscillators (syn_mutex held), paths relative to the Oscillators folder: every WAV down to six folders
// deep, and (2.3.2) every folder holding two or more WAVs as one wavetable of them ("MOOG/", its frames the files in
// natural order). Sorted naturally, a folder's entry before its files
bool is_wav(const std::string &name) { std::string n = lower(name); return n.size() > 4 && n.compare(n.size() - 4, 4, ".wav") == 0 && n[0] != '.'; }
// 2.4.13: Vital's own files: a wavetable (in the Oscillators folders' WAVETABLE list), an LFO shape (<library>/LFOs)
bool ends_ci(const std::string &name, const char *ext) {
    const std::string n = lower(name);
    const size_t e = std::strlen(ext);
    return n.size() > e && n.compare(n.size() - e, e, ext) == 0 && n[0] != '.';
}
bool is_vitaltable(const std::string &name) { return ends_ci(name, ".vitaltable"); }
// 2.4.24: an Arturia AGSS Wavetable preset's own table ("artwav:<preset path>") is a preset-owned reference like Vital's
bool is_artwav_ref(const std::string &s) { return s.size() > 7 && !s.compare(0, 7, "artwav:"); }
bool is_vital_ref(const std::string &s) {   // 2.4.32: also a Serum 2 preset's own table ("serum:<osc>:<path>")
    int n = 0; std::string p;
    return is_artwav_ref(s) || vital_ref_parse(s, "vital", n, p) || vital_ref_parse(s, "serum", n, p) || vital_ref_parse(s, "serumsmp", n, p);
}
constexpr size_t VITAL_TEXT_MAX = size_t(64) << 20;   // = formats/vital.cpp MAX_VITAL
// a file's whole text, through the mounts (a preset inside a .vitalbank)
std::string read_text(Omni *o, const std::string &full) {
    BlobPtr b = o->vfs.open(full);
    if (b->size() > VITAL_TEXT_MAX) throw ParseError("too big");
    const std::vector<uint8_t> v = b->all(VITAL_TEXT_MAX);
    return std::string(v.begin(), v.end());
}
// a Vital wavetable: a preset's oscillator osc 1..3, or a .vitaltable (osc 0); nullptr with err when it cannot be read
WavetablePtr load_vital_table(Omni *o, const std::string &path, int osc, const std::string &name, std::string &err) {
    try {
        std::vector<float> cycles;
        int frames = 0;
        std::string tname;
        if (!vital_wavetable_from_text(read_text(o, path), osc, VITAL_MAX_FRAMES, cycles, frames, tname, err)) return nullptr;
        return build_wavetable(name, cycles, frames);
    } catch (const std::exception &e) {
        err = std::string("Vital: ") + e.what();
        return nullptr;
    }
}
// a text setting ("@<name>=<text>", formats/synth_patch.hpp): its text when key is one for name
bool text_setting(const std::string &key, const char *name, std::string &out) {
    const size_t n = std::strlen(name);
    if (key.size() < n + 2 || key[0] != '@' || key.compare(1, n, name) || key[n + 1] != '=') return false;
    out = key.substr(n + 2);
    return true;
}
void syn_scan_dir(Omni *o, const std::string &root, const std::string &rel, int depth, std::vector<std::string> &out) {
    std::vector<DirEntry> es;
    try { es = o->vfs.list(rel.empty() ? root : path_join(root, rel.substr(0, rel.size() - 1))); } catch (...) { return; }
    int wavs = 0;
    for (const DirEntry &e : es) if (!e.dir && is_wav(e.name)) wavs++;
    if (!rel.empty() && wavs >= 2) out.push_back(rel);
    for (const DirEntry &e : es) {
        if (e.name.empty() || e.name[0] == '.') continue;
        if (e.dir) { if (depth < 6) syn_scan_dir(o, root, rel + e.name + "/", depth + 1, out); }
        else if (is_wav(e.name) || is_vitaltable(e.name)) out.push_back(rel + e.name);   // 2.4.13: + Vital wavetables
    }
}
// 2.4.11 (audit M1): the folder walk runs WITHOUT syn_mutex (get_param takes it under the wrapper lock that render shares:
// a LOAD on a big Oscillators folder stalled the audio); only the swap of the new lists takes it. Callers must not hold it.
void syn_rescan(Omni *o) {
    std::vector<std::string> user;
    std::vector<int> files;
    const std::string d = syn_user_dir(o);
    if (!d.empty()) syn_scan_dir(o, d, "", 0, user);
    std::sort(user.begin(), user.end(), natural_less);
    for (size_t i = 0; i < user.size(); i++) if (user[i].back() != '/' && is_wav(user[i])) files.push_back(int(i));   // 2.3.4 (WAVs)
    std::lock_guard<std::mutex> lock(o->syn_mutex);
    o->syn_user.swap(user);
    o->syn_user_files.swap(files);
}

// a folder entry's files, in natural order
std::vector<std::string> syn_frame_files(Omni *o, const std::string &root, const std::string &rel) {
    std::vector<std::string> out;
    std::string dir = path_join(root, rel.substr(0, rel.size() - 1));
    try { for (const DirEntry &e : o->vfs.list(dir)) if (!e.dir && is_wav(e.name)) out.push_back(path_join(dir, e.name)); } catch (...) {}
    std::sort(out.begin(), out.end(), natural_less);
    return out;
}

// 2.3.4: SAMPLE's list: Slot A..D (the instrument loaded there), then every WAV of the Oscillators folders
constexpr int SYN_SAMPLE_SLOTS = 4;
int syn_list_size(Omni *o, int type) {
    if (type == Synth::OT_CLASSIC) return factory_classic_count();
    if (type == Synth::OT_WAVETABLE) return factory_table_count() + int(o->syn_user.size());
    if (type == Synth::OT_SAMPLE) return SYN_SAMPLE_SLOTS + int(o->syn_user_files.size());
    if (type == Synth::OT_MACRO) return macro_engine_count();   // 2.3.5
    return 0;
}

// a user entry's shown name: its folder and name "MOOG / Saw.wav"; a folder "MOOG  (frames)"
std::string syn_user_name(const std::string &rel) {
    std::string r = rel;
    const bool folder = !r.empty() && r.back() == '/';
    if (folder) r.pop_back();
    size_t cut = r.rfind('/');
    if (folder) return (cut == std::string::npos ? r : r.substr(cut + 1)) + "  (frames)";
    if (cut == std::string::npos) return "Library / " + r;
    size_t cut2 = r.rfind('/', cut - 1);
    return r.substr(cut2 == std::string::npos ? 0 : cut2 + 1, cut - (cut2 == std::string::npos ? 0 : cut2 + 1)) + " / " + r.substr(cut + 1);
}

// the user entry (syn_user's relative path) an index of a type's list is, "" for factory entries and slots
std::string syn_user_rel(Omni *o, int type, int i) {
    if (type == Synth::OT_WAVETABLE) { i -= factory_table_count(); return i >= 0 && size_t(i) < o->syn_user.size() ? o->syn_user[size_t(i)] : ""; }
    if (type == Synth::OT_SAMPLE) { i -= SYN_SAMPLE_SLOTS; return i >= 0 && size_t(i) < o->syn_user_files.size() ? o->syn_user[size_t(o->syn_user_files[size_t(i)])] : ""; }
    return "";
}

// 2.4.9: the user entry an index of a type's list is (no copy; nullptr for factory entries and slots), and the length of
// its folder part ("MOOG/" for "MOOG/Saw.wav" and for the folder entry "MOOG/"; 0 at the top): folders compared without
// allocating, so a list of thousands is cheap to walk (syn_mutex held)
const std::string *syn_user_ref(Omni *o, int type, int i) {
    if (type == Synth::OT_WAVETABLE) { i -= factory_table_count(); return i >= 0 && size_t(i) < o->syn_user.size() ? &o->syn_user[size_t(i)] : nullptr; }
    if (type == Synth::OT_SAMPLE) { i -= SYN_SAMPLE_SLOTS; return i >= 0 && size_t(i) < o->syn_user_files.size() ? &o->syn_user[size_t(o->syn_user_files[size_t(i)])] : nullptr; }
    return nullptr;
}
bool syn_same_folder(Omni *o, int type, int a, int b) {
    const std::string *x = syn_user_ref(o, type, a), *y = syn_user_ref(o, type, b);
    if (!x || !y) return !x && !y;   // the factory entries (or slots) are one group
    auto flen = [](const std::string &r) { if (!r.empty() && r.back() == '/') return r.size(); size_t c = r.rfind('/'); return c == std::string::npos ? size_t(0) : c + 1; };
    const size_t lx = flen(*x);
    return lx == flen(*y) && x->compare(0, lx, *y, 0, lx) == 0;
}
std::string syn_entry_name(Omni *o, int type, int i) {
    if (type == Synth::OT_CLASSIC) return std::string("Classic / ") + factory_classic_name(i);
    if (type == Synth::OT_MACRO) return std::string("Macro / ") + macro_engine_name(i);   // 2.3.5
    if (type == Synth::OT_WAVETABLE && i < factory_table_count()) return std::string("Factory / ") + factory_table_name(i);
    if (type == Synth::OT_SAMPLE && i < SYN_SAMPLE_SLOTS) {
        LoadedInfo li = o->loader.slot_info(i);
        return std::string("Slot ") + char('A' + i) + ": " + (li.path.empty() ? std::string("(empty)") : li.name);
    }
    std::string rel = syn_user_rel(o, type, i);
    return rel.empty() ? "" : syn_user_name(rel);
}

char syn_pre(int k) { return char('a' + k); }
float syn_num(Omni *o, int k, const char *name) { char key[24]; std::snprintf(key, sizeof key, "s%c_%s", syn_pre(k), name); return num(o, key); }
void syn_set(Omni *o, int k, const char *name, float v) {
    char key[24];
    std::snprintf(key, sizeof key, "s%c_%s", syn_pre(k), name);
    int i = num_index(key);
    if (i >= 0 && o->values[i].exchange(v) != v) mark_changed(o, i);
}

// ask the loader thread for oscillator k (its type and table index as they are now)
void syn_request(Omni *o, int k) {   // k: 0 OSC A, 1 OSC B, 2 the SUB's wave (2.3.1)
    std::lock_guard<std::mutex> lock(o->syn_mutex);
    o->syn_req |= 1 << k;
    if (k < SYN_OSCS) o->syn[k].loading = 1;
    o->syn_cv.notify_one();
}

// 2.4.13: the LFOs' CUSTOM shapes. <library>/LFOs holds the user's .vitallfo files (made when the library is set)
std::string syn_lfo_dir(Omni *o) {
    std::string lib = library_dir(o);
    if (lib.empty()) return "";
    std::string d = path_join(lib, "LFOs");
    mkdir(d.c_str(), 0755);   // there for the user to fill
    return d;
}
void syn_lfo_scan(Omni *o, const std::string &root, const std::string &rel, int depth, std::vector<std::string> &out) {
    std::vector<DirEntry> es;
    try { es = o->vfs.list(rel.empty() ? root : path_join(root, rel.substr(0, rel.size() - 1))); } catch (...) { return; }
    for (const DirEntry &e : es) {
        if (e.name.empty() || e.name[0] == '.') continue;
        if (e.dir) { if (depth < 6) syn_lfo_scan(o, root, rel + e.name + "/", depth + 1, out); }
        else if (ends_ci(e.name, ".vitallfo")) out.push_back(rel + e.name);
        if (out.size() >= 20000) return;
    }
}
// loader thread: LFO n's shape from its reference (none: no table, the LFO plays a sine)
void syn_lfo_load(Omni *o, int n) {
    std::string ref;
    { std::lock_guard<std::mutex> lock(o->syn_mutex); ref = o->syn_lfo[n].ref; }
    std::shared_ptr<const std::vector<float>> tab;
    std::string name, err;
    if (!ref.empty()) {
        int idx = 0;
        std::string path;
        const bool serum = vital_ref_parse(ref, "serumlfo", idx, path);   // 2.4.35: a Serum 2 preset's drawn LFO
        const bool preset = serum || vital_ref_parse(ref, "vlfo", idx, path);
        if (!preset) { const std::string d = syn_lfo_dir(o); if (!d.empty()) path = path_join(d, ref); }
        VitalLine line;
        try {
            if (path.empty()) throw ParseError("set a Plugin Library first");
            if (serum) {
                Location loc = o->vfs.resolve(path);
                if (serum_lfo_line(*loc.volume, loc.inner, idx, line)) tab = std::make_shared<const std::vector<float>>(vital_lfo_table(line));
                else err = "Serum 2: LFO " + std::to_string(idx + 1) + " has no curve";
            } else if (vital_lfo_from_text(read_text(o, path), preset ? idx : 0, line, err))
                tab = std::make_shared<const std::vector<float>>(vital_lfo_table(line));
        } catch (const std::exception &e) { err = e.what(); }
        if (preset) name = !line.name.empty() ? line.name : "LFO " + std::to_string(idx);
        else name = path_stem(ref);
        if (!tab) err = "Not found: " + name;
    }
    std::lock_guard<std::mutex> lock(o->syn_mutex);
    Omni::SynLfoShape &L = o->syn_lfo[n];
    if (L.ref != ref) return;   // another shape picked meanwhile: its own load follows
    if (L.tab) o->syn_lfo_retired.emplace_back(L.tab, now_ms());
    L.tab = tab;
    L.name = name;
    L.err = err;
    o->sampler.synth.settings.mod.lfo[n].custom.store(tab ? tab->data() : nullptr, std::memory_order_release);
    o->ui_rev.fetch_add(1);
}
// loader thread: an LFO's < > (Omni::SynLfoShape::step): the next / previous file of the LFOs folder (read again first)
void syn_lfo_step_now(Omni *o) {
    std::vector<std::string> files;
    const std::string d = syn_lfo_dir(o);
    if (!d.empty()) syn_lfo_scan(o, d, "", 0, files);
    std::sort(files.begin(), files.end(), natural_less);
    bool load[SYN_LFOS] = {};
    {
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        o->syn_lfo_files = files;
        for (int n = 0; n < SYN_LFOS; n++) {
            Omni::SynLfoShape &L = o->syn_lfo[n];
            const int step = L.step;
            L.step = 0;
            if (!step) continue;
            if (files.empty()) { L.err = d.empty() ? "Set a Plugin Library first" : "No .vitallfo in <Library>/LFOs"; continue; }
            const int nf = int(files.size());
            int i = -1;
            for (int j = 0; j < nf; j++) if (files[size_t(j)] == L.ref) i = j;
            i = i < 0 ? (step > 0 ? 0 : nf - 1) : (i + step + nf) % nf;
            L.ref = files[size_t(i)];
            load[n] = true;
        }
    }
    for (int n = 0; n < SYN_LFOS; n++) {
        if (!load[n]) continue;
        char key[24];
        std::snprintf(key, sizeof key, "syn_lfo%d_shape", n + 1);
        const int si = num_index(key);
        if (si >= 0 && o->values[si].exchange(float(SYN_LFO_CUSTOM)) != float(SYN_LFO_CUSTOM)) { mark_changed(o, si); apply_settings(o); }
        syn_lfo_load(o, n);
    }
    o->ui_rev.fetch_add(1);
}

void syn_worker(Omni *o) {
    for (;;) {
        int req;
        {
            std::unique_lock<std::mutex> lock(o->syn_mutex);
            o->syn_cv.wait(lock, [o] { return o->syn_quit || o->syn_req; });
            if (o->syn_quit) return;
            req = o->syn_req;
            o->syn_req = 0;
        }
        if (req & 8) {   // 2.4: the MIDI files
            std::vector<std::string> files;
            const std::string d = arp_dir(o);
            if (!d.empty()) arp_scan_dir(o, d, "", 0, files);
            std::map<std::string, Omni::ArpStyleRef> refs;
            arp_scan_styles(o, files, refs);   // 2.4.1
            std::sort(files.begin(), files.end(), natural_less);
            std::lock_guard<std::mutex> lock(o->syn_mutex);
            o->arp_style_refs = refs;
            std::string cur = o->arp_sel >= 0 && o->arp_sel < int(o->arp_files.size()) ? o->arp_files[size_t(o->arp_sel)] : o->arp_loaded;
            o->arp_files = files;
            o->arp_sel = files.empty() ? -1 : 0;
            for (size_t i = 0; i < files.size(); i++) if (files[i] == cur) o->arp_sel = int(i);
        }
        if (req & 16) arp_load_now(o);   // 2.4
        if (req & SYN_REQ_LFO_STEP) syn_lfo_step_now(o);   // 2.4.13
        for (int n = 0; n < SYN_LFOS; n++) if (req & SYN_REQ_LFO_N[n]) syn_lfo_load(o, n);   // 2.4.17: 4 LFOs
        if (req & 4) {   // 2.3.1: the SUB's wave (a classic one: sine, triangle, square)
            static const int SUB_WAVE[3] = {0, 1, 3};
            int sh = std::max(0, std::min(2, int(num(o, "syn_sub_shape") + 0.5f)));
            WavetablePtr t = factory_classic(SUB_WAVE[sh]);
            std::lock_guard<std::mutex> lock(o->syn_mutex);
            if (o->syn_sub) o->syn_retired.emplace_back(o->syn_sub, now_ms());
            o->syn_sub = t;
            o->sampler.synth.settings.mix.sub_table.store(t.get(), std::memory_order_release);
        }
        for (int k = 0; k < SYN_OSCS; k++) {
            if (!(req & (1 << k))) continue;
            int type = int(syn_num(o, k, "type") + 0.5f), idx = int(syn_num(o, k, "table") + 0.5f);
            std::string path, name, err;
            int fold_i = 0, fold_n = 0;
            std::vector<std::string> frame_files;
            WavetablePtr t;
            bool waiting = false;
            // 2.4.13: a Vital preset's own wavetable ("vital:<osc>:<path>"): loaded from the preset, the reference kept (a
            // project saves it), the table index left as it is. The preset gone: a saw, and the error shown
            std::string vref;
            {
                std::lock_guard<std::mutex> lock(o->syn_mutex);
                if ((type == Synth::OT_WAVETABLE || type == Synth::OT_SAMPLE) && is_vital_ref(o->syn_file[k])) vref = o->syn_file[k];   // 2.4.33: + SAMPLE
            }
            SynthSamplePtr smp;
            bool vfail = false, vsample = false;
            if (!vref.empty()) {
                int vn = 0;
                std::string vp;
                if (vital_ref_parse(vref, "serumsmp", vn, vp)) {   // 2.4.33: a Serum 2 sample / multisample oscillator
                    vsample = true;
                    name = "Serum / " + path_stem(vp) + " / OSC " + char('A' + std::max(0, std::min(2, vn)));
                    // 2.4.35: a big multisample takes seconds: another sound picked meanwhile stops it between files
                    auto cancelled = [o, k] { std::lock_guard<std::mutex> lock(o->syn_mutex); return o->syn_quit || (o->syn_req & (1 << k)) != 0; };
                    try { smp = serum_sample(o->vfs, vp, vn, cancelled); name = smp->name; }
                    catch (const std::exception &e) { err = e.what(); }
                } else if (is_artwav_ref(vref)) {   // 2.4.24
                    vp = vref.substr(7);
                    name = "Arturia / " + path_stem(vp);
                    try { t = arturia_table(o->vfs, vp); }
                    catch (const std::exception &e) { err = std::string("Arturia: ") + e.what(); }
                } else if (vital_ref_parse(vref, "serum", vn, vp)) {   // 2.4.32: the preset's embedded / factory table
                    name = "Serum / " + path_stem(vp) + " / OSC " + char('A' + std::max(0, std::min(2, vn)));
                    try { t = serum_table(o->vfs, vp, vn); }
                    catch (const std::exception &e) { err = e.what(); }
                } else {
                    vital_ref_parse(vref, "vital", vn, vp);
                    name = "Vital / " + path_stem(vp) + " / OSC " + std::to_string(vn);
                    t = load_vital_table(o, vp, vn, name, err);
                }
                if (vsample) { if (!smp) vfail = true; }   // 2.4.33: a SAMPLE oscillator has no table
                else if (!t) { vfail = true; t = factory_classic(2); }
            } else {
                {   // 2.4.11: a project's file not in the list: read the Oscillators folder again first (without syn_mutex)
                    bool rescan = false;
                    {
                        std::lock_guard<std::mutex> lock(o->syn_mutex);
                        if (!o->syn_file[k].empty()) {
                            rescan = true;
                            const int n0 = syn_list_size(o, type);
                            for (int i = 0; i < n0 && rescan; i++) if (syn_user_rel(o, type, i) == o->syn_file[k]) rescan = false;
                        }
                    }
                    if (rescan) syn_rescan(o);
                }
                {
                    std::lock_guard<std::mutex> lock(o->syn_mutex);
                    // a project's own file, found again in the rescanned list
                    if (!o->syn_file[k].empty()) {
                        auto find = [&]() {
                            int n0 = syn_list_size(o, type);
                            for (int i = 0; i < n0; i++) if (syn_user_rel(o, type, i) == o->syn_file[k]) { idx = i; return true; }
                            return false;
                        };
                        const bool found = find();   // (2.4.11: after the rescan above: the library may have mounted after create())
                        if (found) { o->syn_file[k].clear(); o->syn_pending[k] = false; }
                        else {
                            // 2.4.11 (audit H2): not there (yet): keep the file name and the project's setting untouched (was:
                            // the index clamped into another list and written back, the name dropped: lost at the next save);
                            // diag_loop asks again for two minutes, get_state keeps saving the name
                            if (!o->syn_pending[k]) o->syn_pending_since[k] = now_ms();
                            o->syn_pending[k] = true;
                            waiting = true;
                        }
                    }
                    int n = syn_list_size(o, type);
                    if (n > 0) idx = std::max(0, std::min(n - 1, idx));
                    if (!syn_user_rel(o, type, idx).empty()) path = path_join(syn_user_dir(o), syn_user_rel(o, type, idx));
                    if (!path.empty() && path.back() == '/')   // 2.3.2: a folder of single cycles
                        frame_files = syn_frame_files(o, syn_user_dir(o), syn_user_rel(o, type, idx));
                    name = syn_entry_name(o, type, idx);
                    // 2.4.9: which folder of how many (the info line): one walk here, not on every poll of the readout
                    fold_i = fold_n = 0;
                    for (int i = 0; i < n; i++) {
                        if (i == 0 || !syn_same_folder(o, type, i - 1, i)) fold_n++;
                        if (i == idx) fold_i = fold_n;
                    }
                }
                if (waiting) continue;   // 2.4.11: the project's file is not there yet (see above)
                syn_set(o, k, "table", float(idx));
                if (type == Synth::OT_SAMPLE && !path.empty()) {   // 2.3.4: a file as a SAMPLE oscillator
                    try {
                        AudioInfo ai;
                        BlobPtr file = open_host_file(path);
                        // 2.4.11 (audit M3): held whole in RAM (and the previous one for 2 s more): a SAMPLE oscillator is a
                        // single sound, not a library; RANDOM ALL walks every WAV of the Oscillators folder
                        if (file->size() > (uint64_t(64) << 20)) throw ParseError("too big for a SAMPLE oscillator (over 64 MB): load it in a slot");
                        PcmPtr pcm = decode_audio(*file, &ai);
                        auto ss = std::make_shared<SynthSample>();
                        ss->name = name;
                        ss->pcm = pcm;
                        ss->root = ai.root >= 0 ? ai.root : 60;
                        ss->tune = ai.fine / 100.0;
                        if (!ai.loops.empty()) { ss->loop_start = ai.loops[0].start; ss->loop_end = ai.loops[0].end + 1; }
                        smp = ss;
                    } catch (const std::exception &e) { err = e.what(); }
                }
                try {
                    if (type == Synth::OT_CLASSIC) t = factory_classic(idx);
                    else if (type == Synth::OT_WAVETABLE)
                        t = path.empty() ? factory_table(idx) : !frame_files.empty() || path.back() == '/' ? load_wavetable_frames(name, frame_files)
                          : is_vitaltable(path) ? load_vital_table(o, path, 0, name, err)   // 2.4.13
                          : load_wavetable_file(path);
                } catch (const std::exception &e) { err = e.what(); }
            }
            std::lock_guard<std::mutex> lock(o->syn_mutex);
            if (o->syn_req & (1 << k)) continue;   // asked again meanwhile: that one wins
            SynthOscSettings &os = o->sampler.synth.settings.osc[k];
            if (o->syn[k].cur) o->syn_retired.emplace_back(o->syn[k].cur, now_ms());
            o->syn[k].cur = t;
            o->syn[k].name = name;
            o->syn[k].fold_i = fold_i; o->syn[k].fold_n = fold_n;
            o->syn[k].err = t || type >= Synth::OT_SAMPLE ? "" : (err.empty() ? "nothing to load" : err);
            o->syn[k].loading = 0;
            os.table.store(t.get(), std::memory_order_release);
            // 2.3.4: SAMPLE: a slot (its instrument) or the file
            if (o->syn[k].smp) o->syn_retired_s.emplace_back(o->syn[k].smp, now_ms());
            o->syn[k].smp = smp;
            os.sample.store(smp.get(), std::memory_order_release);
            os.sample_slot.store(type == Synth::OT_SAMPLE && idx < SYN_SAMPLE_SLOTS && !vsample ? idx : -1);   // 2.4.33: a Serum sample: its file
            if (type == Synth::OT_MACRO) os.macro_engine.store(idx);   // 2.3.5
            if (type == Synth::OT_SAMPLE && idx >= SYN_SAMPLE_SLOTS && !smp) o->syn[k].err = err.empty() ? "nothing to load" : err;
            if (vfail) o->syn[k].err = err.empty() ? "the Vital preset could not be read" : err;   // 2.4.13
            o->ui_rev.fetch_add(1);
        }
    }
}

// 2.3.5: the SYNTH's factory presets: each sets the synth's settings it names over the synth's defaults (slots, FX
// and everything outside the synth stay). Indices: oscillator lists (design.py), synth.hpp's SynthDest / filter types.
// 2.4.2: the LFOs are routed with the ROUTE keys (syn_lfo<n>_route; MOD = the mod env's destination, syn_me_dest: the mod
// env itself stays off), not the hidden older syn_lfo<n>_dest: the keys show the route, and a tap turns it off
struct SynPreset { const char *name, *values; };
const SynPreset SYN_PRESETS[] = {
    {"Init", "syn_on=1"},
    {"Supersaw Lead", "syn_on=1;sa_type=0;sa_table=2;sa_unison=7;sa_detune=35;sa_spread=80;sa_pmode=2;sa_rand=100;syn_flt_on=1;syn_flt_type=1;"
                      "syn_cutoff=82;syn_res=15;syn_attack=5;syn_decay=400;syn_sustain=80;syn_release=350;syn_glide=40;syn_glide_mode=1"},
    {"Fat Bass", "syn_on=1;sa_type=0;sa_table=2;sb_on=1;sb_type=0;sb_table=3;sb_oct=-1;sb_level=60;syn_sub_on=1;syn_sub_level=70;"
                 "syn_flt_on=1;syn_flt_type=2;syn_cutoff=48;syn_res=35;syn_fenv=55;syn_fe_decay=260;syn_fe_sustain=0;syn_vmode=2;syn_glide=60"},
    {"Acid Line", "syn_on=1;sa_type=0;sa_table=2;syn_flt_on=1;syn_flt_type=2;syn_cutoff=40;syn_res=78;syn_drive=35;syn_fenv=70;"
                  "syn_fe_decay=200;syn_fe_sustain=0;syn_vmode=2;syn_glide=70;syn_glide_mode=1;syn_release=80"},
    {"Wavetable Pad", "syn_on=1;sa_type=1;sa_table=0;sa_pos=30;sa_unison=3;sa_detune=15;sb_on=1;sb_type=1;sb_table=4;sb_level=55;"
                      "syn_lfo1_route=3;syn_me_dest=4;syn_lfo1_shape=0;syn_lfo1_rate=25;syn_lfo1_depth=35;syn_attack=900;syn_decay=800;syn_sustain=85;"
                      "syn_release=1800;syn_flt_on=1;syn_flt_type=0;syn_cutoff=74"},
    {"PWM Strings", "syn_on=1;sa_type=1;sa_table=1;sa_pos=40;sa_unison=4;sa_detune=18;sa_spread=70;syn_lfo2_route=3;syn_me_dest=4;syn_lfo2_shape=1;"
                    "syn_lfo2_rate=22;syn_lfo2_depth=30;syn_attack=350;syn_release=900;syn_flt_on=1;syn_flt_type=0;syn_cutoff=72"},
    {"Vowel Lead", "syn_on=1;sa_type=3;sa_table=3;sa_pos=0;sa_warp=50;sa_spread=60;syn_lfo1_route=3;syn_me_dest=4;syn_lfo1_shape=1;syn_lfo1_rate=30;"
                   "syn_lfo1_depth=100;syn_vmode=2;syn_glide=50;syn_release=300"},
    {"FM Bell", "syn_on=1;sa_type=3;sa_table=2;sa_pos=40;sa_warp=35;sa_spread=10;syn_attack=1;syn_decay=2200;syn_sustain=0;syn_release=1600;"
                "syn_mx1_src=6;syn_mx1_dst=6;syn_mx1_amt=30"},
    {"Plucked String", "syn_on=1;sa_type=3;sa_table=5;sa_pos=30;sa_warp=60;sa_spread=70;syn_release=900"},
    {"Glass Mallet", "syn_on=1;sa_type=3;sa_table=6;sa_pos=90;sa_warp=70;sa_spread=55;syn_release=1500"},
    {"Wavefold Bass", "syn_on=1;sa_type=3;sa_table=1;sa_pos=20;sa_warp=20;sa_spread=0;syn_sub_on=1;syn_sub_level=50;syn_me_on=1;"
                      "syn_me_dest=6;syn_me_amt=60;syn_me_attack=1;syn_me_decay=350;syn_me_sustain=0;syn_vmode=1"},
    {"Chord Stab", "syn_on=1;sa_type=3;sa_table=7;sa_pos=50;sa_warp=30;sa_spread=40;syn_flt_on=1;syn_flt_type=1;syn_cutoff=60;"
                   "syn_fenv=45;syn_fe_decay=250;syn_fe_sustain=0;syn_decay=450;syn_sustain=0;syn_release=250"},
    {"Organ", "syn_on=1;sa_type=0;sa_table=7;syn_lfo1_route=3;syn_me_dest=1;syn_lfo1_shape=0;syn_lfo1_rate=62;syn_lfo1_depth=3;syn_attack=5;syn_release=60"},
    {"Sync Lead", "syn_on=1;sa_type=0;sa_table=2;sa_wmode=4;sa_warp=30;syn_lfo1_route=3;syn_me_dest=6;syn_lfo1_shape=1;syn_lfo1_rate=35;syn_lfo1_depth=40;"
                  "syn_flt_on=1;syn_flt_type=1;syn_cutoff=80;syn_vmode=2;syn_glide=35"},
    {"Noise Sweep", "syn_on=1;sa_on=0;syn_noise_on=1;syn_noise_level=90;syn_noise_color=0;syn_flt_on=1;syn_flt_type=3;syn_cutoff=55;"
                    "syn_res=60;syn_lfo1_route=2;syn_lfo1_shape=0;syn_lfo1_rate=20;syn_lfo1_depth=70;syn_attack=400;syn_release=1500"},
    {"Drift Pad", "syn_on=1;sa_type=0;sa_table=6;sa_unison=4;sa_detune=12;sb_on=1;sb_type=0;sb_table=4;sb_oct=1;sb_level=35;syn_drift=60;"
                  "syn_attack=1200;syn_release=2200;syn_flt_on=1;syn_flt_type=0;syn_cutoff=66;syn_lfo2_route=2;syn_lfo2_rate=15;syn_lfo2_depth=15"},
};
constexpr int SYN_PRESET_COUNT = int(sizeof SYN_PRESETS / sizeof SYN_PRESETS[0]);

// is a parameter one of the synth's (what a synth preset sets: everything else is left as it is)
bool syn_param(const char *k) {
    size_t n = std::strlen(k);   // not the screens' bars (sa_w12, syn_ae_b3: drawn from the settings)
    while (n && isdigit((unsigned char)k[n - 1])) n--;
    if (n >= 2 && n < std::strlen(k) && ((k[n - 2] == '_' && k[n - 1] == 'w') || (k[n - 2] == '_' && k[n - 1] == 'b'))) return false;
    return (!std::strncmp(k, "syn_", 4) && std::strcmp(k, "syn_preset")) || ((k[0] == 's') && (k[1] == 'a' || k[1] == 'b') && k[2] == '_');
}

// ---------------------------------------------------------------------------------------------------------------
// 2.4.12 MUTATE / BREED (the user, after Aquanode Modular's Mutator; written here from scratch). MUTATE nudges the
// synth's continuous sound settings by SUBTLE / MEDIUM / WILD; it never changes an oscillator's type or wave, the
// output level or the voice settings, so a mutation cannot load another wave or jump in loudness. STORE A / STORE B keep
// two patches; BREED makes a child: each section (osc A, osc B, filter, the envelopes, each LFO, each MATRIX route, sub,
// noise, the mix) comes whole from one parent, some of the child's values half-way between the two; voice settings come
// from A. UNDO goes back one step (again: forward). Parents live in memory only (not in the project).

struct MutRange { float lo, hi; bool log; };
// the settings MUTATE may move, and how (nullptr: never)
bool mut_range(const char *k, MutRange &r) {
    auto is = [&](const char *x) { return !std::strcmp(k, x); };
    auto tail = [&](const char *x) { const size_t n = std::strlen(k), m = std::strlen(x); return n > m && !std::strcmp(k + n - m, x); };
    const bool osc = (k[0] == 's' && (k[1] == 'a' || k[1] == 'b') && k[2] == '_');
    if (osc) {
        if (tail("_level") || tail("_pos") || tail("_detune") || tail("_spread") || tail("_warp")) { r = {0, 100, false}; return true; }
        if (tail("_pan") || tail("_fine")) { r = {-100, 100, false}; return true; }
        return false;
    }
    if (is("syn_sub_level") || is("syn_noise_level") || is("syn_noise_tone") || is("syn_fm") || is("syn_ring") || is("syn_feedback") ||
        is("syn_cutoff") || is("syn_drive") || is("syn_keytrack") || is("syn_fvel") || is("syn_sustain") || is("syn_fe_sustain") ||
        is("syn_me_sustain") || is("syn_lfo1_rate") || is("syn_lfo2_rate") || is("syn_lfo1_depth") || is("syn_lfo2_depth") ||
        is("syn_me2_sustain") || is("syn_me3_sustain") || is("syn_lfo3_rate") || is("syn_lfo4_rate") || is("syn_lfo3_depth") || is("syn_lfo4_depth")) { r = {0, 100, false}; return true; }   // 2.4.17: + MOD 2
    if (is("syn_res")) { r = {0, 90, false}; return true; }   // (past 90 it screams)
    if (is("syn_balance") || is("syn_fenv") || is("syn_me_amt") || is("syn_me2_amt") || is("syn_me3_amt") || is("syn_sub_pan") || is("syn_noise_pan")) { r = {-100, 100, false}; return true; }
    if (is("syn_attack") || is("syn_fe_attack") || is("syn_me_attack") || is("syn_me2_attack") || is("syn_me3_attack")) { r = {0, 10000, true}; return true; }
    if (is("syn_decay") || is("syn_release") || is("syn_fe_decay") || is("syn_fe_release") || is("syn_me_decay") || is("syn_me_release") ||
        is("syn_me2_decay") || is("syn_me2_release") || is("syn_me3_decay") || is("syn_me3_release")) { r = {1, 10000, true}; return true; }
    if (!std::strncmp(k, "syn_mx", 6) && tail("_amt")) { r = {-100, 100, false}; return true; }
    return false;
}
// the section a synth setting belongs to (BREED takes a section whole from one parent); "" = the voice: from A
std::string mut_group(const char *k) {
    const std::string s = k;
    if (!s.compare(0, 3, "sa_")) return "osc a";
    if (!s.compare(0, 3, "sb_")) return "osc b";
    if (!s.compare(0, 8, "syn_flt_") || s == "syn_cutoff" || s == "syn_res" || s == "syn_drive" || s == "syn_keytrack" || s == "syn_fenv" ||
        s == "syn_fvel" || s == "syn_fat" || s == "syn_fmix" || !s.compare(0, 7, "syn_fe_") || !s.compare(0, 7, "syn_fr_")) return "filter";
    if (s == "syn_attack" || s == "syn_decay" || s == "syn_sustain" || s == "syn_release") return "amp env";
    if (!s.compare(0, 7, "syn_me_")) return "mod env";
    if (!s.compare(0, 8, "syn_me2_")) return "mod env 2";   // 2.4.17
    if (!s.compare(0, 8, "syn_me3_")) return "mod env 3";
    if (!s.compare(0, 7, "syn_lfo") && s.size() > 8 && s[7] >= '1' && s[7] <= '4' && s[8] == '_') return std::string("lfo ") + s[7];   // 2.4.17: 1-4
    if (!s.compare(0, 6, "syn_mx") && s.size() > 7 && std::isdigit((unsigned char)s[6])) return s.substr(0, s.find('_', 6));   // each MATRIX route (2.4.17: syn_mx10.. too)
    if (!s.compare(0, 8, "syn_sub_")) return "sub";
    if (!s.compare(0, 10, "syn_noise_")) return "noise";
    if (s == "syn_fm" || s == "syn_ring" || s == "syn_feedback" || s == "syn_balance" || s == "syn_sync") return "mix";
    return "";
}
double mut_u() { return arp_u(); }   // (one generator: OMNI_ARP_SEED repeats a test)
double mut_gauss() { double a = std::max(1e-9, mut_u()), b = mut_u(); return std::sqrt(-2 * std::log(a)) * std::cos(2 * M_PI * b); }

void mut_snapshot(Omni *o, Omni::SynSnap &sn) {
    sn.v.resize(NUM_COUNT);
    for (int i = 0; i < NUM_COUNT; i++) sn.v[size_t(i)] = o->values[i].load();
    sn.set = true;
}
// the synth's settings from a snapshot (only the synth's: the rest of the plugin stays), oscillators reloaded
void mut_apply(Omni *o, const std::vector<float> &v) {
    for (int i = 0; i < NUM_COUNT && size_t(i) < v.size(); i++) {
        const char *k = NUM_PARAMS[i].key;
        if (!syn_param(k) || !std::strcmp(k, "syn_on")) continue;
        if (o->values[i].exchange(v[size_t(i)]) != v[size_t(i)]) mark_changed(o, i);
    }
    apply_settings(o);
    for (int k = 0; k < 3; k++) syn_request(o, k);
}
void mut_say(Omni *o, const std::string &m) {
    std::lock_guard<std::mutex> c(o->gp_cache_mutex);
    o->mut_msg = m;
    o->mut_msg_at = now_ms();
}

void mut_action(Omni *o, const char *what) {
    if (!std::strcmp(what, "store_a") || !std::strcmp(what, "store_b")) {
        mut_snapshot(o, what[6] == 'a' ? o->mut_a : o->mut_b);
        mut_say(o, std::string("MUTATE: this patch stored as ") + (what[6] == 'a' ? "A" : "B"));
        return;
    }
    if (!std::strcmp(what, "undo")) {
        if (!o->mut_undo.set) { mut_say(o, "MUTATE: nothing to undo"); return; }
        Omni::SynSnap now;
        mut_snapshot(o, now);
        mut_apply(o, o->mut_undo.v);
        o->mut_undo = now;
        mut_say(o, "MUTATE: UNDO (again: forward)");
        return;
    }
    if (!std::strcmp(what, "breed") && (!o->mut_a.set || !o->mut_b.set)) {
        mut_say(o, std::string("BREED: STORE A and STORE B first (") + (o->mut_a.set ? "B" : o->mut_b.set ? "A" : "A and B") + " missing)");
        return;
    }
    Omni::SynSnap before;
    mut_snapshot(o, before);
    std::vector<float> v = before.v;
    int moved = 0;
    if (!std::strcmp(what, "go")) {   // MUTATE: SUBTLE / MEDIUM / WILD = how far and how many
        const int lvl = std::max(0, std::min(2, int(num(o, "mut_amount") + 0.5f)));
        static const double SD[3] = {0.06, 0.15, 0.32}, P[3] = {0.35, 0.55, 0.8};
        for (int i = 0; i < NUM_COUNT; i++) {
            MutRange r;
            const char *k = NUM_PARAMS[i].key;
            if (!syn_param(k) || !mut_range(k, r) || mut_u() >= P[lvl]) continue;
            float x = v[size_t(i)];
            if (x == 0 && (lvl < 2 || mut_u() > 0.2)) continue;   // what is off stays off (WILD: now and then it comes in)
            if (r.log) x = float(std::max<double>(r.lo, std::min<double>(r.hi, std::max(1.0, double(x)) * std::exp2(mut_gauss() * SD[lvl] * 5))));
            else x = float(std::max<double>(r.lo, std::min<double>(r.hi, x + mut_gauss() * SD[lvl] * (r.hi - r.lo))));
            if ((!std::strcmp(k, "sa_level") || !std::strcmp(k, "sb_level")) && before.v[size_t(i)] >= 20) x = std::max(20.0f, x);   // (no oscillator fades out)
            if (!std::strcmp(k, "syn_cutoff")) x = std::max(20.0f, x);   // (no filter shuts the sound)
            if (x != v[size_t(i)]) { v[size_t(i)] = x; moved++; }
        }
        static const char *const LV[3] = {"SUBTLE", "MEDIUM", "WILD"};
        mut_say(o, "MUTATE " + std::string(LV[lvl]) + ": " + std::to_string(moved) + " settings moved  ·  UNDO to go back");
    } else {   // BREED: sections from A or B, some values between them
        const std::vector<float> &A = o->mut_a.v, &B = o->mut_b.v;
        std::map<std::string, bool> from_b;
        for (int i = 0; i < NUM_COUNT; i++) {
            const std::string g = syn_param(NUM_PARAMS[i].key) ? mut_group(NUM_PARAMS[i].key) : "";
            if (!g.empty() && !from_b.count(g)) from_b[g] = mut_u() < 0.5;
        }
        int nb = 0;
        for (auto &kv : from_b) nb += kv.second;
        if (!from_b.empty() && (nb == 0 || nb == int(from_b.size()))) {   // a child of both: one section from the other parent
            auto it = from_b.begin();
            std::advance(it, int(mut_u() * double(from_b.size())) % int(from_b.size()));
            it->second = !it->second;
        }
        int blended = 0;
        for (int i = 0; i < NUM_COUNT; i++) {
            const char *k = NUM_PARAMS[i].key;
            if (!syn_param(k) || !std::strcmp(k, "syn_on")) continue;
            const std::string g = mut_group(k);
            const bool b = !g.empty() && from_b[g];
            float x = b ? B[size_t(i)] : A[size_t(i)];
            MutRange r;
            if (mut_range(k, r) && mut_u() < 0.3) {   // between the parents
                const double t = 0.25 + 0.5 * mut_u();
                x = float(A[size_t(i)] + (B[size_t(i)] - A[size_t(i)]) * t);
                blended++;
            }
            if (x != v[size_t(i)]) moved++;
            v[size_t(i)] = x;
        }
        nb = 0;
        for (auto &kv : from_b) nb += kv.second;
        mut_say(o, "BREED: " + std::to_string(int(from_b.size()) - nb) + " sections from A, " + std::to_string(nb) + " from B, " +
                       std::to_string(blended) + " values between  ·  UNDO to go back");
    }
    o->mut_undo = before;
    mut_apply(o, v);
}

void syn_load_preset(Omni *o, int idx) {
    idx = std::max(0, std::min(SYN_PRESET_COUNT - 1, idx));
    for (int i = 0; i < NUM_COUNT; i++)   // the synth's defaults first
        if (syn_param(NUM_PARAMS[i].key) && o->values[i].exchange(NUM_PARAMS[i].def) != NUM_PARAMS[i].def) mark_changed(o, i);
    std::string v = SYN_PRESETS[idx].values;
    size_t at = 0;
    while (at < v.size()) {
        size_t semi = v.find(';', at);
        std::string kv = v.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
        at = semi == std::string::npos ? v.size() : semi + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        int i = num_index(kv.substr(0, eq).c_str());
        if (i < 0) continue;
        o->values[i].store(float(std::atof(kv.c_str() + eq + 1)));
        mark_changed(o, i);
    }
    int pi = num_index("syn_preset");
    o->values[pi].store(float(idx));
    mark_changed(o, pi);
    {
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        syn_clear_refs(o);   // 2.4.13: (and the CUSTOM LFO shapes)
    }
    apply_settings(o);
    for (int k = 0; k < 3; k++) syn_request(o, k);
    syn_request_bits(o, SYN_REQ_LFO_ALL);
}

// 2.4.6: a patch's / an .omni's settings were just stored: the synth follows them. The synth's settings the file does
// not carry (one saved before the synth, 2.3) go back to their defaults, and the oscillators (and the SUB's wave) load
// again for their new type / table: before, the previous preset's synth went on playing, with its waves
void synth_follow(Omni *o, const Settings_list &kv) {
    for (int i = 0; i < NUM_COUNT; i++) {
        const char *key = NUM_PARAMS[i].key;
        if (!syn_param(key) && std::strcmp(key, "syn_preset")) continue;   // (and the preset's name: not left stale)
        bool has = false;
        for (auto &p : kv) if (p.first == key) { has = true; break; }
        if (!has && o->values[i].exchange(NUM_PARAMS[i].def) != NUM_PARAMS[i].def) mark_changed(o, i);
    }
    {   // 2.4.13: and the files the settings name ("@syn_file_a=...": a Vital preset's tables, LFO shapes)
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        syn_clear_refs(o);
        std::string v;
        for (auto &p : kv) {
            if (text_setting(p.first, "syn_file_a", v)) o->syn_file[0] = v;
            else if (text_setting(p.first, "syn_file_b", v)) o->syn_file[1] = v;
            else if (text_setting(p.first, "syn_lfo1_ref", v)) o->syn_lfo[0].ref = v;
            else if (text_setting(p.first, "syn_lfo2_ref", v)) o->syn_lfo[1].ref = v;
            else if (text_setting(p.first, "syn_lfo3_ref", v)) o->syn_lfo[2].ref = v;   // 2.4.17
            else if (text_setting(p.first, "syn_lfo4_ref", v)) o->syn_lfo[3].ref = v;
        }
    }
    for (int k = 0; k < 3; k++) syn_request(o, k);
    syn_request_bits(o, SYN_REQ_LFO_ALL);
}

// the tables swapped out more than 2 s ago (the audio thread reads the new one within a block)
void syn_free_retired(Omni *o) {
    std::lock_guard<std::mutex> lock(o->syn_mutex);
    long long t = now_ms();
    o->syn_retired.erase(std::remove_if(o->syn_retired.begin(), o->syn_retired.end(),
                                        [t](const std::pair<WavetablePtr, long long> &r) { return t - r.second > 2000; }),
                         o->syn_retired.end());
    o->syn_retired_s.erase(std::remove_if(o->syn_retired_s.begin(), o->syn_retired_s.end(),
                                          [t](const std::pair<SynthSamplePtr, long long> &r) { return t - r.second > 2000; }),
                           o->syn_retired_s.end());
    o->syn_lfo_retired.erase(std::remove_if(o->syn_lfo_retired.begin(), o->syn_lfo_retired.end(),   // 2.4.13
                                            [t](const std::pair<std::shared_ptr<const std::vector<float>>, long long> &r) { return t - r.second > 2000; }),
                             o->syn_lfo_retired.end());
}

// the folder an entry is in (factory ones: "", classic waves: "")
std::string syn_entry_folder(Omni *o, int type, int i) {
    const std::string rel = syn_user_rel(o, type, i);   // (2.3.4: SAMPLE's files too; the slots have none)
    if (rel.empty()) return "";
    if (rel.back() == '/') return rel;   // a folder entry belongs to itself
    size_t cut = rel.rfind('/');
    return cut == std::string::npos ? "/" : rel.substr(0, cut + 1);
}

// 2.3.2: FOLDER: the first entry of the next folder (wrapping to the factory ones, and reading the folder again)
void syn_next_folder(Omni *o, int k) {
    int type = int(syn_num(o, k, "type") + 0.5f);
    bool rescan = false;
    {
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        int n = syn_list_size(o, type), i = int(syn_num(o, k, "table") + 0.5f);
        if (n > 0) {
            const std::string cur = syn_entry_folder(o, type, i);
            int j = i + 1;
            while (j < n && syn_entry_folder(o, type, j) == cur) j++;
            if (j >= n) { rescan = true; j = 0; }   // 2.4.11: read again after the lock (syn_rescan)
            char key[24];
            std::snprintf(key, sizeof key, "s%c_table", syn_pre(k));
            o->values[num_index(key)].store(float(j));
        }
        o->syn_file[k].clear();
        o->syn_pending[k] = false;   // 2.4.11: the user picked another: stop waiting for the project's file
    }
    if (rescan) syn_rescan(o);
    syn_request(o, k);
}

// the folder [first, end) entry i is in
void syn_folder_span(Omni *o, int type, int n, int i, int &a, int &z) {
    a = i; z = i + 1;
    while (a > 0 && syn_same_folder(o, type, a - 1, i)) a--;
    while (z < n && syn_same_folder(o, type, z, i)) z++;
}
void syn_pick(Omni *o, int k, int i) {   // (syn_mutex held) entry i on oscillator k; the caller requests the load
    char key[24];
    std::snprintf(key, sizeof key, "s%c_table", syn_pre(k));
    o->values[num_index(key)].store(float(i));
    o->syn_file[k].clear();
}

// 2.4.9: < FOLDER: the first entry of the previous folder (wrapping to the last one)
void syn_prev_folder(Omni *o, int k) {
    int type = int(syn_num(o, k, "type") + 0.5f);
    {
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        int n = syn_list_size(o, type);
        if (n <= 0) return;
        int i = std::max(0, std::min(n - 1, int(syn_num(o, k, "table") + 0.5f))), a, z;
        syn_folder_span(o, type, n, i, a, z);
        int p = a > 0 ? a - 1 : n - 1;   // an entry of the previous folder: go to its first
        syn_folder_span(o, type, n, p, a, z);
        syn_pick(o, k, a);
    }
    syn_request(o, k);
}

// 2.4.9: RANDOM: any other entry of this folder (all: of the type's whole list)
void syn_random(Omni *o, int k, bool all) {
    thread_local std::minstd_rand rng{uint32_t(now_ms())};   // 2.4.29 (audit F8): per thread (was one for all instances)
    int type = int(syn_num(o, k, "type") + 0.5f);
    {
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        int n = syn_list_size(o, type);
        if (n <= 0) return;
        int i = std::max(0, std::min(n - 1, int(syn_num(o, k, "table") + 0.5f))), a = 0, z = n;
        if (!all) syn_folder_span(o, type, n, i, a, z);
        if (z - a < 2) return;   // nothing else to pick
        int j = a + int(rng() % uint32_t(z - a - 1));
        if (j >= i) j++;   // never the one playing now
        syn_pick(o, k, j);
    }
    syn_request(o, k);
}

// < > LOAD on an OSC screen
void syn_browse(Omni *o, int k, int dir) {
    int type = int(syn_num(o, k, "type") + 0.5f);
    if (dir == 0) syn_rescan(o);   // LOAD: the Oscillators folder read again (2.4.11: without syn_mutex)
    {
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        int n = syn_list_size(o, type);
        if (n > 0) {
            int i = int(syn_num(o, k, "table") + 0.5f) + dir;
            i = ((i % n) + n) % n;
            char key[24];
            std::snprintf(key, sizeof key, "s%c_table", syn_pre(k));
            o->values[num_index(key)].store(float(i));
        }
        o->syn_file[k].clear();
    }
    syn_request(o, k);
}

// the OSC screens' bars: the cycle at POSITION through the warp (not FM), 32 points, 0..127 around 64; only when one
// of those changed (each changed bar is a repaint on MPC's screen thread)
void syn_bars(Omni *o) {
    for (int k = 0; k < SYN_OSCS; k++) {
        const SynthOscSettings &os = o->sampler.synth.settings.osc[k];
        const Wavetable *t = os.table.load(std::memory_order_acquire);
        if (os.type.load() == Synth::OT_SAMPLE) {   // 2.3.4: a file: its level along its length (bars up); a slot: flat
            const SynthSample *sm = os.on.load() ? os.sample.load(std::memory_order_acquire) : nullptr;
            const float key = sm ? 9000.0f : -3.0f;
            if (o->bar_t[k] == reinterpret_cast<const Wavetable *>(sm) && o->bar_key[k] == key) continue;
            o->bar_t[k] = reinterpret_cast<const Wavetable *>(sm); o->bar_key[k] = key;
            for (int b = 0; b < SYN_BARS; b++) {
                uint8_t val = 64;
                if (sm && sm->pcm && sm->pcm->frames() > SYN_BARS) {
                    const int64_t n = sm->pcm->frames(), a = n * b / SYN_BARS, z = n * (b + 1) / SYN_BARS;
                    const int ch = sm->pcm->channels;
                    int pk = 0;
                    for (int64_t i = a; i < z; i += std::max<int64_t>(1, (z - a) / 256)) pk = std::max(pk, std::abs(int(sm->pcm->data[size_t(i * ch)])));
                    val = uint8_t(std::min(126, 64 + int(pk * 62 / 32768)));
                }
                if (o->syn_bar[k][b].exchange(val) != val) mark_changed(o, NUM_COUNT + 64 + k * SYN_BARS + b);
            }
            continue;
        }
        const bool on = os.on.load() && t && os.type.load() <= Synth::OT_WAVETABLE;
        const int wm = os.warp_mode.load() == Synth::WM_FM ? 0 : os.warp_mode.load();
        const float key = on ? os.pos.load() * 1000 + float(wm) * 7 + os.warp.load() * 13 : -2;
        if (t == o->bar_t[k] && key == o->bar_key[k]) continue;
        o->bar_t[k] = t; o->bar_key[k] = key;
        float scan = on ? std::max(0.0f, std::min(1.0f, os.pos.load())) * float(t->frames - 1) : 0;
        int fa = on ? std::min(t->frames - 1, int(scan)) : 0, fb = on ? std::min(t->frames - 1, fa + 1) : 0;
        float ff = scan - float(fa);
        for (int b = 0; b < SYN_BARS; b++) {
            uint8_t val = 64;
            if (on) {
                double ph = synth_warp_phase(wm, (b + 0.5) / SYN_BARS, os.warp.load());
                ph -= std::floor(ph);
                int i = std::min(WT_SIZE - 1, int(ph * WT_SIZE));
                float s = t->cycle(fa, 2)[i] * (1 - ff) + t->cycle(fb, 2)[i] * ff;
                val = uint8_t(std::max(1, std::min(127, int(std::lround(64 + s * 62)))));
            }
            if (o->syn_bar[k][b].exchange(val) != val) mark_changed(o, NUM_COUNT + 64 + k * SYN_BARS + b);
        }
    }
}

// 2.3.2: the ENV page's screens: each envelope's shape as ENV_BARS bars (0..127), from its knobs: attack, decay, a
// held stretch at the sustain level, release, each part's width growing with the square root of its time. Only when
// a knob moved (each changed bar is a repaint on MPC's screen thread)
void env_bars(Omni *o) {
    static const char *const P[ENV_SCREENS][4] = {{"syn_attack", "syn_decay", "syn_sustain", "syn_release"},
                                        {"syn_fe_attack", "syn_fe_decay", "syn_fe_sustain", "syn_fe_release"},
                                        {"syn_me_attack", "syn_me_decay", "syn_me_sustain", "syn_me_release"},
                                        {"syn_me2_attack", "syn_me2_decay", "syn_me2_sustain", "syn_me2_release"},    // 2.4.17
                                        {"syn_me3_attack", "syn_me3_decay", "syn_me3_sustain", "syn_me3_release"}};
    for (int e = 0; e < ENV_SCREENS; e++) {
        const float a = num(o, P[e][0]) / 1000.0f, d = num(o, P[e][1]) / 1000.0f, s = num(o, P[e][2]) / 100.0f, r = num(o, P[e][3]) / 1000.0f;
        const float key = a * 7 + d * 13 + s * 17 + r * 19;
        if (key == o->env_key[e]) continue;
        o->env_key[e] = key;
        const float wa = std::sqrt(std::max(0.001f, a)), wd = std::sqrt(std::max(0.001f, d)), wr = std::sqrt(std::max(0.001f, r));
        const float wh = 0.35f * (wa + wd + wr) + 0.1f, tot = wa + wd + wh + wr;
        for (int b = 0; b < ENV_BARS; b++) {
            const float t = (b + 0.5f) / ENV_BARS * tot;
            float v;
            if (t < wa) v = t / wa;
            else if (t < wa + wd) { float u = (t - wa) / wd; v = 1 + (s - 1) * (1 - (1 - u) * (1 - u) * (1 - u)); }
            else if (t < wa + wd + wh) v = s;
            else { float u = (t - wa - wd - wh) / wr; v = s * (1 - u) * (1 - u); }
            const uint8_t val = uint8_t(std::max(0, std::min(127, int(std::lround(v * 127)))));
            if (o->env_bar[e][b].exchange(val) != val) mark_changed(o, NUM_COUNT + 64 + SYN_OSCS * SYN_BARS + e * ENV_BARS + b);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// state: "key=value" lines

// 2.4.9: a path's file is there: the path itself, or (a path inside a disk image) the image file it starts with
bool host_reachable(const std::string &p) {
    struct stat st;
    if (p.empty()) return false;
    if (stat(p.c_str(), &st) == 0) return true;
    for (size_t cut = p.rfind('/'); cut != std::string::npos && cut > 0; cut = p.rfind('/', cut - 1)) {
        const std::string pre = p.substr(0, cut);
        if (stat(pre.c_str(), &st) == 0) return S_ISREG(st.st_mode);   // an image holding the rest; a folder: not there
    }
    return false;
}
// 2.4.9: a project's file on a drive that is now mounted under another name (/media/<drive>/...): the same path on the
// drive that has it; else the path as it was
std::string relocate_media(const std::string &p) {
    if (host_reachable(p) || p.compare(0, 7, "/media/") != 0) return p;
    const size_t cut = p.find('/', 7);
    if (cut == std::string::npos) return p;
    const std::string rest = p.substr(cut);
    DIR *d = opendir("/media");
    if (!d) return p;
    std::string found;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string cand = std::string("/media/") + e->d_name + rest;
        if (cand != p && host_reachable(cand)) { found = cand; break; }
    }
    closedir(d);
    return found.empty() ? p : found;
}

// 2.4.9: a slot that failed to load (Loader::missing): asked for again once its file is there (a drive plugged in,
// mounted late); once per appearance, so a file that is there but unreadable is not retried over and over
void retry_missing(Omni *o) {
    for (int i = 0; i < SLOTS; i++) {
        const Loader::Missing m = o->loader.missing(i);
        if (m.path.empty()) { o->miss_seen[i] = false; o->miss_path[i].clear(); continue; }
        const std::string p = relocate_media(m.path);
        const bool there = host_reachable(p);
        // 2.4.14: a failure seen for the first time only records whether its file is there: a file that was there when it
        // failed (no ROMs beside it, unreadable) is not loaded a second time, and its error stays on the screen
        if (o->miss_path[i] != m.path) { o->miss_path[i] = m.path; o->miss_seen[i] = m.there && p == m.path; }
        if (there && !o->miss_seen[i]) o->loader.request(p, m.preset, i, false);
        o->miss_seen[i] = there;
    }
}

constexpr size_t STATE_MAX = 8000;   // 2.4.11: vst2_wrap.c chunk[8192], with room to spare

std::string get_state(Omni *o) {
    // 2.4: version 2 writes only the settings that differ from their defaults (a reader of version 2 resets the rest):
    // the wrapper keeps at most 8 KB of state (vst2_wrap.c chunk[8192]), and every setting written out was 4.3 KB already
    std::string s = "omni-sampler 2\n", gates, expr, locks;
    for (int i = 0; i < SLOTS; i++) {
        LoadedInfo li = o->loader.slot_info(i);
        if (li.path.empty()) {   // 2.4.9: a project's instrument that could not load (yet) stays in the project
            const Loader::Missing m = o->loader.missing(i);
            if (m.path.empty()) continue;
            s += "slot" + std::to_string(i + 1) + "_path=" + m.path + "\n";
            s += "slot" + std::to_string(i + 1) + "_preset=" + std::to_string(m.preset) + "\n";
            continue;
        }
        s += "slot" + std::to_string(i + 1) + "_path=" + li.path + "\n";
        s += "slot" + std::to_string(i + 1) + "_preset=" + std::to_string(li.preset) + "\n";
        if (!li.source.empty()) {   // 2.4.9: an extracted instrument's source (a disk image): used if the .omni is gone
            s += "slot" + std::to_string(i + 1) + "_src=" + li.source + "\n";
            s += "slot" + std::to_string(i + 1) + "_srcp=" + std::to_string(li.source_preset) + "\n";
        }
    }
    {
        std::lock_guard<std::mutex> lock(o->ui_mutex);
        s += "browse=" + (o->preset_file.empty() ? o->dir : o->preset_file) + "\n";
    }
    {   // 2.3: an OSC playing a user's own oscillator: its file name (the list's order can change)
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        for (int k = 0; k < SYN_OSCS; k++) {
            const std::string rel = (o->syn_pending[k] && !o->syn_file[k].empty()) || is_vital_ref(o->syn_file[k]) ? o->syn_file[k]   // 2.4.11: not found yet; 2.4.13: a Vital preset's table
                                  : syn_user_rel(o, int(syn_num(o, k, "type") + 0.5f), int(syn_num(o, k, "table") + 0.5f));
            if (!rel.empty()) s += std::string("syn_file_") + syn_pre(k) + "=" + rel + "\n";   // (2.3.4: SAMPLE's files too)
        }
        for (int n = 0; n < SYN_LFOS; n++) {   // 2.4.13: a CUSTOM LFO's shape (2.4.17: 4)
            char key[24];
            std::snprintf(key, sizeof key, "syn_lfo%d_shape", n + 1);
            if (int(num(o, key) + 0.5f) == SYN_LFO_CUSTOM && !o->syn_lfo[n].ref.empty())
                s += "syn_lfo" + std::to_string(n + 1) + "_ref=" + o->syn_lfo[n].ref + "\n";
        }
    }
    {   // 2.4: the ARP's pattern, up to its last step with a dot
        std::string t = arp_pattern_text(o);
        while (t.size() >= 6 && t.compare(t.size() - 6, 4, "0000") == 0) t.resize(t.size() - 6);
        if (!t.empty()) s += "arp_pattern=" + t + "\n";
        if (!t.empty() && o->sampler.arp.settings.base.load()) s += "arp_base=" + std::to_string(o->sampler.arp.settings.base.load()) + "\n";   // 2.4.1
    }
    char buf[64];
    for (int i = 0; i < NUM_COUNT; i++) {
        if (o->values[i].load() == NUM_PARAMS[i].def) continue;   // 2.4 (version 2): defaults left out
        std::snprintf(buf, sizeof buf, "%g", o->values[i].load());
        s += std::string(NUM_PARAMS[i].key) + "=" + buf + "\n";
    }
    {
        std::lock_guard<std::mutex> lock(o->lock_mutex);   // 1.8.2: note locks, "lock<note>=<LOCK_KEYS values>"
        for (int n = 0; n < 128; n++) {
            bool any = false;
            for (float v : o->lock_ui[n]) any = any || v != 0;
            if (!any) continue;
            locks += "lock" + std::to_string(n) + "=";
            for (int k = 0; k < LOCK_N; k++) {
                std::snprintf(buf, sizeof buf, k ? ",%g" : "%g", o->lock_ui[n][k]);
                locks += buf;
            }
            locks += "\n";
        }
    }
    // 2.4.11 (audit H4): the wrapper keeps at most 8191 bytes and would cut the text mid-line (a half value applied on
    // reload). The optional parts go in only while they fit, whole lines only; what is left out is said on the screen.
    std::string dropped;
    {   // 2.4.29 (audit F7): the notes' gates and expression, compact (arp_x), given the room that is left (the locks keep theirs)
        const size_t used = s.size() + locks.size() + 7;   // ("arp_x=" and the line end)
        int omitted = 0;
        const std::string x = arp_x_text(o, used < STATE_MAX ? STATE_MAX - used : 0, &omitted);
        if (!x.empty()) expr = "arp_x=" + x + "\n";
        if (omitted) dropped += std::to_string(omitted) + " ARP notes' gate / expression";
    }
    for (const auto &part : {std::make_pair(&gates, "ARP gates"), std::make_pair(&expr, "ARP expression"), std::make_pair(&locks, "note locks")}) {
        if (part.first->empty()) continue;
        if (s.size() + part.first->size() <= STATE_MAX) s += *part.first;
        else dropped += (dropped.empty() ? "" : ", ") + std::string(part.second);
    }
    if (s.size() > STATE_MAX) {   // (not seen: every setting written out is 4.3 KB) cut at a line end, never mid-value
        const size_t nl = s.rfind('\n', STATE_MAX - 1);
        s.resize(nl == std::string::npos ? 0 : nl + 1);
        dropped += dropped.empty() ? "some settings" : ", some settings";
    }
    {
        std::lock_guard<std::mutex> c(o->gp_cache_mutex);   // (get_state also runs on the diag thread with omni-debug)
        if (!dropped.empty()) o->state_warning = "Project too big for MPC's 8 KB plugin state: " + dropped + " not saved";
        else o->state_warning.clear();
    }
    return s;
}

void set_state(Omni *o, const char *text) {
    o->restore_ms.store(now_ms());   // 2.4.9: (grown_opt_replay)
    o->rk_hold_ms.store(now_ms());   // LFO ROUTE keys: MPC restores their saved values next, not taps (lfo_route_tap)
    std::string path, browse;
    int preset = 0;
    std::string spath[SLOTS], ssrc[SLOTS];   // 2.4.9: ssrc / ssrcp: an extracted instrument's source
    int ssrcp[SLOTS] = {};
    int spreset[SLOTS] = {};
    float locks[128][LOCK_N] = {};   // 1.8.2: a project without them has none
    std::vector<std::string> keys;   // 2.2: the keys it has (types_compat)
    std::string arp_pat;             // 2.4: none in an older project: an empty pattern
    int arp_base = 0;                // 2.4.1: a MIDI file's lowest note above the root at C3
    std::string arp_gts;             // 2.4.1: the notes' own gates
    std::string arp_nxs;             // 2.4.2: and their expression
    std::string arp_xs;              // 2.4.29: both, compact (a project saved by 2.4.29 or later)
    // 2.4.13: the oscillators' files and the CUSTOM LFO shapes the text names, and whether it sets those oscillators / LFOs
    bool named_file[SYN_OSCS] = {}, named_osc[SYN_OSCS] = {}, named_lref[SYN_LFOS] = {}, named_lfo[SYN_LFOS] = {};
    std::string lref[SYN_LFOS];
    // 2.4: version 2 leaves defaults out: every setting starts at its default (version 1 listed them all; partial
    // version 1 texts, as tests and .omni patches use, change only what they name)
    const bool v2 = !std::strncmp(text, "omni-sampler ", 13) && std::atoi(text + 13) >= 2;
    if (v2) {
        for (int i = 0; i < NUM_COUNT; i++) o->values[i].store(NUM_PARAMS[i].def);
        o->env_pending.store(true);   // the project's envelope (as a version 1 project, which always had env_ keys)
    }
    const char *p = text;
    while (*p) {
        const char *e = std::strchr(p, '\n');
        std::string line(p, e ? size_t(e - p) : std::strlen(p));
        p = e ? e + 1 : p + line.size();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "path") path = v;   // projects before 1.4: one instrument, slot A
        else if (k == "preset") preset = std::atoi(v.c_str());
        else if (k.size() > 6 && k.compare(0, 4, "slot") == 0 && isdigit((unsigned char)k[4]) && k.compare(5, 5, "_path") == 0) spath[(k[4] - '1') & 3] = v;
        else if (k.size() > 6 && k.compare(0, 4, "slot") == 0 && isdigit((unsigned char)k[4]) && k.compare(5, 7, "_preset") == 0) spreset[(k[4] - '1') & 3] = std::atoi(v.c_str());
        else if (k.size() == 9 && k.compare(0, 4, "slot") == 0 && isdigit((unsigned char)k[4]) && k.compare(5, 4, "_src") == 0) ssrc[(k[4] - '1') & 3] = v;   // 2.4.9
        else if (k.size() == 10 && k.compare(0, 4, "slot") == 0 && isdigit((unsigned char)k[4]) && k.compare(5, 5, "_srcp") == 0) ssrcp[(k[4] - '1') & 3] = std::atoi(v.c_str());
        else if (k == "browse") browse = v;
        else if (k == "syn_file_a" || k == "syn_file_b") { std::lock_guard<std::mutex> l(o->syn_mutex); o->syn_file[k[9] - 'a'] = v; named_file[k[9] - 'a'] = true; }
        else if (k.size() == 12 && !k.compare(0, 7, "syn_lfo") && k[7] >= '1' && k[7] <= '4' && !k.compare(8, 4, "_ref")) { lref[k[7] - '1'] = v; named_lref[k[7] - '1'] = true; }   // 2.4.13 (2.4.17: 1-4)
        else if (k == "arp_pattern") arp_pat = v;   // 2.4
        else if (k == "arp_gates") arp_gts = v;   // 2.4.1
        else if (k == "arp_nx") arp_nxs = v;      // 2.4.2
        else if (k == "arp_x") arp_xs = v;        // 2.4.29 (audit F7)
        else if (k == "arp_base") arp_base = std::max(-200, std::min(200, std::atoi(v.c_str())));   // 2.4.1
        else if (k.size() > 4 && k.compare(0, 4, "lock") == 0 && isdigit((unsigned char)k[4])) {
            int n = std::atoi(k.c_str() + 4);
            const char *q = v.c_str();
            for (int f = 0; n >= 0 && n < 128 && f < LOCK_N && *q; f++) {
                locks[n][f] = float(std::atof(q));
                q = std::strchr(q, ',');
                if (!q) break;
                q++;
            }
        }
        else {
            int i = num_index(k.c_str());
            keys.push_back(k);
            if (i >= 0) o->values[i].store(float(std::atof(v.c_str())));
            if ((k[0] == 's' && (k[1] == 'a' || k[1] == 'b') && (k.compare(2, std::string::npos, "_type") == 0 || k.compare(2, std::string::npos, "_table") == 0)))
                named_osc[k[1] - 'a'] = true;   // 2.4.13
            if (k.size() == 14 && !k.compare(0, 7, "syn_lfo") && k[7] >= '1' && k[7] <= '4' && !k.compare(8, 6, "_shape")) named_lfo[k[7] - '1'] = true;
            if (i >= 0 && !k.compare(0, 4, "env_")) o->env_pending.store(true);   // the project's envelope: kept on load
        }
    }
    if (!v2) {   // 2.2: a project saved before the types: they are Off by default, so one with its reverb / drive up gets Classic
        std::vector<std::pair<std::string, float>> kv;
        for (auto &k : keys) kv.emplace_back(k, 0.0f);
        types_compat(o, kv, [](const std::pair<std::string, float> &p) { return p.first; });
    }
    apply_settings(o);
    arp_pattern_set(o, arp_pat);   // 2.4 (after apply_settings: the grid shows the project's page)
    o->sampler.arp.settings.base.store(arp_base);   // 2.4.1
    arp_nx_set(o, arp_nxs);   // (before the gates: arp_gates_set redraws)
    arp_gates_set(o, arp_gts);
    arp_x_set(o, arp_xs);   // (a project has either; this one wins when both are there)
    arp_leds_dirty(o);
    {
        std::lock_guard<std::mutex> lock(o->lock_mutex);
        std::memcpy(o->lock_ui, locks, sizeof locks);
        for (int n = 0; n < 128; n++) commit_lock(o, n);
    }
    show_lock(o);
    if (!path.empty() && spath[0].empty()) { spath[0] = path; spreset[0] = preset; }
    // "" clears the slot; the project's own settings win over those an .omni carries
    for (int i = 0; i < SLOTS; i++) {
        // 2.4.9: a drive mounted under another name: the file where it is now; an extracted .omni gone (deleted, or the
        // library on a drive not plugged in): its source image instead. Not there at all: the request fails and the slot
        // remembers it (Loader::missing), the project keeps it and it loads when the drive comes (retry_missing)
        if (!spath[i].empty()) spath[i] = relocate_media(spath[i]);
        if (!spath[i].empty() && !host_reachable(spath[i]) && !ssrc[i].empty()) {
            const std::string src = relocate_media(ssrc[i]);
            if (host_reachable(src)) { spath[i] = src; spreset[i] = ssrcp[i]; }
        }
        o->loader.request(spath[i], spreset[i], i, false);
    }
    {   // 2.4.13: a whole project (version 2) or a text setting an oscillator / LFO without a file: none left over from before
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        for (int k = 0; k < SYN_OSCS; k++)
            if (!named_file[k] && (v2 || named_osc[k])) { o->syn_file[k].clear(); o->syn_pending[k] = false; }
        for (int n = 0; n < SYN_LFOS; n++) {
            if (named_lref[n]) o->syn_lfo[n].ref = lref[n];
            else if (v2 || named_lfo[n]) o->syn_lfo[n].ref.clear();
            o->syn_lfo[n].step = 0;
        }
    }
    for (int k = 0; k < 3; k++) syn_request(o, k);   // 2.3: the project's oscillators (and the SUB's wave)
    syn_request_bits(o, SYN_REQ_LFO_ALL);   // 2.4.13
    if (path.empty()) path = spath[int(num(o, "target_slot") + 0.5f) & 3];
    std::lock_guard<std::mutex> lock(o->ui_mutex);
    std::string folder = browse.empty() ? path_dir(path) : browse;
    if (!folder.empty() && o->vfs.is_container(folder)) list_folder(o, folder);
}

// ---------------------------------------------------------------------------------------------------------------
// engine interface

void browse_worker(Omni *o);
std::string get_state(Omni *o);

// the skin's waveform bars and keyboard maps from the loaded program, the last played zone and the slot settings
void update_displays(Omni *o) {
    auto mp = o->loader.merged_ref(); const MergedInfo &m = *mp;   // 2.4.11: shared, not copied
    Settings &s = o->sampler.settings;
    int zi = o->sampler.last_zone.load();
    bool have = o->sampler.last_zone_prog.load() == m.serial && zi >= 0 && size_t(zi) < m.zones.size();
    if (!have) {   // nothing played yet: the target slot's first zone
        int t = o->loader.target();
        for (size_t i = 0; i < m.zones.size(); i++) if (m.zones[i].slot == t) { zi = int(i); have = true; break; }
    }
    for (int b = 0; b < 48; b++) o->disp_wave[b].store(have ? uint8_t(m.zones[size_t(zi)].wave[b] / 2) : 0);
    bool ks = s.layer_mode.load() == 1;
    // 2.4.11 (audit): the 32 tiles walk every zone (5,000-zone kits: 160,000 steps 30 times a second on the Live II):
    // only when something they show changed (the program, the zone last played, layer mode, a slot's keys / mute / shift)
    uint64_t sig = 1469598103934665603ull;
    auto mix = [&sig](uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
    mix(uint64_t(reinterpret_cast<uintptr_t>(mp.get()))); mix(uint64_t(m.serial)); mix(uint64_t(int64_t(have ? zi : -1)));
    mix(ks); mix(uint64_t(int64_t(o->sampler.active_slot.load())));
    for (int sl = 0; sl < 4; sl++) {
        mix(uint64_t(int64_t(s.slot_shift[sl].load()))); mix(s.slot_mute[sl].load());
        mix(uint64_t(int64_t(s.slot_lo[sl].load()))); mix(uint64_t(int64_t(s.slot_hi[sl].load())));
    }
    if (sig == o->disp_sig) return;
    o->disp_sig = sig;
    for (int t = 0; t < 32; t++) {
        int k0 = t * 4, k1 = t * 4 + 3, play = 0, mask = 0;
        for (size_t i = 0; i < m.zones.size(); i++) {
            const ZoneBrief &z = m.zones[i];
            int sl = z.slot & 3;
            // 1.8: where the zone plays: its slot's key shift moves it (it used to be drawn at its own keys, so a slot
            // shifted down never showed below the instrument's lowest key); keys shifted off the keyboard are not drawn
            int sh = s.slot_shift[sl].load(), zlo = z.key_lo + sh, zhi = z.key_hi + sh;
            if (zhi < k0 || zlo > k1 || zhi < 0 || zlo > 127) continue;
            bool sounding = !s.slot_mute[sl].load() &&
                            (ks ? sl == o->sampler.active_slot.load() : !(s.slot_hi[sl].load() < k0 || s.slot_lo[sl].load() > k1));
            if (sounding) play = std::max(play, have && int(i) == zi ? 2 : 1);
            bool in_range = ks || !(s.slot_hi[sl].load() < k0 || s.slot_lo[sl].load() > k1);
            if (in_range && !s.slot_mute[sl].load()) mask |= 1 << sl;
        }
        if (o->disp_ptile[t].exchange(uint8_t(play)) != uint8_t(play)) mark_changed(o, NUM_COUNT + t);
        if (o->disp_ltile[t].exchange(uint8_t(mask)) != uint8_t(mask)) mark_changed(o, NUM_COUNT + 32 + t);
    }
}

void apply_saved_settings(Omni *o, const Settings_kv &kv);

void diag_loop(Omni *o) {
    int tick = 0;
    while (!o->diag_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        update_displays(o);
        if (tick % 4 == 0) { syn_bars(o); env_bars(o); }   // 2.3: about 7 times a second at most
        syn_free_retired(o);
        follow_notes(o);
        delete o->sampler.take_retired_preview();   // a finished preview, freed here (not on the audio thread)
        if (++tick % 30) continue;
        update_extract(o);   // the library's drive plugged in or pulled out
        retry_missing(o);    // 2.4.9: a project's instrument whose drive just came
        for (int k = 0; k < SYN_OSCS; k++) {   // 2.4.11: a project's oscillator file whose drive was not there yet
            bool again = false;
            { std::lock_guard<std::mutex> l(o->syn_mutex); again = o->syn_pending[k] && now_ms() - o->syn_pending_since[k] < 120000 && tick % 60 == 0; }
            if (again) syn_request(o, k);
        }
        struct stat st;
        if (stat("/sdcard/omni-debug", &st) != 0) continue;
        Settings &s = o->sampler.settings;
        std::string t = get_state(o);
        char b[256];
        for (int k = 0; k < MOD_SLOTS; k++) {
            std::snprintf(b, sizeof b, "engine slot %d: src %d dst %d amt %.2f\n", k + 1, s.mod_src[k].load(), s.mod_dst[k].load(), s.mod_amt[k].load());
            t += b;
        }
        std::snprintf(b, sizeof b, "engine lfo: rate %.3f/%.3f Hz wave %d/%d sync %d/%d out %.3f/%.3f voices %d\n", s.lfo_rate[0].load(),
                      s.lfo_rate[1].load(), s.lfo_wave[0].load(), s.lfo_wave[1].load(), s.lfo_sync[0].load(), s.lfo_sync[1].load(),
                      o->sampler.lfo_out[0].load(), o->sampler.lfo_out[1].load(), o->sampler.active_voices.load());
        t += b;
        t += "status=" + o->loader.status() + "\n";
        FILE *f = std::fopen("/tmp/omni-debug.txt.new", "w");
        if (!f) continue;
        std::fwrite(t.data(), 1, t.size(), f);
        std::fclose(f);
        std::rename("/tmp/omni-debug.txt.new", "/tmp/omni-debug.txt");
    }
}

// 1.8.13: the browser's last folder, for every project and new instance (<data_dir>/last_browse.txt, like the
// library's location); a project's own browse= still wins when it loads
void save_last_browse(Omni *o, const std::string &dir) {
    if (dir.empty() || dir == o->last_browse) return;
    o->last_browse = dir;
    save_browse_pos(o);
}
// 2.4.27 (user): the position in the list too. last_browse.txt: line 1 the folder (as before: older versions read only
// that), line 2 the highlighted row (path TAB preset), line 3 the first row shown. Written on a tap, a followed load, a
// folder change and when the plugin closes, only when it changed (not on every scroll step: the MPC's storage). ui_mutex held
void save_browse_pos(Omni *o) {
    if (o->last_browse.empty()) return;
    std::string s = o->last_browse + "\n";
    if (o->cur >= 0 && o->cur < int(o->rows.size())) {
        const Row &r = o->rows[size_t(o->cur)];
        s += r.path + "\t" + std::to_string(r.preset);
    }
    s += "\n" + std::to_string(o->top) + "\n";
    if (s == o->last_pos) return;
    o->last_pos = s;
    std::string p = path_join(o->data_dir, "last_browse.txt"), t = p + ".new";
    if (FILE *f = std::fopen(t.c_str(), "w")) {
        std::fputs(s.c_str(), f);
        std::fclose(f);
        std::rename(t.c_str(), p.c_str());
    }
}

void *create(const char *data_dir) {
    register_all();
    Omni *o = new Omni();
    o->data_dir = data_dir && *data_dir ? data_dir : "/tmp/omni-sampler";
    mkdir(o->data_dir.c_str(), 0755);
    {   // the user's library location, if one was chosen
        FILE *f = std::fopen(path_join(o->data_dir, "library_path.txt").c_str(), "r");
        if (f) {
            char b[1024] = {};
            if (std::fgets(b, sizeof b, f)) o->lib_choice = trim(b);
            std::fclose(f);
        }
        // a choice on the internal storage (before 1.8 that was allowed, and was the default) is not a library any more;
        // the file keeps it until a new one is set. OMNI_SAMPLER_LIBRARY: tests and desktop builds (no /media there)
        const char *test_lib = std::getenv("OMNI_SAMPLER_LIBRARY");
        if (test_lib && *test_lib) { mkdir(test_lib, 0755); o->lib_choice = test_lib; }
        else if (!external_path(o->lib_choice)) o->lib_choice.clear();
        if (!o->lib_choice.empty()) set_search_dirs({o->lib_choice});   // where readers look for shared samples (M1 disk)
    }
    apply_settings(o);
    {
        std::lock_guard<std::mutex> lock(o->ui_mutex);
        list_drives(o);
        // 1.8.13: back where the browser last was (any instance), when that folder or image is still there
        if (FILE *f = std::fopen(path_join(o->data_dir, "last_browse.txt").c_str(), "r")) {
            char b[2048] = {};
            if (std::fgets(b, sizeof b, f)) {
                std::string d = trim(b);
                try {
                    if (!d.empty() && (host_dir(d) || o->vfs.is_container(d))) {
                        list_folder(o, d);
                        // 2.4.27: the row that was highlighted (found by its file and preset, so a changed folder still
                        // finds it) and the scroll position
                        char r[2048] = {}, t[32] = {};
                        std::string row = std::fgets(r, sizeof r, f) ? std::string(r) : std::string();
                        while (!row.empty() && (row.back() == '\n' || row.back() == '\r')) row.pop_back();
                        const int top = std::fgets(t, sizeof t, f) ? std::atoi(t) : 0;
                        const size_t tab = row.rfind('\t');
                        if (o->dir == d && tab != std::string::npos) {
                            const std::string path = row.substr(0, tab);
                            const int preset = std::atoi(row.c_str() + tab + 1);
                            for (int i = 0; i < int(o->rows.size()); i++) {
                                const Row &x = o->rows[size_t(i)];
                                if (x.path != path || x.preset != preset) continue;
                                o->cur = i;
                                int tt = std::max(0, std::min(top, max_top(o)));
                                if (i < tt || i >= tt + ROWS) tt = std::max(0, std::min(i - ROWS / 2, max_top(o)));
                                set_top(o, tt);
                                break;
                            }
                        }
                    }
                } catch (const std::exception &) { list_drives(o); }
            }
            std::fclose(f);
        }
    }
    o->loader.get_settings = [o] { return settings_json(o); };
    o->loader.apply_settings = [o](const Settings_kv &kv) { apply_saved_settings(o, kv); };
    o->loader.loaded = [o](int slot, bool fresh, bool had) {
        env_loaded(o, slot, fresh, had);
        // 2.4.14: an instrument tapped in without settings of its own (a sampler instrument): the effects a preset set go back
        if (fresh && !had && !o->fx_by_preset.empty()) { fx_follow(o, Settings_kv(), nullptr); apply_settings(o); }
    };
    o->br_thread = std::thread(browse_worker, o);
    {   // 2.3: the SYNTH's oscillators
        syn_rescan(o);
        o->syn_thread = std::thread(syn_worker, o);
        for (int k = 0; k < 3; k++) syn_request(o, k);   // A, B and (2.3.1) the SUB's wave
        syn_request(o, 3);   // 2.4: the MIDI files
    }
    o->diag_thread = std::thread(diag_loop, o);
    return o;
}

void destroy(void *inst) {
    Omni *o = static_cast<Omni *>(inst);
    { std::lock_guard<std::mutex> lock(o->ui_mutex); save_browse_pos(o); }   // 2.4.27: where the list was
    // the loader thread first: its callbacks (saved settings, the envelope faders) use the rest of Omni, which would
    // otherwise be freed under a load still finishing (removing the plugin mid-load)
    o->loader.shutdown();
    {
        std::lock_guard<std::mutex> lock(o->br_qm);
        o->br_quit = true;
    }
    o->br_cv.notify_all();
    if (o->br_thread.joinable()) o->br_thread.join();
    {
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        o->syn_quit = true;
    }
    o->syn_cv.notify_all();
    if (o->syn_thread.joinable()) o->syn_thread.join();
    o->diag_quit.store(true);
    if (o->diag_thread.joinable()) o->diag_thread.join();
    delete o;
}

void midi(void *inst, const uint8_t *msg, int len) {
    Omni *o = static_cast<Omni *>(inst);
    // program change: the n-th preset of the loaded file, loaded by the loader thread (no locks here)
    if (len >= 2 && (msg[0] & 0xF0) == 0xC0) { if (num(o, "prog_change") > 0.5f) o->loader.request_program(msg[1] & 0x7F); return; }
    o->sampler.midi(msg, len);
}

bool is_trigger_on(const char *val) { return std::atof(val) > 0.5f; }

// the trigger buttons, latched while held so the button lights
constexpr int TRIGGER_COUNT = 35;
static_assert(TRIGGER_COUNT <= 40, "Omni::trig_on");
const char *const TRIGGERS[TRIGGER_COUNT] = {"prog_prev", "prog_next", "br_prev", "br_next", "br_up", "br_drives", "br_library",
                                             "br_refresh", "pad_down", "pad_up", "br_extract", "slot_clear", "auto_split",
                                             "br_setlib", "patch_save", "panic", "br_rowup", "br_rowdown", "lock_clear",
                                             "br_enter", "reset_all", "sa_prev", "sa_next", "sa_load", "sb_prev", "sb_next",
                                             "sb_load", "sa_folder", "sb_folder",
                                             "sa_fprev", "sa_rnd", "sa_rndall", "sb_fprev", "sb_rnd", "sb_rndall"};   // 2.4.9
int trigger_index(const char *key) {
    for (int i = 0; i < TRIGGER_COUNT; i++) if (!std::strcmp(key, TRIGGERS[i])) return i;
    return -1;
}

// LFO ROUTE keys (AMP FILTER MOD on the ENV page): MPC's radio group always selects, so a lit key could not turn the
// route off. Each key is a Toggle on its own "syn_lfo<n>_rk_<amp|flt|mod>", lit while syn_lfo<n>_route (the setting:
// state, presets, Q-Links) is its route: the key's value is derived, never stored. A tap on the lit key turns the route
// Off, on another key routes there. A set equal to what the key shows is MPC's echo of our report (audioMasterAutomate)
// or of what it saved, not a tap; and for 2 s after a project comes back (MPC restores saved values) no set is a tap
const char *const LFO_RK[3] = {"amp", "flt", "mod"};
constexpr long long RK_HOLD_MS = 2000;
bool lfo_route_key(const char *key, int &lfo, int &route) {
    if (std::strncmp(key, "syn_lfo", 7) || key[7] < '1' || key[7] > '4' || std::strncmp(key + 8, "_rk_", 4)) return false;   // 2.4.17: 1-4
    for (int r = 0; r < 3; r++)
        if (!std::strcmp(key + 12, LFO_RK[r])) { lfo = key[7] - '1'; route = r + 1; return true; }
    return false;
}
int lfo_route_index(int lfo) {
    static const int i[SYN_LFOS] = {num_index("syn_lfo1_route"), num_index("syn_lfo2_route"), num_index("syn_lfo3_route"), num_index("syn_lfo4_route")};
    return i[lfo];
}
int lfo_route_now(Omni *o, int lfo) { return int(o->values[lfo_route_index(lfo)].load() + 0.5f); }
void lfo_route_tap(Omni *o, int lfo, int route, const char *val) {
    const bool lit = lfo_route_now(o, lfo) == route;
    if ((std::atof(val) > 0.5) == lit) return;   // an echo: no tap
    o->rk_dirty.fetch_or(uint16_t(1u << (lfo * 3 + route - 1)));   // (MPC's toggle shows what it sent until it reads ours)
    if (now_ms() - o->rk_hold_ms.load() < RK_HOLD_MS) return;
    o->values[lfo_route_index(lfo)].store(lit ? 0.0f : float(route));
    mark_changed(o, lfo_route_index(lfo));
    {   // 2.4.2: route Off kept feeding the older destination (syn_lfo<n>_dest: hidden since 2.3.6, still set by older
        // projects and presets), so a tap Off left the LFO running with no key lit. A tap is the user's choice: clear it
        static const int di[SYN_LFOS] = {num_index("syn_lfo1_dest"), num_index("syn_lfo2_dest"), -1, -1};   // (LFO 3 / 4: no hidden dest)
        if (di[lfo] >= 0 && o->values[di[lfo]].exchange(0.0f) != 0.0f) mark_changed(o, di[lfo]);
    }
    apply_settings(o);   // (mpc_engine_changed reports the three keys: the route moved)
}

// 2.4.9: option lists that grew at the end (design.py SYN_ALL_DESTS). MPC replays a project's normalized values right
// after its chunk (vst2_wrap.c born_ms): a value saved over the older, shorter list maps to another option over the new
// one. The chunk (state) carries the right index, so for 3 s after a restore a host write that would change one of these
// is dropped. A user's own change is never that soon after a project loads
bool grown_opt(const char *key) {
    if (!std::strcmp(key, "syn_me_dest") || !std::strcmp(key, "arp_g_contour")) return true;
    if (!std::strcmp(key, "syn_me2_dest") || !std::strcmp(key, "syn_me3_dest")) return true;   // 2.4.35: + Level A / B (all dests)
    if (!std::strcmp(key, "syn_lfo1_shape") || !std::strcmp(key, "syn_lfo2_shape")) return true;   // 2.4.13: + Custom
    if (!std::strncmp(key, "syn_macro", 9) && key[9] >= '1' && key[9] <= '4' && !std::strcmp(key + 10, "_dest")) return true;
    // 2.4.17: the MATRIX's sources grew too (+ LFO 3 / 4, Mod Env 2 / 3) and the destinations by LFO 3 / 4 Rate; routes
    // 9-16 have two digits (syn_mx10_dst: key[7] is a digit)
    if (!std::strncmp(key, "syn_mx", 6) && std::isdigit((unsigned char)key[6])) {
        const char *t = key + 7;
        if (std::isdigit((unsigned char)*t)) t++;
        if (!std::strcmp(t, "_dst") || !std::strcmp(t, "_src")) return true;
    }
    return false;
}
bool grown_opt_replay(Omni *o, const char *key, int ni, const char *val) {
    if (now_ms() - o->restore_ms.load() > 3000 || !grown_opt(key)) return false;
    return std::lround(std::atof(val)) != std::lround(o->values[ni].load());
}

void set_param(void *inst, const char *key, const char *val) {
    Omni *o = static_cast<Omni *>(inst);
    if (!std::strcmp(key, "state")) { set_state(o, val); return; }
    { int lfo, route; if (lfo_route_key(key, lfo, route)) { lfo_route_tap(o, lfo, route, val); return; } }
    if (!std::strncmp(key, "arp_", 4)) {   // 2.4: the ARP tab's grid and keys
        if (!std::strncmp(key, "arp_c", 5) && std::isdigit((unsigned char)key[5])) {   // a dot: arp_c<row>_<column>
            int r = std::atoi(key + 5) - 1;
            const char *us = std::strchr(key + 5, '_');
            int c = us ? std::atoi(us + 1) - 1 : -1;
            if (r < 0 || r > 15 || c < 0 || c > 15) return;
            // 2.4.1: the cell shows a picture code (arp_cell_code); MPC's tap sends 0 or 127 (its toggle), and repeats a
            // code we report back as it is (audioMasterAutomate): that repeat is not a tap
            const int v = int(std::lround(std::atof(val)));
            if (v == o->arp_view[r * 16 + c].load()) return;
            const int page = std::max(0, std::min(3, int(num(o, "arp_page") + 0.5f))), st = page * 16 + c;
            // TAP selects only (the user: drawing is DRAW's): a note -> selected; the selected one or an empty cell ->
            // nothing selected
            int s0, s1;
            if (arp_note_span(o, r, st, s0, s1) && !(o->arp_sel_on && o->arp_sel_r == r && o->arp_sel_s0 == s0))
                arp_select(o, true, r, s0);
            else if (o->arp_sel_on) arp_select(o, false);
            arp_refresh_view(o);
            mark_changed(o, Omni::ARP_REP0 + r * 16 + c);   // (MPC's toggle shows what it sent until it reads ours)
            return;
        }
        if (!std::strncmp(key, "arp_led", 7) || !std::strncmp(key, "arp_row", 7) || !std::strcmp(key, "arp_name") || !std::strcmp(key, "arp_info")
            || !std::strcmp(key, "arp_status") || !std::strcmp(key, "arp_chord")) return;
        if (!std::strcmp(key, "arp_xy_x") || !std::strcmp(key, "arp_xy_y")) {   // 2.4.1: the DRAW pad
            o->arp_xy[key[7] == 'y'] = float(std::atof(val));
            arp_stroke(o);
            return;
        }
        if (!std::strncmp(key, "arp_g_", 6) && (!std::strcmp(key + 6, "generate") || !std::strcmp(key + 6, "fill") || !std::strcmp(key + 6, "invert") ||
            !std::strcmp(key + 6, "reverse") || !std::strcmp(key + 6, "double") || !std::strcmp(key + 6, "half") ||
            !std::strcmp(key + 6, "mutate_go") || !std::strcmp(key + 6, "undo"))) {   // 2.4.2 GEN
            if (is_trigger_on(val)) arp_gen(o, !std::strcmp(key + 6, "mutate_go") ? "mutate" : key + 6);
            return;
        }
        if (!std::strcmp(key, "arp_e_apply")) { if (is_trigger_on(val)) arp_gen(o, "euclid"); return; }   // 2.4.2
        if (!std::strcmp(key, "arp_v_go")) { if (is_trigger_on(val)) arp_gen(o, "voice"); return; }      // 2.4.14
        if (!std::strcmp(key, "arp_shift_u") || !std::strcmp(key, "arp_shift_d")) {   // 2.4.9
            if (is_trigger_on(val)) arp_shift_row(o, key[10] == 'u' ? 1 : -1);
            return;
        }
        if (!std::strcmp(key, "arp_shift_l") || !std::strcmp(key, "arp_shift_r")) {   // 2.4.1
            if (is_trigger_on(val)) arp_shift(o, key[10] == 'r' ? 1 : -1);
            return;
        }
        if (!std::strcmp(key, "arp_prev") || !std::strcmp(key, "arp_next")) {   // pick the next / previous file (LOAD loads it)
            if (!is_trigger_on(val)) return;
            bool scan = false;
            {
                std::lock_guard<std::mutex> lock(o->syn_mutex);
                const int n = int(o->arp_files.size());
                if (n == 0) scan = true;
                else o->arp_sel = ((o->arp_sel < 0 ? 0 : o->arp_sel) + (key[4] == 'n' ? 1 : -1) + n) % n;
                o->arp_msg.clear();
            }
            if (scan) syn_request(o, 3);
            return;
        }
        if (!std::strcmp(key, "arp_load")) {
            if (!is_trigger_on(val)) return;
            {
                std::lock_guard<std::mutex> lock(o->syn_mutex);
                o->arp_want = o->arp_sel >= 0 && o->arp_sel < int(o->arp_files.size()) ? o->arp_files[size_t(o->arp_sel)] : "";
                o->arp_msg = o->arp_want.empty() ? "No MIDI file picked: put .mid files in <Library>/MIDI" : "Loading...";
            }
            syn_request(o, 3);   // (a fresh list first: files added since)
            syn_request(o, 4);
            return;
        }
        if (!std::strcmp(key, "arp_clear")) {   // the page shown
            if (is_trigger_on(val)) arp_clear_steps(o, std::max(0, std::min(3, int(num(o, "arp_page") + 0.5f))) * 16, 16);
            return;
        }
    }
    int ni = num_index(key);
    if (ni >= 0 && grown_opt_replay(o, key, ni, val)) return;   // 2.4.9
    if (ni >= 0) {
        bool auto_loop_flip = !std::strcmp(key, "auto_loop") && (o->values[ni].load() > 0.5f) != (std::atof(val) > 0.5);
        if (!std::strncmp(key, "slot", 4) && !std::strcmp(key + 5, "_shift")) {
            // moving a slot moves its key range with it (an open end, Low at C-2 or High at G8, stays open)
            int old_shift = int(std::lround(o->values[ni].load())), new_shift = int(std::lround(std::atof(val)));
            int slot = (key[4] - '1') & 3;
            char k[24];
            for (int e = 0; e < 2; e++) {
                std::snprintf(k, sizeof k, "slot%c%s", key[4], e ? "_hi" : "_lo");
                int ki = num_index(k), v = int(std::lround(o->values[ki].load())), edge = e ? 127 : 0;
                Omni::RangeBase &rb = o->range_base[slot][e];
                // the stored range still describes this end unless something else moved it (a knob, a patch, auto split)
                bool same = rb.set && v == (rb.open ? edge : std::max(0, std::min(127, rb.base + old_shift)));
                if (!same) { rb.set = true; rb.open = v == edge; rb.base = v - old_shift; }
                if (!rb.open && new_shift != old_shift) o->values[ki].store(float(std::max(0, std::min(127, rb.base + new_shift))));
            }
        }
        const float before = o->values[ni].load();   // 2.4.13 (a Vital preset's table: kept, below)
        o->values[ni].store(float(std::atof(val)));
        if (!std::strcmp(key, "br_folders")) {   // 2.1.3: SAMPLE FOLDERS switched: the folder shown again, with / without them
            std::lock_guard<std::mutex> lock(o->ui_mutex);
            if (!o->dir.empty() && o->preset_file.empty()) { int top = o->top; list_folder(o, o->dir, o->group); set_top(o, std::min(top, max_top(o))); }
            o->ui_rev.fetch_add(1);
            return;
        }
        if (key[0] == 's' && (key[1] == 'a' || key[1] == 'b') && key[2] == '_' && (!std::strcmp(key + 3, "type") || !std::strcmp(key + 3, "table"))) {
            {   // 2.4.13: an oscillator playing a Vital preset's own table keeps it when the value does not change, and
                // against MPC's replay of a project's older normalized values right after the restore (grown_opt_replay)
                std::lock_guard<std::mutex> lock(o->syn_mutex);
                if (is_vital_ref(o->syn_file[key[1] - 'a'])) {
                    const bool same = std::lround(before) == std::lround(o->values[ni].load());
                    if (same || now_ms() - o->restore_ms.load() <= 3000) { o->values[ni].store(before); return; }
                }
            }
            apply_settings(o);   // 2.3: another oscillator type or table: load it
            { std::lock_guard<std::mutex> lock(o->syn_mutex); o->syn_file[key[1] - 'a'].clear(); }
            syn_request(o, key[1] - 'a');
            return;
        }
        if (!std::strcmp(key, "syn_sub_shape")) { apply_settings(o); syn_request(o, 2); return; }   // 2.3.1
        if (!std::strcmp(key, "arp_page")) { apply_settings(o); arp_refresh_view(o); arp_leds_dirty(o); return; }   // 2.4: the grid shows that page
        if (!std::strcmp(key, "arp_length")) { apply_settings(o); arp_leds_dirty(o); arp_refresh_view(o); return; }
        if (!std::strcmp(key, "arp_rate")) { apply_settings(o); arp_leds_dirty(o); return; }   // 2.4.1: beats and bars move   // 2.4.1: lights, dim cells
        if (!std::strcmp(key, "arp_tie") || !std::strcmp(key, "arp_gate")) {   // 2.4.1: the notes' pictures follow
            apply_settings(o);
            if (!o->arp_sel_on) arp_select(o, false);   // NOTE GATE shows the pattern's GATE with nothing selected
            arp_refresh_view(o);
            return;
        }
        if (!std::strcmp(key, "arp_sel")) {   // 2.4.2: the plugin's own (what is selected); MPC writing it back changes nothing
            o->values[ni].store(o->arp_sel_on ? 1.0f : 0.0f);
            return;
        }
        if (!std::strncmp(key, "arp_n_", 6)) {   // 2.4.2: the selected note's VEL, CHANCE, RATCHET, NOTE OCT, SLIDE
            if (o->arp_sel_on && arp_lit(o, o->arp_sel_r, o->arp_sel_s0)) {
                Arp &a = o->sampler.arp;
                const int i = o->arp_sel_s0 * 16 + o->arp_sel_r, v = int(std::lround(o->values[ni].load()));
                if (!std::strcmp(key + 6, "vel")) a.nvel[i].store(uint8_t(v == 100 ? 0 : std::max(1, std::min(200, v))));
                else if (!std::strcmp(key + 6, "chance")) a.nskip[i].store(uint8_t(100 - std::max(0, std::min(100, v))));
                else if (!std::strcmp(key + 6, "ratchet")) a.nratch[i].store(uint8_t(std::max(1, std::min(4, v))));
                else if (!std::strcmp(key + 6, "oct")) a.noct[i].store(int8_t(std::max(-2, std::min(2, v))));
                else if (!std::strcmp(key + 6, "slide")) a.nslide[i].store(uint8_t(v > 0 ? 1 : 0));
            }
            return;
        }
        if (!std::strcmp(key, "arp_note_gate")) {   // 2.4.1: the selected note's own gate
            if (o->arp_sel_on && arp_lit(o, o->arp_sel_r, o->arp_sel_s0)) {
                const int g = std::max(5, std::min(100, int(std::lround(o->values[ni].load()))));
                o->sampler.arp.ngate[o->arp_sel_s0 * 16 + o->arp_sel_r].store(uint8_t(g));
                arp_refresh_view(o);
            }
            return;
        }
        if (!std::strcmp(key, "lock_note")) { show_lock(o); return; }   // 1.8.2: NOTE LOCK shows that note's lock
        if (lock_key(key)) { take_lock(o); return; }                    // and a moved control locks it
        if (!std::strncmp(key, "env_", 4)) env_take(o);            // an envelope fader: how far from the preset's
        else if (!std::strcmp(key, "target_slot")) { apply_settings(o); env_show(o); }   // the faders show that slot's
        else apply_settings(o);
        // 1.8.11: Auto Loop switched: the loaded presets follow at once, not at the next load (after apply_settings: the
        // loader has the new setting)
        if (auto_loop_flip) o->loader.reload();
        return;
    }
    int ti = trigger_index(key);
    if (ti >= 0) o->trig_on[ti].store(is_trigger_on(val) ? 1 : 0);
    if (!std::strncmp(key, "pad_", 4) && std::isdigit(static_cast<unsigned char>(key[4]))) {
        int n = std::atoi(key + 4) - 1;
        if (n >= 0 && n < 16 && is_trigger_on(val)) o->pad_req[n].store(1);
        return;
    }
    if ((!std::strcmp(key, "pad_up") || !std::strcmp(key, "pad_down")) && is_trigger_on(val)) {
        int pb = num_index("pad_base");
        float b = o->values[pb].load() + (key[4] == 'u' ? 16.0f : -16.0f);
        o->values[pb].store(std::max(0.0f, std::min(112.0f, b)));
        o->pad_base_rev.fetch_add(1);
        return;
    }
    if (!std::strcmp(key, "br_scroll")) {
        // the scroll fader, top = the start of the list: its position maps straight onto the list (a drag, the data wheel,
        // a Q-Link). The fader keeps exactly the value MPC sent: snapping it to whole rows and reporting that back fought
        // the finger and made the drag jerky (1.7.0 on the device)
        float v = std::max(0.0f, std::min(1.0f, float(std::atof(val))));
        std::unique_lock<std::mutex> lock(o->ui_mutex, std::try_to_lock);   // 2.4.11: never stall audio behind the browser
        if (!lock.owns_lock()) return;
        int m = max_top(o), t = int(std::lround((1.0f - v) * float(m)));
        if (t != o->top) set_top(o, t);
        o->scroll_pos.store(v);
        o->ui_rev.fetch_add(1);
        return;
    }
    if (!std::strcmp(key, "br_wheel") || !std::strcmp(key, "br_enter") || !std::strcmp(key, "br_row")) {
        // 1.8.x diagnostics: with /sdcard/omni-debug present, each write of the browser's wheel, Enter and Q-Link scroll
        // is appended to /tmp/omni-wheel.log (what MPC actually sends; the file stops growing at 64 KB)
        struct stat st;
        if (stat("/sdcard/omni-debug", &st) == 0) {
            if (stat("/tmp/omni-wheel.log", &st) != 0 || st.st_size < 65536) {
                if (FILE *f = std::fopen("/tmp/omni-wheel.log", "a")) {
                    std::fprintf(f, "%lld %s=%s\n", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count(), key, val);
                    std::fclose(f);
                }
            }
        }
    }
    if (!std::strcmp(key, "br_row") || !std::strcmp(key, "br_wheel")) {
        // 1.8.x: a highlight in the browser list (the first step shows it on the top row shown); the list scrolls to keep
        // it in view. Enter (br_enter) opens a folder or loads the highlighted file / preset, as a tap does.
        // Q-Link 1 and the data wheel (MPC sends both here on BROWSE; logged on the device 2026-11-02). Each event is one
        // option either side of the middle ("-"): 2 = down, 0 = up; MPC mixes "-" (1) into a turn, which only keeps the
        // turn alive. One row per turn: events 200 ms or less apart are one turn and its first step moves one row; a turn
        // held past 300 ms scrolls on at 10 rows a second (long lists). A new direction is a new turn.
        // OMNI_WHEEL_MS=0 (tests): every 0 / 2 is one row
        int v = int(std::lround(std::atof(val)));
        int hint = v >= 2 ? 1 : v <= 0 ? -1 : 0;
        std::unique_lock<std::mutex> lock(o->ui_mutex, std::try_to_lock);   // 2.4.11: a turn while the browser is busy is dropped
        if (!lock.owns_lock()) return;
        const char *ms_env = std::getenv("OMNI_WHEEL_MS");
        bool grouped = !(ms_env && std::atoi(ms_env) == 0);
        auto now = std::chrono::steady_clock::now();
        bool new_turn = now - o->turn_last > std::chrono::milliseconds(200);
        o->turn_last = now;
        if (hint == 0) return;
        if (grouped && !new_turn && hint == o->turn_dir) {
            if (now - o->turn_start < std::chrono::milliseconds(300) || now - o->turn_step < std::chrono::milliseconds(100)) return;
        } else {
            o->turn_dir = hint;
            o->turn_start = now;
        }
        o->turn_step = now;
        int dir = hint;
        int n = int(o->rows.size());
        if (n == 0) return;
        if (o->cur < 0 || o->cur < o->top || o->cur >= o->top + ROWS) o->cur = o->top;
        else o->cur = std::max(0, std::min(n - 1, o->cur + dir));
        if (o->cur < o->top) set_top(o, o->cur);
        else if (o->cur >= o->top + ROWS) set_top(o, o->cur - ROWS + 1);
        else rebuild_on(o);
        // with PREVIEW ON, load as you scroll (the user's choice, 2026-11-02: MPC doesn't pass the wheel press to the plugin):
        // a preset row loads as soon as it is highlighted, as a tap would; with PREVIEW OFF it is only highlighted and a tap
        // loads it. Folders, disk images and bank files always open with a tap, so scrolling never jumps into another list.
        // The loader keeps only the newest request, so a fast spin loads where it stops
        if (num(o, "br_preview") > 0.5f && o->rows[size_t(o->cur)].kind == RowKind::Preset) tap_row(o, o->cur);
        o->ui_rev.fetch_add(1);
        return;
    }
    if (!std::strcmp(key, "br_extract")) { if (is_trigger_on(val)) o->loader.save(); return; }
    if (!std::strcmp(key, "panic")) { if (is_trigger_on(val)) { o->pad_panic.store(1); o->sampler.panic(); } return; }
    if (!std::strcmp(key, "patch_save")) { if (is_trigger_on(val)) o->loader.save_patch(settings_json(o, patch_param)); return; }
    if (!std::strcmp(key, "br_setlib")) {
        if (!is_trigger_on(val)) return;
        std::lock_guard<std::mutex> lock(o->br_qm);
        o->br_q.emplace_back("setlib", "1");
        o->br_busy.store(1);
        o->br_cv.notify_one();
        return;
    }
    if (!std::strcmp(key, "lock_clear")) {   // 1.8.2: the shown note's lock off
        if (!is_trigger_on(val)) return;
        for (const char *k : LOCK_KEYS) o->values[num_index(k)].store(0);
        take_lock(o);
        return;
    }
    if (!std::strcmp(key, "slot_clear")) { if (is_trigger_on(val)) o->loader.request("", 0); return; }
    if (!std::strncmp(key, "mut_", 4) && std::strcmp(key, "mut_amount")) {   // 2.4.12 MUTATE / BREED (the synth)
        if (is_trigger_on(val) && (!std::strcmp(key + 4, "go") || !std::strcmp(key + 4, "store_a") || !std::strcmp(key + 4, "store_b") ||
                                   !std::strcmp(key + 4, "breed") || !std::strcmp(key + 4, "undo")))
            mut_action(o, key + 4);
        return;
    }
    if (!std::strcmp(key, "arp_b_store_a") || !std::strcmp(key, "arp_b_store_b")) {   // 2.4.12 the GRID's parents
        if (is_trigger_on(val)) {
            const bool b = key[12] == 'b';
            arp_snapshot(o, b ? o->brd_b : o->brd_a);
            arp_gen_msg(o, std::string("BREED: this pattern stored as ") + (b ? "B" : "A"));
        }
        return;
    }
    if (!std::strcmp(key, "arp_b_breed")) { if (is_trigger_on(val)) arp_gen(o, "breed"); return; }
    if (!std::strcmp(key, "auto_split")) { if (is_trigger_on(val)) auto_split(o); return; }
    if (key[0] == 's' && (key[1] == 'a' || key[1] == 'b') && key[2] == '_') {   // 2.3: < > LOAD on an OSC screen
        int k = key[1] - 'a';
        if (!std::strcmp(key + 3, "prev")) { if (is_trigger_on(val)) syn_browse(o, k, -1); return; }
        if (!std::strcmp(key + 3, "next")) { if (is_trigger_on(val)) syn_browse(o, k, 1); return; }
        if (!std::strcmp(key + 3, "load")) { if (is_trigger_on(val)) syn_browse(o, k, 0); return; }
        if (!std::strcmp(key + 3, "folder")) { if (is_trigger_on(val)) syn_next_folder(o, k); return; }   // 2.3.2
        if (!std::strcmp(key + 3, "fprev")) { if (is_trigger_on(val)) syn_prev_folder(o, k); return; }    // 2.4.9
        if (!std::strcmp(key + 3, "rnd")) { if (is_trigger_on(val)) syn_random(o, k, false); return; }
        if (!std::strcmp(key + 3, "rndall")) { if (is_trigger_on(val)) syn_random(o, k, true); return; }
    }
    if (!std::strncmp(key, "syn_lfo", 7) && key[7] >= '1' && key[7] <= '4' &&   // 2.4.17: 1-4
        (!std::strcmp(key + 8, "_file_prev") || !std::strcmp(key + 8, "_file_next"))) {   // 2.4.13: a CUSTOM LFO's < >
        if (is_trigger_on(val)) {
            { std::lock_guard<std::mutex> lock(o->syn_mutex); o->syn_lfo[key[7] - '1'].step = key[14] == 'n' ? 1 : -1; }
            syn_request_bits(o, SYN_REQ_LFO_STEP);
        }
        return;
    }
    {   // 2.3.3: a stepper's arrows: "<key>_prev" / "<key>_next" step that option setting, wrapping round
        // 2.4.32: FX BANK program keypad. fxb_num_tap opens / closes it (clearing the entry); fxb_k0..9 type up to three
        // digits; fxb_kclr clears; fxb_kenter selects that program (0 = Off .. the last) and closes, or shows ERR
        if (!std::strncmp(key, "fxb_k", 5) || !std::strcmp(key, "fxb_num_tap")) {
            if (!is_trigger_on(val)) return;
            // 2.4.33: the program of the bay's slot (A: fxb_prog over the factory programs; B..D: their own, up to 256)
            const int slot = std::max(0, std::min(3, int(num(o, "fxb_slot") + 0.5f)));
            const int pad = num_index("fxb_pad"), prog = num_index(slot ? FXB_XPROG[slot - 1] : "fxb_prog");
            const int last = slot ? o->sampler.fx_expansion_count(slot) : fxb_catalog_count();
            if (pad < 0 || prog < 0) return;
            auto set_pad = [&](float v) { o->values[pad].store(v); mark_changed(o, pad); };
            int e = o->fxb_entry.load();
            const int count = e < 0 ? 0 : e / 1000, value = e < 0 ? 0 : e % 1000;
            if (!std::strcmp(key, "fxb_num_tap")) {
                o->fxb_entry.store(0);
                set_pad(o->values[pad].load() > 0.5f ? 0.0f : 1.0f);
            } else if (!std::strcmp(key, "fxb_kclr")) {
                o->fxb_entry.store(0);
            } else if (!std::strcmp(key, "fxb_kenter")) {
                if (count == 0) { set_pad(0.0f); return; }   // nothing typed: just close
                if (value > last) { o->fxb_entry.store(-1); return; }   // past the last program (an empty slot: past 0): ERR
                o->values[prog].store(float(value));
                mark_changed(o, prog);
                apply_settings(o);
                o->fxb_entry.store(0);
                set_pad(0.0f);
            } else if (key[5] >= '0' && key[5] <= '9' && !key[6]) {
                if (o->values[pad].load() < 0.5f) set_pad(1.0f);   // a digit (Q-Link / automation) opens it too
                const int d = key[5] - '0';
                o->fxb_entry.store(count >= 3 ? 1000 + d : (count + 1) * 1000 + value * 10 + d);   // a 4th digit starts over
            }
            return;
        }
        if (!std::strcmp(key, "fxb_prog_prev") || !std::strcmp(key, "fxb_prog_next")) {   // 2.4.30: FX BANK < >, wrapping
            // 2.4.33: over the bay's slot's programs (an empty slot: Off only)
            const int slot = std::max(0, std::min(3, int(num(o, "fxb_slot") + 0.5f)));
            const int i = num_index(slot ? FXB_XPROG[slot - 1] : "fxb_prog");
            if (is_trigger_on(val) && i >= 0) {
                const int n = (slot ? o->sampler.fx_expansion_count(slot) : fxb_catalog_count()) + 1;   // Off + the programs
                o->values[i].store(float((int(o->values[i].load() + 0.5f) + (key[9] == 'n' ? 1 : -1) + n) % n));
                mark_changed(o, i);
                apply_settings(o);
            }
            return;
        }
        if (!std::strcmp(key, "syn_preset_prev") || !std::strcmp(key, "syn_preset_next")) {   // 2.3.5: step and load
            if (is_trigger_on(val))
                syn_load_preset(o, (int(num(o, "syn_preset") + 0.5f) + (key[11] == 'n' ? 1 : -1) + SYN_PRESET_COUNT) % SYN_PRESET_COUNT);
            return;
        }
        static const struct { const char *prefix, *suffix; int n; } STEP[] = {
            {"syn_mx", "_src", 19}, {"syn_mx", "_dst", 30}, {"syn_lfo", "_dest", 12}, {"syn_lfo", "_sync", 15}, {"syn_me", "_dest", 30}};   // 2.4.35: + Level A / B   // 2.4.17: 19 sources, 28 destinations   // 2.4.11: 26 = design.py SYN_ALL_DESTS (were 19 / 12: the 2.4.9 destinations unreachable by the arrows)
        size_t kl = std::strlen(key);
        int dir = kl > 5 && !std::strcmp(key + kl - 5, "_prev") ? -1 : kl > 5 && !std::strcmp(key + kl - 5, "_next") ? 1 : 0;
        if (dir) {
            std::string base(key, kl - 5);
            for (auto &st : STEP) {
                if (base.compare(0, std::strlen(st.prefix), st.prefix) || base.size() < std::strlen(st.suffix) ||
                    base.compare(base.size() - std::strlen(st.suffix), std::string::npos, st.suffix)) continue;
                int i = num_index(base.c_str());
                if (i < 0) break;
                if (is_trigger_on(val)) {
                    int v = (int(o->values[i].load() + 0.5f) + dir + st.n) % st.n;
                    o->values[i].store(float(v));
                    mark_changed(o, i);
                    apply_settings(o);
                }
                return;
            }
        }
    }
    if (!std::strcmp(key, "reset_all")) {   // 2.1.16: two taps (RESET_ARM_MS apart at most)
        if (!is_trigger_on(val)) return;
        long long t = now_ms(), armed = o->reset_armed_ms.load();
        if (armed && t - armed <= RESET_ARM_MS) { o->reset_armed_ms.store(0); reset_all(o); }
        else { o->reset_armed_ms.store(t); o->reset_done_ms.store(0); }
        return;
    }
    bool prog = !std::strcmp(key, "prog_prev") || !std::strcmp(key, "prog_next");
    if (!prog && std::strncmp(key, "br_", 3)) return;
    const char *k = prog ? key : key + 3;   // the worker's action: "prog_prev"/"prog_next", or the br_ suffix
    if (!std::strcmp(k, "path")) {   // tests, state: synchronous
        std::lock_guard<std::mutex> lock(o->ui_mutex);
        if (*val) list_folder(o, val); else list_drives(o);
        return;
    }
    bool row = std::isdigit(static_cast<unsigned char>(*k)) && !std::strchr(k, '_');
    if (!row && !is_trigger_on(val)) return;
    {
        std::lock_guard<std::mutex> lock(o->br_qm);
        if (!browser_queue_push(o->br_q, k, val, 32, o->br_queue_dropped)) {
            o->ui_rev.fetch_add(1);
            return;
        }
        o->br_busy.store(1);
    }
    o->ui_rev.fetch_add(1);
    o->br_cv.notify_one();
}

// one browser action (the worker thread, ui_mutex held)
void browse_action(Omni *o, const std::string &key) {
    const char *k = key.c_str();
    if (!std::strcmp(k, "prog_prev") || !std::strcmp(k, "prog_next")) {   // may list the folder for the next file
        step_program(o, k[5] == 'p' ? -1 : 1);
        return;
    }
    {
        if (std::isdigit(static_cast<unsigned char>(*k))) {
            tap_row(o, o->top + std::atoi(k) - 1);
            return;
        }
        if (!std::strcmp(k, "enter")) {
            if (o->cur >= 0 && o->cur < int(o->rows.size())) tap_row(o, o->cur);
            else if (!o->rows.empty()) { o->cur = o->top; rebuild_on(o); }
            return;
        }
        // 1.8.x (the user, 2026-11-02): the big blue buttons beside the list page by 10, the small arrows under it by one row
        if (!std::strcmp(k, "prev")) set_top(o, o->top - 1);
        else if (!std::strcmp(k, "next")) set_top(o, o->top + 1);
        else if (!std::strcmp(k, "rowup")) set_top(o, o->top - ROWS);
        else if (!std::strcmp(k, "rowdown")) set_top(o, o->top + ROWS);
        else if (!std::strcmp(k, "up")) { if (!o->rows.empty() && o->rows[0].kind == RowKind::Up) tap_row(o, 0); else list_drives(o); }
        else if (!std::strcmp(k, "drives")) list_drives(o);
        else if (!std::strcmp(k, "library")) {
            std::string lib = library_dir(o);
            if (!lib.empty()) list_folder(o, lib);
            else {
                std::string c = library_choice(o);
                list_drives(o);
                o->browse_msg = c.empty() ? "No Plugin Library yet: open a folder on an SD card or SSD, then SET LIBRARY HERE"
                                          : "Plugin Library drive not connected: " + c;
            }
        }
        else if (!std::strcmp(k, "setlib")) {
            // the folder shown becomes the Plugin Library: a real folder on an SD card / USB drive / SSD, never internal
            std::string dir = o->preset_file.empty() ? o->dir : "";
            bool ok = false;
            if (!dir.empty()) try { ok = o->vfs.resolve(dir).volume->kind() == "Folder" && host_dir(dir); } catch (const std::exception &) {}
            if (!ok) o->browse_msg = "Open a folder on an SD card or SSD (not inside a disk image), then SET LIBRARY HERE";
            else if (!external_path(dir)) o->browse_msg = "The Plugin Library must be on an SD card or SSD, not the internal storage";
            else {
                {
                    std::lock_guard<std::mutex> lock(o->lib_mutex);
                    o->lib_choice = dir;
                }
                set_search_dirs({dir});
                // remembered for every project and kept by reinstalls (<data_dir> is the installer's user data);
                // written whole then renamed, so a power cut can't leave it empty
                std::string lp = path_join(o->data_dir, "library_path.txt");
                FILE *f = std::fopen((lp + ".new").c_str(), "w");
                if (f) {
                    bool w = std::fputs(dir.c_str(), f) >= 0;
                    w = (std::fflush(f) == 0) && w;
                    fsync(fileno(f));
                    if (std::fclose(f) == 0 && w) std::rename((lp + ".new").c_str(), lp.c_str());
                }
                apply_settings(o);
                o->browse_msg = "Plugin Library: " + short_path(o, dir);
            }
        }
        else if (!std::strcmp(k, "refresh")) { o->vfs.forget(); o->xpm_kinds.clear(); o->heat_folders.clear(); if (o->dir.empty()) list_drives(o); else list_folder(o, o->dir, o->group); }
    }
}

void browse_worker(Omni *o) {
    for (;;) {
        std::pair<std::string, std::string> a;
        {
            std::unique_lock<std::mutex> lock(o->br_qm);
            o->br_cv.wait(lock, [o] { return o->br_quit || !o->br_q.empty(); });
            if (o->br_quit) return;
            a = std::move(o->br_q.front());
            o->br_q.pop_front();
        }
        {
            std::lock_guard<std::mutex> lock(o->ui_mutex);
            try { browse_action(o, a.first); }
            catch (const std::exception &e) { o->browse_msg = e.what(); }
        }
        {
            std::lock_guard<std::mutex> lock(o->br_qm);
            if (o->br_q.empty()) o->br_busy.store(0);
        }
        o->ui_rev.fetch_add(1);
    }
}

int put(char *buf, int len, const std::string &s) {
    if (len <= 0) return 0;
    std::snprintf(buf, size_t(len), "%s", s.empty() ? " " : s.c_str());
    return int(std::strlen(buf)) > 0 ? int(std::strlen(buf)) : 1;
}

std::string mb(int64_t bytes) {
    char b[32];
    std::snprintf(b, sizeof b, bytes >= (100 << 20) ? "%.0f MB" : "%.1f MB", double(bytes) / 1048576.0);
    return b;
}

// MPC's octave numbering: note 60 = C3
std::string note_name(int n) {
    static const char *names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    n = std::max(0, std::min(127, n));
    return std::string(names[n % 12]) + std::to_string(n / 12 - 2);
}

// 2.1.5: the logo spark's frame from the clock: 1..127 over the first 2.56 s of every 10 s, else 0 (blank)
int logo_frame_now() {
    using namespace std::chrono;
    long long ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() % 10000;
    // 2.2.3: about 15 frames a second (a step every 67 ms, 39 steps over the 2.56 s sweep): it used to move every frame,
    // ~43 a second, and each move is a repaint on MPC's screen thread (open-mpc NOTES "Animation is expensive")
    if (ms >= 2560) return 0;
    const int step = int(ms / 67), steps = 2560 / 67;   // 0..38
    return 1 + step * 126 / steps;
}

// the screen texts that read the browser state (ui_mutex held by the caller)
int get_param_ui(Omni *o, const char *key, char *buf, int len) {
    if (!std::strncmp(key, "syn_lfo", 7) && key[7] >= '1' && key[7] <= '4' && !std::strcmp(key + 8, "_file")) {   // 2.4.13 (2.4.17: 1-4)
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        const Omni::SynLfoShape &L = o->syn_lfo[key[7] - '1'];
        if (!L.err.empty()) return put(buf, len, L.err);
        if (!L.name.empty()) return put(buf, len, L.name);
        return put(buf, len, library_dir(o).empty() ? "Set a Plugin Library" : "Vital LFOs: <Library>/LFOs");
    }
    if (!std::strncmp(key, "br_", 3)) {
        const char *k = key + 3;
        if (std::isdigit(static_cast<unsigned char>(*k))) {
            size_t i = size_t(o->top + std::atoi(k) - 1);
            if (i < o->rows.size() && int(i) == o->cur) return put(buf, len, fit_row("> " + o->rows[i].label));
            return put(buf, len, i < o->rows.size() ? fit_row(o->rows[i].label) : " ");
        }
        if (!std::strcmp(k, "loc")) {
            if (!o->preset_file.empty()) return put(buf, len, fit_loc(short_path(o, o->preset_file)));
            return put(buf, len, o->dir.empty() ? "Drives" : fit_loc(short_path(o, o->group.empty() ? o->dir : path_join(o->dir, o->group))));
        }
        if (!std::strcmp(k, "page")) {   // "11-20 of 37"
            int n = int(o->rows.size());
            if (n <= ROWS) return put(buf, len, std::to_string(n) + (n == 1 ? " item" : " items"));
            return put(buf, len, std::to_string(o->top + 1) + "-" + std::to_string(std::min(n, o->top + ROWS)) + " of " + std::to_string(n));
        }
        if (!std::strcmp(k, "info")) return put(buf, len, browse_info(o));
        if (!std::strncmp(k, "sel", 3) && k[3] >= '1' && k[3] <= '3' && !k[4]) {   // the SELECTED box, line 1-3
            std::vector<std::string> l = wrap_lines(browse_info(o), SEL_CHARS, 3);
            size_t first = l.size() == 1 ? 1 : 0;   // one line sits in the middle
            size_t i = size_t(k[3] - '1');
            return put(buf, len, i >= first && i - first < l.size() ? l[i - first] : std::string(" "));
        }
        return std::snprintf(buf, size_t(len), "0");
    }
    return 0;
}

int get_param(void *inst, const char *key, char *buf, int len) {
    Omni *o = static_cast<Omni *>(inst);
    // audio-thread safe lookups first: list highlights and the status bit
    if (!std::strncmp(key, "br_", 3) && std::isdigit(static_cast<unsigned char>(key[3]))) {
        const char *us = std::strchr(key + 3, '_');
        if (us && !std::strcmp(us, "_on")) {
            int r = std::atoi(key + 3) - 1;
            return std::snprintf(buf, size_t(len), "%d", r >= 0 && r < ROWS ? o->row_on[r].load() : 0);
        }
    }
    if (!std::strcmp(key, "pad_info_on")) return std::snprintf(buf, size_t(len), "%d", o->pad_base_rev.load() & 1);
    { int lfo, route; if (lfo_route_key(key, lfo, route)) return std::snprintf(buf, size_t(len), "%d", lfo_route_now(o, lfo) == route); }
    if (!std::strncmp(key, "arp_c", 5) && std::isdigit((unsigned char)key[5])) {   // 2.4: a grid dot as shown
        int r = std::atoi(key + 5) - 1;
        const char *us = std::strchr(key + 5, '_');
        int c = us ? std::atoi(us + 1) - 1 : -1;
        return std::snprintf(buf, size_t(len), "%d", r >= 0 && r < 16 && c >= 0 && c < 16 ? o->arp_view[r * 16 + c].load() : 0);
    }
    if (!std::strncmp(key, "arp_led", 7)) return std::snprintf(buf, size_t(len), "%d", arp_led_value(o, std::max(0, std::min(15, std::atoi(key + 7) - 1))));
    if (!std::strcmp(key, "arp_status")) return put(buf, len, arp_status(o));   // 2.4.1
    if (!std::strcmp(key, "arp_chord")) return put(buf, len, arp_chord_text(o));   // 2.4.14
    if (!std::strcmp(key, "arp_xy_x") || !std::strcmp(key, "arp_xy_y")) return std::snprintf(buf, size_t(len), "%g", o->arp_xy[key[7] == 'y']);
    if (!std::strncmp(key, "arp_row", 7)) return put(buf, len, arp_row_name(o, std::max(0, std::min(15, std::atoi(key + 7) - 1))));
    if (!std::strcmp(key, "arp_name")) {
        std::lock_guard<std::mutex> lock(o->syn_mutex);
        if (o->arp_sel < 0 || o->arp_sel >= int(o->arp_files.size())) return put(buf, len, library_dir(o).empty() ? "Set a Plugin Library first" : "No MIDI files in <Library>/MIDI");
        std::string f = o->arp_files[size_t(o->arp_sel)];
        f = f.substr(f.find_last_of('/') == std::string::npos ? 0 : f.find_last_of('/') + 1);
        return put(buf, len, f.substr(0, f.find_last_of('.')));
    }
    if (!std::strcmp(key, "arp_info")) {
        std::string m;
        { std::lock_guard<std::mutex> lock(o->syn_mutex); m = o->arp_msg; }
        if (!m.empty()) return put(buf, len, m);
        return put(buf, len, arp_status(o));   // 2.4.1
    }
    if (!std::strncmp(key, "pad_", 4) && std::isdigit(static_cast<unsigned char>(key[4]))) {   // lit while its key is down
        int n = std::atoi(key + 4) - 1, note = int(num(o, "pad_base")) + n;
        return std::snprintf(buf, size_t(len), "%d", n >= 0 && n < 16 && note < 128 && o->sampler.key_down[note].load() ? 1 : 0);
    }
    if (!std::strncmp(key, "layer_", 6) && std::strstr(key, "_on"))
        return std::snprintf(buf, size_t(len), "%d", int((o->sampler.layer_events.load() + o->loader.revision.load()) & 1));
    if (!std::strncmp(key, "slot", 4) && std::strstr(key, "_name_on"))
        return std::snprintf(buf, size_t(len), "%d", int(o->loader.revision.load() & 1));
    if (!std::strcmp(key, "status_on") || !std::strcmp(key, "prog_name_on") || !std::strcmp(key, "prog_info_on") ||
        !std::strcmp(key, "prog_format_on"))
        return std::snprintf(buf, size_t(len), "%d", int((o->loader.revision.load() + o->ui_rev.load()) & 1));
    if (!std::strcmp(key, "lib_path_on")) return std::snprintf(buf, size_t(len), "%d", int(o->ui_rev.load() & 1));
    if (!std::strcmp(key, "br_row") || !std::strcmp(key, "br_wheel")) return std::snprintf(buf, size_t(len), "1");   // the middle again
    // 1.8.13 rack BROWSE's LEDs (read-only, polled live): loading, and a note sounding
    if (!std::strcmp(key, "led_busy")) return std::snprintf(buf, size_t(len), "%d", o->br_busy.load() ? 1 : 0);
    if (key[0] == 's' && (key[1] == 'a' || key[1] == 'b') && key[2] == '_') {   // 2.3: the OSC screens
        int k = key[1] - 'a';
        if (key[3] == 'w' && isdigit((unsigned char)key[4])) {
            int b = std::atoi(key + 4) - 1;
            return std::snprintf(buf, size_t(len), "%d", b >= 0 && b < SYN_BARS ? o->syn_bar[k][b].load() : 64);
        }
        if (!std::strcmp(key + 3, "pos_display")) {   // 2.4.9: POSITION's value: the frame on a wavetable, else %
            const float pos = syn_num(o, k, "pos");
            const int type = int(syn_num(o, k, "type") + 0.5f);
            const Wavetable *t = o->sampler.synth.settings.osc[k].table.load(std::memory_order_acquire);
            if (type == Synth::OT_WAVETABLE && t && t->frames > 1)
                return std::snprintf(buf, size_t(len), "%d/%d", 1 + int(std::lround(pos / 100.0f * float(t->frames - 1))), t->frames);
            return std::snprintf(buf, size_t(len), "%d%%", int(std::lround(pos)));
        }
        if (!std::strcmp(key + 3, "name") || !std::strcmp(key + 3, "info")) {
            int type = int(syn_num(o, k, "type") + 0.5f);
            std::lock_guard<std::mutex> lock(o->syn_mutex);
            const Omni::SynOsc &so = o->syn[k];
            if (key[3] == 'n') {
                return put(buf, len, so.loading ? "Loading..." : so.name);
            }
            if (!so.err.empty()) return put(buf, len, "Can't load: " + so.err);
            char fd[32] = "";   // 2.4.9: which folder (more than one: the < FOLDER / FOLDER buttons step them)
            if (so.fold_n > 1) std::snprintf(fd, sizeof fd, "FOLDER %d/%d  \xC2\xB7  ", so.fold_i, so.fold_n);
            if (type == Synth::OT_MACRO)   // 2.3.5
                return put(buf, len, "HARMONICS = POSITION  \xC2\xB7  TIMBRE = WARP  \xC2\xB7  MORPH = SPREAD");
            if (type == Synth::OT_SAMPLE) {   // 2.3.4
                const SynthOscSettings &os = o->sampler.synth.settings.osc[k];
                if (os.sample_slot.load() >= 0)
                    return put(buf, len, std::string("PLAYS SLOT ") + char('A' + os.sample_slot.load()) + "'S INSTRUMENT  \xC2\xB7  POSITION = START");
                if (so.smp && so.smp->pcm) {
                    char tb[96];
                    std::snprintf(tb, sizeof tb, "%s%.2f s  \xC2\xB7  ROOT %s%s%s", fd,
                                  double(so.smp->pcm->frames()) / so.smp->pcm->rate, note_name(so.smp->root).c_str(),
                                  so.smp->loop_end > so.smp->loop_start ? "  \xC2\xB7  LOOP" : "",
                                  *fd ? "" : "  \xC2\xB7  POSITION = START");   // 2.4.9: (no room beside the folder)
                    return put(buf, len, tb);
                }
                return put(buf, len, "");
            }
            const Wavetable *t = o->sampler.synth.settings.osc[k].table.load();
            if (type >= Synth::OT_SAMPLE || !t) return put(buf, len, "");
            if (t->frames <= 1) return put(buf, len, std::string(fd) + "SINGLE CYCLE  \xC2\xB7  2048");
            // 2.4.9: no frame number here: a readout's text change makes the wrapper ask MPC for an UpdateDisplay, and MPC
            // rebuilds the whole screen on each one, so turning POSITION blanked the screen (the user, 2.4.8). The frame
            // is the POSITION knob's own value text now (s?_pos_display), which MPC polls by itself
            char tb[96];
            std::snprintf(tb, sizeof tb, "%s%d FRAMES  \xC2\xB7  2048 \xC3\x97 %d", fd, t->frames, t->frames);
            return put(buf, len, tb);
        }
    }
    if (!std::strncmp(key, "syn_lfo", 7) && isdigit((unsigned char)key[7]) && !std::strcmp(key + 8, "_rate_display")) {   // 2.3.2
        char k2[32];
        std::snprintf(k2, sizeof k2, "syn_lfo%c_sync", key[7]);
        int sy = int(num(o, k2) + 0.5f);
        if (sy > 0 && sy < int(sizeof SYN_LFO_SYNC / sizeof SYN_LFO_SYNC[0])) return put(buf, len, SYN_LFO_SYNC[sy]);
        std::snprintf(k2, sizeof k2, "syn_lfo%c_rate", key[7]);
        float hz = syn_lfo_hz(num(o, k2));
        return std::snprintf(buf, size_t(len), hz < 10 ? "%.2f Hz" : "%.1f Hz", hz);
    }
    if (!std::strncmp(key, "syn_", 4) && key[4] && std::strchr("afmno", key[4]) && key[5] == 'e' && key[6] == '_' && key[7] == 'b') {   // 2.3.2: env bars (2.4.17: n, o = MOD ENV 2 / 3)
        int e = int(std::strchr("afmno", key[4]) - "afmno"), b = std::atoi(key + 8) - 1;
        return std::snprintf(buf, size_t(len), "%d", b >= 0 && b < ENV_BARS ? o->env_bar[e][b].load() : 0);
    }
    if (!std::strcmp(key, "syn_key_lo_display") || !std::strcmp(key, "syn_key_hi_display"))   // 2.3.3: note names
        return put(buf, len, note_name(int(num(o, key[8] == 'l' ? "syn_key_lo" : "syn_key_hi") + 0.5f)));
    if (!std::strcmp(key, "syn_preset_display"))   // 2.3.5
        return put(buf, len, SYN_PRESETS[std::max(0, std::min(SYN_PRESET_COUNT - 1, int(num(o, "syn_preset") + 0.5f)))].name);
    if (!std::strcmp(key, "syn_cutoff_display")) {   // 2.3.1
        float hz = syn_cutoff_hz(num(o, "syn_cutoff"));
        return hz < 1000 ? std::snprintf(buf, size_t(len), "%.0f Hz", hz) : std::snprintf(buf, size_t(len), "%.1f kHz", hz / 1000);
    }
    if (!std::strcmp(key, "gr_meter")) return std::snprintf(buf, size_t(len), "%d", gr_meter_frame(o));   // 2.2
    if (!std::strcmp(key, "reset_info")) {   // 2.1.16
        long long t = now_ms(), armed = o->reset_armed_ms.load(), done = o->reset_done_ms.load();
        if (armed && t - armed <= RESET_ARM_MS) return put(buf, len, "Tap RESET again to clear everything");
        if (done && t - done <= RESET_ARM_MS) return put(buf, len, "Omni Sampler reset");
        return put(buf, len, "");
    }
    if (!std::strcmp(key, "logo_fx")) return std::snprintf(buf, size_t(len), "%d", logo_frame_now());
    if (!std::strcmp(key, "led_notes")) return std::snprintf(buf, size_t(len), "%d", o->sampler.active_voices.load() > 0 ? 1 : 0);
    if (!std::strcmp(key, "br_scroll")) return std::snprintf(buf, size_t(len), "%.4f", o->scroll_pos.load());
    if (!std::strcmp(key, "br_info_on") || !std::strcmp(key, "br_sel1_on") || !std::strcmp(key, "br_sel2_on") ||
        !std::strcmp(key, "br_sel3_on"))   // also shows the loader's errors: follows its revision too
        return std::snprintf(buf, size_t(len), "%d", int((o->ui_rev.load() + o->loader.revision.load()) & 1));
    if (!std::strcmp(key, "br_loc_on") || !std::strcmp(key, "br_page_on"))
        return std::snprintf(buf, size_t(len), "%d", int(o->ui_rev.load() & 1));
    if (!std::strncmp(key, "wave_", 5) && isdigit((unsigned char)key[5])) {
        int i = std::atoi(key + 5) - 1;
        return std::snprintf(buf, size_t(len), "%d", i >= 0 && i < 48 ? o->disp_wave[i].load() : 0);
    }
    if ((!std::strncmp(key, "ptile_", 6) || !std::strncmp(key, "ltile_", 6)) && isdigit((unsigned char)key[6])) {
        int i = std::atoi(key + 6) - 1;
        if (i < 0 || i >= 32) return std::snprintf(buf, size_t(len), "0");
        return std::snprintf(buf, size_t(len), "%d", key[0] == 'p' ? o->disp_ptile[i].load() : o->disp_ltile[i].load());
    }
    if (std::strstr(key, "_display") && (!std::strncmp(key, "slot", 4) || !std::strncmp(key, "ks_base", 7))) {
        std::string k(key, std::strstr(key, "_display") - key);
        int ni = num_index(k.c_str());
        if (ni >= 0 && (k.find("_lo") != std::string::npos || k.find("_hi") != std::string::npos || k == "ks_base"))
            return put(buf, len, note_name(int(o->values[ni].load() + 0.5f)));
        if (ni >= 0 && k.find("_vol") != std::string::npos) return std::snprintf(buf, size_t(len), "%+.0f dB", o->values[ni].load());
        if (ni >= 0 && k.find("_tune") != std::string::npos) return std::snprintf(buf, size_t(len), "%+.0f st", o->values[ni].load());
        if (ni >= 0 && k.find("_shift") != std::string::npos) return std::snprintf(buf, size_t(len), "%+.0f keys", o->values[ni].load());
        if (ni >= 0 && k.find("_pan") != std::string::npos) {
            int p = int(std::lround(o->values[ni].load()));
            return p ? std::snprintf(buf, size_t(len), "%c%d", p < 0 ? 'L' : 'R', std::abs(p)) : put(buf, len, "C");
        }
    }
    if (!std::strcmp(key, "cho_rate_display")) return std::snprintf(buf, size_t(len), "%.2f Hz", chorus_hz(num(o, "cho_rate")));
    if (!std::strcmp(key, "fxb_num")) {   // 2.4.32: the 3-digit screen: the typed digits (right-aligned), else the program
        const int e = o->fxb_entry.load();
        if (e < 0) return put(buf, len, "ERR");
        if (e >= 1000 && num(o, "fxb_pad") > 0.5f) {
            const int count = e / 1000, value = e % 1000;
            char s[4] = {'_', '_', '_', 0};
            for (int k = 0, v = value; k < count; k++, v /= 10) s[2 - k] = char('0' + v % 10);
            return put(buf, len, s);
        }
        const int slot = std::max(0, std::min(3, int(num(o, "fxb_slot") + 0.5f)));   // 2.4.33: the slot's program
        return std::snprintf(buf, size_t(len), "%03d", int(num(o, slot ? FXB_XPROG[slot - 1] : "fxb_prog") + 0.5f));
    }
    if (!std::strcmp(key, "fxb_pname")) {   // 2.4.33: the OLED's top line: the slot's program
        const int slot = std::max(0, std::min(3, int(num(o, "fxb_slot") + 0.5f)));
        if (!slot) return put(buf, len, fxb_label(int(num(o, "fxb_prog") + 0.5f)));
        if (o->sampler.fx_expansion_count(slot) <= 0) return put(buf, len, "EMPTY SLOT");
        const int p = int(num(o, FXB_XPROG[slot - 1]) + 0.5f);
        return put(buf, len, p > 0 ? o->sampler.fx_expansion_name(slot, p) : "Off");
    }
    if (!std::strcmp(key, "fxb_info")) {   // 2.4.30: the OLED's second line: the card, the mode, ON / BYPASS
        const int slot = std::max(0, std::min(3, int(num(o, "fxb_slot") + 0.5f)));   // 2.4.33: B..D: the expansion's
        if (slot) {
            const char sl = char('A' + slot);
            const int n = o->sampler.fx_expansion_count(slot);
            if (n <= 0) return std::snprintf(buf, size_t(len), "SLOT %c  -  NO EXPANSION FITTED", sl);
            const int p = int(num(o, FXB_XPROG[slot - 1]) + 0.5f);
            if (p <= 0) return std::snprintf(buf, size_t(len), "SLOT %c  %s  -  OFF", sl, o->sampler.fx_expansion_title(slot));
            return std::snprintf(buf, size_t(len), "SLOT %c  %03d / %03d  %s", sl, p, n, num(o, "fxb_bypass") > 0.5f ? "BYPASS" : "ON");
        }
        const int opt = int(num(o, "fxb_prog") + 0.5f), cat = fxb_catalog(opt);
        if (cat <= 0) return put(buf, len, "NO CARD  -  FX BANK OFF");
        std::size_t n = 0;
        const omnisampler::fxbank::Program *list = omnisampler::fxbank::programs(n);
        const std::string bank = list[cat].bank;
        const char *card = bank == "700/reverb" ? "REV" : bank == "700/chorus" ? "MOD" : bank == "700/variation" ? "VAR" : "MULTI";
        return std::snprintf(buf, size_t(len), "%s CARD  MODE %03d / %03d  %s", card, opt, fxb_catalog_count(),
                             num(o, "fxb_bypass") > 0.5f ? "BYPASS" : "ON");
    }
    if (!std::strcmp(key, "fxb_rate_display")) {   // 2.4.30
        float hz = fxb_hz(num(o, "fxb_rate"));
        return std::snprintf(buf, size_t(len), hz < 1 ? "%.2f Hz" : "%.1f Hz", hz);
    }
    if (!std::strcmp(key, "phs_rate_display")) return std::snprintf(buf, size_t(len), "%.2f Hz", chorus_hz(num(o, "phs_rate")));
    if (!std::strcmp(key, "flg_rate_display")) return std::snprintf(buf, size_t(len), "%.2f Hz", chorus_hz(num(o, "flg_rate")));
    if (!std::strcmp(key, "lofi_rate_display")) return std::snprintf(buf, size_t(len), "%.1f kHz", lofi_hz(num(o, "lofi_rate")) / 1000);
    if (!std::strcmp(key, "trm_rate_display")) {
        static const char *const DIV[] = {"", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T", "1/4.", "1/8.", "1/16."};
        int sync = int(num(o, "trm_sync") + 0.5f);
        if (sync > 0 && sync < 15) return put(buf, len, DIV[sync]);
        float hz = trem_hz(num(o, "trm_rate"));
        return std::snprintf(buf, size_t(len), hz < 1 ? "%.2f Hz" : "%.1f Hz", hz);
    }
    if (!std::strcmp(key, "cutoff_display")) {
        float hz = 20.0f * std::pow(1000.0f, num(o, "cutoff") / 100.0f);
        return hz >= 1000 ? std::snprintf(buf, size_t(len), "%.1f kHz", hz / 1000) : std::snprintf(buf, size_t(len), "%.0f Hz", hz);
    }
    if (!std::strcmp(key, "lfo1_rate_display") || !std::strcmp(key, "lfo2_rate_display")) {
        static const char *const DIV[] = {"", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T", "1/4.", "1/8.", "1/16."};
        int sync = int(num(o, key[3] == '1' ? "lfo1_sync" : "lfo2_sync") + 0.5f);
        if (sync > 0 && sync < 15) return put(buf, len, DIV[sync]);
        float hz = lfo_hz(num(o, key[3] == '1' ? "lfo1_rate" : "lfo2_rate"));
        return std::snprintf(buf, size_t(len), hz < 1 ? "%.2f Hz" : "%.1f Hz", hz);
    }
    for (int k = 0; k < 4; k++) {   // the envelope faders' values: "350 ms", "1.25 s", "80 %"
        size_t n = std::strlen(ENV_KEYS[k]);
        if (!std::strncmp(key, ENV_KEYS[k], n) && !std::strcmp(key + n, "_display")) {
            if (k == 2) return std::snprintf(buf, size_t(len), "%.0f %%", num(o, ENV_KEYS[k]));
            float ms = env_ms(num(o, ENV_KEYS[k]));
            return ms < 1000 ? std::snprintf(buf, size_t(len), "%.0f ms", ms) : std::snprintf(buf, size_t(len), "%.2f s", ms / 1000);
        }
    }
    if (!std::strcmp(key, "bend_display")) {
        int b = int(std::lround(num(o, "bend")));
        return b ? std::snprintf(buf, size_t(len), "%d st", b) : put(buf, len, "Inst");
    }
    // 1.8.2: note locks: the pads' marks (1 locked, 2 the note NOTE LOCK shows, 3 both) and the values' text
    if (!std::strncmp(key, "plock_", 6) && isdigit((unsigned char)key[6])) {
        int note = int(num(o, "pad_base")) + std::atoi(key + 6) - 1;
        if (note < 0 || note > 127) return std::snprintf(buf, size_t(len), "0");
        return std::snprintf(buf, size_t(len), "%d", (o->sampler.settings.lock[note].on.load() ? 1 : 0) + (note == lock_note(o) ? 2 : 0));
    }
    if (!std::strcmp(key, "lock_note_display")) return put(buf, len, note_name(lock_note(o)));
    if (!std::strcmp(key, "lock_cut_display")) {
        float c = num(o, "lock_cut") * 0.04f;
        return std::fabs(c) < 0.005f ? put(buf, len, "Off") : std::snprintf(buf, size_t(len), "%+.2f oct", c);
    }
    if (!std::strcmp(key, "lock_pan_display")) {
        int p = int(std::lround(num(o, "lock_pan")));
        return p ? std::snprintf(buf, size_t(len), "%c%d", p < 0 ? 'L' : 'R', std::abs(p)) : put(buf, len, "C");
    }
    if (!std::strcmp(key, "lock_vel_display")) {
        int v = int(std::lround(num(o, "lock_vel")));
        return v ? std::snprintf(buf, size_t(len), "%d", v) : put(buf, len, "Played");
    }
    if (!std::strcmp(key, "lock_info_on")) return std::snprintf(buf, size_t(len), "%d", int(lock_rev.load() & 1));
    if (!std::strcmp(key, "lock_info")) {
        static const char *const MODE[] = {"", "Fwd", "Rev", "Ping-Pong", "PP Inv"};
        int n = lock_note(o);
        std::string s = note_name(n) + ": ";
        if (!o->sampler.settings.lock[n].on.load()) return put(buf, len, s + "not locked - turn a control to lock it");
        float r[LOCK_N];
        for (int k = 0; k < LOCK_N; k++) r[k] = num(o, LOCK_KEYS[k]);
        std::string d;
        char t[48];
        auto add = [&](const std::string &x) { d += (d.empty() ? "" : ", ") + x; };
        if (int(r[0]) > 0 && int(r[0]) < 5) add(MODE[int(r[0])]);
        if (int(r[1]) > 0) add(int(r[1]) == 2 ? "Invert" : "Normal");
        if (r[2] != 0 || r[3] != 0) { std::snprintf(t, sizeof t, "%+.2f st", r[2] + r[3] / 100); add(t); }
        if (r[4] != 0) { std::snprintf(t, sizeof t, "start %.0f%%", r[4]); add(t); }
        if (r[5] != 0) { std::snprintf(t, sizeof t, "cutoff %+.2f oct", r[5] * 0.04f); add(t); }
        if (r[6] != 0) { std::snprintf(t, sizeof t, "%+.0f dB", r[6]); add(t); }
        if (r[7] != 0) { std::snprintf(t, sizeof t, "pan %+.0f", r[7]); add(t); }
        if (r[8] != 0) { std::snprintf(t, sizeof t, "vel %.0f", r[8]); add(t); }
        return put(buf, len, s + "LOCKED  " + d);
    }
    int ni = num_index(key);
    if (ni >= 0) return std::snprintf(buf, size_t(len), "%g", o->values[ni].load());
    if (!std::strcmp(key, "state")) return put(buf, len, get_state(o));
    int ti = trigger_index(key);
    if (ti >= 0) return std::snprintf(buf, size_t(len), "%d", o->trig_on[ti].load());

    if (!std::strncmp(key, "slot", 4) && isdigit((unsigned char)key[4]) && !std::strcmp(key + 5, "_name")) {
        int i = (key[4] - '1') & 3;
        auto lp = o->loader.slot_ref(i); const LoadedInfo &li = *lp;   // 2.4.11: shared, not copied
        if (li.path.empty()) {   // 2.4.9: a project's instrument not loaded (its drive not there): it says which
            const Loader::Missing m = o->loader.missing(i);
            if (!m.path.empty()) return put(buf, len, "MISSING: " + path_name(m.path));
        }
        return put(buf, len, li.path.empty() ? "(empty)" : li.name);
    }
    if (!std::strcmp(key, "prog_name")) {
        auto lp = o->loader.info_ref(); const LoadedInfo &li = *lp;
        if (li.path.empty()) return put(buf, len, std::string("Slot ") + char('A' + o->loader.target()) + " empty - open BROWSE");
        std::string n = std::string(1, char('A' + o->loader.target())) + ": " + li.name;
        if (li.presets.size() > 1) n = std::to_string(li.preset + 1) + "/" + std::to_string(li.presets.size()) + " " + n;
        return put(buf, len, n);
    }
    if (!std::strcmp(key, "prog_info")) {
        auto lp = o->loader.info_ref(); const LoadedInfo &li = *lp;
        if (li.path.empty()) return put(buf, len, "SFZ SF2 Akai E-mu Kontakt and disk images");
        return put(buf, len, li.format + " - " + std::to_string(li.zones) + " zones, " + std::to_string(li.samples) +
                             " samples, " + mb(li.bytes));
    }
    if (!std::strcmp(key, "pad_info")) {
        int b = int(num(o, "pad_base"));
        return put(buf, len, "PADS " + note_name(b) + " - " + note_name(std::min(127, b + 15)));
    }
    if (!std::strcmp(key, "lib_path")) {
        std::string c = library_choice(o);
        if (c.empty()) return put(buf, len, "LIBRARY: not set - choose a folder on an SD card or SSD");
        if (!host_dir(c)) return put(buf, len, "LIBRARY: " + c + " (drive not connected)");
        return put(buf, len, "LIBRARY: " + c);
    }
    if (!std::strcmp(key, "prog_format")) {
        auto lp = o->loader.info_ref(); const LoadedInfo &li = *lp;
        std::string f = li.path.empty() ? "NO INSTRUMENT" : li.format;
        for (char &c : f) c = char(std::toupper(static_cast<unsigned char>(c)));
        return put(buf, len, "FORMAT: " + f);
    }
    if (!std::strncmp(key, "layer_", 6)) {
        auto mp = o->loader.merged_ref(); const MergedInfo &m = *mp;   // 2.4.11: shared, not copied
        int zi = o->sampler.last_zone.load();
        bool have = o->sampler.last_zone_prog.load() == m.serial && zi >= 0 && size_t(zi) < m.zones.size();
        const char *k = key + 6;
        if (!have) return put(buf, len, !std::strcmp(k, "name") ? (m.zones.empty() ? "Load an instrument on the BROWSE tab" : "Play a pad or key") : " ");
        const ZoneBrief &z = m.zones[size_t(zi)];
        if (!std::strcmp(k, "name"))
            return put(buf, len, std::string("ACTIVE LAYER ") + char('A' + (z.slot & 3)) + ": " + (z.name.empty() ? "zone " + std::to_string(zi + 1) : z.name));
        if (!std::strcmp(k, "root")) return put(buf, len, "ROOT: " + note_name(z.root));
        if (!std::strcmp(k, "keys")) return put(buf, len, "KEYS: " + note_name(z.key_lo) + " - " + note_name(z.key_hi));
        if (!std::strcmp(k, "vel")) return put(buf, len, "VEL: " + std::to_string(std::max(1, z.vel_lo)) + " - " + std::to_string(z.vel_hi));
        if (!std::strcmp(k, "hit")) return put(buf, len, "HIT: " + std::to_string(o->sampler.last_vel.load()));
        return put(buf, len, " ");
    }
    if (!std::strcmp(key, "status")) {
        std::string s = o->loader.status();
        if (s.empty()) {
            std::lock_guard<std::mutex> c(o->gp_cache_mutex);
            s = !o->mut_msg.empty() && now_ms() - o->mut_msg_at < 4000 ? o->mut_msg : o->state_warning;   // 2.4.11 / 2.4.12
        }
        if (s.empty()) {
            int v = o->sampler.active_voices.load();
            s = v ? std::to_string(v) + " voices" : "Ready";
        }
        return put(buf, len, s);
    }
    // 2.4.14: the voice count alone (status shows a load note instead of it while one is up); read-only, lock-free
    if (!std::strcmp(key, "voices")) return std::snprintf(buf, size_t(len), "%d", o->sampler.active_voices.load());
    // 2.4.11 (audit): never wait for ui_mutex here. The wrapper runs get_param and render under one lock per instance,
    // and the browser worker holds ui_mutex while it lists a folder, mounts an image or reads Omnisphere patches: waiting
    // here stalled the audio for as long. While the browser is busy, the screen keeps the text it showed last.
    std::unique_lock<std::mutex> lock(o->ui_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::lock_guard<std::mutex> c(o->gp_cache_mutex);
        auto it = o->gp_cache.find(key);
        if (it != o->gp_cache.end()) return put(buf, len, it->second);
        return std::strncmp(key, "br_", 3) ? 0 : put(buf, len, " ");
    }
    const int r = get_param_ui(o, key, buf, len);
    if (r > 0 && len > 0) {
        std::lock_guard<std::mutex> c(o->gp_cache_mutex);
        o->gp_cache[key].assign(buf, size_t(std::min(r, len - 1)));
    }
    return r;
}

void render(void *inst, int16_t *out, int frames) {
    Omni *o = static_cast<Omni *>(inst);
    // skin pads: MPC's momentary buttons release on their own, so a tap holds the note for a fixed time: 0.6 s, or
    // the sound's attack plus half a second (slow strings and pads would barely start otherwise), at most 3 s
    if (o->pad_panic.exchange(0))
        for (int n = 0; n < 16; n++) { o->pad_gate[n] = 0; o->pad_req[n].store(0); }
    for (int n = 0; n < 16; n++) {
        if (o->pad_gate[n] > 0) {
            o->pad_gate[n] -= frames;
            if (o->pad_gate[n] <= 0) { uint8_t off[3] = {0x80, uint8_t(o->pad_note[n]), 0}; o->sampler.midi(off, 3); }
        }
        if (o->pad_req[n].exchange(0)) {
            if (o->pad_gate[n] > 0) { uint8_t off[3] = {0x80, uint8_t(o->pad_note[n]), 0}; o->sampler.midi(off, 3); }
            int note = std::min(127, int(num(o, "pad_base")) + n);
            int vel = std::max(1, std::min(127, int(num(o, "pad_vel"))));
            o->pad_note[n] = note;
            float gate = std::max(0.6f, std::min(3.0f, o->sampler.slowest_attack.load() + 0.5f));
            o->pad_gate[n] = int(gate * 44100);
            uint8_t on[3] = {0x90, uint8_t(note), uint8_t(vel)};
            o->sampler.midi(on, 3);
        }
    }
    o->sampler.render(out, frames);
}

// the host's song position (audio thread, once per host block before render; vst.json HAS_SONG_POS, 2.4): tempo-synced
// LFOs and delays, the ARP's clock. ppq < 0 / bpm 0: the host gave none. (Before 2.4 this was never called: 120 BPM.)
void transport(void *inst, double ppq, double tempo, int playing, int /*frames*/) {
    static_cast<Omni *>(inst)->sampler.transport(ppq, tempo, playing, ppq >= 0 && tempo > 0);
}

const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, nullptr, transport};

}  // namespace

extern "C" const mpc_engine_t *mpc_engine(void) { return &ENGINE; }

// 2.1.5: parameters the engine changes by itself, for the wrapper to report to the host (audioMasterAutomate), so MPC
// redraws their widgets with no screen rebuild: the logo spark's frame. The wrapper asks after every block (audio
// thread; lock-free) and reports at most every 1024 frames; one key per call, 0 = nothing more this block
extern "C" int mpc_engine_changed(void *inst, char *key, int len) {
    Omni *o = static_cast<Omni *>(inst);
    int f = logo_frame_now();
    if (f != o->logo_frame.load(std::memory_order_relaxed)) {
        o->logo_frame.store(f, std::memory_order_relaxed);
        return std::snprintf(key, size_t(len), "logo_fx");
    }
    {   // 2.2: the GR meter, when a segment lit or went out; 2.2.3: at most 12 times a second (each report is a repaint
        // on MPC's screen thread: open-mpc NOTES "Animation is expensive")
        int g = gr_meter_seg(o);
        if (g != o->gr_frame) {
            long long t = now_ms();
            if (t - o->gr_ms >= 80) { o->gr_frame = g; o->gr_ms = t; return std::snprintf(key, size_t(len), "gr_meter"); }
        }
    }
    {   // 2.4: the ARP's playhead: the light over its column, while it plays the page shown
        const int st = o->sampler.arp.step_now.load(std::memory_order_relaxed);
        static const int page_i = num_index("arp_page"), follow_i = num_index("arp_follow");
        int page = int(o->values[page_i].load() + 0.5f);
        // 2.4.1 FOLLOW: the grid turns to the page playing (not during a DRAW stroke)
        if (st >= 0 && st / 16 != page && o->values[follow_i].load() > 0.5f && !o->arp_stroke.active) {
            page = st / 16;
            o->values[page_i].store(float(page));
            mark_changed(o, page_i);
            arp_refresh_view(o);
            arp_leds_dirty(o);
        }
        const int col = st >= 0 && st / 16 == page ? st % 16 : -1;
        if (col != o->arp_led_shown) {
            if (o->arp_led_shown >= 0) mark_changed(o, Omni::ARP_REP0 + 256 + o->arp_led_shown);
            if (col >= 0) mark_changed(o, Omni::ARP_REP0 + 256 + col);
            o->arp_led_shown = col;
        }
    }
    {   // LFO ROUTE keys: all three of an LFO's whenever its route moved (a tap, a Q-Link, a project, a preset), and a
        // key tapped (lfo_route_tap)
        for (int k = 0; k < SYN_LFOS; k++) {   // 2.4.17: 4
            const int r = lfo_route_now(o, k);
            if (r != o->rk_shown[k]) { o->rk_shown[k] = r; o->rk_dirty.fetch_or(uint16_t(7u << (k * 3))); }
        }
        if (uint16_t d = o->rk_dirty.load(std::memory_order_relaxed)) {
            const int b = __builtin_ctz(d);
            o->rk_dirty.fetch_and(uint16_t(~(1u << b)));
            return std::snprintf(key, size_t(len), "syn_lfo%d_rk_%s", b / 3 + 1, LFO_RK[b % 3]);
        }
    }
    // 2.1.16: what the engine changed by itself and the key-map tiles (mark_changed)
    if (o->rep_pending.load(std::memory_order_relaxed) <= 0) return 0;
    for (int t = 0; t < Omni::REP_COUNT; t++) {
        int i = o->rep_cur;
        o->rep_cur = (i + 1) % Omni::REP_COUNT;
        if (!o->rep_dirty[i].exchange(0)) continue;
        o->rep_pending.fetch_sub(1);
        if (i < NUM_COUNT) return std::snprintf(key, size_t(len), "%s", NUM_PARAMS[i].key);
        if (i >= Omni::ARP_REP0) {   // 2.4: a grid dot, a playhead light
            i -= Omni::ARP_REP0;
            if (i < 256) return std::snprintf(key, size_t(len), "arp_c%d_%d", i / 16 + 1, i % 16 + 1);
            return std::snprintf(key, size_t(len), "arp_led%d", i - 256 + 1);
        }
        i -= NUM_COUNT;
        if (i >= 64 + SYN_OSCS * SYN_BARS) {   // 2.3.2: the envelope bars
            i -= 64 + SYN_OSCS * SYN_BARS;
            return std::snprintf(key, size_t(len), "syn_%ce_b%d", "afmno"[i / ENV_BARS], i % ENV_BARS + 1);
        }
        if (i >= 64) { i -= 64; return std::snprintf(key, size_t(len), "s%c_w%d", 'a' + i / SYN_BARS, i % SYN_BARS + 1); }   // 2.3
        return std::snprintf(key, size_t(len), "%ctile_%d", i < 32 ? 'p' : 'l', i % 32 + 1);
    }
    return 0;
}

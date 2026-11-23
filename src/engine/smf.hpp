// Omni Sampler 2.4: Standard MIDI Files (.mid) for the ARP tab: their notes, and those notes as a step pattern.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace omni {

struct SmfNote { int64_t on = 0, off = 0; int note = 0, vel = 0, chan = 0; };
struct SmfSong {
    int division = 96;            // ticks per quarter note
    int ts_num = 4, ts_den = 4;   // the first time signature (4/4 when none)
    std::vector<SmfNote> notes;   // every track's notes, by start
};

// Parses a format 0 or 1 file (SMPTE time division is refused). Throws ParseError.
SmfSong smf_parse(const std::vector<uint8_t> &data);

// The notes as a pattern of up to 64 steps.
//   snap  0 Auto (the coarsest of 1/4 1/8 1/16 1/32 every note fits, the whole file if it fits 64 steps), 1 1/32,
//         2 1/16, 3 1/8, 4 1/4 (the step), 5 1 Bar (starts and ends on bars, steps of 1/4)
//   mode  0 SEQ: rows are scale steps up from the lowest note; 1 ARP: a chord's notes lowest first (row 1 = its lowest)
// A note held over several steps lights them all (the TIE runs); one that starts where the same row's note ends leaves
// that one a step short, so it plays again. The key: the one given if every note is in it, else the best fit
// (detect); `base` is the lowest note's degree above the root at C3.
struct ArpImport {
    int snap = 0, mode = 0, scale = 1, root = 0;
    bool detect = true;
    bool rank_all = false;   // ARP: rows = each pitch's rank among all the file's pitches (a phrase of single notes)
};
struct ArpPattern {
    uint16_t mask[64] = {}; uint8_t vel[64] = {};
    // 2.4.18 SEQ: a note above the 16 rows sits an octave (two) lower on the grid with NOTE OCT +1 (+2), at its first
    // step (step * 16 + row), so wide voicings keep their pitches instead of landing on the notes below
    int8_t oct[64 * 16] = {};
    int raised = 0;                  // notes placed so (NOTE OCT); 0 = all fit the rows
    // 2.4.30 (audit F4): notes whose exact pitch the grid cannot hold: an octave double that folds onto another note's
    // cell (one cell = one row and one NOTE OCT), or a fold beyond NOTE OCT's +2. The lower pitch keeps the cell; the
    // others are counted here (they were merged silently) and the LOAD message says so
    int lost = 0;
    int length = 0, lowest = -1, rate = 2, base = 0, scale = 1, root = 0;
    bool key_found = false;          // the key was detected (the given one did not fit)
    int bars = 0, bars_used = 0, off_scale = 0, notes = 0;
};
ArpPattern smf_to_pattern(const SmfSong &song, const ArpImport &how);

}  // namespace omni

// F4 (proposed): SMF import reports octave collisions (ArpPattern::lost). Needs the work/ tree.
#include <algorithm>
#include <cstdio>
#include <vector>
#include "engine/arp.hpp"
#include "engine/smf.hpp"
using namespace omni;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static SmfSong song_of(std::vector<int> notes) {   // all at tick 0, one beat long
    SmfSong s; s.division = 96;
    for (int n : notes) s.notes.push_back({0, 96, n, 100, 0});
    return s;
}
int main() {
    ArpImport how; how.detect = false; how.scale = 0; how.root = 0; how.mode = 0; how.snap = 3;   // forced Chromatic
    {   // C3 C4 C5 together: C5 folds onto C4's cell
        ArpPattern p = smf_to_pattern(song_of({48, 60, 72}), how);
        std::printf("notes=%d lost=%d raised=%d mask0=%d\n", p.notes, p.lost, p.raised, p.mask[0]);
        check(p.lost == 1 && p.notes == 3, "C3 C4 C5: one octave reported lost");
        check(p.raised == 0 && p.oct[0 * 16 + 12] == 0, "the lower pitch (C4) keeps its cell, plain");
    }
    {   // the same file with the notes in the other order: same result (lower pitch wins)
        ArpPattern p = smf_to_pattern(song_of({72, 60, 48}), how);
        check(p.lost == 1 && p.oct[12] == 0 && p.raised == 0, "order in the file does not change which note keeps the cell");
    }
    {   // C3 and C5 only: C5 folds to row 12 alone: NOTE OCT +1, nothing lost
        ArpPattern p = smf_to_pattern(song_of({48, 72}), how);
        check(p.lost == 0 && p.raised == 1 && p.oct[12] == 1, "C3 C5: raised with NOTE OCT, nothing lost");
    }
    {   // fold beyond +2 octaves (a 5-octave spread in chromatic: 16 rows = 1.3 octaves)
        ArpPattern p = smf_to_pattern(song_of({24, 24 + 60}), how);
        check(p.lost == 1, "a 5-octave jump (fold > NOTE OCT +2): reported");
    }
    {   // ARP mode, 18 simultaneous notes: rows 16..18 share row 16
        std::vector<int> v; for (int i = 0; i < 18; i++) v.push_back(40 + i);
        ArpImport a; a.mode = 1; a.snap = 3;
        ArpPattern p = smf_to_pattern(song_of(v), a);
        check(p.lost == 2, "ARP: an 18-note chord on 16 rows: 2 notes reported lost");
    }
    {   // a plain file loses nothing
        ArpPattern p = smf_to_pattern(song_of({60, 64, 67}), how);
        check(p.lost == 0, "a triad: nothing lost");
    }
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails;
}

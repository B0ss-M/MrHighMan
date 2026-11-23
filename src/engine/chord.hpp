// Omni Sampler 2.4.14: chord detection for the GRID (the user: "the grid needs an algorithm that can detect chords").
// A pure function of a list of MIDI notes: the pitch-class set against 26 chord templates, every pitch class as a candidate
// root. Score = 100 per template tone present - 45 per note outside the template - 25 per optional tone missing (only the
// fifth may be missing) - 4 x the template's cost (simple chords first) + 20 when the root is the bass. Ties between
// symmetric chords (dim7, aug) and relative pairs (Am7 / C6) go to the bass. Below 150 the pitch classes are listed.
// No allocation, no locks, no statics beyond constant tables: at most 12 x 26 mask tests (~1-2 us). Design: GRID_AUDIT.md
// section 3 (2026-11-17), written here and tested in tests/test_arp.cpp.
#pragma once
#include <cstdint>
#include <cstdio>

namespace omni {

struct ChordInfo {
    int root = -1;         // pitch class 0..11 (C = 0), -1 none
    int bass = -1;         // pitch class of the lowest note
    int type = -1;         // index into chord_detail::TPL, -1 = no template (one note, or a set no template fits)
    int inversion = 0;     // the template tone in the bass: 0 root position, 1 first, 2 second, 3 third
    int pcs = 0;           // distinct pitch classes
    bool exact = false;    // every note belongs to the template
    bool no5 = false;      // matched with the fifth left out
    char name[24] = "-";   // "Cmaj7/E", "C5", "Am7", "C7(no5)", "C", "C D G" (unknown), "-" (empty)
};

namespace chord_detail {
constexpr uint16_t bits(int a, int b = -1, int c = -1, int d = -1, int e = -1) {
    return uint16_t((1u << a) | (b >= 0 ? 1u << b : 0u) | (c >= 0 ? 1u << c : 0u) | (d >= 0 ? 1u << d : 0u) | (e >= 0 ? 1u << e : 0u));
}
constexpr uint16_t FIFTH = 1u << 7;
// iv: the intervals above the root (bit i = i semitones); opt: tones that may be missing; cost: preference penalty
struct Tpl { const char *suf; uint16_t iv, opt; int cost; };
constexpr Tpl TPL[] = {
    {"", bits(0, 4, 7), FIFTH, 0},          {"m", bits(0, 3, 7), FIFTH, 0},
    {"dim", bits(0, 3, 6), 0, 1},           {"aug", bits(0, 4, 8), 0, 1},
    {"sus2", bits(0, 2, 7), 0, 1},          {"sus4", bits(0, 5, 7), 0, 1},
    {"5", bits(0, 7), 0, 1},
    {"7", bits(0, 4, 7, 10), FIFTH, 2},     {"maj7", bits(0, 4, 7, 11), FIFTH, 2},
    {"m7", bits(0, 3, 7, 10), FIFTH, 2},    {"mMaj7", bits(0, 3, 7, 11), FIFTH, 3},
    {"m7b5", bits(0, 3, 6, 10), 0, 2},      {"dim7", bits(0, 3, 6, 9), 0, 2},
    {"7sus4", bits(0, 5, 7, 10), FIFTH, 3}, {"7#5", bits(0, 4, 8, 10), 0, 3},
    {"maj7#5", bits(0, 4, 8, 11), 0, 4},
    {"6", bits(0, 4, 7, 9), FIFTH, 3},      {"m6", bits(0, 3, 7, 9), FIFTH, 3},
    {"add9", bits(0, 2, 4, 7), FIFTH, 4},   {"madd9", bits(0, 2, 3, 7), FIFTH, 4},
    {"9", bits(0, 2, 4, 7, 10), FIFTH, 5},  {"maj9", bits(0, 2, 4, 7, 11), FIFTH, 5},
    {"m9", bits(0, 2, 3, 7, 10), FIFTH, 5}, {"6/9", bits(0, 2, 4, 7, 9), FIFTH, 6},
    {"7b9", bits(0, 1, 4, 7, 10), FIFTH, 6}, {"7#9", bits(0, 3, 4, 7, 10), FIFTH, 6},
};
constexpr int NT = int(sizeof TPL / sizeof TPL[0]);
constexpr const char *PC[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
inline int pop(unsigned x) { return __builtin_popcount(x); }
inline unsigned rotate_to(unsigned pcs, int r) { return ((pcs >> r) | (pcs << (12 - r))) & 0xFFFu; }   // bit i = i above r
}  // namespace chord_detail

// notes: MIDI notes in any order (octave doubles and gaps fine; values outside 0..127 are ignored), n >= 0
inline ChordInfo chord_detect(const int *notes, int n) {
    using namespace chord_detail;
    ChordInfo ci;
    if (!notes || n <= 0) return ci;
    unsigned pcs = 0;
    int lo = 1 << 30;
    for (int i = 0; i < n; i++) {
        if (notes[i] < 0 || notes[i] > 127) continue;
        pcs |= 1u << (notes[i] % 12);
        if (notes[i] < lo) lo = notes[i];
    }
    if (!pcs) return ci;
    ci.bass = lo % 12;
    ci.pcs = pop(pcs);
    if (ci.pcs == 1) {
        ci.root = ci.bass;
        ci.exact = true;
        std::snprintf(ci.name, sizeof ci.name, "%s", PC[ci.root]);
        return ci;
    }
    int best = -(1 << 30), bt = -1, br = -1;
    for (int r = 0; r < 12; r++) {
        if (!(pcs >> r & 1)) continue;   // the root is one of the notes (a rootless voicing is not guessed)
        const unsigned rel = rotate_to(pcs, r);
        for (int t = 0; t < NT; t++) {
            const Tpl &T = TPL[t];
            const unsigned need = unsigned(T.iv) & ~unsigned(T.opt);
            if ((rel & need) != need) continue;   // a required tone is missing
            const int matched = pop(rel & T.iv), extra = pop(rel & ~unsigned(T.iv)), miss = pop(unsigned(T.iv) & ~rel);
            const int score = 100 * matched - 45 * extra - 25 * miss - 4 * T.cost + (r == ci.bass ? 20 : 0);
            if (score > best) { best = score; bt = t; br = r; }
        }
    }
    if (bt < 0 || best < 150) {   // no chord: its pitch classes, from the bass up
        int w = 0;
        for (int i = 0; i < 12; i++) {
            const int p = (ci.bass + i) % 12;
            if (!(pcs >> p & 1)) continue;
            const int left = int(sizeof ci.name) - w;
            if (left < 4) break;
            w += std::snprintf(ci.name + w, size_t(left), w ? " %s" : "%s", PC[p]);
        }
        return ci;
    }
    const Tpl &T = TPL[bt];
    const unsigned rel = rotate_to(pcs, br);
    ci.root = br;
    ci.type = bt;
    ci.exact = (rel & ~unsigned(T.iv)) == 0;
    ci.no5 = (T.iv & FIFTH) && !(rel & FIFTH);
    const int bi = (ci.bass - br + 12) % 12;   // the bass as an interval above the root
    int idx = 0;
    for (int i = 0; i < bi; i++) if (T.iv >> i & 1) idx++;
    ci.inversion = (T.iv >> bi & 1) ? (idx > 3 ? 3 : idx) : 0;
    std::snprintf(ci.name, sizeof ci.name, "%s%s%s%s%s%s", PC[br], T.suf, ci.no5 ? "(no5)" : "", ci.exact ? "" : "?",
                  ci.bass != br ? "/" : "", ci.bass != br ? PC[ci.bass] : "");
    return ci;
}

}  // namespace omni

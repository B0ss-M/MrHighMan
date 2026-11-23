// Omni Sampler 2.4.14: VOICE for the GRID (the user: "a button for either inversion or to spread the notes apart but be
// within the key and scale the user sets"). Pure functions: a chord's pitches -> the new voicing (inversions up / down,
// drop 2, drop 3, open, close), every note kept in the GRID's scale, then the GRID cell (row + NOTE OCT) that plays each.
// Why it stays in key: a GRID row is a scale degree, and one octave is exactly scale_size rows, so the octave moves these
// voicings make are in the scale by construction; snap_to_scale is the safety net. Design: GRID_AUDIT.md section 4.
// No allocation (fixed arrays of 16), no locks: callable from the UI thread and the tests.
#pragma once
#include <algorithm>
#include <cmath>
#include "arp.hpp"

namespace omni {

enum VoiceMode { VM_INV_UP, VM_INV_DOWN, VM_DROP2, VM_DROP3, VM_OPEN, VM_CLOSE, VM_COUNT };
constexpr const char *VOICE_MODE_NAMES[VM_COUNT] = {"INVERT UP", "INVERT DOWN", "DROP 2", "DROP 3", "OPEN", "CLOSE"};
struct VoiceNote { int pitch = 0, src = 0; };   // src: the note's index in the input (it keeps its gate / velocity / ...)
struct VoiceResult { bool ok = false; int n = 0; VoiceNote v[16]; const char *why = ""; };

// the nearest note of the scale (dir 0; a tie goes down), or the one below (-1) / above (+1)
inline int snap_to_scale(int note, int scale, int root, int dir = 0) {
    root = ((root % 12) + 12) % 12;
    if (Arp::in_scale(note, scale, root)) return note;
    const int d = Arp::degree_of(note, scale, root);   // rounds down onto the scale
    const int lo = Arp::note_of(d, scale, root), hi = Arp::note_of(d + 1, scale, root);
    return dir < 0 ? lo : dir > 0 ? hi : (note - lo <= hi - note ? lo : hi);
}

// in: the chord's MIDI pitches (2..16, any order). lo / hi: the pitch range allowed. max_span: the widest result Drop /
// Open may make (semitones). anchor: the mean pitch to stay near (< 0: the input's own), so repeated inversions cycle
// instead of climbing: the result is moved by an octave (-12 / 0 / +12) to the mean nearest the anchor.
inline VoiceResult voice_apply(const int *in, int n, VoiceMode m, int scale, int root, int lo, int hi, int max_span,
                               double anchor = -1) {
    VoiceResult R;
    if (!in || n < 2 || n > 16) { R.why = "needs 2 to 16 notes"; return R; }
    VoiceNote a[16];
    double mean0 = 0;
    for (int i = 0; i < n; i++) { a[i].pitch = in[i]; a[i].src = i; mean0 += in[i]; }
    mean0 /= n;
    const double ref = anchor >= 0 ? anchor : mean0;
    auto order = [&] {
        std::sort(a, a + n, [](const VoiceNote &x, const VoiceNote &y) { return x.pitch != y.pitch ? x.pitch < y.pitch : x.src < y.src; });
    };
    order();
    const int span0 = a[n - 1].pitch - a[0].pitch;
    switch (m) {   // a[] ascending
    case VM_INV_UP: a[0].pitch += 12; break;                                        // the lowest note to the top
    case VM_INV_DOWN: a[n - 1].pitch -= 12; break;                                  // the highest note to the bottom
    case VM_DROP2: a[n >= 3 ? n - 2 : 0].pitch -= 12; break;                        // 2nd from the top an octave down
    case VM_DROP3:
        if (n < 3) { R.why = "DROP 3 needs 3 or more notes"; return R; }
        a[n >= 4 ? n - 3 : 0].pitch -= 12; break;                                   // 3rd from the top (a triad: its bass)
    case VM_OPEN: for (int i = 1; i < n; i += 2) a[i].pitch += 12; break;           // every second note above the bass up
    case VM_CLOSE:
        for (int i = 1; i < n; i++) {
            while (a[i].pitch - a[0].pitch >= 12) a[i].pitch -= 12;                 // inside an octave above the bass
            while (a[i].pitch < a[0].pitch) a[i].pitch += 12;
        }
        break;
    default: R.why = "unknown voicing"; return R;
    }
    for (int i = 0; i < n; i++) a[i].pitch = snap_to_scale(a[i].pitch, scale, root);   // in key and scale, always
    order();
    for (int i = 1; i < n; i++)
        if (a[i].pitch == a[i - 1].pitch) { R.why = "two notes would play the same pitch"; return R; }
    int bt = 0;
    double bd = 1e9;
    bool any = false;
    for (int t : {0, -12, 12}) {
        if (a[0].pitch + t < lo || a[n - 1].pitch + t > hi) continue;
        double mean = 0;
        for (int i = 0; i < n; i++) mean += a[i].pitch + t;
        mean /= n;
        const double d = std::fabs(mean - ref);
        if (d < bd - 1e-9) { bd = d; bt = t; any = true; }
    }
    if (!any) { R.why = "no room in the range"; return R; }
    for (int i = 0; i < n; i++) a[i].pitch += bt;
    const int span1 = a[n - 1].pitch - a[0].pitch;
    if (span1 > max_span && span1 > span0) { R.why = "wider than SPAN"; return R; }
    R.ok = true;
    R.n = n;
    for (int i = 0; i < n; i++) R.v[i] = a[i];
    return R;
}

// a pitch as written (the root at C3 the reference, as Arp::seq_note plays a pattern) -> the GRID cell that plays it:
// row 0..15 and NOTE OCT -2..+2 (preferring 0, then +1, -1, +2, -2). octave: the GRID's OCTAVE; base: ArpSettings::base.
// row_free (optional): whether a row may be used. false: no cell plays it.
template <class RowFree>
inline bool pitch_to_cell(int pitch, int scale, int root, int base, int octave, int &row, int &noct, RowFree row_free) {
    root = ((root % 12) + 12) % 12;
    const int p = pitch - 12 * octave;
    if (!Arp::in_scale(p, scale, root)) return false;
    const int n = Arp::scale_size(scale), R = Arp::degree_of(60 + root, scale, root);
    const int d = Arp::degree_of(p, scale, root) - (R + base);   // its row with NOTE OCT 0
    for (int k : {0, 1, -1, 2, -2}) {                             // k octaves of NOTE OCT = k * n rows fewer
        const int r = d - k * n;
        if (r < 0 || r >= Arp::ROWS) continue;
        if (!row_free(r)) continue;
        row = r;
        noct = k;
        return true;
    }
    return false;
}
inline bool pitch_to_cell(int pitch, int scale, int root, int base, int octave, int &row, int &noct) {
    return pitch_to_cell(pitch, scale, root, base, octave, row, noct, [](int) { return true; });
}

}  // namespace omni

// Omni Sampler 2.4: Standard MIDI Files for the ARP tab (smf.hpp).
#include <cstddef>
#include "smf.hpp"
#include <algorithm>
#include <cmath>
#include "../core/bytes.hpp"
#include "arp.hpp"

namespace omni {

namespace {
struct Rd {
    const uint8_t *p, *end;
    bool more() const { return p < end; }
    uint8_t u8() { if (p >= end) throw ParseError("MIDI file: cut short"); return *p++; }
    uint32_t be(int n) { uint32_t v = 0; while (n--) v = v << 8 | u8(); return v; }
    uint32_t vlq() {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) { uint8_t b = u8(); v = v << 7 | (b & 0x7F); if (!(b & 0x80)) return v; }
        throw ParseError("MIDI file: bad length");
    }
};
}  // namespace

SmfSong smf_parse(const std::vector<uint8_t> &data) {
    Rd r{data.data(), data.data() + data.size()};
    // a RIFF-wrapped MIDI file (.rmi): its "data" chunk
    if (data.size() > 20 && std::equal(data.begin(), data.begin() + 4, "RIFF"))
        for (size_t i = 12; i + 8 <= data.size(); i++)
            if (std::equal(data.begin() + long(i), data.begin() + long(i) + 4, "MThd")) { r.p = data.data() + i; break; }
    if (r.end - r.p < 14 || !std::equal(r.p, r.p + 4, "MThd")) throw ParseError("not a MIDI file");
    r.p += 4;
    const uint32_t hlen = r.be(4);
    const uint8_t *hend = r.p + hlen;
    const int format = int(r.be(2)), ntracks = int(r.be(2));
    const int div = int(r.be(2));
    if (div & 0x8000) throw ParseError("MIDI file: SMPTE timing is not supported");
    if (format > 1) throw ParseError("MIDI file: format 2 is not supported");
    if (hend > r.end) throw ParseError("MIDI file: cut short");
    r.p = hend;
    SmfSong song;
    song.division = div > 0 ? div : 96;
    bool have_ts = false;
    for (int t = 0; t < ntracks && r.end - r.p >= 8; t++) {
        const bool trk = std::equal(r.p, r.p + 4, "MTrk");
        r.p += 4;
        const uint32_t len = r.be(4);
        if (uint32_t(r.end - r.p) < len) throw ParseError("MIDI file: cut short");
        Rd tr{r.p, r.p + len};
        r.p += len;
        if (!trk) continue;   // an unknown chunk
        int64_t tick = 0;
        int running = 0;
        int64_t open[16][128];
        int open_vel[16][128];
        for (auto &row : open) std::fill(row, row + 128, -1);
        while (tr.more()) {
            tick += tr.vlq();
            int st = tr.u8();
            if (st < 0x80) { if (!running) throw ParseError("MIDI file: bad event"); tr.p--; st = running; }
            if (st == 0xFF) {
                const int type = tr.u8();
                const uint32_t n = std::min<uint32_t>(tr.vlq(), uint32_t(tr.end - tr.p));
                if (type == 0x58 && n >= 2 && !have_ts) {   // time signature: the first one
                    song.ts_num = std::max(1, int(tr.p[0]));
                    song.ts_den = 1 << std::min(5, int(tr.p[1]));
                    have_ts = true;
                }
                tr.p += n;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) { tr.p += std::min<uint32_t>(tr.vlq(), uint32_t(tr.end - tr.p)); continue; }
            if (st > 0xF0) {
                // 2.4.29 (GRID audit F6): system common / real-time messages at their own lengths (were read as 2-byte
                // channel messages: F1 / F3 / F6 / F8-FE put every later note out of step). System common clears running
                // status, real-time leaves it
                const int len = st == 0xF2 ? 2 : (st == 0xF1 || st == 0xF3) ? 1 : 0;
                tr.p += std::min<ptrdiff_t>(len, tr.end - tr.p);
                if (st < 0xF8) running = 0;
                continue;
            }
            running = st;
            const int type = st & 0xF0, ch = st & 15;
            const int a = tr.u8(), b = (type == 0xC0 || type == 0xD0) ? 0 : tr.u8();
            const bool on = type == 0x90 && b > 0, off = type == 0x80 || (type == 0x90 && b == 0);
            if (on || off) {
                const int n = a & 127;
                if (open[ch][n] >= 0) {   // ends the note before (an off, or the same note again)
                    song.notes.push_back({open[ch][n], tick, n, open_vel[ch][n], ch});
                    open[ch][n] = -1;
                }
                if (on) { open[ch][n] = tick; open_vel[ch][n] = b; }
            }
        }
        for (int ch = 0; ch < 16; ch++)
            for (int n = 0; n < 128; n++)
                if (open[ch][n] >= 0) song.notes.push_back({open[ch][n], tick, n, open_vel[ch][n], ch});
    }
    std::stable_sort(song.notes.begin(), song.notes.end(), [](const SmfNote &x, const SmfNote &y) { return x.on < y.on; });
    return song;
}

ArpPattern smf_to_pattern(const SmfSong &song, const ArpImport &how) {
    ArpPattern p;
    p.scale = how.scale;
    p.root = ((how.root % 12) + 12) % 12;
    if (song.notes.empty()) return p;
    const double tpb = song.division;                                   // ticks per beat
    const double bpb = song.ts_num * 4.0 / song.ts_den;                 // beats per bar
    const double bar_t = bpb * tpb;
    // the first note's bar is bar 1 (silence before it is dropped by whole bars)
    const int64_t t0 = int64_t(std::floor(song.notes.front().on / bar_t) * bar_t);
    double end_b = 0;
    for (const SmfNote &n : song.notes) end_b = std::max(end_b, (n.off - t0) / tpb);
    p.bars = std::max(1, int(std::ceil(end_b / bpb - 1e-6)));
    // the step (in beats) and the grid notes snap to
    static const double STEP[4] = {1.0, 0.5, 0.25, 0.125};   // 1/4 1/8 1/16 1/32 (rates 0..3)
    double step = 0.25, grid = 0.25;
    if (how.snap >= 1 && how.snap <= 4) step = grid = STEP[4 - how.snap];
    else if (how.snap == 5) { step = 1.0; grid = bpb; }
    else {   // Auto
        auto fits = [&](double g) {
            for (const SmfNote &n : song.notes)
                for (double t : {double(n.on - t0) / tpb, double(n.off - t0) / tpb})
                    if (std::fabs(t / g - std::round(t / g)) > 0.1) return false;
            return true;
        };
        int pick = -1;
        for (int i = 0; i < 4 && pick < 0; i++)   // the coarsest exact grid that holds the whole file
            if (fits(STEP[i]) && p.bars * bpb / STEP[i] <= Arp::STEPS + 1e-6) pick = i;
        if (pick < 0)   // nothing exact fits: the finest grid that still holds the whole file (or 1/4)
            for (int i = 3; i >= 0 && pick < 0; i--) if (p.bars * bpb / STEP[i] <= Arp::STEPS + 1e-6) pick = i;
        if (pick < 0) pick = 0;
        step = grid = STEP[pick];
    }
    p.rate = step == 1.0 ? 0 : step == 0.5 ? 1 : step == 0.25 ? 2 : 3;
    const double spb = bpb / step;                                       // steps per bar
    // each note on the grid, in steps [s0, s1)
    struct Q { int s0, s1, note, vel; };
    std::vector<Q> q;
    for (const SmfNote &n : song.notes) {
        double a = std::round((n.on - t0) / tpb / grid) * grid, b = std::round((n.off - t0) / tpb / grid) * grid;
        if (b <= a) b = a + grid;
        const int s0 = int(std::lround(a / step)), s1 = int(std::lround(b / step));
        if (s0 >= Arp::STEPS) continue;
        q.push_back({s0, std::max(s0 + 1, std::min(Arp::STEPS, s1)), n.note, n.vel});
    }
    if (q.empty()) return p;
    // the key
    auto off_count = [&](int sc, int rt) {
        int off = 0;
        for (const Q &x : q) off += Arp::in_scale(x.note, sc, rt) ? 0 : x.s1 - x.s0;
        return off;
    };
    // 2.4.18: Chromatic fits every file, so a file that needed it (borrowed notes: no scale holds them all) kept every
    // file loaded after it Chromatic too (16 rows = 16 semitones, wide chords folded): with Chromatic set, look again
    if (how.mode == 0 && how.detect && (off_count(p.scale, p.root) > 0 || p.scale == 0)) {
        static const int PREF[] = {1, 2, 3, 6, 4, 5, 8, 9, 10, 11, 12, 7, 15, 14, 13, 0};   // Major and Minor first
        int best = 1 << 30;
        for (int sc : PREF)
            for (int k = 0; k < 12; k++) {
                const int rt = (p.root + k) % 12;   // (from the root given: a tie keeps it)
                const int off = off_count(sc, rt);
                if (off < best) { best = off; p.scale = sc; p.root = rt; }
            }
        p.key_found = true;
    }
    // rows
    int lowest = 128;
    for (const Q &x : q) lowest = std::min(lowest, x.note);
    p.lowest = lowest;
    std::vector<int> row(q.size()), up(q.size(), 0);
    if (how.mode == 0) {
        const int R = Arp::degree_of(60 + p.root, p.scale, p.root), d0 = Arp::degree_of(lowest, p.scale, p.root);
        const int size = Arp::scale_size(p.scale);
        p.base = d0 - R;
        for (size_t i = 0; i < q.size(); i++) {
            int r = Arp::degree_of(q[i].note, p.scale, p.root) - d0;
            while (r >= Arp::ROWS) { r -= size; up[i]++; }   // fold down by octaves (2.4.18: NOTE OCT keeps the pitch)
            row[i] = std::max(0, r);
            if (!Arp::in_scale(q[i].note, p.scale, p.root)) p.off_scale++;
        }
    } else if (how.rank_all) {   // ARP, a phrase: each pitch's place among all of them (lowest = row 1)
        std::vector<int> pitches;
        for (const Q &x : q) pitches.push_back(x.note);
        std::sort(pitches.begin(), pitches.end());
        pitches.erase(std::unique(pitches.begin(), pitches.end()), pitches.end());
        // 2.4.29: more pitches than rows spread evenly over the 16 (were piled on the top row)
        const int n = int(pitches.size());
        for (size_t i = 0; i < q.size(); i++) {
            const int rank = int(std::lower_bound(pitches.begin(), pitches.end(), q[i].note) - pitches.begin());
            row[i] = n > Arp::ROWS ? rank * (Arp::ROWS - 1) / (n - 1) : rank;
        }
    } else {   // ARP: the notes starting together, lowest first
        for (size_t i = 0; i < q.size(); i++) {
            int rank = 0;
            for (size_t j = 0; j < q.size(); j++)
                if (q[j].s0 == q[i].s0 && (q[j].note < q[i].note || (q[j].note == q[i].note && j < i))) rank++;
            row[i] = std::min(Arp::ROWS - 1, rank);
        }
    }
    // a note that ends where the next one on its row starts is a step short (the two would join in one run)
    for (size_t i = 0; i < q.size(); i++)
        for (size_t j = 0; j < q.size(); j++)
            if (i != j && row[i] == row[j] && q[j].s0 == q[i].s1 && q[i].s1 - q[i].s0 >= 2) { q[i].s1--; break; }
    // 2.4.30 (audit F4): lower pitches first, so which of two notes that need one cell keeps it does not depend on the file
    std::vector<size_t> ord(q.size());
    for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
    std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return q[a].s0 != q[b].s0 ? q[a].s0 < q[b].s0 : q[a].note < q[b].note; });
    std::vector<int8_t> cell_oct(size_t(Arp::STEPS) * Arp::ROWS, -1);   // the octave each lit cell plays at (-1 = unlit)
    std::vector<int16_t> cell_note(size_t(Arp::STEPS) * Arp::ROWS, -1);  // ARP: the pitch it stands for (rows past the 16th share row 16)
    int last = 0;
    for (size_t oi = 0; oi < ord.size(); oi++) {
        const size_t i = ord[oi];
        // 2.4.18: a folded note's octave (NOTE OCT, +2 at most), unless its cell is already a note's: the first one keeps it
        // (an octave doubling of the same pitch class; nothing of the harmony is lost)
        // 2.4.30: ... which is only true when that note plays at the same octave: otherwise it is a lost pitch, counted
        const int ci = q[i].s0 * Arp::ROWS + row[i];
        const int eff = up[i] <= 2 ? up[i] : 0;
        if (up[i] > 2 || (cell_oct[ci] >= 0 && cell_oct[ci] != eff) || (how.mode != 0 && cell_note[ci] >= 0 && cell_note[ci] != q[i].note)) p.lost++;
        if (up[i] > 0 && up[i] <= 2 && !(p.mask[q[i].s0] >> row[i] & 1)) { p.oct[ci] = int8_t(up[i]); p.raised++; }
        for (int s = q[i].s0; s < q[i].s1; s++) { p.mask[s] |= uint16_t(1u << row[i]); if (cell_oct[s * Arp::ROWS + row[i]] < 0) { cell_oct[s * Arp::ROWS + row[i]] = int8_t(eff); cell_note[s * Arp::ROWS + row[i]] = int16_t(q[i].note); } }
        p.vel[q[i].s0] = uint8_t(std::max<int>(p.vel[q[i].s0], q[i].vel));
        last = std::max(last, q[i].s1 - 1);
        p.notes++;
    }
    // velocities as a percentage of the loudest step (100 = as played)
    int top = 1;
    for (int s = 0; s < Arp::STEPS; s++) top = std::max<int>(top, p.vel[s]);
    for (int s = 0; s < Arp::STEPS; s++) p.vel[s] = uint8_t(p.vel[s] ? std::max(10, p.vel[s] * 100 / top) : 100);
    // whole bars (as many as fit)
    const int bars_used = std::max(1, int(std::ceil((last + 1) / spb - 1e-6)));
    p.length = std::min(Arp::STEPS, std::max(1, int(std::ceil(bars_used * spb - 1e-6))));
    p.bars_used = std::min({p.bars, bars_used, std::max(1, int(Arp::STEPS / spb + 1e-6))});
    return p;
}

}  // namespace omni

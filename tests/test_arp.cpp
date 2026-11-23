// Omni Sampler 2.4: the ARP tab. The engine (scales, timing, SEQ / ARP, latch, the host's song position), the MIDI
// file reader, and the plugin (the grid, pages, a MIDI file from <library>/MIDI, the pattern in the state).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include "../src/engine/arp.hpp"
#include "../src/engine/smf.hpp"
extern "C" {
#include "engine.h"
}

using namespace omni;

static int fails = 0;
extern "C" int mpc_engine_changed(void *inst, char *key, int len);

static void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

struct Rec : Arp::Out {   // the notes the ARP plays, with their frame
    struct Ev { int64_t at; int note, vel; bool on; };
    std::vector<Ev> ev;
    int64_t now = 0;
    void arp_on(int n, int v) override { ev.push_back({now, n, v, true}); }
    void arp_off(int n) override { ev.push_back({now, n, 0, false}); }
    std::vector<Ev> ons() const { std::vector<Ev> o; for (auto &e : ev) if (e.on) o.push_back(e); return o; }
};

// run the ARP for `frames` in host blocks of 128 (song position given when ppq >= 0)
static void run(Arp &a, Rec &r, int frames, double bpm, bool playing = false, double ppq0 = -1) {
    for (int done = 0; done < frames; done += 128) {
        const double ppq = ppq0 >= 0 ? ppq0 + double(r.now) * bpm / 60.0 / 44100.0 : -1;
        a.transport(ppq, bpm, playing, ppq >= 0);
        int d = 0;
        while (d < 128) {
            int n = a.process(r, 128 - d);
            a.advance(n);
            r.now += n;
            d += n;
        }
    }
}

static std::vector<uint8_t> smf_bytes(int ppq, const std::vector<std::vector<int>> &notes /* tick, len, note, vel */, int ts_num = 0) {
    std::vector<std::pair<int, std::vector<uint8_t>>> evs;
    for (auto &n : notes) {
        evs.push_back({n[0], {0x90, uint8_t(n[2]), uint8_t(n[3])}});
        evs.push_back({n[0] + n[1], {0x80, uint8_t(n[2]), 0}});
    }
    std::stable_sort(evs.begin(), evs.end(), [](auto &x, auto &y) { return x.first < y.first; });
    std::vector<uint8_t> trk;
    if (ts_num) trk.insert(trk.end(), {0x00, 0xFF, 0x58, 0x04, uint8_t(ts_num), 0x02, 0x18, 0x08});   // n/4
    int last = 0;
    for (auto &e : evs) {
        uint32_t d = uint32_t(e.first - last); last = e.first;
        uint8_t v[4]; int k = 0; v[k++] = d & 0x7F; while (d >>= 7) v[k++] = 0x80 | (d & 0x7F);
        while (k) trk.push_back(v[--k]);
        trk.insert(trk.end(), e.second.begin(), e.second.end());
    }
    trk.insert(trk.end(), {0x00, 0xFF, 0x2F, 0x00});
    std::vector<uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, uint8_t(ppq >> 8), uint8_t(ppq), 'M', 'T', 'r', 'k'};
    uint32_t L = uint32_t(trk.size());
    f.insert(f.end(), {uint8_t(L >> 24), uint8_t(L >> 16), uint8_t(L >> 8), uint8_t(L)});
    f.insert(f.end(), trk.begin(), trk.end());
    return f;
}

static std::string get(const mpc_engine_t *e, void *inst, const char *key) {
    char buf[4096];
    int n = e->get_param(inst, key, buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}

// 2.4.29 (audit F7): the state keeps the notes' gates + expression as one compact "arp_x" (plugin.cpp arp_x_text).
// Decoded here back into the old "arp_gates" ("<step 2><row 1><gate 2>") and "arp_nx"
// ("<step 2><row 1><vel 2><skip 2><ratchet 1><oct+2 1><slide 1>", hex) texts, cell for cell as the old writers made them,
// so the checks below keep their meaning.
static void arp_x_to_old(const std::string &t, std::string &gates, std::string &nx) {
    auto a64 = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : c >= 'a' && c <= 'z' ? c - 'a' + 36
                                 : c == '-' ? 62 : c == '_' ? 63 : -1; };
    size_t i = 0;
    auto rd = [&](int n) { int v = 0; for (; n > 0; n--) { const int d = i < t.size() ? a64(t[i]) : -1; if (d < 0) return -1; v = v << 6 | d; i++; } return v; };
    int prev = -1, g = 0, v = 0, k = 0, x = 0;
    gates.clear(); nx.clear();
    while (i < t.size()) {
        const int h = rd(1); if (h < 0) break;
        const int gap = rd(h & 32 ? 2 : 1); if (gap < 0) break;
        if (!(h & 16)) {
            g = v = k = x = 0;
            if (h & 1) { g = rd(2); if (g < 0) break; }
            if (h & 2) { v = rd(2); if (v < 0) break; }
            if (h & 4) { k = rd(2); if (k < 0) break; }
            if (h & 8) { x = rd(1); if (x < 0) break; }
        }
        const int ci = prev < 0 ? gap : prev + 1 + gap;
        prev = ci;
        const int xr = x ^ 8, rt = (xr & 3) + 1, oc2 = xr >> 2 & 7, sl = xr & 32 ? 1 : 0;
        char b[16];
        if (g) { std::snprintf(b, sizeof b, "%02x%x%02x", ci / 16, ci % 16, g); gates += b; }
        if (v || k || rt > 1 || oc2 != 2 || sl) { std::snprintf(b, sizeof b, "%02x%x%02x%02x%x%x%x", ci / 16, ci % 16, v, k, rt, oc2, sl); nx += b; }
    }
}

int main() {
    // ---- scales
    {
        int got[8];
        for (int r = 0; r < 8; r++) got[r] = Arp::seq_note(48, r, 1, 0);   // C3, C major
        check(got[0] == 48 && got[1] == 50 && got[2] == 52 && got[3] == 53 && got[4] == 55 && got[7] == 60, "SEQ rows climb C major from C");
        check(Arp::seq_note(50, 2, 1, 0) == 53, "row 3 from D in C major is F (transposed within the scale)");
        check(Arp::seq_note(49, 0, 1, 0) == 49 && Arp::seq_note(49, 1, 1, 0) == 51, "an off-scale key (C#): the pattern a semitone up (C# D#)");
        check(Arp::seq_note(57, 1, 11, 9) == 60, "A minor pentatonic: A then C");
        check(Arp::seq_note(60, 3, 0, 0) == 63, "Chromatic: rows are semitones");
        int pool[3] = {48, 52, 55};
        check(Arp::arp_note(pool, 3, 0) == 48 && Arp::arp_note(pool, 3, 2) == 55 && Arp::arp_note(pool, 3, 3) == 60 && Arp::arp_note(pool, 3, 5) == 67,
              "ARP rows: the held notes, then an octave up");
    }
    // ---- timing: 1/16 at 120 BPM = 5512.5 frames a step, free running from the key
    {
        Arp a; Rec r;
        a.settings.on.store(1); a.settings.mode.store(0); a.settings.rate.store(2); a.settings.length.store(4);
        a.settings.scale.store(1); a.settings.root.store(0); a.settings.gate.store(0.5f);
        a.mask[0].store(1); a.mask[1].store(1 << 2); a.mask[2].store(1 << 4); a.mask[3].store(1 | 1 << 7);
        a.key_on(48, 100);
        run(a, r, 44100, 120);
        auto on = r.ons();
        bool notes = on.size() >= 9 && on[0].note == 48 && on[1].note == 52 && on[2].note == 55 && on[3].note == 48 && on[4].note == 60;
        std::printf("arp: first steps at %lld %lld %lld frames; notes %d %d %d %d,%d\n", (long long)on[0].at, (long long)on[1].at,
                    (long long)on[2].at, on[0].note, on[1].note, on[2].note, on[3].note, on[4].note);
        check(notes, "SEQ plays the rows from the key: C E G, then C+C' (a chord step)");
        check(on[0].at == 0 && std::llabs(on[1].at - 5513) <= 1 && std::llabs(on[2].at - 11025) <= 1, "steps land 1/16 apart at 120 BPM, the first at once");
        int64_t off_at = -1;
        for (auto &e : r.ev) if (!e.on && e.note == 48) { off_at = e.at; break; }
        check(std::llabs(off_at - 2756 + 32) < 40, "GATE 50%: the note ends half way through its step");
        // the pattern repeats after LENGTH steps
        check(on.size() >= 6 && on[5].note == 48 && std::llabs(on[5].at - 4 * 5512.5) <= 2, "LENGTH 4: step 5 plays step 1 again");
        // transpose: a new key moves the pattern
        Rec r2; r2.now = r.now;
        a.key_on(50, 90);
        run(a, r2, 6000, 120);
        auto on2 = r2.ons();
        check(!on2.empty() && (on2[0].note == 50 || on2[0].note == 53 || on2[0].note == 57 || on2[0].note == 62), "the latest key transposes the pattern (from D)");
        a.key_off(48); a.key_off(50);
        Rec r3; r3.now = r2.now;
        run(a, r3, 20000, 120);
        check(r3.ons().empty(), "keys up: it stops");
        // swing delays the even-numbered (2nd, 4th) steps
        Arp b; Rec rb;
        b.settings.on.store(1); b.settings.length.store(4); b.settings.swing.store(0.5f); b.settings.gate.store(0.3f);
        for (int s = 0; s < 4; s++) b.mask[s].store(1);
        b.key_on(60, 100);
        run(b, rb, 30000, 120);
        auto ob = rb.ons();
        check(ob.size() >= 3 && std::llabs(ob[1].at - int64_t(5512.5 * 1.25)) <= 2 && std::llabs(ob[2].at - 11025) <= 2, "SWING 50%: the 2nd step a quarter step late, the 3rd on time");
    }
    // ---- ARP mode and latch
    {
        Arp a; Rec r;
        a.settings.on.store(1); a.settings.mode.store(1); a.settings.length.store(4); a.settings.latch.store(1);
        for (int s = 0; s < 4; s++) a.mask[s].store(uint16_t(1 << s));
        a.key_on(60, 100); a.key_on(64, 100); a.key_on(67, 100);
        a.key_off(60); a.key_off(64); a.key_off(67);   // LATCH keeps them
        run(a, r, 25000, 120);
        auto on = r.ons();
        check(on.size() >= 4 && on[0].note == 60 && on[1].note == 64 && on[2].note == 67 && on[3].note == 72, "ARP: rows 1-4 = C E G and C an octave up (latched)");
        // a new chord replaces the latched one. Every step on row 1 here: 2.4.11 walks the scale up the rows when ONE key is
        // held (row 1 = the key itself), so this checks only the replacing (the scale walk has its own check below)
        for (int s = 0; s < 4; s++) a.mask[s].store(1);
        a.key_on(62, 100); a.key_off(62);
        Rec r2; r2.now = r.now;
        run(a, r2, 6000, 120);
        bool all_d = !r2.ons().empty();
        for (auto &e : r2.ons()) all_d = all_d && e.note == 62;
        check(all_d, "LATCH: a new key after release replaces the chord");
        a.settings.on.store(0);
        Rec r3; r3.now = r2.now;
        run(a, r3, 2000, 120);
        check(r3.ons().empty() && !a.busy(), "ARP off: silent, keys forgotten");
    }
    // ---- the host's song position: steps on the grid, from where the song is
    {
        Arp a; Rec r;
        a.settings.on.store(1); a.settings.length.store(16);
        for (int s = 0; s < 16; s++) a.mask[s].store(1);
        a.key_on(60, 100);
        run(a, r, 30000, 100, true, 1.1);   // playing from beat 1.1, 100 BPM: the next 1/16 is beat 1.25
        auto on = r.ons();
        const double fpb = 60.0 / 100 * 44100;
        check(!on.empty() && std::llabs(on[0].at - int64_t((1.25 - 1.1) * fpb)) <= 2, "with the transport playing, the first step lands on the song's next 1/16");
        check(on.size() >= 2 && std::llabs((on[1].at - on[0].at) - int64_t(0.25 * fpb)) <= 2, "and the next a 1/16 of 100 BPM later");
        check(a.step_now.load() >= 0, "the playhead shows a step");
    }
    // ---- MIDI files
    {
        auto bytes = smf_bytes(96, {{0, 24, 60, 100}, {24, 24, 64, 80}, {48, 24, 67, 100}, {48, 24, 72, 50}, {96 * 4 + 24, 24, 62, 100}});
        SmfSong song = smf_parse(bytes);
        check(song.notes.size() == 5 && song.division == 96, "MIDI file: 5 notes, 96 ticks a beat");
        ArpImport how;   // Auto snap, SEQ, C major
        ArpPattern p = smf_to_pattern(song, how);
        check(p.mask[0] == 1 && p.mask[1] == (1 << 2) && p.mask[2] == ((1 << 4) | (1 << 7)) && p.mask[17] == (1 << 1), "notes on steps 1, 2, 3 (a chord), 18 as C major rows");
        check(p.length == 32 && p.lowest == 60, "length: two pages (the last note on step 18), from C3");
        check(p.vel[1] == 80, "a quieter note keeps its level (80% of the loudest)");
        bool threw = false;
        try { std::vector<uint8_t> junk = {'R', 'I', 'F', 'F', 1, 2, 3}; smf_parse(junk); } catch (const std::exception &) { threw = true; }
        check(threw, "not a MIDI file: an error, not a crash");
    }
    // ---- 2.4.1: TIE, ORDER, STRUM, the key, a frozen host clock, MIDI SNAP, key detection, time signatures
    {
        const double sf = 5512.5;   // frames a 1/16 step at 120 BPM
        Arp a;
        a.settings.on = 1; a.settings.mode = 0; a.settings.gate = 0.5f; a.settings.length = 16; a.settings.tie = 1;
        for (int s = 0; s < 4; s++) a.mask[s] = 1;   // a run of 4 on row 1
        a.mask[6] = 1;                               // and a single dot
        Rec r;
        a.key_on(60, 100);
        run(a, r, int(sf * 8), 120);
        auto on = r.ons();
        int64_t off_at = -1;
        for (auto &e : r.ev) if (!e.on && e.note == 60) { off_at = e.at; break; }
        check(on.size() == 2 && std::llabs(on[1].at - int64_t(6 * sf)) <= 2, "TIE: the run of 4 is one note, the dot after it another");
        check(std::llabs(off_at - int64_t(3.5 * sf - 32)) < 40, "TIE: the held note ends half way through the run's last step");
        Arp b;
        b.settings.on = 1; b.settings.gate = 0.5f; b.settings.length = 16;
        for (int s = 0; s < 4; s++) b.mask[s] = 1;
        Rec rb;
        b.key_on(60, 100);
        run(b, rb, int(sf * 4), 120);
        check(rb.ons().size() == 4, "TIE off: each dot plays");
    }
    {
        const double sf = 5512.5;
        auto order_run = [&](int order, int oct, int steps) {
            Arp a;
            a.settings.on = 1; a.settings.mode = 1; a.settings.order = order; a.settings.octaves = oct; a.settings.length = 16;
            for (int s = 0; s < 16; s++) a.mask[s] = 1;
            Rec r;
            a.key_on(64, 100); a.key_on(60, 100); a.key_on(67, 100);   // E C G (as played)
            run(a, r, int(sf * steps) - 64, 120);
            std::vector<int> n;
            for (auto &e : r.ons()) n.push_back(e.note);
            return n;
        };
        check(order_run(Arp::O_UP, 2, 7) == std::vector<int>({60, 64, 67, 72, 76, 79, 60}), "ORDER Up, 2 octaves: C E G C' E' G' C");
        check(order_run(Arp::O_DOWN, 1, 4) == std::vector<int>({67, 64, 60, 67}), "ORDER Down: G E C G");
        check(order_run(Arp::O_UPDOWN, 1, 6) == std::vector<int>({60, 64, 67, 64, 60, 64}), "ORDER Up/Down: C E G E C E");
        check(order_run(Arp::O_DOWNUP, 1, 5) == std::vector<int>({67, 64, 60, 64, 67}), "ORDER Down/Up: G E C E G");
        check(order_run(Arp::O_PLAYED, 1, 4) == std::vector<int>({64, 60, 67, 64}), "ORDER As Played: E C G E");
        auto rnd = order_run(Arp::O_RANDOM, 1, 12);
        bool ok = rnd.size() >= 12;
        for (size_t i = 1; i < rnd.size(); i++) ok = ok && rnd[i] != rnd[i - 1] && (rnd[i] == 60 || rnd[i] == 64 || rnd[i] == 67);
        check(ok, "ORDER Random: the held notes, never the same twice running");
        check(order_run(Arp::O_CHORD, 1, 2).size() == 6, "ORDER Chord: all three on each step");
        // STRUM: the chord over the step, low to high; DOWN high to low
        for (int dir = 0; dir < 2; dir++) {
            Arp a;
            a.settings.on = 1; a.settings.mode = 1; a.settings.order = Arp::O_CHORD; a.settings.strum = 1.0f; a.settings.strum_dir = dir;
            a.mask[0] = 1;
            Rec r;
            a.key_on(60, 100); a.key_on(64, 100); a.key_on(67, 100);
            run(a, r, int(sf) + 256, 120);
            auto on = r.ons();
            bool good = on.size() == 3 && on[0].at == 0 && std::llabs(on[1].at - int64_t(sf / 2)) <= 2 && std::llabs(on[2].at - int64_t(sf)) <= 2;
            good = good && (dir == 0 ? on[0].note == 60 && on[2].note == 67 : on[0].note == 67 && on[2].note == 60);
            check(good, dir == 0 ? "STRUM Up 100%: C, E half a step later, G a step later" : "STRUM Down: G first, C last");
        }
    }
    {   // SEQ in the key: base 2 (the file's lowest note E3 in C major); C3 plays it as written
        check(Arp::seq_note(60, 0, 1, 0, 2) == 64 && Arp::seq_note(60, 2, 1, 0, 2) == 67, "SEQ base 2: from C3, row 1 E3, row 3 G3 (as written)");
        check(Arp::seq_note(62, 0, 1, 0, 2) == 65, "a D3 key: a scale step up (F3)");
        check(Arp::seq_note(48, 0, 1, 0, 2) == 52, "a C2 key: an octave down (E2)");
        check(Arp::seq_note(67, 0, 2, 9, 0) == 67 && Arp::seq_note(69, 2, 2, 9, 0) == 72, "A minor: G3 plays itself; from A3 row 3 is C4");
        check(Arp::seq_note(64, 0, 1, 7, 0) == 64 && Arp::seq_note(64, 1, 1, 7, 0) == 66, "ROOT G (G major): E3 then F#3");
    }
    {   // 2.4.2: a note's expression: RATCHET, CHANCE, VEL, NOTE OCT, SLIDE
        const double sf = 5512.5;
        auto one = [&](auto setup, int frames) {
            Arp a;
            a.settings.on = 1; a.settings.length = 16; a.settings.gate = 0.5f;
            a.mask[0] = 1; a.mask[4] = 1;   // row 1 at steps 1 and 5
            setup(a);
            Rec r;
            a.key_on(60, 100);
            run(a, r, frames, 120);
            return r;
        };
        Rec rt = one([](Arp &a) { a.nratch[0] = 3; }, int(sf * 2));
        auto on = rt.ons();
        check(on.size() == 3 && on[0].at == 0 && std::llabs(on[1].at - int64_t(sf / 3)) <= 2 && std::llabs(on[2].at - int64_t(2 * sf / 3)) <= 2,
              "RATCHET 3: three strikes inside the step");
        Rec ch = one([](Arp &a) { a.nskip[0] = 100; }, int(sf * 6));
        check(ch.ons().size() == 1 && ch.ons()[0].at >= int64_t(4 * sf) - 2, "CHANCE 0%: the note never plays (the other still does)");
        int plays = 0;
        {
            Arp a;
            a.settings.on = 1; a.settings.length = 1; a.settings.gate = 0.5f;
            a.mask[0] = 1; a.nskip[0] = 50;
            Rec r;
            a.key_on(60, 100);
            run(a, r, int(sf * 100) - 64, 120);
            plays = int(r.ons().size());
        }
        check(plays > 25 && plays < 75, "CHANCE 50%: about half of 100 steps play");
        Rec vv = one([](Arp &a) { a.nvel[0] = 50; }, 256);
        check(!vv.ons().empty() && vv.ons()[0].vel == 50, "VEL 50%: the note at half the velocity");
        Rec oc = one([](Arp &a) { a.noct[0] = 1; }, 256);
        check(!oc.ons().empty() && oc.ons()[0].note == 72, "NOTE OCT +1: an octave up");
        Rec sl = one([](Arp &a) { a.nslide[0] = 1; }, int(sf * 2));
        int64_t off_at = -1;
        for (auto &e : sl.ev) if (!e.on && e.note == 60) { off_at = e.at; break; }
        check(off_at > int64_t(sf), "SLIDE: the note holds past its step, into the next note");
    }
    {   // 2.4.2 GROOVE: DIRECTION, CHAIN, HUMANIZE
        const double sf = 5512.5;
        auto rows_played = [&](auto setup, int steps) {
            Arp a;
            a.settings.on = 1; a.settings.length = 4; a.settings.gate = 0.5f; a.settings.mode = 1;   // ARP Pattern: row r = r-th key
            for (int s = 0; s < 4; s++) a.mask[s] = uint16_t(1u << s);   // step s: row s+1
            setup(a);
            Rec r;
            for (int k : {60, 62, 64, 65}) a.key_on(k, 100);
            run(a, r, int(sf * steps) - 64, 120);
            std::vector<int> n;
            for (auto &e : r.ons()) n.push_back(e.note);
            return n;
        };
        check(rows_played([](Arp &) {}, 5) == std::vector<int>({60, 62, 64, 65, 60}), "DIRECTION Forward: 1 2 3 4 1");
        check(rows_played([](Arp &a) { a.settings.direction = 1; }, 5) == std::vector<int>({65, 64, 62, 60, 65}), "DIRECTION Back: 4 3 2 1 4");
        check(rows_played([](Arp &a) { a.settings.direction = 2; }, 7) == std::vector<int>({60, 62, 64, 65, 64, 62, 60}), "DIRECTION Ping-Pong: 1 2 3 4 3 2 1");
        {   // 2.4.11: ARP Pattern with ONE key held: the rows walk the scale from it (was: the key in octaves)
            Arp a1;
            a1.settings.on = 1; a1.settings.length = 4; a1.settings.gate = 0.5f; a1.settings.mode = 1;
            for (int s = 0; s < 4; s++) a1.mask[s] = uint16_t(1u << s);
            Rec r1;
            a1.key_on(60, 100);
            run(a1, r1, int(sf * 4) - 64, 120);
            std::vector<int> n1;
            for (auto &e : r1.ons()) n1.push_back(e.note);
            check(n1 == std::vector<int>({60, 62, 64, 65}), "ARP Pattern, one key held: rows 1-4 play C D E F (the scale), not C in octaves");
        }
        auto rw = rows_played([](Arp &a) { a.settings.direction = 3; }, 20);
        check(rw.size() == 20, "DIRECTION Random walk: a note every step");
        // CHAIN B A: page B (steps 17-32) then page A
        Arp c;
        c.settings.on = 1; c.settings.gate = 0.5f; c.settings.length = 16;
        c.mask[0] = 1; c.mask[16] = 2;   // page A step 1 row 1, page B step 1 row 2
        c.settings.chain[0] = 1; c.settings.chain[1] = 0;
        Rec rc;
        c.key_on(60, 100);
        run(c, rc, int(sf * 32) + 64, 120);
        auto co = rc.ons();
        check(co.size() == 3 && co[0].note == 62 && co[1].note == 60 && co[2].note == 62 && std::llabs(co[1].at - int64_t(16 * sf)) <= 2,
              "CHAIN B A: page B's note, page A's 16 steps later, then B again");
        // HUMANIZE: velocity varies, time a little late (never early)
        Arp h;
        h.settings.on = 1; h.settings.length = 1; h.settings.gate = 0.5f; h.settings.human_vel = 1.0f; h.settings.human_time = 1.0f;
        h.mask[0] = 1;
        Rec rh;
        h.key_on(60, 100);
        run(h, rh, int(sf * 40), 120);
        std::set<int> vs;
        bool late_ok = true;
        auto ho = rh.ons();
        for (size_t i = 0; i < ho.size(); i++) {
            vs.insert(ho[i].vel);
            const int64_t d = ho[i].at - int64_t(double(i) * sf);
            late_ok = late_ok && d >= -2 && d <= int64_t(sf * 0.25) + 2;
        }
        check(ho.size() >= 38 && vs.size() > 5, "HUMANIZE VEL: the velocities vary");
        check(late_ok, "HUMANIZE TIME: notes up to a quarter step late, never early");
    }
    {   // MPC says "playing" but its position stands still: run on our own clock
        Arp a;
        a.settings.on = 1; a.settings.length = 16;
        for (int s = 0; s < 16; s++) a.mask[s] = 1;
        Rec r;
        a.key_on(60, 100);
        for (int done = 0; done < 5512 * 6; done += 128) {
            a.transport(8.0, 120, true, true);   // frozen at beat 8
            int d = 0;
            while (d < 128) { int n = a.process(r, 128 - d); a.advance(n); r.now += n; d += n; }
        }
        check(r.ons().size() >= 5, "a frozen song position: the steps still play (free clock)");
    }
    {   // MIDI SNAP 1 Bar, ARP mode: chords as chord-tone rows, held for their bars
        auto bytes = smf_bytes(96, {{0, 96 * 4, 60, 100}, {0, 96 * 4, 64, 100}, {0, 96 * 4, 67, 100},
                                    {96 * 4 + 10, 96 * 4, 62, 100}, {96 * 4 + 10, 96 * 4, 65, 100}, {96 * 4 + 10, 96 * 4, 69, 100}});
        ArpImport how;
        how.snap = 5; how.mode = 1;
        ArpPattern p = smf_to_pattern(smf_parse(bytes), how);
        check(p.rate == 0 && p.length == 8, "1 Bar: steps of 1/4, two bars = 8 steps");
        check(p.mask[0] == 7 && p.mask[2] == 7 && p.mask[3] == 0 && p.mask[4] == 7 && p.mask[7] == 7,
              "chords snapped to the bar, rows 1-3 (lowest first), held; the first a step short so the second plays again");
        how.snap = 1;
        ArpPattern q = smf_to_pattern(smf_parse(bytes), how);
        check(q.rate == 3 && q.length == 64 && q.bars_used == 2, "1/32: steps of 1/32, the two bars fill 64 steps");
        how.snap = 0;
        ArpPattern au = smf_to_pattern(smf_parse(bytes), how);
        check(au.rate == 3 || au.rate == 2, "Auto: a note 10 ticks late needs a fine grid");
        // the key: D F# A given C major -> found (D major)
        auto dm = smf_bytes(96, {{0, 96, 62, 100}, {96, 96, 66, 100}, {192, 96, 69, 100}});
        ArpImport k;
        ArpPattern pk = smf_to_pattern(smf_parse(dm), k);
        check(pk.key_found && pk.scale == 1 && pk.root == 2 && pk.base == 0 && pk.rate == 0,
              "a file in D major, C major chosen: the key is taken from the file (D major; D3 plays it as written), steps of 1/4");
        // 3/4: a bar is 3 beats
        auto w = smf_bytes(96, {{0, 96, 60, 100}, {96 * 3, 96, 64, 100}}, 3);
        SmfSong ws = smf_parse(w);
        ArpPattern pw = smf_to_pattern(ws, k);
        check(ws.ts_num == 3 && ws.ts_den == 4 && pw.length == 6, "3/4: two bars of 1/4 steps = 6 steps");
    }
    // ---- the plugin
    {
        const std::string lib = "/tmp/omni-arp-test/library";
        std::system("rm -rf /tmp/omni-arp-test");
        mkdir("/tmp/omni-arp-test", 0755); mkdir(lib.c_str(), 0755); mkdir((lib + "/MIDI").c_str(), 0755);
        auto bytes = smf_bytes(96, {{0, 24, 60, 100}, {24, 24, 64, 100}, {48, 24, 67, 100}, {72, 24, 72, 100}});
        FILE *f = std::fopen((lib + "/MIDI/Rise.mid").c_str(), "wb");
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        const mpc_engine_t *e = mpc_engine();
        setenv("OMNI_SAMPLER_LIBRARY", lib.c_str(), 1);
        void *y = e->create("/tmp/omni-arp-test/data");
        unsetenv("OMNI_SAMPLER_LIBRARY");
        std::vector<int16_t> buf(256);
        auto wait_for = [&](const char *key, const std::string &want) {
            for (int i = 0; i < 2000; i++) {
                if (get(e, y, key).find(want) != std::string::npos) return true;
                e->render(y, buf.data(), 128);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        };
        check(wait_for("arp_name", "Rise"), "the MIDI folder is scanned: Rise is picked");
        e->set_param(y, "arp_load", "1"); e->set_param(y, "arp_load", "0");
        check(wait_for("arp_info", "4 notes"), "LOAD: 4 notes");
        std::printf("arp: %s\n", get(e, y, "arp_info").c_str());
        check(get(e, y, "arp_c1_1") != "0" && get(e, y, "arp_c3_2") != "0" && get(e, y, "arp_c5_3") != "0" && get(e, y, "arp_c8_4") != "0" && get(e, y, "arp_c2_1") == "0",
              "the grid shows the file: rows 1, 3, 5, 8 on steps 1-4");
        check(get(e, y, "arp_row1") == "C3" && get(e, y, "arp_row3") == "E3", "row names: C major up from the root at C3");
        // DRAW (the pad over the grid; x: a 52 px margin + 16 columns of 52; y up: 14 px + 16 rows of 27) and TAP helpers
        auto pad = [&](double col, double row) {   // a cell's centre (col, row from 0), or the resting (0, 0)
            char bx[32], by[32];
            std::snprintf(bx, sizeof bx, "%g", col < 0 ? 0.0 : (52 + (col + 0.5) * 52) / 884.0);
            std::snprintf(by, sizeof by, "%g", row < 0 ? 0.0 : (14 + (row + 0.5) * 27) / 446.0);
            e->set_param(y, "arp_xy_x", bx);
            e->set_param(y, "arp_xy_y", by);
        };
        auto lit = [&](int r, int c) { char k[24]; std::snprintf(k, sizeof k, "arp_c%d_%d", r + 1, c + 1); return get(e, y, k) != "0"; };   // (a picture code: 0 = empty)
        auto run_of = [&](int r, int a, int b) { for (int c = 0; c < 16; c++) if (lit(r, c) != (c >= a && c <= b)) return false; return true; };
        // a dot drawn on page B, then back
        e->set_param(y, "arp_page", "1");
        check(get(e, y, "arp_c1_1") == "0", "page B is empty");
        pad(4, 1); pad(-1, -1);
        e->set_param(y, "arp_page", "0");
        check(get(e, y, "arp_c2_5") == "0" && get(e, y, "arp_c1_1") != "0", "page A again: its own dots");
        // the pattern is in the state
        std::string st = get(e, y, "state");
        check(st.find("arp_pattern=") != std::string::npos, "the state has the pattern");
        void *z = e->create("/tmp/omni-arp-test/data");
        e->set_param(z, "state", st.c_str());
        e->set_param(z, "arp_page", "1");
        check(get(e, z, "arp_c2_5") != "0", "a project brings its pattern back (page B's dot)");
        e->set_param(z, "arp_page", "0");
        check(get(e, z, "arp_c8_4") != "0", "and page A's");
        // state version 2 leaves defaults out: loading it resets what it doesn't name
        e->set_param(z, "cutoff", "30");
        e->set_param(z, "state", "omni-sampler 2\nrev_mix=25\n");
        std::string zs = get(e, z, "state");
        check(zs.find("cutoff=") == std::string::npos && zs.find("rev_mix=25") != std::string::npos && zs.find("arp_pattern") == std::string::npos,
              "a version 2 state: the settings it leaves out are back at their defaults (cutoff, the pattern)");
        check(st.size() < 1500, "the state is small (defaults left out)");
        std::printf("arp: state %zu bytes with a pattern and a few settings\n", st.size());
        e->destroy(z);
        // CLEAR PAGE clears only the page shown
        e->set_param(y, "arp_page", "1"); e->set_param(y, "arp_clear", "1"); e->set_param(y, "arp_clear", "0");
        check(get(e, y, "arp_c2_5") == "0", "CLEAR PAGE: page B empty");
        e->set_param(y, "arp_page", "0");
        check(get(e, y, "arp_c1_1") != "0", "page A kept");
        // plays: the synth on, ARP on, a key held -> sound in steps; off -> the key plays as itself
        e->set_param(y, "syn_on", "1"); e->set_param(y, "arp_on", "1"); e->set_param(y, "arp_gate", "40");
        e->set_param(y, "syn_attack", "0"); e->set_param(y, "syn_release", "5");   // short notes: the steps are countable
        for (int i = 0; i < 100; i++) e->render(y, buf.data(), 128);
        uint8_t on[3] = {0x90, 60, 100};
        e->midi(y, on, 3);
        std::vector<double> lv;
        for (int i = 0; i < 344 * 2; i++) {
            if (e->song_pos) e->song_pos(y, -1, 120, 0, 128);
            e->render(y, buf.data(), 128);
            double q = 0; for (int k = 0; k < 256; k++) q += double(buf[size_t(k)]) * buf[size_t(k)];
            lv.push_back(std::sqrt(q / 256));
        }
        int rises = 0;
        for (size_t i = 1; i < lv.size(); i++) if (lv[i] > 300 && lv[i - 1] < 150) rises++;
        std::printf("arp: %d note starts in 2 s at 120 BPM (1/16, gate 40%%)\n", rises);
        check(rises >= 3 && rises <= 5, "ARP on, a key held: it plays the file's 4 steps (one bar in 2 s at 120 BPM)");
        check(get(e, y, "arp_row1") == "C3", "row names follow the key held (C3)");
        uint8_t off[3] = {0x80, 60, 0};
        e->midi(y, off, 3);
        // 2.4.1 DRAW: the pad over the grid (x: a 52 px margin + 16 columns of 52; y up: 14 px + 16 rows of 27)
        e->set_param(y, "arp_clear", "1"); e->set_param(y, "arp_clear", "0");
        e->set_param(y, "arp_tie", "0");
        pad(2, 0); pad(3, 0); pad(5, 0); pad(-1, -1);
        check(run_of(0, 2, 5) && get(e, y, "arp_tie") == "1", "DRAW: a drag along row 1 from step 3 to 6 draws a held note (TIE on)");
        pad(5, 0); pad(7, 0); pad(-1, -1);
        check(run_of(0, 2, 7), "DRAW: dragging the note's end longer (to step 8)");
        pad(7, 0); pad(4, 0); pad(-1, -1);
        check(run_of(0, 2, 4), "DRAW: and shorter (to step 5)");
        pad(3, 0); pad(6, 2); pad(-1, -1);
        check(run_of(0, -1, -1) && run_of(2, 5, 7), "DRAW: dragging the note by its middle moves it (row 3, steps 6-8)");
        pad(6, 2); pad(-1, -1);
        check(run_of(2, -1, -1), "DRAW: a tap on a note deletes it");
        pad(9, 4); pad(-1, -1);
        check(run_of(4, 9, 9), "DRAW: a tap on an empty cell puts a dot");
        // SHIFT, the lights, FOLLOW, the status
        e->set_param(y, "arp_shift_r", "1"); e->set_param(y, "arp_shift_r", "0");
        check(run_of(4, 10, 10), "SHIFT right: a step later");
        // TAP selects only (MPC's toggle sends 127 or 0): an empty cell stays empty; a tap on a note selects it (amber);
        // MPC repeating the code back is not a tap; NOTE GATE sets the selected note's own gate (its picture shows it); a
        // tap on the selected note deselects it
        e->set_param(y, "arp_c7_2", "127");
        check(get(e, y, "arp_c7_2") == "0", "TAP on an empty cell: nothing drawn (TAP only selects)");
        pad(1, 6); pad(-1, -1);   // (DRAW puts the dot: selected)
        e->set_param(y, "arp_c7_2", "127");   // TAP on the selected one: deselected
        check(std::atoi(get(e, y, "arp_c7_2").c_str()) < 21, "TAP on the selected note: deselected (cyan)");
        e->set_param(y, "arp_c7_2", "127");
        const std::string c1 = get(e, y, "arp_c7_2");
        check(std::atoi(c1.c_str()) >= 21 && std::atoi(c1.c_str()) <= 40, "TAP on a note: selected (amber)");
        e->set_param(y, "arp_c7_2", c1.c_str());
        check(get(e, y, "arp_c7_2") == c1, "MPC repeating the cell's code back is not a tap");
        check(get(e, y, "arp_sel") == "1", "a note selected: arp_sel On (the panel shows the note's knobs)");
        e->set_param(y, "arp_n_ratchet", "3"); e->set_param(y, "arp_n_vel", "150"); e->set_param(y, "arp_n_chance", "75");
        e->set_param(y, "arp_n_oct", "-1"); e->set_param(y, "arp_n_slide", "1");
        { const std::string st3 = get(e, y, "arp_status");
          check(st3.find("x3") != std::string::npos && st3.find("VEL 150%") != std::string::npos && st3.find("75% CHANCE") != std::string::npos &&
                st3.find("OCT -1") != std::string::npos && st3.find("SLIDE") != std::string::npos, "the note's expression is set and shown"); }
        { char sb[8192]; int n = e->get_param(y, "state", sb, sizeof sb); check(n > 0 && std::strstr(sb, "\narp_x=") != nullptr, "the state keeps the note's expression");
          void *w = e->create("/tmp/omni-arp-test/data2");
          e->set_param(w, "state", sb);
          // 2.4.14 (audit B1): saved with a note selected, it opens with none selected: the panel shows LENGTH / SWING / GATE
          check(get(e, w, "arp_sel") == "0" && get(e, w, "arp_n_ratchet") == "1", "a project opens with no note selected (arp_sel 0)");
          // 2.4.14 (audit B2): UNDO belongs to the pattern that was there: after a project load nothing to undo
          e->set_param(w, "arp_g_undo", "1");
          check(get(e, w, "arp_info").find("Nothing to undo") != std::string::npos, "UNDO after a project load: nothing to undo");
          e->set_param(w, "arp_c7_2", "127");   // select it there
          check(get(e, w, "arp_n_ratchet") == "3" && get(e, w, "arp_n_slide") == "1", "a project brings the note's expression back");
          e->destroy(w); }
        e->set_param(y, "arp_n_ratchet", "1"); e->set_param(y, "arp_n_vel", "100"); e->set_param(y, "arp_n_chance", "100");
        e->set_param(y, "arp_n_oct", "0"); e->set_param(y, "arp_n_slide", "0");
        e->set_param(y, "arp_note_gate", "25");
        check(get(e, y, "arp_c7_2") == "32", "NOTE GATE 25%: the selected dot's own gate, drawn as 2/8 of its step");
        check(get(e, y, "arp_status").find("GATE 25%") != std::string::npos, "the status shows the selected note");
        { char sb[8192]; int n = e->get_param(y, "state", sb, sizeof sb); check(n > 0 && std::strstr(sb, "\narp_x=") != nullptr, "the state keeps the note's gate"); }
        pad(1, 6); pad(-1, -1);   // DRAW: a tap on the selected note deletes it
        check(get(e, y, "arp_c7_2") == "0", "DRAW tap on the selected dot: it goes");
        pad(0, 8); pad(-1, -1); pad(4, 8); pad(-1, -1);   // dots on row 9 at steps 1 and 5 (the second selected)
        e->set_param(y, "arp_shift_r", "1"); e->set_param(y, "arp_shift_r", "0");
        check(get(e, y, "arp_c9_1") != "0" && get(e, y, "arp_c9_5") == "0" && get(e, y, "arp_c9_6") != "0",
              "SHIFT with a note selected: only that note moves (step 5 -> 6), the other stays");
        e->set_param(y, "arp_shift_l", "1"); e->set_param(y, "arp_shift_l", "0");
        e->set_param(y, "arp_shift_l", "1"); e->set_param(y, "arp_shift_l", "0");
        check(get(e, y, "arp_c9_4") != "0" && get(e, y, "arp_c9_1") != "0", "SHIFT left twice: the selected note at step 4");
        pad(3, 8); pad(-1, -1);   // (DRAW taps: the selected one goes, then select the other and it goes too)
        pad(0, 8); pad(-1, -1); pad(0, 8); pad(-1, -1);
        check(get(e, y, "arp_c9_1") == "0" && get(e, y, "arp_c9_4") == "0", "row 9 empty again");
        // the lights show beats and bars at the RATE (1/16: a beat every 4 steps, a bar every 16; 1/8 T: every 3 and 12)
        check(get(e, y, "arp_led1") == "88" && get(e, y, "arp_led5") == "72" && get(e, y, "arp_led2") == "48", "lights at 1/16: bar, beat, step");
        e->set_param(y, "arp_rate", "4");
        check(get(e, y, "arp_led4") == "72" && get(e, y, "arp_led13") == "88", "lights at 1/8 T: a beat every 3 steps, a bar every 12");
        e->set_param(y, "arp_rate", "2");
        e->set_param(y, "arp_octave", "1");
        check(get(e, y, "arp_row1") == "C4", "OCTAVE +1: row 1 is C4 (the key's notes as written, an octave up)");
        e->set_param(y, "arp_octave", "0");
        e->set_param(y, "arp_root", "9"); e->set_param(y, "arp_scale", "2");
        check(get(e, y, "arp_row1") == "A3" && get(e, y, "arp_row3") == "C4", "KEY A minor: the rows are A minor from A3");
        e->set_param(y, "arp_root", "0"); e->set_param(y, "arp_scale", "1");
        pad(10, 4); pad(-1, -1);   // a tap on a note selects it (amber) ...
        check(run_of(4, 10, 10) && get(e, y, "arp_status").find("NOTE") != std::string::npos, "DRAW: a tap on a note selects it");
        pad(10, 4); pad(-1, -1);   // ... a second one deletes it, and nothing stays selected
        check(run_of(4, -1, -1), "DRAW: a tap on the selected note deletes it");
        e->set_param(y, "arp_length", "24");
        check(get(e, y, "arp_led8") == "48" && get(e, y, "arp_page") == "0", "the lights: a step played is dim");
        e->set_param(y, "arp_page", "1");
        check(get(e, y, "arp_led8") == "48" && get(e, y, "arp_led9") == "0", "page B with LENGTH 24: steps 17-24 dim, 25-32 dark");
        e->set_param(y, "arp_page", "0");
        check(get(e, y, "arp_status").find("LEN 24") != std::string::npos, "the status shows the length");
        e->set_param(y, "arp_rate", "3");   // 1/32: page B comes round in half a second
        e->midi(y, on, 3);
        char ck[64];
        bool turned = false;
        for (int i = 0; i < 344 && !turned; i++) {
            if (e->song_pos) e->song_pos(y, -1, 120, 0, 128);
            e->render(y, buf.data(), 128);
            while (mpc_engine_changed(y, ck, sizeof ck) > 0) {}
            turned = get(e, y, "arp_page") == "1";
        }
        check(turned, "FOLLOW: the grid turns to page B when the playhead gets there");
        e->midi(y, off, 3);
        e->set_param(y, "arp_on", "0");
        check(get(e, y, "arp_status").find("OFF") != std::string::npos, "the status: ARP is OFF");
        std::string st2;
        { char sb[8192]; int n = e->get_param(y, "state", sb, sizeof sb); st2 = n > 0 ? sb : ""; }
        check(st2.find("arp_tie=1") != std::string::npos && st2.find("arp_length=24") != std::string::npos, "the state keeps TIE and LENGTH");
        // 2.4.2 GEN: GENERATE, the motif tools, UNDO, EUCLID
        {
            auto count = [&]() { int n = 0; for (int r = 1; r <= 16; r++) for (int c = 1; c <= 16; c++) { char k[24]; std::snprintf(k, sizeof k, "arp_c%d_%d", r, c); n += get(e, y, k) != "0"; } return n; };
            e->set_param(y, "arp_page", "0"); e->set_param(y, "arp_length", "16"); e->set_param(y, "arp_tie", "0");
            e->set_param(y, "arp_g_generate", "1"); e->set_param(y, "arp_g_generate", "0");
            const int n1 = count();
            check(n1 >= 4 && get(e, y, "arp_info").find("GENERATED") != std::string::npos, "GENERATE: a melody over LENGTH");
            std::printf("arp: %s\n", get(e, y, "arp_info").c_str());
            const char *tools[] = {"arp_g_invert", "arp_g_reverse", "arp_g_double", "arp_g_half", "arp_g_mutate_go", "arp_g_fill"};
            bool all_ok = true;
            for (const char *t : tools) { e->set_param(y, t, "1"); e->set_param(y, t, "0"); all_ok = all_ok && count() > 0; }
            check(all_ok, "INVERT, REVERSE, DOUBLE, HALF, MUTATE, FILL: each leaves a pattern");
            e->set_param(y, "arp_clear", "1"); e->set_param(y, "arp_clear", "0");
            e->set_param(y, "arp_g_generate", "1"); e->set_param(y, "arp_g_generate", "0");
            std::string before;
            for (int c = 1; c <= 16; c++) for (int r = 1; r <= 16; r++) { char k[24]; std::snprintf(k, sizeof k, "arp_c%d_%d", r, c); before += get(e, y, k) != "0" ? '1' : '0'; }
            e->set_param(y, "arp_g_reverse", "1"); e->set_param(y, "arp_g_reverse", "0");
            e->set_param(y, "arp_g_undo", "1"); e->set_param(y, "arp_g_undo", "0");
            std::string after;
            for (int c = 1; c <= 16; c++) for (int r = 1; r <= 16; r++) { char k[24]; std::snprintf(k, sizeof k, "arp_c%d_%d", r, c); after += get(e, y, k) != "0" ? '1' : '0'; }
            check(before == after, "UNDO: back to before REVERSE");
            e->set_param(y, "arp_clear", "1"); e->set_param(y, "arp_clear", "0");
            e->set_param(y, "arp_e_hits", "3"); e->set_param(y, "arp_e_rotate", "0");
            e->set_param(y, "arp_e_apply", "1"); e->set_param(y, "arp_e_apply", "0");
            int hits = 0;
            for (int c = 1; c <= 16; c++) { char k[24]; std::snprintf(k, sizeof k, "arp_c1_%d", c); hits += get(e, y, k) != "0"; }
            check(hits == 3 && get(e, y, "arp_c1_1") != "0", "EUCLID 3 over 16: three hits on row 1, the first on step 1");
            e->set_param(y, "arp_clear", "1"); e->set_param(y, "arp_clear", "0");
            // 2.4.9: every mode from the loaded file (Rise: rows 1, 3, 5, 8); the melodic ones keep to its notes
            static const char *const MODES[] = {"Rise", "Fall", "Arch", "Valley", "Wave", "Random", "Ostinato", "Orch Riff",
                                                "High to Low", "Low to High", "Call + Answer", "Pedal", "Arpeggio"};
            e->set_param(y, "arp_g_range", "8");
            bool src_ok = true, vocab_ok = true;
            for (int m = 0; m < 13; m++) {
                char mv[8]; std::snprintf(mv, sizeof mv, "%d", m);
                e->set_param(y, "arp_g_contour", mv);
                e->set_param(y, "arp_g_generate", "1"); e->set_param(y, "arp_g_generate", "0");
                const std::string inf = get(e, y, "arp_info");
                if (inf.find(MODES[m]) == std::string::npos || inf.find("from Rise") == std::string::npos || count() < 2) {
                    std::printf("  mode %d: %s (%d cells)\n", m, inf.c_str(), count()); src_ok = false;
                }
                const bool melodic = m <= 5 || m == 10 || m == 12;
                for (int r = 0; r < 16 && melodic; r++) for (int c = 0; c < 16; c++)
                    if (lit(r, c) && r != 0 && r != 2 && r != 4 && r != 7) { std::printf("  mode %d: row %d step %d off the file's notes\n", m, r + 1, c + 1); vocab_ok = false; }
            }
            check(src_ok, "GENERATE with a MIDI file loaded: every mode, from the file");
            check(vocab_ok, "GENERATE from the file: the contours, CALL + ANSWER and ARPEGGIO keep to its notes");
            e->set_param(y, "arp_g_contour", "2");
            e->set_param(y, "arp_clear", "1"); e->set_param(y, "arp_clear", "0");
        }
        {   // 2.4.9: no MIDI file loaded (a second instance): its own melody
            void *z = e->create("/tmp/omni-arp-test/data2");
            auto zlit = [&](int r, int c) { char k[24]; std::snprintf(k, sizeof k, "arp_c%d_%d", r + 1, c + 1); return get(e, z, k) != "0"; };
            auto top_row = [&](int c) { for (int r = 15; r >= 0; r--) if (zlit(r, c)) return r; return -1; };
            e->set_param(z, "arp_length", "16"); e->set_param(z, "arp_tie", "0"); e->set_param(z, "arp_rate", "2");
            e->set_param(z, "arp_g_range", "8");
            bool own_ok = true, home_ok = true, range_ok = true;
            for (int rep = 0; rep < 20; rep++)
                for (int m = 0; m < 13; m++) {
                    char mv[8]; std::snprintf(mv, sizeof mv, "%d", m);
                    e->set_param(z, "arp_g_contour", mv);
                    e->set_param(z, "arp_g_generate", "1"); e->set_param(z, "arp_g_generate", "0");
                    if (get(e, z, "arp_info").find("own melody") == std::string::npos) { own_ok = false; std::printf("  mode %d: %s\n", m, get(e, z, "arp_info").c_str()); }
                    int last = -1, notes = 0;
                    for (int c = 0; c < 16; c++) if (top_row(c) >= 0) { last = c; notes++; }
                    if (notes < 2) std::printf("  mode %d (rep %d): %d note(s) on the grid | %s\n", m, rep, notes, get(e, z, "arp_info").c_str());
                    for (int r = 8; r < 16; r++) for (int c = 0; c < 16; c++) if (zlit(r, c)) range_ok = false;   // RANGE 8
                    if (notes < 2) own_ok = false;
                    if ((m <= 5 || m == 10) && last >= 0 && top_row(last) != 0 && top_row(last) != 7) {   // C major: rows 1 and 8 are C
                        if (rep == 0) std::printf("  mode %d ends on row %d\n", m, top_row(last) + 1);
                        home_ok = false;
                    }
                }
            check(own_ok, "GENERATE without a MIDI file: its own melody, every mode, 20 times");
            check(home_ok, "the contours and CALL + ANSWER end on the root");
            check(range_ok, "every mode stays inside RANGE");
            e->set_param(z, "arp_g_contour", "6");   // OSTINATO: the first beat again and again
            e->set_param(z, "arp_g_generate", "1"); e->set_param(z, "arp_g_generate", "0");
            bool same = true;
            for (int c = 0; c < 12; c++) for (int r = 0; r < 16; r++) if (zlit(r, c) != zlit(r, c + 4) && zlit(r, c) != zlit(r, (c + 8) % 16)) same = false;
            check(same, "OSTINATO: a cell repeated");
            e->set_param(z, "arp_g_contour", "8");
            e->set_param(z, "arp_g_generate", "1"); e->set_param(z, "arp_g_generate", "0");
            const bool down = top_row(0) > top_row(4) && top_row(4) > top_row(8);
            e->set_param(z, "arp_g_contour", "9");
            e->set_param(z, "arp_g_generate", "1"); e->set_param(z, "arp_g_generate", "0");
            const bool up = top_row(0) < top_row(4) && top_row(4) < top_row(8);
            check(down && up, "HIGH TO LOW steps down beat by beat, LOW TO HIGH up");
            e->set_param(z, "arp_tie", "1");   // TIE on: held notes, never two notes joined on a row
            for (int m = 0; m < 13; m++) {
                char mv[8]; std::snprintf(mv, sizeof mv, "%d", m);
                e->set_param(z, "arp_g_contour", mv);
                e->set_param(z, "arp_g_generate", "1"); e->set_param(z, "arp_g_generate", "0");
            }
            check(get(e, z, "arp_info").find("Arpeggio") != std::string::npos, "TIE on: every mode generates");
            {   // 2.4.11: GENERATE plays with expression and articulation (the grid's own per-note settings)
                e->set_param(z, "arp_length", "64"); e->set_param(z, "arp_tie", "0");
                auto field = [&](const std::string &st, const char *k) {
                    const size_t a = st.find(std::string("\n") + k + "=");
                    if (a == std::string::npos) return std::string();
                    const size_t b = st.find('\n', a + 1);
                    return st.substr(a + std::strlen(k) + 2, b - a - std::strlen(k) - 2);
                };
                std::set<int> gates_m, vels_m;
                int slides = 0, ghosts = 0, ratchets = 0, notes_m = 0;
                for (int rep = 0; rep < 10; rep++)
                    for (const char *mv : {"2", "10", "7", "6"}) {   // ARCH, CALL + ANSWER (melodic), RIFF, OSTINATO (rhythmic)
                        e->set_param(z, "arp_g_contour", mv);
                        e->set_param(z, "arp_g_generate", "1"); e->set_param(z, "arp_g_generate", "0");
                        const std::string st = get(e, z, "state"), ax = field(st, "arp_x"); std::string gt, nx; arp_x_to_old(ax, gt, nx);
                        const bool melodic = mv[0] == '2' || mv[1] == '0';
                        for (size_t i = 0; i + 5 <= gt.size(); i += 5) if (melodic) gates_m.insert(int(std::strtoul(gt.substr(i + 3, 2).c_str(), nullptr, 16)));
                        for (size_t i = 0; i + 10 <= nx.size(); i += 10) {
                            const int v = int(std::strtoul(nx.substr(i + 3, 2).c_str(), nullptr, 16)), k = int(std::strtoul(nx.substr(i + 5, 2).c_str(), nullptr, 16));
                            const int rt = int(std::strtoul(nx.substr(i + 7, 1).c_str(), nullptr, 16)), sl = nx[i + 9] == '1';
                            if (melodic) { vels_m.insert(v / 8); notes_m++; slides += sl; }
                            else { ghosts += k > 0; ratchets += rt > 1; }
                        }
                    }
                std::printf("     GENERATE expression: %d melodic notes, %zu gate values, %zu dynamic levels, %d slides; rhythmic: %d ghost notes, %d ratchet fills\n",
                            notes_m, gates_m.size(), vels_m.size(), slides, ghosts, ratchets);
                check(gates_m.size() >= 3, "GENERATE (melodic): notes of different lengths (legato, detached, held phrase ends)");
                check(vels_m.size() >= 4, "GENERATE (melodic): dynamics shaped over the phrase, not one level");
                check(slides > 0, "GENERATE (melodic): some steps slide into the next note");
                check(ghosts > 0 && ratchets > 0, "GENERATE (rhythmic): ghost notes that may skip, and ratchet fills");
                // 2.4.11 ARTIC / EXPR / GHOST knobs: the extremes do what they say; 50 (above) = the natural phrasing
                struct Stat { int gmin = 999, gmax = -1, vmin = 999, vmax = -1, ghosts = 0, ratchets = 0, slides = 0, notes = 0, weak = 0, weak_ghosts = 0; };
                auto gen_stat = [&](const char *artic, const char *expr, const char *ghost, int reps = 6) {
                    e->set_param(z, "arp_g_artic", artic); e->set_param(z, "arp_g_expr", expr); e->set_param(z, "arp_g_ghost", ghost);
                    Stat t;
                    for (int rep = 0; rep < reps; rep++)
                        for (const char *mv : {"2", "10", "7", "6"}) {
                            e->set_param(z, "arp_g_contour", mv);
                            e->set_param(z, "arp_g_generate", "1"); e->set_param(z, "arp_g_generate", "0");
                            const std::string st = get(e, z, "state"), ax = field(st, "arp_x"); std::string gt, nx; arp_x_to_old(ax, gt, nx);
                            for (size_t i = 0; i + 5 <= gt.size(); i += 5) {
                                const int g = int(std::strtoul(gt.substr(i + 3, 2).c_str(), nullptr, 16));
                                t.gmin = std::min(t.gmin, g); t.gmax = std::max(t.gmax, g);
                            }
                            for (size_t i = 0; i + 10 <= nx.size(); i += 10) {
                                const int v = int(std::strtoul(nx.substr(i + 3, 2).c_str(), nullptr, 16)), k = int(std::strtoul(nx.substr(i + 5, 2).c_str(), nullptr, 16));
                                t.vmin = std::min(t.vmin, v); t.vmax = std::max(t.vmax, v);
                                t.ghosts += k > 0; t.ratchets += int(std::strtoul(nx.substr(i + 7, 1).c_str(), nullptr, 16)) > 1; t.slides += nx[i + 9] == '1';
                                const int cell = int(std::strtoul(nx.substr(i, 3).c_str(), nullptr, 16)), stp = cell / 16;   // (a weak 16th: 1/16 steps)
                                if ((mv[0] == '7' || mv[0] == '6') && stp % 2 == 1) { t.weak++; t.weak_ghosts += k > 0; }
                                t.notes++;
                            }
                        }
                    return t;
                };
                const Stat legato = gen_stat("0", "50", "50"), stacc = gen_stat("100", "50", "50");
                const Stat flat = gen_stat("50", "0", "50"), wide = gen_stat("50", "100", "50"), clean = gen_stat("50", "50", "0"), busy = gen_stat("50", "50", "100", 20);
                std::printf("     ARTIC 0: gates %d..%d, 100: %d..%d; EXPR 0: vel %d..%d, 100: %d..%d; GHOST 0: %d/%d/%d, 100: %d/%d/%d (ghosts/ratchets/slides)\n",
                            legato.gmin, legato.gmax, stacc.gmin, stacc.gmax, flat.vmin, flat.vmax, wide.vmin, wide.vmax,
                            clean.ghosts, clean.ratchets, clean.slides, busy.ghosts, busy.ratchets, busy.slides);
                check(legato.notes > 0 && legato.gmin == 100, "ARTIC 0: every note legato (full gate)");
                check(stacc.notes > 0 && stacc.gmax <= 30 && stacc.gmin >= 5, "ARTIC 100: short, staccato notes");
                check(flat.notes > 0 && flat.vmin == 100 && flat.vmax == 100, "EXPR 0: one dynamic level");
                check(wide.vmax - wide.vmin > (vels_m.empty() ? 0 : 40), "EXPR 100: wide dynamics");
                check(clean.notes > 0 && clean.ghosts == 0 && clean.ratchets == 0 && clean.slides == 0, "GHOST 0: no ghost notes, fills or slides");
                // (a rate, not a count: how many weak 16ths a random pattern has varies a lot; GHOST 100 = 80 % of them by design)
                std::printf("     GHOST 100: %d of %d weak 16ths ghosted\n", busy.weak_ghosts, busy.weak);
                check((busy.weak == 0 || busy.weak_ghosts * 10 >= busy.weak * 6) && busy.ratchets > 0 && busy.slides > 0, "GHOST 100: ghost notes, fills and slides");
                e->set_param(z, "arp_g_artic", "50"); e->set_param(z, "arp_g_expr", "50"); e->set_param(z, "arp_g_ghost", "50");
                e->set_param(z, "arp_length", "16");
            }
            // 2.4.9 SHIFT up / down: the pattern a row up, then back
            e->set_param(z, "arp_tie", "0"); e->set_param(z, "arp_g_contour", "2");
            e->set_param(z, "arp_g_generate", "1"); e->set_param(z, "arp_g_generate", "0");
            std::string g0, g1, g2;
            for (int c = 0; c < 16; c++) for (int r = 0; r < 16; r++) g0 += zlit(r, c) ? '1' : '0';
            e->set_param(z, "arp_shift_u", "1"); e->set_param(z, "arp_shift_u", "0");
            for (int c = 0; c < 16; c++) for (int r = 0; r < 16; r++) g1 += zlit(r, c) ? '1' : '0';
            bool moved_up = true;
            for (int c = 0; c < 16; c++) for (int r = 0; r < 15; r++) if (g0[size_t(c * 16 + r)] != g1[size_t(c * 16 + r + 1)]) moved_up = false;
            e->set_param(z, "arp_shift_d", "1"); e->set_param(z, "arp_shift_d", "0");
            for (int c = 0; c < 16; c++) for (int r = 0; r < 16; r++) g2 += zlit(r, c) ? '1' : '0';
            check(moved_up && g2 == g0, "SHIFT up: every note a row higher; SHIFT down: back");
            e->set_param(z, "arp_shift_d", "1"); e->set_param(z, "arp_shift_d", "0");   // (a note on row 1: nothing moves)
            std::string g3;
            for (int c = 0; c < 16; c++) for (int r = 0; r < 16; r++) g3 += zlit(r, c) ? '1' : '0';
            bool bottom = false;
            for (int c = 0; c < 16; c++) bottom = bottom || g0[size_t(c * 16)] == '1';
            if (bottom) check(g3 == g0 && get(e, z, "arp_info").find("bottom row") != std::string::npos,
                              "SHIFT down with a note on row 1: refused (it says so), nothing lost");
            e->destroy(z);
        }
        // 2.4.12 (user: "if the user's MIDI has a chord, all those notes should be included in the generation"): a chord
        // progression C - F - G - C (quarter-note chords, a bar each) loaded, then GENERATE: the chords stay whole and the new
        // notes follow each bar's chord
        {
            const std::string lib2 = "/tmp/omni-arp-chords/library";
            std::system("rm -rf /tmp/omni-arp-chords");
            mkdir("/tmp/omni-arp-chords", 0755); mkdir(lib2.c_str(), 0755); mkdir((lib2 + "/MIDI").c_str(), 0755);
            const int CH[4][3] = {{60, 64, 67}, {65, 69, 72}, {67, 71, 74}, {60, 64, 67}};
            std::vector<std::vector<int>> ev;
            for (int b = 0; b < 4; b++) for (int q = 0; q < 4; q++) for (int k = 0; k < 3; k++) ev.push_back({(b * 4 + q) * 96, 90, CH[b][k], 100});
            auto cb = smf_bytes(96, ev);
            FILE *cf = std::fopen((lib2 + "/MIDI/Chords.mid").c_str(), "wb");
            std::fwrite(cb.data(), 1, cb.size(), cf);
            std::fclose(cf);
            setenv("OMNI_SAMPLER_LIBRARY", lib2.c_str(), 1);
            void *c = e->create("/tmp/omni-arp-chords/data");
            unsetenv("OMNI_SAMPLER_LIBRARY");
            auto wait_c = [&](const char *key, const std::string &want) {
                for (int i = 0; i < 2000; i++) {
                    if (get(e, c, key).find(want) != std::string::npos) return true;
                    e->render(c, buf.data(), 128);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return false;
            };
            check(wait_c("arp_name", "Chords"), "chords: the MIDI folder is scanned");
            e->set_param(c, "arp_load", "1"); e->set_param(c, "arp_load", "0");
            check(wait_c("arp_info", "notes"), "chords: LOAD");
            std::printf("arp chords: %s\n", get(e, c, "arp_info").c_str());
            auto clit = [&](int r, int col) { char k[24]; std::snprintf(k, sizeof k, "arp_c%d_%d", r + 1, col + 1); return get(e, c, k) != "0"; };
            // C major from C3 on row 1: a row's pitch class
            auto pc = [](int r) { static const int D[7] = {0, 2, 4, 5, 7, 9, 11}; return D[r % 7]; };
            auto chord_rows = [&](int col) { std::set<int> s; for (int k = 0; k < 3; k++) { const int n = CH[col / 4][k]; static const int R[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6}; s.insert(R[n % 12] + 7 * (n / 12 - 5)); } return s; };
            bool loaded_ok = true;
            for (int col = 0; col < 16; col++) for (int r : chord_rows(col)) loaded_ok = loaded_ok && clit(r, col);
            check(loaded_ok, "chords: the grid shows every note of each chord");
            // 2.4.18 (user: "the motif doesn't spread the notes ... just adds on top of the notes rather than using those notes
            // as a baseline to generate a new sequence, riff"): GENERATE over the chords makes a NEW part from them: the chords'
            // blocks go, their tones are spread over the range; the grid goes from 1/4 to 1/8; UNDO puts the step back
            auto grid_of = [&](int steps) {   // "row,step" of every lit cell over the pages
                std::set<std::pair<int, int>> g;
                for (int pg = 0; pg * 16 < steps; pg++) {
                    e->set_param(c, "arp_page", std::to_string(pg).c_str());
                    for (int col = 0; col < 16 && pg * 16 + col < steps; col++) for (int r = 0; r < 16; r++) if (clit(r, col)) g.insert({r, pg * 16 + col});
                }
                e->set_param(c, "arp_page", "0");
                return g;
            };
            const std::set<std::pair<int, int>> loaded = grid_of(16);
            const std::string len0 = get(e, c, "arp_length"), rate0 = get(e, c, "arp_rate");
            std::printf("     loaded: length %s rate %s\n", len0.c_str(), rate0.c_str());
            for (int mvi = 0; mvi < 13; mvi++) {
                const std::string mv = std::to_string(mvi);
                e->set_param(c, "arp_g_contour", mv.c_str());
                e->set_param(c, "arp_g_generate", "1"); e->set_param(c, "arp_g_generate", "0");
                const std::string len1 = get(e, c, "arp_length"), rate1 = get(e, c, "arp_rate");
                const int steps = std::atoi(len1.c_str());
                const std::set<std::pair<int, int>> g = grid_of(steps);
                const bool rhythmic = mvi >= 6 && mvi != 10;   // OSTINATO .. ARPEGGIO but CALL: chord tones only
                bool on_chord = true, beats_on_chord = true;
                std::set<int> rows, pcs_used;
                int whole = 0, onsets = 0;
                for (int st = 0; st < steps; st++) {
                    const int b = st / 8;   // 1/8: 8 steps a bar
                    std::set<int> pcs;
                    for (int k = 0; k < 3; k++) pcs.insert(CH[b % 4][k] % 12);
                    bool all = true;
                    for (int r : chord_rows(b * 4)) all = all && g.count({r, st});
                    whole += all;
                    for (int r = 0; r < 16; r++) {
                        if (!g.count({r, st})) continue;
                        rows.insert(r);
                        pcs_used.insert(pc(r));
                        if (st > 0 && g.count({r, st - 1})) continue;   // held from the step before
                        onsets++;
                        if (!pcs.count(pc(r))) { on_chord = false; if (st % 2 == 0) beats_on_chord = false; }
                    }
                }
                bool every_tone = true;
                for (int b = 0; b < 4; b++) for (int k = 0; k < 3; k++) every_tone = every_tone && pcs_used.count(CH[b][k] % 12);
                std::printf("     chords %-2s: %s  [len %s rate %s, rows %zu, onsets %d, whole chords on %d steps]\n", mv.c_str(),
                            get(e, c, "arp_info").c_str(), len1.c_str(), rate1.c_str(), rows.size(), onsets, whole);
                const std::string tag = "chords mode " + mv + ": ";
                check(steps == 32 && rate1 != rate0, (tag + "the grid goes from 1/4 to 1/8 (16 -> 32 steps)").c_str());
                check(beats_on_chord, (tag + "every note on a beat is a tone of its bar's chord").c_str());
                if (rhythmic) check(on_chord, (tag + "every note is a chord tone (riff, ostinato, cascade, pedal, arpeggio)").c_str());
                check(whole <= steps / 4, (tag + "the chords' blocks are not kept as they were (a new part)").c_str());
                check(rows.size() >= 4, (tag + "the notes spread over 4+ rows").c_str());
                // (a free melody over the bass need not touch every tone of a one-bar chord: 5 of the file's 7 notes at least;
                // the bass under it plays every chord's lowest note)
                if (rhythmic) check(every_tone, (tag + "every note of the file's chords is used").c_str());
                else check(pcs_used.size() >= (mvi == 5 ? 4u : 5u), (tag + "the file's chords' notes are used (5 of 7 at least, RANDOM's walk 4; a bass under the melody)").c_str());
                if (!every_tone && rhythmic) {   // the grid, to see which tone is missing where
                    std::printf("       pcs used:");
                    for (int x : pcs_used) std::printf(" %d", x);
                    std::printf("\n");
                    for (int r = 15; r >= 0; r--) {
                        std::printf("       row %2d ", r + 1);
                        for (int st = 0; st < steps; st++) std::printf("%c", g.count({r, st}) ? (st > 0 && g.count({r, st - 1}) ? '-' : '#') : '.');
                        std::printf("\n");
                    }
                }
                check(onsets >= 8, (tag + "a sequence (8+ notes)").c_str());
                if (mvi == 0) {   // UNDO: the file as it was, its step and length; again: the new part back
                    e->set_param(c, "arp_g_undo", "1"); e->set_param(c, "arp_g_undo", "0");
                    check(get(e, c, "arp_length") == len0 && get(e, c, "arp_rate") == rate0 && grid_of(16) == loaded,
                          "chords: UNDO puts the chords, the step (1/4) and the length back");
                    e->set_param(c, "arp_g_undo", "1"); e->set_param(c, "arp_g_undo", "0");
                    check(get(e, c, "arp_length") == len1 && get(e, c, "arp_rate") == rate1 && grid_of(steps) == g, "chords: UNDO again: the new part back (1/8)");
                }
                // back to the file as loaded (1/4, 16 steps) for the next mode
                e->set_param(c, "arp_rate", "0"); e->set_param(c, "arp_length", "16");
            }
            for (int i = 0; i < 50; i++) e->render(c, buf.data(), 128);
            e->destroy(c);
        }
        // 2.4.1: UJAM styles (only where the real SE-CHOOIR part 1 is: a copy of it under the library lists its phrases)
        const char *blob = "/workspaces/MPC_Mod/omni_sampler/XTRA/SE-CHOOIR_part1.blob";
        if (FILE *bf = std::fopen(blob, "rb")) {
            std::fclose(bf);
            mkdir((lib + "/UJAM SE-CHOOIR").c_str(), 0755);
            std::system(("cp '" + std::string(blob) + "' '" + lib + "/UJAM SE-CHOOIR/'").c_str());
            e->set_param(y, "arp_mode", "1");
            e->set_param(y, "arp_next", "1"); e->set_param(y, "arp_next", "0");   // (a rescan: the list was there)
            e->set_param(y, "arp_load", "1"); e->set_param(y, "arp_load", "0");   // (LOAD rescans first)
            bool found = false;
            for (int i = 0; i < 600 && !found; i++) {
                if (get(e, y, "arp_name").find("SE-CHOOIR ~ BR") != std::string::npos) found = true;
                else { e->set_param(y, "arp_prev", "1"); e->set_param(y, "arp_prev", "0"); }
                e->render(y, buf.data(), 128);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            check(found, "a UJAM library's styles are in the MIDI file list (SE-CHOOIR ~ BR ...)");
            std::printf("arp: style picked: %s\n", get(e, y, "arp_name").c_str());
            e->set_param(y, "arp_load", "1"); e->set_param(y, "arp_load", "0");
            check(wait_for("arp_info", "notes"), "LOAD a style phrase: its notes in the grid");
            std::printf("arp: %s\n", get(e, y, "arp_info").c_str());
            int lit_cells = 0;
            for (int r = 1; r <= 16; r++) for (int c = 1; c <= 16; c++) { char k[24]; std::snprintf(k, sizeof k, "arp_c%d_%d", r, c); lit_cells += get(e, y, k) != "0"; }
            check(lit_cells > 0, "the style's phrase shows on the grid");
            e->set_param(y, "arp_mode", "0");
        }
        for (int i = 0; i < 200; i++) e->render(y, buf.data(), 128);
        e->destroy(y);
    }
    // 2.4.14 the GRID's chords: the readout names the chord; VOICE re-voices it inside the key and scale; UNDO; ARP mode refuses
    {
        const mpc_engine_t *e = mpc_engine();
        void *y = e->create("/tmp/omni-arp-test/voice");
        // step 1: rows 1, 3, 5 (C E G in C major); step 5: one note; step 9: rows 6, 8, 10 (A C E)
        std::string pat = "001564";
        for (int i = 0; i < 3; i++) pat += "000064";
        pat += "000150";
        for (int i = 0; i < 3; i++) pat += "000064";
        pat += "02a064";
        e->set_param(y, "state", ("omni-sampler 2\narp_length=16\narp_pattern=" + pat + "\n").c_str());
        auto chord = [&] { return get(e, y, "arp_chord"); };
        auto go = [&](const char *mode) { e->set_param(y, "arp_v_mode", mode); e->set_param(y, "arp_v_go", "1"); e->set_param(y, "arp_v_go", "0"); };
        auto pattern = [&] { const std::string st = get(e, y, "state"); const size_t a = st.find("arp_pattern="); return a == std::string::npos ? std::string() : st.substr(a + 12, st.find('\n', a) - a - 12); };
        check(chord() == "C", ("CHORD: rows 1 3 5 in C major read as C: " + chord()).c_str());
        go("0");
        check(chord() == "C/E", ("VOICE INVERT UP: C/E: " + chord()).c_str());
        check(get(e, y, "arp_info").find("2 of 2 chords") != std::string::npos, ("both chords voiced, the one-note step left: " + get(e, y, "arp_info")).c_str());
        go("0");
        check(chord() == "C/G", ("again: C/G: " + chord()).c_str());
        go("0");
        check(chord() == "C", ("and again: back to C (no climbing): " + chord()).c_str());
        e->set_param(y, "arp_g_undo", "1"); e->set_param(y, "arp_g_undo", "0");
        check(chord() == "C/G", ("UNDO: the voicing before: " + chord()).c_str());
        e->set_param(y, "arp_g_undo", "1"); e->set_param(y, "arp_g_undo", "0");   // (redo)
        const std::string p0 = pattern();
        check(p0.size() >= 6 && p0.substr(4, 2) == "64" && p0.substr(28, 2) == "50", ("the steps' velocities are kept: " + p0.substr(0, 30)).c_str());
        check(p0.size() >= 30 && p0.substr(24, 4) == "0001", "the one-note step is untouched");
        go("4");
        check(chord() == "C", ("OPEN: still C, spread: " + chord()).c_str());
        go("5");
        check(chord() == "C", ("CLOSE: C: " + chord()).c_str());
        go("1");
        check(chord() == "C/G", ("INVERT DOWN: C/G: " + chord()).c_str());
        // A minor: the same rows are A C E
        e->set_param(y, "state", ("omni-sampler 2\narp_length=16\narp_scale=2\narp_root=9\narp_pattern=" + pat + "\n").c_str());
        check(chord() == "Am", ("A minor: rows 1 3 5 read as Am: " + chord()).c_str());
        go("0");
        check(chord() == "Am/C", ("A minor INVERT UP: Am/C: " + chord()).c_str());
        e->set_param(y, "arp_mode", "1");
        go("0");
        check(get(e, y, "arp_info").find("SEQ") != std::string::npos, ("ARP mode: VOICE says it needs SEQ: " + get(e, y, "arp_info")).c_str());
        check(chord() == "ARP", "ARP mode: no written chord");
        e->destroy(y);
    }
    // 2.4.14 (audit B4): TIE follows the playhead: DIRECTION Back strikes a held note where it is reached first (its last dot),
    // a CHAIN A -> C does not drop page C's first note because page B (not played) ends on the same row
    {
        Arp a; Rec r;
        a.settings.on.store(1); a.settings.mode.store(0); a.settings.rate.store(2); a.settings.length.store(8);
        a.settings.scale.store(1); a.settings.root.store(0); a.settings.gate.store(0.9f); a.settings.tie.store(1);
        a.settings.direction.store(1);
        for (int s = 0; s < 4; s++) a.mask[s].store(1);   // row 1 held over steps 1-4
        a.key_on(48, 100);
        run(a, r, 8 * 5513, 120);
        auto on = r.ons();
        int n48 = 0; int64_t first = -1;
        for (auto &e : on) if (e.note == 48) { n48++; if (first < 0) first = e.at; }
        check(n48 == 1 && std::llabs(first - 4 * 5512) <= 4, ("TIE + Back: the held note struck once, where it is reached (step 4 of the walk): at " + std::to_string(first)).c_str());
        a.key_off(48);
    }
    {
        Arp a; Rec r;
        a.settings.on.store(1); a.settings.mode.store(0); a.settings.rate.store(2); a.settings.length.store(64);
        a.settings.scale.store(1); a.settings.root.store(0); a.settings.gate.store(0.5f); a.settings.tie.store(1);
        a.settings.chain[0].store(0); a.settings.chain[1].store(2);
        a.mask[31].store(1); a.mask[32].store(1);   // page B's last step and page C's first, the same row
        a.key_on(48, 100);
        run(a, r, 33 * 5513, 120);
        bool c_first = false;
        for (auto &e : r.ons()) if (e.note == 48 && std::llabs(e.at - 16 * 5512) <= 8) c_first = true;
        check(c_first, "TIE + CHAIN A, C: page C's first note plays (page B is not played)");
        a.key_off(48);
    }
    // 2.4.14 (audit B3, B5): a step's velocity belongs to the chord on it
    {
        const mpc_engine_t *e = mpc_engine();
        void *y = e->create("/tmp/omni-arp-test/vel");
        e->set_param(y, "state", "omni-sampler 2\narp_length=4\narp_pattern=001546000064000064000064\n");   // C E G at velocity 70
        auto pattern = [&] { const std::string st = get(e, y, "state"); const size_t a = st.find("arp_pattern="); return a == std::string::npos ? std::string() : st.substr(a + 12, st.find('\n', a) - a - 12); };
        e->set_param(y, "arp_c5_1", "127");   // select the G
        e->set_param(y, "arp_shift_r", "1"); e->set_param(y, "arp_shift_r", "0");
        const std::string p1 = pattern();
        check(p1.substr(0, 12) == "000546001046", ("SHIFT one note of a chord: the chord keeps velocity 70, the note takes it along: " + p1).c_str());
        e->set_param(y, "state", "omni-sampler 2\narp_length=4\narp_pattern=001546\n");
        e->set_param(y, "arp_g_double", "1"); e->set_param(y, "arp_g_double", "0");
        const std::string p2 = pattern();
        check(p2.size() >= 18 && p2.substr(12, 6) == "001546", ("DOUBLE: the second half keeps the velocity: " + p2).c_str());
        e->destroy(y);
    }
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

// Regression tests for the 2.4.29 GRID audit fixes F1 (TIE history), F3 (ORDER Chord once per step), F6 (SMF system messages).
// Engine only: g++ -std=gnu++17 -I<src> test_grid_fix.cpp <src>/engine/arp.cpp <src>/engine/smf.cpp
// Same style as tests/test_arp.cpp, so the blocks can be pasted into main() there.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "engine/arp.hpp"
#include "engine/smf.hpp"
using namespace omni;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

struct Rec : Arp::Out {
    struct Ev { int64_t at; int note, vel; bool on; };
    std::vector<Ev> ev;
    int64_t now = 0;
    void arp_on(int n, int v) override { ev.push_back({now, n, v, true}); }
    void arp_off(int n) override { ev.push_back({now, n, 0, false}); }
    std::vector<Ev> ons() const { std::vector<Ev> o; for (auto &e : ev) if (e.on) o.push_back(e); return o; }
};
static void run(Arp &a, Rec &r, int frames, double bpm) {
    for (int done = 0; done < frames; done += 128) {
        a.transport(-1, bpm, false, false);
        int d = 0;
        while (d < 128) { int n = a.process(r, 128 - d); a.advance(n); r.now += n; d += n; }
    }
}

int main() {
    // ---- F1: TIE on, a run ends at step 1, all keys up, a new key: step 0 must strike
    {
        Arp a; Rec r;
        a.settings.on.store(1); a.settings.mode.store(0); a.settings.rate.store(2); a.settings.length.store(4);
        a.settings.tie.store(1); a.settings.gate.store(0.5f);
        a.mask[0].store(1); a.mask[1].store(1);   // a two-step run on row 1
        a.key_on(48, 100);
        run(a, r, 5513 + 2000, 120);              // step 0 struck, step 1 reached (the run's end): last played step = 1
        a.key_off(48);
        run(a, r, 1000, 120);                     // stops
        Rec r2; r2.now = r.now;
        a.key_on(48, 100);
        run(a, r2, 2000, 120);                    // a new phrase from step 0
        check(r2.ons().size() == 1, "TIE: a new phrase after a run that ended on step 1 strikes step 0 (stale last_step_)");
    }
    // ---- F3: ORDER Chord, 3 held notes, one step with 3 lit rows: one chord, no same-instant offs
    {
        Arp a; Rec r;
        a.settings.on.store(1); a.settings.mode.store(1); a.settings.order.store(Arp::O_CHORD);
        a.settings.rate.store(2); a.settings.length.store(4); a.settings.gate.store(0.5f);
        a.mask[0].store(1 | 1 << 1 | 1 << 2);
        a.key_on(48, 100); a.key_on(52, 100); a.key_on(55, 100);
        run(a, r, 3000, 120);                     // only step 0 plays in this window
        int on0 = 0, off0 = 0;
        for (auto &e : r.ev) if (e.at == 0) (e.on ? on0 : off0)++;
        std::printf("chord step: %d note-ons, %d note-offs at frame 0\n", on0, off0);
        check(on0 == 3 && off0 == 0, "ORDER Chord: 3 lit rows, 3 held notes = 3 note-ons and no same-instant note-offs");
    }
    // ---- F6: system common / real-time bytes between notes
    {
        const std::vector<uint8_t> trk = {
            0x00, 0x90, 0x3C, 0x64,   0x30, 0x80, 0x3C, 0x00,
            0x00, 0xF1, 0x05,         0x00, 0xF3, 0x02,
            0x30, 0x90, 0x3E, 0x64,   0x00, 0xF8,   0x30, 0x3E, 0x00,   // F8 keeps running status: "3E 00" = note off
            0x00, 0xFE,               0x00, 0xF6,
            0x0C, 0x90, 0x40, 0x64,   0x30, 0x80, 0x40, 0x00,
            0x00, 0xFF, 0x2F, 0x00};
        std::vector<uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k',
                                  0, 0, 0, uint8_t(trk.size())};
        f.insert(f.end(), trk.begin(), trk.end());
        SmfSong s;
        bool threw = false;
        try { s = smf_parse(f); } catch (...) { threw = true; }
        check(!threw, "SMF with F1/F3/F6/F8/FE bytes parses");
        bool ok = s.notes.size() == 3;
        if (ok) ok = s.notes[0].note == 60 && s.notes[0].on == 0 && s.notes[0].off == 48 &&
                     s.notes[1].note == 62 && s.notes[1].on == 96 && s.notes[1].off == 144 &&
                     s.notes[2].note == 64 && s.notes[2].on == 156 && s.notes[2].off == 204;
        std::printf("smf: %zu notes\n", s.notes.size());
        for (auto &n : s.notes) std::printf("  note %d %lld-%lld\n", n.note, (long long)n.on, (long long)n.off);
        check(ok, "SMF system messages: all 3 notes, right pitches and ticks");
    }
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

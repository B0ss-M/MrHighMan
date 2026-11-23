// F2 (proposed): ARP output keeps the MIDI channel of the key it came from. Needs the work/ tree (Arp::key_on chan, arp_on_ch).
#include <cstdint>
#include <cstdio>
#include <vector>
#include "engine/arp.hpp"
using namespace omni;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
struct Rec : Arp::Out {
    struct Ev { int64_t at; int chan, note; bool on; };
    std::vector<Ev> ev; int64_t now = 0;
    void arp_on(int n, int) override { ev.push_back({now, 0, n, true}); }
    void arp_off(int n) override { ev.push_back({now, 0, n, false}); }
    void arp_on_ch(int c, int n, int) override { ev.push_back({now, c, n, true}); }
    void arp_off_ch(int c, int n) override { ev.push_back({now, c, n, false}); }
};
static void run(Arp &a, Rec &r, int frames) {
    for (int done = 0; done < frames; done += 128) {
        a.transport(-1, 120, false, false);
        int d = 0; while (d < 128) { int n = a.process(r, 128 - d); a.advance(n); r.now += n; d += n; }
    }
}
int main() {
    {   // SEQ from a key on channel 2 (index 1): every note and its note-off on channel 1
        Arp a; Rec r;
        a.settings.on.store(1); a.settings.mode.store(0); a.settings.length.store(4); a.settings.gate.store(0.5f);
        for (int s = 0; s < 4; s++) a.mask[s].store(1 << s);
        a.key_on(48, 100, 5);
        run(a, r, 20000);
        bool all = !r.ev.empty();
        for (auto &e : r.ev) all = all && e.chan == 5;
        check(all && r.ev.size() >= 6, "SEQ: notes and note-offs keep the key's channel (6)");
    }
    {   // ARP Up over two keys on different channels: each note on its own key's channel
        Arp a; Rec r;
        a.settings.on.store(1); a.settings.mode.store(1); a.settings.order.store(Arp::O_UP); a.settings.length.store(2); a.settings.gate.store(0.5f);
        a.mask[0].store(1); a.mask[1].store(1);
        a.key_on(48, 100, 1); a.key_on(55, 100, 2);
        run(a, r, 12000);
        bool ok = true; int seen48 = 0, seen55 = 0;
        for (auto &e : r.ev) { if (e.note == 48) { seen48++; ok = ok && e.chan == 1; } if (e.note == 55) { seen55++; ok = ok && e.chan == 2; } }
        check(ok && seen48 && seen55, "ARP: each note on the channel of the key it came from");
    }
    {   // same pitch on two channels: one key, plays on the latest press; up only when both are up
        Arp a; Rec r;
        a.settings.on.store(1); a.settings.mode.store(0); a.settings.length.store(1); a.settings.gate.store(0.9f);
        a.mask[0].store(1);
        a.key_on(60, 100, 0); a.key_on(60, 100, 3);
        run(a, r, 3000);
        check(!r.ev.empty() && r.ev[0].on && r.ev[0].chan == 3, "same pitch on ch1 and ch4: plays on the latest (ch4)");
        a.key_off(60, 3);
        Rec r2; r2.now = r.now; run(a, r2, 8000);
        bool played = false; for (auto &e : r2.ev) played = played || (e.on && e.chan == 0);
        check(played, "ch4 lets go: the pitch is still held by ch1, plays on ch1");
        a.key_off(60, 0);
        Rec r3; r3.now = r2.now; run(a, r3, 8000);
        bool on3 = false; for (auto &e : r3.ev) on3 = on3 || e.on;
        check(!on3 && !a.running(), "both let go: it stops");
    }
    {   // a sink that only knows the 2-argument interface still works (existing tests)
        struct Old : Arp::Out { int n = 0; void arp_on(int, int) override { n++; } void arp_off(int) override {} } o;
        Arp a; a.settings.on.store(1); a.settings.length.store(1); a.mask[0].store(1);
        a.key_on(60, 100, 7);
        for (int i = 0; i < 40; i++) { int n = a.process(o, 128); a.advance(n); }
        check(o.n >= 1, "old-style sink (arp_on / arp_off only) still gets its notes");
    }
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails;
}

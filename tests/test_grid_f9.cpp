// F9 (proposed): SYNTH SAMPLE oscillators honour the zone's loop type (backward / alternating) and UNTIL RELEASE.
// Host test: a ramp sample (1000 frames, 44.1 kHz) with a loop over frames 100..199, played at its root by osc A.
//   g++ -std=gnu++17 -O1 -I<tree> test_grid_f9.cpp <tree>/engine/synth.cpp   (links alone: Synth needs no other engine .cpp)
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "engine/sampler.hpp"
#include "engine/synth.hpp"
using namespace omni;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

struct Out { std::vector<float> l; };
static Out play(LoopType lt, bool until_release, bool release_at, int frames, int rel_at) {
    auto pcm = std::make_shared<Pcm>();
    pcm->rate = 44100; pcm->channels = 1;
    for (int i = 0; i < 1000; i++) pcm->data.push_back(int16_t(i * 20));   // a ramp: its direction shows in the output
    Program prog;
    Zone z; z.slot = 0; z.root = 60; z.key_lo = 0; z.key_hi = 127;
    Loop lp; lp.type = lt; lp.start = 100; lp.end = 199; lp.until_release = until_release;
    z.loops.push_back(lp);
    prog.inst.zones.push_back(z);
    prog.pcm.push_back(pcm);
    prog.rr.assign(1, 0);
    prog.by_key.assign(128, std::vector<int>{0});
    Synth s;
    s.settings.on.store(1);
    s.settings.release.store(5.0f);
    s.settings.sustain.store(1.0f);
    s.settings.osc[0].on.store(1); s.settings.osc[0].type.store(Synth::OT_SAMPLE); s.settings.osc[0].sample_slot.store(0);
    s.settings.osc[0].unison.store(1);
    s.set_program(&prog);
    s.note_on(60, 127);
    Out o; o.l.assign(size_t(frames), 0); std::vector<float> r(size_t(frames), 0);
    int done = 0;
    bool off_sent = false;
    while (done < frames) {
        int n = std::min(128, frames - done);
        if (release_at && !off_sent && done <= rel_at && rel_at < done + n) { s.render(o.l.data() + done, r.data() + done, rel_at - done); done += rel_at - done; s.note_off(60); off_sent = true; continue; }
        s.render(o.l.data() + done, r.data() + done, n);
        done += n;
    }
    return o;
}
// the share of steady-state samples that move down (a forward loop's only fall is its wrap)
static double down_share(const Out &o, int from, int to) {
    int dn = 0, tot = 0;
    for (int i = from + 1; i < to; i++) { if (std::fabs(o.l[size_t(i)]) < 1e-6f) continue; tot++; if (o.l[size_t(i)] < o.l[size_t(i) - 1] - 1e-7f) dn++; }
    return tot ? double(dn) / tot : -1;
}
static double energy(const Out &o, int from, int to) { double e = 0; for (int i = from; i < to; i++) e += double(o.l[size_t(i)]) * o.l[size_t(i)]; return e / (to - from); }
int main() {
    {
        Out f = play(LoopType::Forward, false, false, 12000, 0), b = play(LoopType::Backward, false, false, 12000, 0), a = play(LoopType::Alternating, false, false, 12000, 0);
        const double df = down_share(f, 3000, 12000), db = down_share(b, 3000, 12000), da = down_share(a, 3000, 12000);
        std::printf("share of samples moving down: forward %.3f  backward %.3f  alternating %.3f\n", df, db, da);
        check(df < 0.05, "Forward loop: a saw (falls only at the wrap)");
        check(db > 0.9, "Backward loop: plays the loop backwards (after the first pass it only falls, wrapping at the start)");
        check(da > 0.4 && da < 0.6, "Alternating loop: a triangle (half rising, half falling)");
    }
    {   // UNTIL RELEASE: the loop ends with the note: the sample plays on to its end (frame 1000) and stops; the release tail has nothing left
        Out u = play(LoopType::Forward, true, true, 30000, 6000), n = play(LoopType::Forward, false, true, 30000, 6000);
        const double eu = energy(u, 12000, 24000), en = energy(n, 12000, 24000), before = energy(u, 3000, 6000);
        std::printf("energy 6000 frames after release: until-release %.3g, plain loop %.3g (before release %.3g)\n", eu, en, before);
        check(before > 1e-6 && en > 1e-6, "(sanity) both sound while held");
        check(eu < before * 1e-4, "UNTIL RELEASE: silent once the sample ran out after the note-off");
        check(en > before * 0.05, "a plain loop keeps sounding through its release");
    }
    {   // an alternating loop released: no more loop, on to the end
        Out a = play(LoopType::Alternating, true, true, 30000, 6000);
        check(energy(a, 12000, 24000) < 1e-9, "Alternating + UNTIL RELEASE: silent after the sample ran out");
    }
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

// Omni Sampler 2.4.11: Env::tick_n(n) (an unread envelope advanced a chunk at a time) must land where n tick() calls do
#include <cmath>
#include <cstdio>
#include <random>
#include "engine/dsp.hpp"
using namespace omni;
int main() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0, 1);
    int fails = 0, cases = 0;
    double worst = 0;
    for (int c = 0; c < 4000; c++) {
        Envelope e;
        e.delay = u(rng) < 0.3f ? u(rng) * 0.05 : 0;
        e.attack = u(rng) < 0.2f ? 0 : u(rng) * u(rng) * 2;
        e.attack_level = u(rng) < 0.5f ? 1 : u(rng);
        e.hold = u(rng) < 0.5f ? 0 : u(rng) * 0.1;
        e.decay = u(rng) * u(rng) * 3;
        e.break_level = u(rng) < 0.7f ? -1 : u(rng);
        e.slope_time = u(rng) * 0.2f;
        e.sustain = u(rng);
        e.release = u(rng) * u(rng) * 4;
        e.attack_slope = u(rng) * 2 - 1; e.decay_slope = u(rng) * 2 - 1; e.release_slope = u(rng) * 2 - 1;
        Env a, b;
        a.begin(e, 1, 1, 0, 0, 1);
        b.begin(e, 1, 1, 0, 0, 1);
        const int chunk = 1 + int(u(rng) * 64);
        const int rel_at = int(u(rng) * 44100 * 2);   // the key let go after up to 2 s
        for (int pos = 0; pos < 44100 * 6; pos += chunk) {
            if (pos >= rel_at && pos < rel_at + chunk) { a.release(); b.release(); }
            for (int i = 0; i < chunk; i++) a.tick();
            b.tick_n(chunk);
            const double d = std::fabs(double(a.level) - double(b.level));
            worst = std::max(worst, d);
            if (a.stage != b.stage && !(d < 1e-6)) { if (fails < 5) std::printf("FAIL case %d pos %d: stage %d vs %d, level %f vs %f\n", c, pos, a.stage, b.stage, a.level, b.level); fails++; break; }
            if (d > 1e-6) { if (fails < 5) std::printf("FAIL case %d pos %d: level %f vs %f\n", c, pos, a.level, b.level); fails++; break; }
        }
        cases++;
    }
    std::printf("%d envelopes, worst level difference %.2e\n%s\n", cases, worst, fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}

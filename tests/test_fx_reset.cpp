#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../src/engine/dsp.hpp"

using namespace omni;

static int fails = 0;
static void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

int main() {
    {
        Reverb r;
        r.set(0.8f, 0.2f);
        float l = 0, rr = 0;
        for (int i = 0; i < 5000; ++i) r.tick(0.5f, 0.0f, l, rr);
        check(r.cl[0].store != 0, "fixture: Classic reverb has live filtered state");
        r.clear();
        check(r.cl[0].i == 0 && r.cl[0].store == 0 && r.al[0].i == 0, "reverb clear resets comb and all-pass state");
        float peak = 0;
        for (int i = 0; i < 2500; ++i) {
            r.tick(0, 0, l, rr);
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(rr)));
        }
        check(peak < 1e-7f, "reverb clear leaves no delayed residual");
    }
    {
        Compressor c;
        c.set(1);
        for (int i = 0; i < 20000; ++i) { float l = 0.99f, r = 0.99f; c.tick(l, r); }
        check(c.g < 0.5f, "fixture: compressor is attenuating");
        c.clear();
        float l = 0.01f, r = 0.01f;
        c.tick(l, r);
        check(c.g > 0.99f && std::fabs(l) > 0.0099f, "compressor re-enable starts from unity gain");
    }
    {
        LoFi f;
        f.set(1000, 8, 1);
        float l = 0.75f, r = 0.75f;
        f.tick(l, r);
        check(f.hl > 0.7f, "fixture: Lo-Fi holds a sample");
        f.clear();
        l = r = -0.5f;
        f.tick(l, r);
        check(l < -0.49f && l > -0.51f, "Lo-Fi re-enable captures the current input, not the old hold");
    }
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

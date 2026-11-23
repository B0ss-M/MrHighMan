// 2.4.5: the synth's MACRO engines and filters: finite, bounded, and how long they take
// (run against an older build to compare: the per-engine RMS should match with DRIFT and DRIVE at 0)
#include "engine/synth.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
using namespace omni;

struct Out { double rms, peak, ms; bool finite; };

static Out run(int engine, int ftype, float drive, float res, float drift) {
    auto s = std::make_unique<Synth>();
    SynthSettings &st = s->settings;
    st.on = 1;
    st.osc[0].on = 1; st.osc[0].type = Synth::OT_MACRO; st.osc[0].macro_engine = engine;
    st.osc[0].pos = 0.4f; st.osc[0].warp = 0.6f; st.osc[0].spread = 0.3f;
    st.mix.flt_on = ftype >= 0; st.mix.flt_type = ftype < 0 ? 1 : ftype;
    st.mix.cutoff = 900; st.mix.res = res; st.mix.drive = drive; st.mix.env_amt = 0.4f;
    st.voice.drift = drift;
    std::vector<float> l(48000 * 2), r(l.size());
    const auto t0 = std::chrono::steady_clock::now();
    for (int n : {36, 48, 55, 60, 64, 67, 72, 84}) s->note_on(n, 100);
    for (size_t i = 0; i < l.size(); i += 64) {
        if (i == 48000) for (int n : {36, 48, 55, 60, 64, 67, 72, 84}) s->note_off(n);
        s->render(&l[i], &r[i], 64);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Out o{0, 0, ms, true};
    for (size_t i = 0; i < l.size(); i++) {
        if (!std::isfinite(l[i]) || !std::isfinite(r[i])) o.finite = false;
        o.rms += double(l[i]) * l[i]; o.peak = std::max(o.peak, double(std::fabs(l[i])));
    }
    o.rms = std::sqrt(o.rms / double(l.size()));
    return o;
}

int main() {
    int bad = 0;
    std::printf("engine  rms      peak    ms (8 voices, 2 s)\n");
    for (int e = 0; e < macro_engine_count(); e++) {
        const Out o = run(e, -1, 0, 0, 0);
        std::printf("%-16s %.5f %.4f %6.1f\n", macro_engine_name(e), o.rms, o.peak, o.ms);
        if (!o.finite || o.peak > 16) { std::printf("  FAIL\n"); bad++; }
    }
    static const char *const FT[] = {"LP12", "LP24", "Ladder", "BP", "HP", "Notch", "Comb"};
    for (int f = 0; f < 7; f++)
        for (float drv : {0.0f, 0.5f, 1.0f}) {
            const Out o = run(0, f, drv, 1.0f, 0.5f);
            std::printf("%-6s drive %.1f res 1: rms %.5f peak %.4f %6.1f ms\n", FT[f], drv, o.rms, o.peak, o.ms);
            if (!o.finite || o.peak > 16) { std::printf("  FAIL\n"); bad++; }
        }
    std::printf(bad ? "%d FAILED\n" : "all ok\n", bad);
    return bad != 0;
}

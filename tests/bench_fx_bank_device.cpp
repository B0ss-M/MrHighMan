// Omni Sampler 2.4.30: FX BANK cost on the device itself (static binary, run from /tmp on the MPC, then removed).
//   bench_fx_bank_device [seconds per program = 4] [block = 128]
// For every program of engine/omni_fx_bank.hpp (the plugin's own build of it): the Rack at 44.1 kHz on a stereo test
// signal (two detuned saws + a decaying noise burst every 0.5 s), in `block`-frame blocks as the plugin calls it,
// thread CPU time / audio time = % of one core. Also: output finite, peak, that mix 0 / Bypass cost nothing.
// Prints one JSON object (stdout).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include "../src/engine/omni_fx_bank.hpp"

using namespace omnisampler::fxbank;

static double cpu_now() {
    timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return double(t.tv_sec) + 1e-9 * double(t.tv_nsec);
}

static std::string esc(const char *s) {
    std::string o;
    for (; *s; s++) { if (*s == '"' || *s == '\\') o += '\\'; o += *s; }
    return o;
}

int main(int argc, char **argv) {
    const double secs = argc > 1 ? std::atof(argv[1]) : 4.0;
    const int block = argc > 2 ? std::atoi(argv[2]) : 128;
    const double sr = 44100;
    const long total = long(secs * sr);
    std::size_t n = 0;
    const Program *list = programs(n);
    std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    static const char *cards[][2] = {{"700/reverb", "REV"}, {"700/chorus", "MOD"}, {"700/variation", "VAR"}, {"Z-FX", "MULTI"}};
    std::printf("{\n  \"tool\": \"bench_fx_bank_device\", \"sample_rate\": 44100, \"block\": %d, \"seconds_per_program\": %.1f,\n"
                "  \"programs\": [\n", block, secs);
    double worst = 0, sum = 0;
    int counted = 0, nonfinite = 0, option = 0;
    std::string worst_name;
    for (std::size_t i = 0; i < n; i++) {
        const bool bypass = list[i].engine == Engine::Bypass;
        if (!bypass) option++;
        Rack rack;
        rack.prepare(sr);
        rack.select(i);
        Controls c;   // the page's defaults (plugin.cpp NUM_PARAMS fxb_*)
        rack.set(c);
        unsigned seed = 1;
        double ph1 = 0, ph2 = 0, burst = 0, peak = 0;
        bool finite = true;
        double cpu = 0;
        for (long done = 0; done < total; done += block) {
            for (int k = 0; k < block; k++) {
                const long t = done + k;
                if (t % 22050 == 0) burst = 1;
                seed = seed * 1664525u + 1013904223u;
                const double noise = (double(seed >> 9) / double(1u << 23) - 0.5) * burst;
                burst *= 0.9995;
                ph1 += 110.0 / sr; ph1 -= std::floor(ph1);
                ph2 += 110.7 / sr; ph2 -= std::floor(ph2);
                l[size_t(k)] = float(0.2 * (2 * ph1 - 1) + 0.25 * noise);
                r[size_t(k)] = float(0.2 * (2 * ph2 - 1) + 0.25 * noise);
            }
            const double t0 = cpu_now();
            rack.process(l.data(), r.data(), size_t(block));
            cpu += cpu_now() - t0;
            for (int k = 0; k < block; k++) {
                if (!std::isfinite(l[size_t(k)]) || !std::isfinite(r[size_t(k)])) finite = false;
                peak = std::max(peak, double(std::max(std::fabs(l[size_t(k)]), std::fabs(r[size_t(k)]))));
            }
        }
        const double pct = 100.0 * cpu / (double(total) / sr);
        const char *card = "MULTI";
        for (auto &cd : cards) if (std::string(list[i].bank) == cd[0]) card = cd[1];
        if (!finite) nonfinite++;
        if (!bypass) {
            sum += pct; counted++;
            if (pct > worst) { worst = pct; worst_name = std::string(card) + " " + list[i].name; }
        }
        std::printf("    {\"catalog\": %zu, \"option\": %d, \"card\": \"%s\", \"name\": \"%s\", \"bypass\": %s, \"cpu_pct_one_core\": %.4f, "
                    "\"peak\": %.3f, \"finite\": %s}%s\n",
                    i, bypass ? 0 : option, card, esc(list[i].name).c_str(), bypass ? "true" : "false", pct, peak,
                    finite ? "true" : "false", i + 1 < n ? "," : "");
    }
    // mix 0 on a busy program: must cost (almost) nothing
    double mix0 = 0;
    {
        Rack rack;
        rack.prepare(sr);
        rack.select(n - 1);
        Controls c;
        c.mix = 0;
        rack.set(c);
        for (long done = 0; done < total; done += block) {
            const double t0 = cpu_now();
            rack.process(l.data(), r.data(), size_t(block));
            mix0 += cpu_now() - t0;
        }
        mix0 = 100.0 * mix0 / (double(total) / sr);
    }
    std::printf("  ],\n  \"summary\": {\"programs\": %d, \"mean_cpu_pct\": %.4f, \"worst_cpu_pct\": %.4f, \"worst_program\": \"%s\", "
                "\"mix0_cpu_pct\": %.4f, \"nonfinite\": %d, \"gate_pct\": 2.5, \"gate_pass\": %s}\n}\n",
                counted, counted ? sum / counted : 0, worst, esc(worst_name.c_str()).c_str(), mix0, nonfinite,
                worst <= 2.5 && nonfinite == 0 ? "true" : "false");
    return nonfinite ? 1 : 0;
}

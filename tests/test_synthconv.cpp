// Omni Sampler 2.4.10: the synth patch translator over a corpus. For each file (paths on stdin, one a line): its reader,
// preset 0, the translated SYNTH settings applied to one plugin instance, notes C3 / C4 / C5 played; prints a TSV line:
//   path <TAB> result <TAB> rms <TAB> peak <TAB> clipped <TAB> settings summary
// result: ok | silent | clips | nan | no-synth (no settings: sampler only) | error: ...
// The exit code is 1 when any file is nan, or (with --strict) silent / clips.   test_synthconv [--strict] < list
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "../src/formats/format.hpp"
extern "C" {
#include "engine.h"
}

using namespace omni;
static const mpc_engine_t *e;
static std::string get(void *inst, const char *key) {
    char buf[512];
    int n = e->get_param(inst, key, buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}

int main(int argc, char **argv) {
    const bool strict = argc > 1 && !std::strcmp(argv[1], "--strict");
    register_all();
    Vfs vfs;
    e = mpc_engine();
    void *y = e->create("/tmp/omni-synthconv-test");
    std::vector<int16_t> buf(2 * 256);
    auto render = [&](int frames, double &sq, int &peak, int &clips, long &n, bool &bad) {
        for (int done = 0; done < frames; done += 256) {
            e->render(y, buf.data(), 256);
            for (int16_t s : buf) {
                const int a = std::abs(int(s));
                sq += double(s) * s; n++;
                peak = std::max(peak, a);
                if (a >= 32760) clips++;
            }
        }
        (void)bad;
    };
    std::string line;
    int nans = 0, silent = 0, clipping = 0, ok = 0, nosyn = 0, errors = 0;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::string res, summary;
        double rms = 0;
        int peak = 0, clips = 0;
        try {
            Location loc = vfs.resolve(line);
            const FormatReader *r = find_reader(*loc.volume, loc.inner);
            if (!r) throw ParseError("no reader");
            auto presets = r->list(loc.volume, loc.inner);
            if (presets.empty()) throw ParseError("no presets");
            Instrument inst = r->load(loc.volume, loc.inner, presets[0].index);
            if (inst.settings.empty()) { res = "no-synth"; nosyn++; }
            else {
                bool nonfinite = false;
                for (auto &kv : inst.settings) {
                    if (!std::isfinite(kv.second)) { nonfinite = true; continue; }
                    char v[32];
                    std::snprintf(v, sizeof v, "%g", double(kv.second));
                    e->set_param(y, kv.first.c_str(), v);
                }
                // let the oscillators load (the synth worker), then three notes, each held 0.6 s and released 0.4 s
                for (int i = 0; i < 400 && (get(y, "sa_name") == "Loading..." || get(y, "sb_name") == "Loading..."); i++) {
                    e->render(y, buf.data(), 256);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                double sq = 0;
                long n = 0;
                bool bad = false;
                for (int note : {48, 60, 72}) {
                    const uint8_t on[3] = {0x90, uint8_t(note), 100}, off[3] = {0x80, uint8_t(note), 0};
                    e->midi(y, on, 3);
                    render(int(0.6 * 44100), sq, peak, clips, n, bad);
                    e->midi(y, off, 3);
                    render(int(0.4 * 44100), sq, peak, clips, n, bad);
                }
                const uint8_t all_off[3] = {0xB0, 123, 0};
                e->midi(y, all_off, 3);
                render(44100, sq, peak, clips, n, bad);   // (the tails die before the next patch)
                rms = n ? std::sqrt(sq / double(n)) : 0;
                if (nonfinite || !std::isfinite(rms)) { res = "nan"; nans++; }
                else if (rms < 30) { res = "silent"; silent++; }
                else if (clips > 2000) { res = "clips"; clipping++; }
                else { res = "ok"; ok++; }
                char s[256];
                std::snprintf(s, sizeof s, "A=%s/%s B=%s/%s flt=%s cut=%s env=%s att=%s", get(y, "sa_on").c_str(), get(y, "sa_table").c_str(),
                              get(y, "sb_on").c_str(), get(y, "sb_table").c_str(), get(y, "syn_flt_type").c_str(), get(y, "syn_cutoff").c_str(),
                              get(y, "syn_fenv").c_str(), get(y, "syn_attack").c_str());
                summary = s;
            }
        } catch (const std::exception &ex) {
            res = std::string("error: ") + ex.what();
            errors++;
        }
        std::printf("%s\t%s\t%.0f\t%d\t%d\t%s\n", line.c_str(), res.c_str(), rms, peak, clips, summary.c_str());
        std::fflush(stdout);
    }
    e->destroy(y);
    std::fprintf(stderr, "ok %d, silent %d, clips %d, nan %d, no-synth %d, errors %d\n", ok, silent, clipping, nans, nosyn, errors);
    return nans || (strict && (silent || clipping)) ? 1 : 0;
}

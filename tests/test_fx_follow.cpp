// Omni Sampler 2.4.14: a Vital preset brings its effects onto Omni's output effects; the next instrument without effects
// gets back what was there before (plugin.cpp fx_follow), but never undoes the user's own settings.
//   test_fx_follow <a Vital preset with compressor + reverb> <an instrument that names no effects (e.g. a .dwp)>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}
static const mpc_engine_t *e;
static std::vector<int16_t> buf(256);
static int fails = 0;
static std::string get(void *y, const std::string &k) { char b[512] = {}; e->get_param(y, k.c_str(), b, sizeof b); return b; }
static double numv(void *y, const std::string &k) { return std::atof(get(y, k).c_str()); }
static void idle(void *y, int ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) { e->render(y, buf.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}
static void check(bool ok, const std::string &w) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", w.c_str()); if (!ok) fails++; }
// tap a file in its folder's browser list (the way a finger loads it: with its settings)
static bool tap(void *y, const std::string &f) {
    const std::string dir = f.substr(0, f.rfind('/')), name = f.substr(f.rfind('/') + 1);
    e->set_param(y, "br_path", dir.c_str());
    idle(y, 50);
    for (int page = 0; page < 200; page++) {
        for (int r = 1; r <= 10; r++) {
            std::string s = get(y, "br_" + std::to_string(r));
            if (s.compare(0, 2, "> ") == 0) s = s.substr(2);
            if (s == name) {
                e->set_param(y, ("br_" + std::to_string(r)).c_str(), "1");
                for (int i = 0; i < 600; i++) {   // up to 12 s for the load
                    idle(y, 20);
                    const std::string st = get(y, "status");
                    if (i > 15 && st.compare(0, 7, "Loading") && st.compare(0, 7, "Reading")) break;
                }
                idle(y, 200);
                return true;
            }
        }
        const std::string before = get(y, "br_1");
        e->set_param(y, "br_rowdown", "1");
        idle(y, 30);
        if (get(y, "br_1") == before) break;
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: test_fx_follow <vital preset> <instrument without effects>\n"); return 2; }
    e = mpc_engine();
    (void)!std::system("rm -rf /tmp/omni-fx-follow");
    void *y = e->create("/tmp/omni-fx-follow");
    e->set_param(y, "limiter", "1");          // the user's own limiter, on before any preset
    e->set_param(y, "rev_size", "33");        // and a reverb size of their own
    idle(y, 50);
    check(tap(y, argv[1]), "the Vital preset loads");
    const double comp = numv(y, "comp"), rmix = numv(y, "rev_mix");
    std::printf("     after Vital: comp %.1f type %s, rev_mix %.1f, rev_size %s, drive %s, limiter %s\n", comp, get(y, "comp_type").c_str(),
                rmix, get(y, "rev_size").c_str(), get(y, "drive").c_str(), get(y, "limiter").c_str());
    check(comp > 1 && get(y, "comp_type") == "2", "its compressor is on Omni's (Glue)");
    check(rmix > 0, "its reverb is on");
    e->set_param(y, "cho_mix", "77");         // the user moves a knob after the preset
    idle(y, 50);
    check(tap(y, argv[2]), "an instrument without effects loads");
    std::printf("     after it: comp %s, rev_mix %s, rev_size %s, cho_mix %s, limiter %s\n", get(y, "comp").c_str(), get(y, "rev_mix").c_str(),
                get(y, "rev_size").c_str(), get(y, "cho_mix").c_str(), get(y, "limiter").c_str());
    check(numv(y, "comp") == 0 && numv(y, "rev_mix") == 0, "the Vital preset's compressor and reverb are gone");
    check(numv(y, "rev_size") == 33, "the reverb size is the user's again (33)");
    check(numv(y, "cho_mix") == 77, "the knob the user moved after the preset stays (77)");
    check(get(y, "limiter") == "1", "the user's limiter is still on");
    e->destroy(y);
    std::printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}

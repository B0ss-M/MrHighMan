// 2.4.13: Vital's wavetables and LFO shapes through the plugin, as on the MPC.
//   test_vital_plugin <a .vital preset> <a folder of .vitallfo files (Vital's LFOs/Basic)> <a .vitaltable>
// 1. tapping the preset: oscillator A plays the preset's own table ("Vital / <preset> / OSC n"), an LFO it uses shows
//    Custom with the preset's shape; it plays
// 2. the project keeps both (syn_file_a=vital:..., syn_lfo<n>_ref=vlfo:...) and a fresh instance restores them; MPC's
//    replay of an older sa_table value right after the restore does not drop the table; a user's change later does
// 3. CUSTOM's < > steps the library's .vitallfo files (wrapping), the project keeps the file; a square shape at a slow
//    rate on the AMP route really gates the sound (the table reaches the audio)
// 4. a .vitaltable in <library>/Oscillators is in the WAVETABLE list (and plays), never in SAMPLE's
// 5. a project whose preset is gone: the oscillator says so, still plays (a saw), and keeps the reference
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}

static const mpc_engine_t *e;
static int fails = 0;
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
static std::string get(void *inst, const std::string &key) {
    char buf[1024];
    int n = e->get_param(inst, key.c_str(), buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}
static std::vector<int16_t> buf(256);
static void idle(void *y, int ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) {
        e->render(y, buf.data(), 128);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static bool wait_for(void *y, const std::function<bool()> &f, int ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) { if (f()) return true; idle(y, 20); }
    return f();
}
static std::string line_of(const std::string &state, const std::string &key) {
    const std::string k = key + "=";
    size_t at = state.rfind("\n" + k);
    at = at == std::string::npos ? (state.compare(0, k.size(), k) ? std::string::npos : 0) : at + 1;
    if (at == std::string::npos) return "";
    const size_t end = state.find('\n', at);
    return state.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
}
// peak, and the RMS of each 50 ms window over `seconds`
static int play(void *y, double seconds, std::vector<double> *wins = nullptr) {
    uint8_t on[3] = {0x90, 48, 110}, off[3] = {0x80, 48, 0};
    e->midi(y, on, 3);
    int peak = 0;
    const int blocks = int(seconds * 44100 / 128), per = int(0.05 * 44100 / 128);
    double sum = 0;
    int n = 0;
    for (int b = 0; b < blocks; b++) {
        e->render(y, buf.data(), 128);
        for (int16_t v : buf) { peak = std::max(peak, std::abs(int(v))); sum += double(v) * v; }
        if (++n == per) { if (wins) wins->push_back(std::sqrt(sum / (per * 256.0))); sum = 0; n = 0; }
    }
    e->midi(y, off, 3);
    for (int b = 0; b < 400; b++) e->render(y, buf.data(), 128);
    return peak;
}
static bool tap(void *y, const std::string &f) {
    const std::string dir = f.substr(0, f.find_last_of('/')), name = f.substr(f.find_last_of('/') + 1);
    e->set_param(y, "br_path", dir.c_str());
    for (int page = 0; page < 400; page++) {
        for (int r = 1; r <= 10; r++) {
            std::string s = get(y, "br_" + std::to_string(r));
            if (s.compare(0, 2, "> ") == 0) s = s.substr(2);
            if (s == name) { e->set_param(y, ("br_" + std::to_string(r)).c_str(), "1"); return true; }
        }
        const std::string before = get(y, "br_1");
        e->set_param(y, "br_rowdown", "1");
        idle(y, 30);
        if (get(y, "br_1") == before) break;
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: test_vital_plugin <preset.vital> <folder of .vitallfo> <table.vitaltable>\n"); return 2; }
    const std::string preset = argv[1], lfo_dir = argv[2], table = argv[3];
    const std::string T = "/tmp/omni-vital-plugin-test", lib = T + "/library";
    (void)!std::system(("rm -rf '" + T + "' && mkdir -p '" + lib + "/LFOs/Basic' '" + lib + "/Oscillators/Vital'").c_str());
    (void)!std::system(("cp '" + lfo_dir + "'/*.vitallfo '" + lib + "/LFOs/Basic/'").c_str());
    (void)!std::system(("cp '" + table + "' '" + lib + "/Oscillators/Vital/'").c_str());
    setenv("OMNI_SAMPLER_LIBRARY", lib.c_str(), 1);
    e = mpc_engine();

    // 1. tap the preset
    void *a = e->create(T.c_str());
    check(tap(a, preset), "the preset is listed and tapped");
    const bool loaded = wait_for(a, [&] { return get(a, "sa_name").rfind("Vital / ", 0) == 0; }, 20000);
    const std::string name_a = get(a, "sa_name"), info_a = get(a, "sa_info");
    check(loaded, "OSC A plays the preset's own table: " + name_a + " | " + info_a);
    check(info_a.find("FRAMES") != std::string::npos || info_a.find("SINGLE CYCLE") != std::string::npos, "its info line shows the table");
    int lfo = 0;
    for (int n = 1; n <= 4 && !lfo; n++) if (std::atoi(get(a, "syn_lfo" + std::to_string(n) + "_shape").c_str()) == 6) lfo = n;   // (raw value: 6 = Custom)
    const std::string lk = "syn_lfo" + std::to_string(lfo ? lfo : 1);
    check(lfo > 0, "an LFO the preset uses plays its drawn shape (Custom)");
    const bool lfo_named = wait_for(a, [&] { const std::string r = get(a, lk + "_file"); return !r.empty() && r.rfind("Not found", 0) && r.rfind("Vital LFOs", 0); }, 10000);
    check(lfo_named, "its shape is named: " + get(a, lk + "_file"));
    check(play(a, 1.0) > 50, "the preset plays");
    const std::string st = get(a, "state");
    const std::string ref_a = line_of(st, "syn_file_a"), lref = line_of(st, lk + "_ref");
    check(ref_a.rfind("vital:", 0) == 0 && ref_a.find(preset) != std::string::npos, "the project keeps the table: " + ref_a);
    check(lref.rfind("vlfo:", 0) == 0, "the project keeps the LFO shape: " + lref);

    // 2. a fresh instance restores them; MPC's replay of an older value right after does not drop the table
    void *b = e->create(T.c_str());
    e->set_param(b, "state", st.c_str());
    e->set_param(b, "sa_table", "0");   // (MPC replays a project's normalized values right after the chunk)
    { const bool w_ = wait_for(b, [&] { return get(b, "sa_name") == name_a; }, 20000); check(w_, "restored: OSC A plays the same table (" + get(b, "sa_name") + ")"); }
    check(wait_for(b, [&] { return get(b, lk + "_file") == get(a, lk + "_file"); }, 10000), "restored: the same LFO shape (" + get(b, lk + "_file") + ")");
    check(line_of(get(b, "state"), "syn_file_a") == ref_a, "the replay right after the restore did not drop the table");
    check(play(b, 0.5) > 50, "the restored project plays");
    idle(b, 3200);
    e->set_param(b, "sa_table", "1");   // the user's own pick, later
    { const bool w_ = wait_for(b, [&] { return get(b, "sa_name").rfind("Vital / ", 0) != 0 && get(b, "sa_name") != "Loading..."; }, 10000);
      check(w_, "a later change of table is the user's: " + get(b, "sa_name")); }
    check(line_of(get(b, "state"), "syn_file_a").empty(), "and the project no longer names the preset's table");

    // 3. CUSTOM's < > through <library>/LFOs (natural order: Saw Down, Saw Up, Sin, Square, Staircase Down, Triangle)
    e->set_param(b, "syn_lfo1_shape", "6");
    auto step = [&](const char *dir, const std::string &want) {
        const std::string k = std::string("syn_lfo1_file_") + dir;
        e->set_param(b, k.c_str(), "0");
        e->set_param(b, k.c_str(), "1");
        const bool w_ = wait_for(b, [&] { return get(b, "syn_lfo1_file") == want; }, 10000);
        check(w_, std::string(dir) + " -> " + want + " (" + get(b, "syn_lfo1_file") + ")");
    };
    step("next", "Saw Down");   // none picked yet: the first
    step("next", "Saw Up");
    step("prev", "Saw Down");
    step("prev", "Triangle");   // wraps to the last
    check(line_of(get(b, "state"), "syn_lfo1_ref") == "Basic/Triangle.vitallfo", "the project keeps the file: " + line_of(get(b, "state"), "syn_lfo1_ref"));
    step("next", "Saw Down");   // wraps to the first
    step("next", "Saw Up");
    step("next", "Sin");
    step("next", "Square");
    {   // a square at ~0.4 Hz on the AMP route, depth 100: the sound switches between loud and quiet
        e->set_param(b, "syn_lfo1_rate", "30");
        e->set_param(b, "syn_lfo1_sync", "0");
        e->set_param(b, "syn_lfo1_trig", "1");
        e->set_param(b, "syn_lfo1_depth", "100");
        e->set_param(b, "syn_lfo1_rk_amp", "1");
        e->set_param(b, "syn_attack", "1");
        e->set_param(b, "syn_sustain", "100");
        idle(b, 100);
        std::vector<double> w;
        play(b, 3.0, &w);
        double hi = 0, lo = 1e9;
        for (size_t i = 2; i < w.size(); i++) { hi = std::max(hi, w[i]); lo = std::min(lo, w[i]); }
        std::printf("  square LFO on AMP: window RMS %.0f .. %.0f\n", lo, hi);
        check(hi > 200 && lo < hi * 0.25, "a Custom square LFO gates the sound (its table reaches the audio)");
    }

    // 4. a .vitaltable in <library>/Oscillators: WAVETABLE's list has it, SAMPLE's not
    void *c = e->create(T.c_str());
    e->set_param(c, "syn_on", "1");
    e->set_param(c, "sa_on", "1");
    e->set_param(c, "sa_type", "1");
    bool found = false;
    for (int i = 0; i < 400 && !found; i++) {
        e->set_param(c, "sa_next", "0");
        e->set_param(c, "sa_next", "1");
        wait_for(c, [&] { return get(c, "sa_name") != "Loading..."; }, 10000);
        found = get(c, "sa_name").find(".vitaltable") != std::string::npos;
        if (get(c, "sa_name").rfind("Factory / ", 0) == 0 && i > 0 && get(c, "sa_info").empty()) break;
    }
    check(found, "WAVETABLE lists the .vitaltable: " + get(c, "sa_name"));
    if (found) {
        check(wait_for(c, [&] { return get(c, "sa_info").find("FRAMES") != std::string::npos || get(c, "sa_info").find("SINGLE") != std::string::npos; }, 10000),
              "it loads: " + get(c, "sa_info"));
        check(play(c, 0.5) > 50, "and plays");
    }
    e->set_param(c, "sa_type", "2");
    bool in_sample = false;
    std::string first;
    for (int i = 0; i < 400; i++) {
        e->set_param(c, "sa_next", "0");
        e->set_param(c, "sa_next", "1");
        wait_for(c, [&] { return get(c, "sa_name") != "Loading..."; }, 10000);
        const std::string n = get(c, "sa_name");
        if (n.find(".vitaltable") != std::string::npos) in_sample = true;
        if (i == 0) first = n; else if (n == first) break;
    }
    check(!in_sample, "SAMPLE's list has no .vitaltable");

    // 5. the preset gone: said, plays a saw, the reference kept
    void *d = e->create(T.c_str());
    e->set_param(d, "state", "omni-sampler 2\nsyn_on=1\nsa_on=1\nsa_type=1\nsyn_file_a=vital:1:/tmp/omni-vital-plugin-test/gone.vital\n");
    { const bool w_ = wait_for(d, [&] { return get(d, "sa_info").rfind("Can't load", 0) == 0; }, 10000); check(w_, "a missing preset is said: " + get(d, "sa_info")); }
    check(play(d, 0.5) > 50, "and the oscillator still plays (a saw)");
    check(line_of(get(d, "state"), "syn_file_a") == "vital:1:/tmp/omni-vital-plugin-test/gone.vital", "the project keeps the reference (the drive may come back)");

    for (void *y : {a, b, c, d}) e->destroy(y);
    std::printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

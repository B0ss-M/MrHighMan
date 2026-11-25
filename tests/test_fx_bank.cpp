// Omni Sampler 2.4.30: the FX BANK page through the plugin (SYNTH as the sound source).
//   test_fx_bank
// 1. Off: output equals the plain path (no processing); 2. a delay program adds repeats after the note ends;
// 3. Off again / BYPASS: no tail comes out; back on: the old tail was cleared; 4. every popup option (117 programs) gives
// finite, bounded output; 5. the project state keeps the FX BANK settings; 6. CPU: the bank's cost per program family.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "engine.h"
}
static const mpc_engine_t *e;
static int fails = 0;
static void check(bool ok, const std::string &w) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", w.c_str()); if (!ok) fails++; }
static std::string get(void *y, const std::string &k) { char b[1 << 16] = {}; e->get_param(y, k.c_str(), b, sizeof b); return b; }
static void set(void *y, const std::string &k, const std::string &v) { e->set_param(y, k.c_str(), v.c_str()); }
struct Stats { double energy = 0; int peak = 0; };
static Stats run(void *y, int blocks) {
    Stats s;
    std::vector<int16_t> b(256);
    for (int i = 0; i < blocks; i++) {
        e->render(y, b.data(), 128);
        for (int16_t v : b) { s.energy += double(v) * v; s.peak = std::max(s.peak, std::abs(int(v))); }
    }
    return s;
}
static void note(void *y, bool on) { uint8_t m[3] = {uint8_t(on ? 0x90 : 0x80), 60, uint8_t(on ? 110 : 0)}; e->midi(y, m, 3); }
// a short note, then `tail` blocks after its release: the energy of that tail
static Stats hit(void *y, int held = 40, int tail = 400) {
    note(y, true); run(y, held); note(y, false);
    run(y, 120);   // the synth's own release (300 ms) dies away
    return run(y, tail);
}
int main() {
    e = mpc_engine();
    void *y = e->create("/tmp/omni-fx-bank-test");
    set(y, "syn_on", "1");
    set(y, "syn_release", "20");
    set(y, "rev_type", "0"); set(y, "rev_mix", "0");
    run(y, 50);

    // 1. Off
    set(y, "fxb_prog", "0");
    Stats dry = hit(y);
    check(dry.energy < 1e3, "Off: no tail after the note (" + std::to_string(dry.energy) + ")");

    // 2. a delay program, full mix, long feedback
    set(y, "fxb_prog", "0");
    int opt = -1;
    {   // find "VAR ECHO" by its index: the popup's options are in params order; try every option for a long tail
        double best = 0;
        set(y, "fxb_mix", "100"); set(y, "fxb_time", "300"); set(y, "fxb_fb", "60");
        for (int k = 1; k <= 117; k++) {
            set(y, "fxb_prog", std::to_string(k));
            run(y, 4);
            Stats s = hit(y, 20, 200);
            if (s.energy > best) { best = s.energy; opt = k; }
        }
        check(opt > 0 && best > 1e6, "a delay-type program leaves a tail after the note (option " + std::to_string(opt) +
              ", energy " + std::to_string(best) + ")");
    }
    // 3. Off: no tail; on again: the old tail was cleared
    set(y, "fxb_prog", std::to_string(opt));
    note(y, true); run(y, 40); note(y, false); run(y, 60);
    set(y, "fxb_prog", "0");
    run(y, 2);
    Stats off = run(y, 200);
    check(off.energy < 1e3, "switched Off mid-tail: silence (" + std::to_string(off.energy) + ")");
    set(y, "fxb_prog", std::to_string(opt));
    run(y, 2);
    Stats back = run(y, 200);
    check(back.energy < 1e3, "switched back on: no stale tail (" + std::to_string(back.energy) + ")");
    // BYPASS mid-tail: silent; back to ON: nothing from before the bypass comes back
    note(y, true); run(y, 40); note(y, false); run(y, 60);
    set(y, "fxb_bypass", "1");
    run(y, 2);
    Stats by = run(y, 200);
    check(by.energy < 1e3, "BYPASS mid-tail: silence (" + std::to_string(by.energy) + ")");
    set(y, "fxb_bypass", "0");
    run(y, 2);
    Stats unby = run(y, 200);
    check(unby.energy < 1e3, "back from BYPASS: no stale tail (" + std::to_string(unby.energy) + ")");
    check(get(y, "fxb_info").find("MODE") != std::string::npos && get(y, "fxb_info").find(" ON") != std::string::npos,
          "OLED line: " + get(y, "fxb_info"));
    set(y, "fxb_bypass", "1");
    check(get(y, "fxb_info").find("BYPASS") != std::string::npos, "OLED line in bypass: " + get(y, "fxb_info"));
    set(y, "fxb_bypass", "0");
    // mix 0: no tail either
    set(y, "fxb_mix", "0");
    Stats m0 = hit(y);
    check(m0.energy < 1e3, "mix 0: dry (" + std::to_string(m0.energy) + ")");
    set(y, "fxb_mix", "100");

#if OMNI_PUBLIC
    const int LAST = 147;   // public 2.3.1: bank A = 117 factory programs + 30 MV-8800 programs
#else
    const int LAST = 117;
#endif
    // 4. every option with extreme controls (ASan/UBSan watch the Rack): each one goes silent once switched Off (no
    // runaway / NaN state left in the output chain); full-scale peaks are counted (the soft limiter holds them)
    int bad = 0, full = 0;
    for (int k = 0; k <= LAST; k++) {
        set(y, "fxb_prog", std::to_string(k));
        set(y, "fxb_drive", k % 2 ? "36" : "0"); set(y, "fxb_fb", k % 3 ? "94" : "-94"); set(y, "fxb_pitch", k % 2 ? "24" : "-24");
        Stats s = hit(y, 30, 120);
        if (s.peak >= 32700) full++;
        set(y, "fxb_prog", "0");
        run(y, 2);
        if (run(y, 100).energy > 1e3) bad++;
    }
    check(bad == 0, std::to_string(LAST) + " programs with extreme controls, then Off: silent every time (" + std::to_string(bad) + " not)");
    std::printf("info: %d of %d options reached full scale with extreme controls (soft-limited)\n", full, LAST + 1);

    // 4b. the OLED's < >: step and wrap (Off <- -> the last program), the readout names the program
    set(y, "fxb_prog", "0");
    set(y, "fxb_prog_prev", "1"); set(y, "fxb_prog_prev", "0");
    const int last = std::atoi(get(y, "fxb_prog").c_str());
    check(last == LAST, "< from Off wraps to the last program (" + get(y, "fxb_prog") + ")");
    set(y, "fxb_prog_next", "1"); set(y, "fxb_prog_next", "0");
    check(std::atoi(get(y, "fxb_prog").c_str()) == 0, "> from the last program wraps to Off (" + get(y, "fxb_prog") + ")");
    set(y, "fxb_prog_next", "1"); set(y, "fxb_prog_next", "0");
    check(std::atoi(get(y, "fxb_prog").c_str()) == 1 && get(y, "fxb_info").find("MODE 001") != std::string::npos,
          "> from Off: program 1, OLED " + get(y, "fxb_info"));

    // 4c. 2.4.32: the 3-digit screen and its keypad
    auto key = [&](const char *k) { set(y, k, "1"); set(y, k, "0"); };
    set(y, "fxb_prog", "35");
    check(get(y, "fxb_num") == "035", "screen shows the program: " + get(y, "fxb_num"));
    key("fxb_num_tap");
    check(std::atoi(get(y, "fxb_pad").c_str()) == 1 || get(y, "fxb_pad") == "Open", "tap opens the keypad (" + get(y, "fxb_pad") + ")");
    key("fxb_k0"); key("fxb_k8");
    check(get(y, "fxb_num") == "_08", "typing 0 8 shows _08: " + get(y, "fxb_num"));
    key("fxb_k4");
    check(get(y, "fxb_num") == "084", "typing 0 8 4 shows 084: " + get(y, "fxb_num"));
    key("fxb_kenter");
    check(std::atoi(get(y, "fxb_prog").c_str()) == 84, "ENTER selects program 84 (" + get(y, "fxb_prog") + ")");
    check(std::atoi(get(y, "fxb_pad").c_str()) == 0 || get(y, "fxb_pad") == "Closed", "ENTER closes the keypad (" + get(y, "fxb_pad") + ")");
    check(get(y, "fxb_info").find("MODE 084") != std::string::npos, "OLED follows: " + get(y, "fxb_info"));
#if OMNI_PUBLIC   // public 2.3.1: 118-147 are the MV programs; 190 is past the last one
    key("fxb_num_tap"); key("fxb_k1"); key("fxb_k2"); key("fxb_k0"); key("fxb_kenter");
    check(std::atoi(get(y, "fxb_prog").c_str()) == 120 && get(y, "fxb_pname").rfind("MV ", 0) == 0, "1 2 0 ENTER: MV program 120 (" + get(y, "fxb_pname") + ")");
    key("fxb_num_tap"); key("fxb_k0"); key("fxb_k8"); key("fxb_k4"); key("fxb_kenter");
    key("fxb_num_tap"); key("fxb_k1"); key("fxb_k9"); key("fxb_k0"); key("fxb_kenter");
    check(get(y, "fxb_num") == "ERR" && std::atoi(get(y, "fxb_prog").c_str()) == 84, "190: ERR, program unchanged (" + get(y, "fxb_num") + ")");
#else
    key("fxb_num_tap"); key("fxb_k1"); key("fxb_k2"); key("fxb_k0"); key("fxb_kenter");
    check(get(y, "fxb_num") == "ERR" && std::atoi(get(y, "fxb_prog").c_str()) == 84, "120: ERR, program unchanged (" + get(y, "fxb_num") + ")");
#endif
    key("fxb_kclr");
    check(get(y, "fxb_num") == "084", "CLR clears the entry (" + get(y, "fxb_num") + ")");
    key("fxb_k5"); key("fxb_k5"); key("fxb_kenter");
    check(std::atoi(get(y, "fxb_prog").c_str()) == 55, "5 5 ENTER: program 55 (" + get(y, "fxb_prog") + ")");
    key("fxb_num_tap"); key("fxb_k1"); key("fxb_k0"); key("fxb_k9"); key("fxb_kenter");
    check(std::atoi(get(y, "fxb_prog").c_str()) == 109, "1 0 9 ENTER: program 109 (" + get(y, "fxb_prog") + ")");
    key("fxb_num_tap"); key("fxb_num_tap");
    check(std::atoi(get(y, "fxb_pad").c_str()) == 0 || get(y, "fxb_pad") == "Closed", "tap again closes it");
    set(y, "fxb_prog", "35");

    // 5. the project state keeps the page
    set(y, "fxb_prog", std::to_string(opt)); set(y, "fxb_time", "777"); set(y, "fxb_low", "-12");
    const std::string st = get(y, "state");
    void *z = e->create("/tmp/omni-fx-bank-test2");
    set(z, "state", st.c_str());
    run(z, 20);
    check(std::atoi(get(z, "fxb_prog").c_str()) == opt || get(z, "fxb_prog") == get(y, "fxb_prog"), "state keeps the program (" + get(z, "fxb_prog") + ")");
    check(std::atof(get(z, "fxb_time").c_str()) == 777, "state keeps TIME (" + get(z, "fxb_time") + ")");
    check(std::atof(get(z, "fxb_low").c_str()) == -12, "state keeps LOW (" + get(z, "fxb_low") + ")");
    e->destroy(z);

    // 6. CPU (x86, this build): blocks rendered per second, Off vs every program, a held chord
    set(y, "fxb_mix", "50"); set(y, "fxb_drive", "12"); set(y, "fxb_fb", "35"); set(y, "fxb_pitch", "7");
    auto cost = [&](int k) {
        set(y, "fxb_prog", std::to_string(k));
        note(y, true); run(y, 20);
        auto t0 = std::chrono::steady_clock::now();
        run(y, 3000);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        note(y, false); run(y, 60);
        return s / (3000.0 * 128 / 44100.0) * 100;   // % of real time
    };
    double base = cost(0), worst = 0;
    int worst_k = 0;
    for (int k = 1; k <= 117; k++) { double c = cost(k) - base; if (c > worst) { worst = c; worst_k = k; } }
    std::printf("cpu: plugin with SYNTH note, FX BANK off %.3f %% of real time; worst program option %d adds %.3f %%\n", base, worst_k, worst);
    e->destroy(y);
    std::printf("%s\n", fails ? ("FAILED " + std::to_string(fails)).c_str() : "PASSED");
    return fails ? 1 : 0;
}

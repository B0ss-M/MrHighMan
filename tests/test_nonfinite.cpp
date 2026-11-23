// Omni Sampler 2.4.32: the user's bug end to end (2026-11-22: "a pop, then the SYNTH silent; PANIC does nothing, only RESET").
// SYNTH on, EQ + reverb on, OSC A = Macro / Noise at a high pitch with no resonance (2.4.31: its filter diverged to inf /
// NaN, which then sat in the EQ and the FX), hold a note; then a Classic oscillator and a new note, and after PANIC: both
// must sound. Exit 1 = stuck silent.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
extern "C" {
#include "engine.h"
}
static const mpc_engine_t *e;
static std::vector<int16_t> buf(256);
static std::string get(void *y, const char *k) { char b[256]; int n = e->get_param(y, k, b, sizeof b); return n > 0 ? b : ""; }
static void idle(void *y, int ms) { for (int i = 0; i < ms / 3; i++) { e->render(y, buf.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(3)); } }
static int play(void *y, int note, double secs) {
    uint8_t on[3] = {0x90, uint8_t(note), 100}, off[3] = {0x80, uint8_t(note), 0};
    e->midi(y, on, 3);
    int pk = 0;
    for (int b = 0; b < int(secs * 44100 / 128); b++) { e->render(y, buf.data(), 128); for (int16_t v : buf) pk = std::max(pk, std::abs(int(v))); }
    e->midi(y, off, 3);
    for (int b = 0; b < 200; b++) e->render(y, buf.data(), 128);
    return pk;
}
int main() {
    e = mpc_engine();
    std::system("rm -rf /tmp/omni-heal");
    void *y = e->create("/tmp/omni-nonfinite-test");
    auto set = [&](const char *k, const char *v) { e->set_param(y, k, v); };
    set("syn_on", "1"); set("eq_low", "6"); set("rev_type", "2"); set("rev_mix", "30");
    idle(y, 1500);
    const int before = play(y, 60, 0.5);
    set("sa_type", "3"); set("sa_table", "8"); set("sa_pos", "0"); set("sa_warp", "100");
    idle(y, 1500);
    std::printf("osc A: %s\n", get(y, "sa_name").c_str());
    const int blow = play(y, 96, 1.0);
    set("sa_type", "0"); set("sa_table", "0");   // back to a Classic oscillator, no RESET, no PANIC
    idle(y, 1500);
    const int after = play(y, 60, 0.5);
    set("panic", "1"); set("panic", "0");
    idle(y, 1500);
    const int after_panic = play(y, 60, 0.5);
    std::printf("peak: clean %d | noise high %d | clean again %d | after PANIC %d\n", before, blow, after, after_panic);
    const bool ok = before > 1000 && after > 1000 && after_panic > 1000;
    std::printf("%s\n", ok ? "RECOVERS" : "STUCK SILENT");
    e->destroy(y);
    return ok ? 0 : 1;
}

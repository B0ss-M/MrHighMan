// Omni Sampler 2.4.33: the FX BANK bay's slots A-D through the plugin (SYNTH as the sound source).
//   test_fx_slots
// A test-only expansion bank (256 programs, halves the level, counts its calls) fills slots B and D; C stays empty.
// 1. slot A as before (Off, a factory program by name); 2. slot B: < > wrap over Off + 256, the keypad reaches 256 and
// refuses 257 (ERR), the OLED lines; 3. audio: B's program runs (half level), BYPASS / Off pass dry, A's rack is off
// while B is chosen; 4. slot C empty: EMPTY SLOT, < > stay on Off, the keypad refuses 1, dry; 5. each slot keeps its own
// program; 6. the project state keeps the slot and the programs.
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
extern "C" {
#include "engine.h"
}
#include "engine/fx_expansion.hpp"

namespace {
int g_selects = 0, g_last_select = 0, g_processed = 0, g_prepared = 0;
struct FakeBank final : omni::FxExpansion {
    std::string names[257];
    FakeBank() { for (int i = 1; i <= 256; i++) { char b[16]; std::snprintf(b, sizeof b, "PROG %03d", i); names[i] = b; } }
    const char *title() const override { return "FAKE BANK"; }
    int count() const override { return 256; }
    const char *name(int p) const override { return p >= 1 && p <= 256 ? names[p].c_str() : ""; }
    void prepare(double, int) override { g_prepared++; }
    void select(int p) override { g_selects++; g_last_select = p; }
    void set(const omnisampler::fxbank::Controls &) override {}
    void process(float *l, float *r, int frames) override {
        g_processed++;
        for (int i = 0; i < frames; i++) { l[i] *= 0.5f; r[i] *= 0.5f; }
    }
    void reset() override {}
};
std::unique_ptr<omni::FxExpansion> fake_factory(int slot) {
    return slot == 1 || slot == 3 ? std::make_unique<FakeBank>() : nullptr;   // B and D fitted, C empty
}
}  // namespace

static const mpc_engine_t *e;
static int fails = 0;
static void check(bool ok, const std::string &w) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", w.c_str()); if (!ok) fails++; }
static std::string get(void *y, const std::string &k) { char b[1 << 16] = {}; e->get_param(y, k.c_str(), b, sizeof b); return b; }
static void set(void *y, const std::string &k, const std::string &v) { e->set_param(y, k.c_str(), v.c_str()); }
static void tap(void *y, const std::string &k) { set(y, k, "1"); set(y, k, "0"); }
static int peak(void *y, int blocks) {   // a held note's peak over `blocks` (after 20 blocks to settle)
    std::vector<int16_t> b(256);
    uint8_t on[3] = {0x90, 60, 110}, off[3] = {0x80, 60, 0};
    e->midi(y, on, 3);
    int pk = 0;
    for (int i = 0; i < 20 + blocks; i++) {
        e->render(y, b.data(), 128);
        if (i >= 20) for (int16_t v : b) pk = std::max(pk, std::abs(int(v)));
    }
    e->midi(y, off, 3);
    for (int i = 0; i < 200; i++) e->render(y, b.data(), 128);
    return pk;
}
static void idle(void *y, int ms) {   // the SYNTH's oscillators load on a worker: real time with audio running
    std::vector<int16_t> b(256);
    for (int i = 0; i < ms / 3; i++) { e->render(y, b.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(3)); }
}
static void keypad(void *y, const std::string &digits) {
    tap(y, "fxb_num_tap");
    for (char c : digits) tap(y, std::string("fxb_k") + c);
    tap(y, "fxb_kenter");
}

int main() {
    omni::set_fx_expansion_factory(fake_factory);
    e = mpc_engine();
    void *y = e->create("/tmp/omni-fx-slots-test");
    check(g_prepared == 2, "the fitted slots (B, D) were prepared once each in the constructor (" + std::to_string(g_prepared) + ")");
    set(y, "syn_on", "1");
    set(y, "rev_type", "0"); set(y, "rev_mix", "0");
    std::vector<int16_t> b(256);
    idle(y, 1500);

    // 1. slot A, as in 2.4.32
    check(get(y, "fxb_slot") == "0", "default slot A (" + get(y, "fxb_slot") + ")");
    check(get(y, "fxb_pname") == "Off", "A: Off on the OLED (" + get(y, "fxb_pname") + ")");
    const int dry = peak(y, 40);
    check(dry > 1000, "the synth plays dry (" + std::to_string(dry) + ")");
    set(y, "fxb_prog", "1");
    check(get(y, "fxb_pname") != "Off" && !get(y, "fxb_pname").empty(), "A: program 1 named (" + get(y, "fxb_pname") + ")");
    check(get(y, "fxb_num") == "001", "A: PROGRAM # 001 (" + get(y, "fxb_num") + ")");
    set(y, "fxb_prog", "0");

    // 2. slot B: arrows, keypad, OLED
    set(y, "fxb_slot", "1");
    check(get(y, "fxb_slot") == "1", "slot B chosen (" + get(y, "fxb_slot") + ")");
    check(get(y, "fxb_pname") == "Off", "B: Off at first (" + get(y, "fxb_pname") + ")");
    check(get(y, "fxb_info").find("FAKE BANK") != std::string::npos, "B: the bank's title on the OLED (" + get(y, "fxb_info") + ")");
    tap(y, "fxb_prog_next");
    check(get(y, "fxb_prog_b") == "1" && get(y, "fxb_pname") == "PROG 001", "B: > goes to program 1 (" + get(y, "fxb_pname") + ")");
    tap(y, "fxb_prog_prev"); tap(y, "fxb_prog_prev");
    check(get(y, "fxb_prog_b") == "256" && get(y, "fxb_pname") == "PROG 256", "B: < wraps over Off to 256 (" + get(y, "fxb_prog_b") + ")");
    check(get(y, "fxb_num") == "256", "B: PROGRAM # 256 (" + get(y, "fxb_num") + ")");
    check(get(y, "fxb_info").find("256 / 256") != std::string::npos, "B: 256 / 256 on the OLED (" + get(y, "fxb_info") + ")");
    keypad(y, "084");
    check(get(y, "fxb_prog_b") == "84", "B: keypad 084 (" + get(y, "fxb_prog_b") + ")");
    keypad(y, "257");
    check(get(y, "fxb_num") == "ERR" && get(y, "fxb_prog_b") == "84", "B: keypad 257 = ERR, program kept (" + get(y, "fxb_num") + ")");
    tap(y, "fxb_kclr"); tap(y, "fxb_num_tap");
    keypad(y, "256");
    check(get(y, "fxb_prog_b") == "256", "B: keypad 256 (" + get(y, "fxb_prog_b") + ")");
    check(get(y, "fxb_prog") == "0", "A's own program untouched by B's keypad (" + get(y, "fxb_prog") + ")");

    // 3. audio through slot B
    keypad(y, "001");
    const int sel0 = g_selects, proc0 = g_processed;
    const int wet = peak(y, 40);
    check(g_processed > proc0, "B: the bank processed the mix");
    check(g_last_select == 1 && g_selects > sel0, "B: program 1 selected in the bank (" + std::to_string(g_last_select) + ")");
    check(std::abs(wet - dry / 2) <= dry / 20 + 2, "B: half level through the bank (" + std::to_string(wet) + " vs dry " + std::to_string(dry) + ")");
    set(y, "fxb_bypass", "1");
    int proc1 = g_processed;
    check(std::abs(peak(y, 40) - dry) <= dry / 50 + 2 && g_processed == proc1, "B: BYPASS passes dry, the bank idle");
    set(y, "fxb_bypass", "0");
    set(y, "fxb_prog_b", "0");
    proc1 = g_processed;
    check(std::abs(peak(y, 40) - dry) <= dry / 50 + 2 && g_processed == proc1, "B: Off passes dry, the bank idle");
    set(y, "fxb_prog", "1");   // A has a program, but slot B is chosen: the factory rack must not run
    set(y, "fxb_prog_b", "1");
    check(std::abs(peak(y, 40) - dry / 2) <= dry / 20 + 2, "B chosen: A's rack is off, only B runs");
    set(y, "fxb_prog", "0");

    // 4. slot C: empty
    set(y, "fxb_slot", "2");
    check(get(y, "fxb_pname") == "EMPTY SLOT", "C: EMPTY SLOT (" + get(y, "fxb_pname") + ")");
    check(get(y, "fxb_info").find("NO EXPANSION") != std::string::npos, "C: no expansion fitted (" + get(y, "fxb_info") + ")");
    tap(y, "fxb_prog_next");
    check(get(y, "fxb_prog_c") == "0", "C: > stays on Off (" + get(y, "fxb_prog_c") + ")");
    keypad(y, "1");
    check(get(y, "fxb_num") == "ERR" && get(y, "fxb_prog_c") == "0", "C: keypad 1 = ERR (" + get(y, "fxb_num") + ")");
    tap(y, "fxb_kclr"); tap(y, "fxb_num_tap");
    proc1 = g_processed;
    check(std::abs(peak(y, 40) - dry) <= dry / 50 + 2 && g_processed == proc1, "C: dry, nothing runs");

    // 5. each slot keeps its own program
    set(y, "fxb_slot", "3");
    keypad(y, "200");
    set(y, "fxb_slot", "1");
    check(get(y, "fxb_pname") == "PROG 001", "B kept program 1 (" + get(y, "fxb_pname") + ")");
    set(y, "fxb_slot", "3");
    check(get(y, "fxb_pname") == "PROG 200", "D kept program 200 (" + get(y, "fxb_pname") + ")");

    // 6. the project state
    const std::string st = get(y, "state");
    void *z = e->create("/tmp/omni-fx-slots-test");
    set(z, "state", st.c_str());
    idle(z, 500);
    check(get(z, "fxb_slot") == "3" && get(z, "fxb_prog_d") == "200" && get(z, "fxb_prog_b") == "1",
          "restored: slot D, D 200, B 1 (" + get(z, "fxb_slot") + " " + get(z, "fxb_prog_d") + " " + get(z, "fxb_prog_b") + ")");
    e->destroy(z);
    e->destroy(y);
    std::printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}

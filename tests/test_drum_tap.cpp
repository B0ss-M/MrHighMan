// Omni Sampler 2.4.11: a drum machine ROM in the browser, as a finger uses it: tapping the chip offers WHOLE KIT (every
// sound on its own key from C1, one layer) or ONE SOUND (the kit's sounds; one across the keyboard); back goes to the choice.
//   test_drum_tap <a drum ROM chip file>   (e.g. Roland TR-707 sound ROMs/IC34_TR707_SNDROM.bin, with its folder's chips)
#include <algorithm>
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
static int fails = 0;
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
static std::string get(void *y, const std::string &k) { char b[512]; int n = e->get_param(y, k.c_str(), b, sizeof b); return n > 0 ? std::string(b) : ""; }
static std::vector<int16_t> buf(256);
static void idle(void *y, int ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) { e->render(y, buf.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}
static int peak_of(void *y, int note) {
    uint8_t on[3] = {0x90, uint8_t(note), 110}, off[3] = {0x80, uint8_t(note), 0};
    e->midi(y, on, 3);
    int pk = 0;
    for (int b = 0; b < 40; b++) { e->render(y, buf.data(), 128); for (int16_t v : buf) pk = std::max(pk, std::abs(int(v))); }
    e->midi(y, off, 3);
    for (int b = 0; b < 300; b++) e->render(y, buf.data(), 128);
    return pk;
}
static int row_with(void *y, const std::string &part) {   // the visible row (1..10) whose text holds part, else 0
    for (int r = 1; r <= 10; r++) if (get(y, "br_" + std::to_string(r)).find(part) != std::string::npos) return r;
    return 0;
}
static bool tap(void *y, int r) {
    if (!r) return false;
    e->set_param(y, ("br_" + std::to_string(r)).c_str(), "1");
    idle(y, 150);
    return true;
}
static void wait_load(void *y) {
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        idle(y, 20);
        const std::string st = get(y, "status");
        if ((st.compare(0, 7, "Loading") && st.compare(0, 7, "Reading")) && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(300)) break;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30)) break;
    }
    idle(y, 200);
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: test_drum_tap <drum ROM chip> [kit name]\n"); return 2; }
    // 2.4.18: [kit name] picks the kit when the chip's folder holds several (else the first WHOLE KIT row)
    const std::string kit = argc > 2 ? argv[2] : "";
    const std::string f = argv[1], dir = f.substr(0, f.find_last_of('/')), name = f.substr(f.find_last_of('/') + 1);
    e = mpc_engine();
    void *y = e->create("/tmp/omni-drum-tap-test");
    e->set_param(y, "br_path", dir.c_str());
    check(tap(y, row_with(y, name)), "the chip is listed and tapped");
    for (int r = 1; r <= 10; r++) { const std::string s = get(y, "br_" + std::to_string(r)); if (s != " ") std::printf("     %s\n", s.c_str()); }
    const int whole = row_with(y, "WHOLE KIT:" + (kit.empty() ? "" : " " + kit)), one = row_with(y, "[ONE SOUND:" + (kit.empty() ? "" : " " + kit));
    check(whole && one, "tapping it asks: WHOLE KIT or ONE SOUND");
    // the whole kit: one layer, every sound on its own key from C1 (36), no gaps
    tap(y, whole);
    wait_load(y);
    const std::string info = get(y, "prog_info");
    const int zones = std::atoi(info.substr(info.find(" - ") == std::string::npos ? 0 : info.find(" - ") + 3).c_str());
    std::printf("     loaded: %s | %s\n", get(y, "prog_name").c_str(), info.c_str());
    bool all = zones > 1;
    for (int k = 36; k < 36 + zones && all; k++) if (peak_of(y, k) < 100) { std::printf("     key %d silent\n", k); all = false; }
    check(all, "WHOLE KIT: " + std::to_string(zones) + " sounds on keys C1 up, every key plays");
    check(peak_of(y, 35) == 0 && peak_of(y, 36 + zones) == 0, "WHOLE KIT: nothing below C1 or past its last sound (no gaps, no spread)");
    // ONE SOUND: the kit's sounds; one plays across the keyboard
    tap(y, row_with(y, "[ONE SOUND:" + (kit.empty() ? "" : " " + kit)));
    const std::string back = get(y, "br_1");
    check(back.find("WHOLE KIT / ONE SOUND") != std::string::npos && !get(y, "br_2").empty() && get(y, "br_2") != " ", "ONE SOUND lists the kit's sounds (back row first)");
    tap(y, 3);   // its second sound
    wait_load(y);
    std::printf("     loaded: %s | %s\n", get(y, "prog_name").c_str(), get(y, "prog_info").c_str());
    check(get(y, "prog_info").find(" 1 zones") != std::string::npos && peak_of(y, 48) > 100 && peak_of(y, 72) > 100,
          "ONE SOUND: one sound, it plays across the keyboard");
    tap(y, 1);   // back: the choice again
    check(row_with(y, "WHOLE KIT:") != 0, "back from the sounds: WHOLE KIT / ONE SOUND again");
    e->destroy(y);
    std::printf("%s\n", fails ? ("FAILED " + std::to_string(fails)).c_str() : "PASSED");
    return fails ? 1 : 0;
}

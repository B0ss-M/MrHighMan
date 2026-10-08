// Omni Sampler patch test: save a multi-slot patch, reopen it in a fresh instance, check slots + settings.
// Also: project reload keeps the project's settings over an .omni's; mono fallback leaves no pad lit.
//   test_patch <slot A file> <slot B: a preset inside a disk image> <slot C file>   (data in /tmp/omni-patch-test)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}
static const mpc_engine_t *e;
static std::vector<int16_t> buf(256);
static int fails = 0;
static std::string get(void *in, const char *k) { char b[8192] = {}; e->get_param(in, k, b, sizeof b); return b; }
static void run(void *in, int blocks) { for (int i = 0; i < blocks; i++) e->render(in, buf.data(), 128); }
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
// render until the loader is idle and status is not a transient one
static void settle(void *in, int min_ms = 300) {
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        run(in, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::string s = get(in, "status");
        bool transient = s.compare(0, 7, "Loading") == 0 || s.compare(0, 7, "Reading") == 0 || s.compare(0, 6, "Saving") == 0 ||
                         s.compare(0, 10, "Extracting") == 0 || get(in, "br_info") == "Opening...";
        if (ms > min_ms && !transient) return;
        if (ms > 120000) { std::printf("settle timeout (%s)\n", s.c_str()); return; }
    }
}
static int voices_for(void *in, int note) {
    uint8_t on[3] = {0x90, uint8_t(note), 100}, off[3] = {0x80, uint8_t(note), 0};
    e->midi(in, on, 3); run(in, 20);
    int v = std::atoi(get(in, "status").c_str());
    e->midi(in, off, 3); run(in, 800);
    return v;
}
static bool tap_named(void *in, const std::string &dir, const std::string &label) {
    e->set_param(in, "br_path", dir.c_str());
    for (int page = 0; page < 20; page++) {
        for (int r = 1; r <= 10; r++) {
            std::string k = "br_" + std::to_string(r);
            if (get(in, k.c_str()) == label) { e->set_param(in, k.c_str(), "1"); settle(in); return true; }
        }
        e->set_param(in, "br_next", "1"); settle(in, 20);
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    e = mpc_engine();
    (void)!std::system("rm -rf /tmp/omni-patch-test");
    const char *data = "/tmp/omni-patch-test";

    // 1. three instruments in A, B, C with their own ranges, a keyswitch-free layer, a tweaked filter
    void *a = e->create(data);
    std::string st = std::string("omni-sampler 1\nslot1_path=") + argv[1] + "\nslot1_preset=0\nslot2_path=" + argv[2] +
                     "\nslot2_preset=0\nslot3_path=" + argv[3] + "\nslot3_preset=0\n";
    e->set_param(a, "state", st.c_str());
    settle(a, 1500);
    std::string nA = get(a, "slot1_name"), nB = get(a, "slot2_name"), nC = get(a, "slot3_name");
    std::printf("loaded: A=%s B=%s C=%s | %s\n", nA.c_str(), nB.c_str(), nC.c_str(), get(a, "status").c_str());
    e->set_param(a, "slot1_lo", "0");  e->set_param(a, "slot1_hi", "59");
    e->set_param(a, "slot2_lo", "60"); e->set_param(a, "slot2_hi", "127");
    e->set_param(a, "slot3_lo", "48"); e->set_param(a, "slot3_hi", "72"); e->set_param(a, "slot3_vol", "-6"); e->set_param(a, "slot3_tune", "12");
    e->set_param(a, "cutoff", "63");
    e->set_param(a, "mod2_amt", "40");
    int v50 = voices_for(a, 50), v66 = voices_for(a, 66);
    std::printf("before save: note 50 -> %d voices, note 66 -> %d voices\n", v50, v66);
    e->set_param(a, "patch_save", "1"); e->set_param(a, "patch_save", "0");
    settle(a, 500);
    std::string saved = get(a, "status");
    check(saved.compare(0, 11, "Saved patch") == 0, "save: " + saved);
    check(get(a, "br_info") == saved, "browse info shows it: " + get(a, "br_info"));
    std::string pdir = std::string(data) + "/library/Patches";
    (void)!std::system(("ls -R '" + pdir + "' | head -20; cat '" + pdir + "'/*.omnipatch | head -c 900; echo").c_str());
    std::string state_a = get(a, "state");
    e->destroy(a);

    // 2. a fresh instance with nothing loaded opens the patch from the browser
    void *b = e->create(data);
    std::string pname = saved.substr(saved.rfind('/') + 1);
    check(tap_named(b, pdir, pname), "patch listed in the browser: " + pname);
    settle(b, 1500);
    std::printf("opened: %s | A=%s B=%s C=%s D=%s\n", get(b, "br_info").c_str(), get(b, "slot1_name").c_str(), get(b, "slot2_name").c_str(),
                get(b, "slot3_name").c_str(), get(b, "slot4_name").c_str());
    check(get(b, "slot1_name") == nA && get(b, "slot2_name") == nB && get(b, "slot3_name") == nC && get(b, "slot4_name") == "(empty)", "slot instruments");
    check(get(b, "slot1_hi") == "59" && get(b, "slot2_lo") == "60" && get(b, "slot3_vol") == "-6" && get(b, "slot3_tune") == "12", "slot settings");
    check(get(b, "cutoff") == "63" && get(b, "mod2_amt") == "40", "sound settings (cutoff " + get(b, "cutoff") + ")");
    int w50 = voices_for(b, 50), w66 = voices_for(b, 66);
    check(w50 == v50 && w66 == v66, "plays the same: note 50 " + std::to_string(w50) + ", note 66 " + std::to_string(w66));
    // the slots now refer to the patch's .omni files: a project needs no disk image
    std::string st_b = get(b, "state");
    check(st_b.find("slot2_path=" + pdir) != std::string::npos || st_b.find("slot2_path=" + std::string(data) + "/library/Extracted") != std::string::npos,
          "slot B saved as an extracted .omni");
    e->destroy(b);

    // 3. project reload: the project's cutoff wins over the one an .omni carries
    void *c = e->create(data);
    e->set_param(c, "state", (std::string("omni-sampler 1\nslot1_path=") + argv[3] + "\nslot1_preset=0\n").c_str());
    settle(c, 800);
    e->set_param(c, "cutoff", "40");
    e->set_param(c, "br_extract", "1"); e->set_param(c, "br_extract", "0");   // SAVE TO LIBRARY: .omni with cutoff 40
    settle(c, 500);
    std::printf("save to library: %s\n", get(c, "status").c_str());
    e->set_param(c, "cutoff", "77");                                           // tweaked after saving
    std::string proj = get(c, "state");
    e->destroy(c);
    void *d = e->create(data);
    e->set_param(d, "state", proj.c_str());
    settle(d, 1000);
    check(get(d, "cutoff") == "77", "project reload keeps its cutoff (got " + get(d, "cutoff") + ", want 77)");
    // but tapping that .omni in the browser still brings its stored sound
    e->set_param(d, "cutoff", "90");
    std::string f = proj.substr(proj.find("slot1_path=") + 11); f = f.substr(0, f.find('\n'));
    check(tap_named(d, f.substr(0, f.rfind('/')), f.substr(f.rfind('/') + 1)), "tap the saved .omni");
    settle(d, 800);
    check(get(d, "cutoff") == "40", "browsing an .omni applies its settings (got " + get(d, "cutoff") + ")");

    // 4. mono mode: the fallback note leaves no key counted as down
    e->set_param(d, "voice_mode", "1");
    uint8_t on60[3] = {0x90, 60, 100}, on64[3] = {0x90, 64, 100}, off60[3] = {0x80, 60, 0}, off64[3] = {0x80, 64, 0};
    e->midi(d, on60, 3); run(d, 5); e->midi(d, on64, 3); run(d, 5); e->midi(d, off64, 3); run(d, 5); e->midi(d, off60, 3); run(d, 50);
    e->set_param(d, "pad_base", "52");
    std::string lit;
    for (int p = 1; p <= 16; p++) lit += get(d, ("pad_" + std::to_string(p)).c_str());
    check(lit.find('1') == std::string::npos, "mono fallback leaves no pad lit: " + lit);
    e->destroy(d);

    // 5. hanging notes: a note-off on another channel than its note-on; PANIC silences everything and clears held keys
    {
        void *h = e->create(data);
        e->set_param(h, "state", (std::string("omni-sampler 1\nslot1_path=") + argv[3] + "\nslot1_preset=0\n").c_str());
        settle(h, 800);
        auto v = [&] { return std::atoi(get(h, "status").c_str()); };
        auto msg = [&](uint8_t a, uint8_t b, uint8_t c) { uint8_t m[3] = {a, b, c}; e->midi(h, m, 3); };
        msg(0x90, 62, 100); run(h, 20); msg(0x81, 62, 0); run(h, 800);
        check(v() == 0, "note-on channel 1, note-off channel 2: released (" + std::to_string(v()) + " voices)");
        msg(0x90, 60, 100); run(h, 20);
        int per = v();                                         // voices one note starts (layered zones)
        msg(0x91, 60, 100); run(h, 20); msg(0x81, 60, 0); run(h, 800);
        check(per > 0 && v() == per, "the same note held on two channels: one note-off leaves the other sounding (" +
              std::to_string(v()) + " of " + std::to_string(per) + ")");
        msg(0x80, 60, 0); run(h, 800);
        check(v() == 0, "and its own note-off ends it");
        msg(0xB0, 64, 127);                                    // pedal down, keys held, no note-offs at all
        for (int n : {48, 52, 55, 60, 64}) msg(0x90, uint8_t(n), 100);
        e->set_param(h, "pad_base", "48");
        e->set_param(h, "pad_1", "1");
        run(h, 20);
        int before = v();
        e->set_param(h, "panic", "1"); e->set_param(h, "panic", "0");
        run(h, 4);                                             // ~12 ms
        std::string lit;
        for (int p = 1; p <= 16; p++) lit += get(h, ("pad_" + std::to_string(p)).c_str());
        check(before >= 5 && v() == 0 && lit.find('1') == std::string::npos,
              "PANIC: " + std::to_string(before) + " held voices silenced within 12 ms, no pad lit (" + lit + ")");
        msg(0x90, 67, 100); run(h, 20);
        int after = v();
        msg(0x80, 67, 0); run(h, 800);
        check(after >= 1 && v() == 0, "after PANIC (pedal cleared) a new note plays and releases normally");
        e->destroy(h);
    }

    std::printf(fails ? "FAILED %d\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

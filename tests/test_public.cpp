// Omni Sampler PUBLIC 2.3 test (build with -DOMNI_PUBLIC=1): what the public build changes.
//   test_public <Serum preset> <Ableton .adv> <Decent .dspreset inside a .dslibrary>     (data in /tmp/omni-public-test)
//   1. the reader set: the public formats are there, the private-only ones are not
//   2. the FX BANK bay: slots B-D are empty (no expansion bank)
//   3. SAVE TO LIBRARY: a Serum preset (a synth's preset) and a Decent library preset save a REFERENCE .omni (no samples);
//      an Ableton preset saves an .omni WITH its samples; a reference .omni loads its source again and plays
//   4. a patch with a reference slot and a sample slot: each saved the right way, reopened in a fresh instance
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include "engine/fx_expansion.hpp"
#include "formats/format.hpp"
#include "formats/omni_native.hpp"
extern "C" {
#include "engine.h"
}
#if !OMNI_PUBLIC
#error "test_public is for the public build: compile with -DOMNI_PUBLIC=1"
#endif
static const mpc_engine_t *e;
static std::vector<int16_t> buf(256);
static int fails = 0;
static std::string get(void *in, const char *k) { char b[8192] = {}; e->get_param(in, k, b, sizeof b); return b; }
static void run(void *in, int blocks) { for (int i = 0; i < blocks; i++) e->render(in, buf.data(), 128); }
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
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
static int peak_of(void *in, int note) {   // play a note for 1.5 s: the loudest sample
    uint8_t on[3] = {0x90, uint8_t(note), 110}, off[3] = {0x80, uint8_t(note), 0};
    e->midi(in, on, 3);
    int peak = 0;
    for (int b = 0; b < 517; b++) { e->render(in, buf.data(), 128); for (int16_t v : buf) peak = std::max(peak, std::abs(int(v))); }
    e->midi(in, off, 3); run(in, 400);
    return peak;
}
// the browser, as a user picks a preset (test_tap's helpers): a preset's own settings (a synth patch) apply only then
static void wait_browser(void *y) {
    for (int i = 0; i < 30; i++) { run(y, 1); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }   // (as test_tap's idle)
    for (int i = 0; i < 3000 && get(y, "br_info") == "Opening..."; i++) { run(y, 1); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
}
static bool same_label(std::string s, const std::string &name) {
    if (s.compare(0, 2, "> ") == 0) s = s.substr(2);
    const size_t note = s.rfind("  [");
    if (note != std::string::npos && !s.empty() && s.back() == ']') s = s.substr(0, note);
    if (s == name) return true;
    const size_t dots = s.find("...");
    if (dots == std::string::npos) return false;
    const size_t n = dots >= 8 ? dots : 20;
    return name.compare(0, n, s.substr(0, n)) == 0;
}
// the row showing `name` in the current list (1..10, paging down), 0 if none; group rows "[Type (n)]" collected
static int find_row(void *y, const std::string &name, std::vector<std::string> *groups) {
    for (int page = 0; page < 500; page++) {
        for (int r = 1; r <= 10; r++) {
            std::string s = get(y, ("br_" + std::to_string(r)).c_str());
            if (s.compare(0, 2, "> ") == 0) s = s.substr(2);
            if (same_label(s, name)) return r;
            if (groups && s.size() > 4 && s.front() == '[' && s.back() == ']' && s.find(" (") != std::string::npos) groups->push_back(s);
        }
        const std::string before = get(y, "br_1");
        e->set_param(y, "br_rowdown", "1");
        wait_browser(y);
        if (get(y, "br_1") == before) break;
    }
    return 0;
}
static bool tap_file(void *y, const std::string &file) {
    const std::string dir = file.substr(0, file.rfind('/')), name = file.substr(file.rfind('/') + 1);
    e->set_param(y, "br_path", dir.c_str());
    wait_browser(y);
    std::vector<std::string> groups;
    int hit = find_row(y, name, &groups);
    for (size_t g = 0; !hit && g < groups.size(); g++) {   // a folder of mixed types: inside the type's group
        e->set_param(y, "br_path", dir.c_str());
        wait_browser(y);
        const int gr = find_row(y, groups[g], nullptr);
        if (!gr) continue;
        e->set_param(y, ("br_" + std::to_string(gr)).c_str(), "1");
        wait_browser(y);
        hit = find_row(y, name, nullptr);
    }
    if (!hit) return false;
    e->set_param(y, ("br_" + std::to_string(hit)).c_str(), "1");
    settle(y, 6000);
    return true;
}
static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static std::string slot_path(void *in) { std::string s = get(in, "state"); size_t a = s.find("slot1_path="); if (a == std::string::npos) return ""; a += 11; return s.substr(a, s.find('\n', a) - a); }

// SAVE TO LIBRARY on slot A loaded from `file` (preset 0): the .omni written
static std::string save_one(const char *data, const std::string &file, int &peak_before) {
    void *a = e->create(data);
    if (!tap_file(a, file)) std::printf("  NOT LISTED: %s\n", file.c_str());   // (a synth preset's samples / tables load on the synth worker)
    peak_before = std::max(peak_of(a, 60), peak_of(a, 72));
    e->set_param(a, "br_extract", "1"); e->set_param(a, "br_extract", "0");
    settle(a, 800);
    std::printf("  %s -> %s\n", file.substr(file.rfind('/') + 1).c_str(), get(a, "status").c_str());
    std::string out = slot_path(a);
    e->destroy(a);
    return out;
}

int main(int argc, char **argv) {
    if (argc < 4) { std::printf("usage: test_public <Serum preset> <Ableton .adv> <Decent preset in a .dslibrary>\n"); return 2; }
    e = mpc_engine();
    (void)!std::system("rm -rf /tmp/omni-public-test");
    const char *data = "/tmp/omni-public-test";
    setenv("OMNI_SAMPLER_LIBRARY", "/tmp/omni-public-test/library", 1);

    // 1. the reader set
    omni::register_all();
    auto has = [](const char *prefix) {
        for (const auto &f : omni::formats()) if (std::strncmp(f.name, prefix, std::strlen(prefix)) == 0) return true;
        return false;
    };
    for (const char *p : {"Serum 2", "Vital", "Heat Up", "LUXONIX Purity", "Kurzweil K2000", "Roland JV-880", "Ableton", "Alesis drum",
                          "Bliss", "Decent Sampler", "DirectWave", "Ensoniq KT-76", "Ensoniq MR", "Omni ROM map", "Drum machine ROM",
                          "SFZ", "Korg M1", "Korg TRITON", "Korg Wavestation", "Ensoniq SD-1", "Logic EXS", "Reason NN-XT", "NI Kontakt"})
        check(has(p), std::string("public reader present: ") + p);
    for (const char *p : {"Omnisphere", "reFX Nexus", "Nexus", "Yamaha DX7", "DX7", "Kawai", "Roland D-50", "Roland D-70", "Roland D-110",
                          "Roland JD-800", "Roland MT-32", "Roland SC-55", "Roland SC-88", "Roland SRX", "UJAM", "Arturia", "Maize", "Korg DW-8000"})
        check(!has(p), std::string("private-only reader absent: ") + p);
    std::printf("  %zu readers registered\n", omni::formats().size());

    // 2. the FX BANK bay
    std::unique_ptr<omni::FxExpansion> x[3];
    omni::make_fx_expansions(x);
    check(!x[0] && !x[1] && !x[2], "FX BANK slots B-D are empty");

    // 3. SAVE TO LIBRARY: reference vs samples
    int pk = 0;
    std::string ref = save_one(data, argv[1], pk);
    check(pk > 50, "the Serum preset plays (peak " + std::to_string(pk) + ")");
    check(!ref.empty() && omni::omni_is_reference(ref), "Serum preset saved as a REFERENCE .omni: " + ref);
    check(!exists(ref.substr(0, ref.size() - 5) + " Samples"), "  no samples written for it");
    {   // the reference loads its source again and plays
        void *b = e->create(data);
        check(tap_file(b, ref), "the reference .omni is listed in the browser");
        const int p2 = std::max(peak_of(b, 60), peak_of(b, 72));
        check(p2 > 50, "the reference .omni loads and plays (peak " + std::to_string(p2) + ", status " + get(b, "status") + ")");
        check(slot_path(b) == ref, "  the project keeps the reference .omni as the slot's file");
        e->destroy(b);
    }
    std::string smp = save_one(data, argv[2], pk);
    check(pk > 50, "the Ableton preset plays (peak " + std::to_string(pk) + ")");
    check(!smp.empty() && !omni::omni_is_reference(smp) && exists(smp.substr(0, smp.size() - 5) + " Samples"),
          "Ableton preset saved WITH its samples: " + smp);
    std::string dref = save_one(data, argv[3], pk);
    check(pk > 50, "the Decent library preset plays (peak " + std::to_string(pk) + ")");
    check(!dref.empty() && omni::omni_is_reference(dref), "Decent library preset saved as a REFERENCE .omni: " + dref);

    // 4. a patch: slot A the Serum preset (reference), slot B the Ableton preset (samples)
    {
        void *a = e->create(data);
        e->set_param(a, "state", (std::string("omni-sampler 1\nslot1_path=") + argv[1] + "\nslot1_preset=0\nslot2_path=" + argv[2] + "\nslot2_preset=0\n").c_str());
        settle(a, 2000);
        e->set_param(a, "patch_save", "1"); e->set_param(a, "patch_save", "0");
        settle(a, 800);
        std::string saved = get(a, "status");
        check(saved.compare(0, 11, "Saved patch") == 0, "patch saved: " + saved);
        std::string st = get(a, "state");
        e->destroy(a);
        auto path_of = [&](int slot) { std::string k = "slot" + std::to_string(slot) + "_path="; size_t p = st.find(k); if (p == std::string::npos) return std::string(); p += k.size(); return st.substr(p, st.find('\n', p) - p); };
        const std::string pa = path_of(1), pb = path_of(2);
        check(omni::omni_is_reference(pa), "patch slot A (Serum) is a reference: " + pa);
        check(!omni::omni_is_reference(pb) && exists(pb.substr(0, pb.size() - 5) + " Samples"), "patch slot B (Ableton) has its samples: " + pb);
        void *b = e->create(data);
        e->set_param(b, "state", st.c_str());
        settle(b, 3000);
        check(get(b, "slot1_name") != "(empty)" && get(b, "slot2_name") != "(empty)", "the patch's project reopens both slots: " + get(b, "slot1_name") + " / " + get(b, "slot2_name"));
        e->destroy(b);
    }
    std::printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}

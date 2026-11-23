// Omni Sampler 2.4.18: the GRID with real MIDI files (the user's library: chord progressions). Every file in <dir> is
// loaded, then GENERATE in all 13 modes, each from the file as loaded; every new part is checked against the file's
// chords (read at the grid's step): a new part (not the chords' blocks), spread over the rows, chord tones on the
// beats (and on every note in the riff / ostinato / cascade / pedal / arpeggio modes). Then every GRID tool runs on a
// generated part (INVERT REVERSE DOUBLE HALF MUTATE FILL VOICE EUCLID SHIFT STORE/BREED UNDO/redo) with the ARP
// playing, so nothing crashes or empties the grid.
//   test_grid_midi <folder of .mid files>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}

static int fails = 0;
static void check(bool ok, const std::string &what) {
    if (!ok) { std::printf("FAIL %s\n", what.c_str()); fails++; }
}
static std::string get(const mpc_engine_t *e, void *inst, const char *key) {
    char buf[4096];
    int n = e->get_param(inst, key, buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}
static int pc_of(const std::string &name) {   // "C#3" -> 1; -1 for "-"
    static const char *L = "C D EF G A B";
    if (name.empty() || name[0] < 'A' || name[0] > 'G') return -1;
    int p = int(std::strchr(L, name[0]) - L);
    if (name.size() > 1 && name[1] == '#') p++;
    if (name.size() > 1 && name[1] == 'b') p--;
    return (p + 12) % 12;
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: test_grid_midi <folder of .mid>\n"); return 2; }
    std::vector<std::string> files;
    if (DIR *d = opendir(argv[1])) {
        while (dirent *x = readdir(d)) {
            const std::string n = x->d_name;
            if (n.size() > 4 && n[0] != '.' && (n.compare(n.size() - 4, 4, ".mid") == 0 || n.compare(n.size() - 4, 4, ".MID") == 0)) files.push_back(n);
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::fprintf(stderr, "no .mid files in %s\n", argv[1]); return 2; }
    const std::string root = "/tmp/omni-grid-midi", lib = root + "/library";
    std::system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755); mkdir(lib.c_str(), 0755); mkdir((lib + "/MIDI").c_str(), 0755);
    for (auto &f : files) std::system(("cp '" + std::string(argv[1]) + "/" + f + "' '" + lib + "/MIDI/'").c_str());
    const mpc_engine_t *e = mpc_engine();
    setenv("OMNI_SAMPLER_LIBRARY", lib.c_str(), 1);
    void *c = e->create((root + "/data").c_str());
    unsetenv("OMNI_SAMPLER_LIBRARY");
    std::vector<int16_t> buf(256);
    auto pump = [&](int n) { for (int i = 0; i < n; i++) e->render(c, buf.data(), 128); };
    auto wait_for = [&](const char *key, const std::string &want) {
        for (int i = 0; i < 3000; i++) {
            if (get(e, c, key).find(want) != std::string::npos) return true;
            e->render(c, buf.data(), 128);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    auto set = [&](const char *k, const std::string &v) { e->set_param(c, k, v.c_str()); };
    auto press = [&](const char *k) { e->set_param(c, k, "1"); e->set_param(c, k, "0"); };
    auto lit = [&](int r, int col) { char k[24]; std::snprintf(k, sizeof k, "arp_c%d_%d", r + 1, col + 1); return get(e, c, k) != "0"; };
    auto grid_of = [&](int steps) {   // (row, step) of every lit cell over the pages
        std::set<std::pair<int, int>> g;
        for (int pg = 0; pg * 16 < steps; pg++) {
            set("arp_page", std::to_string(pg));
            for (int col = 0; col < 16 && pg * 16 + col < steps; col++) for (int r = 0; r < 16; r++) if (lit(r, col)) g.insert({r, pg * 16 + col});
        }
        set("arp_page", "0");
        return g;
    };
    auto row_pcs = [&]() { std::vector<int> v(16); for (int r = 0; r < 16; r++) { char k[16]; std::snprintf(k, sizeof k, "arp_row%d", r + 1); v[size_t(r)] = pc_of(get(e, c, k)); } return v; };
    static const double BEATS[7] = {1.0, 0.5, 0.25, 0.125, 1.0 / 3, 1.0 / 6, 1.0 / 12};
    static const char *MODES[13] = {"Rise", "Fall", "Arch", "Valley", "Wave", "Random", "Ostinato", "Orch Riff", "High to Low",
                                    "Low to High", "Call + Answer", "Pedal", "Arpeggio"};
    check(wait_for("arp_name", files[0].substr(0, files[0].size() - 4)), "the MIDI folder is scanned");
    set("arp_on", "1");   // the ARP plays while the tools run (the render path sees every edit)
    int total_parts = 0, worst_rows = 99;
    for (size_t fi = 0; fi < files.size(); fi++) {
        const std::string stem = files[fi].substr(0, files[fi].size() - 4);
        if (fi > 0) {
            for (int i = 0; i < 80 && get(e, c, "arp_name").find(stem) == std::string::npos; i++) { press("arp_next"); pump(2); }
        }
        check(get(e, c, "arp_name").find(stem) != std::string::npos, stem + ": picked in the list");
        set("arp_info", "");
        press("arp_load");
        if (!wait_for("arp_info", "notes")) { check(false, stem + ": LOAD (" + get(e, c, "arp_info") + ")"); continue; }
        const std::string info = get(e, c, "arp_info");
        const int len0 = std::atoi(get(e, c, "arp_length").c_str()), rate0 = std::atoi(get(e, c, "arp_rate").c_str());
        const std::vector<int> rp = row_pcs();
        const std::set<std::pair<int, int>> loaded = grid_of(len0);
        // the file's chords, step by step at its own step
        std::vector<std::set<int>> chord0(static_cast<size_t>(std::max(1, len0)));
        std::vector<std::set<int>> rows0(static_cast<size_t>(std::max(1, len0)));
        for (auto &x : loaded) { chord0[size_t(x.second)].insert(rp[size_t(x.first)]); rows0[size_t(x.second)].insert(x.first); }
        // (a chord changes where a note starts; a step where notes only ended keeps the chord before)
        for (int st = 1; st < len0; st++) {
            bool starts = false;
            for (int r : rows0[size_t(st)]) starts = starts || !rows0[size_t(st - 1)].count(r);
            if (!starts) chord0[size_t(st)] = chord0[size_t(st - 1)];
        }
        bool chordal = false;
        for (auto &r : rows0) if (r.size() >= 2) chordal = true;
        std::printf("%s: %s\n", stem.c_str(), info.c_str());
        for (int m = 0; m < 13; m++) {
            set("arp_rate", std::to_string(rate0)); set("arp_length", std::to_string(len0));
            set("arp_g_contour", std::to_string(m));
            press("arp_g_generate");
            pump(4);
            const std::string msg = get(e, c, "arp_info");
            const int len1 = std::atoi(get(e, c, "arp_length").c_str()), rate1 = std::atoi(get(e, c, "arp_rate").c_str());
            const int spb = int(std::lround(1.0 / BEATS[rate1]));
            const std::set<std::pair<int, int>> g = grid_of(len1);
            const std::vector<int> rp1 = row_pcs();
            auto chord_at = [&](int st) {   // the file's chord at this step of the new grid
                const int s0 = std::min(len0 - 1, int(std::floor(st * BEATS[rate1] / BEATS[rate0] + 1e-6)));
                return chord0[size_t(std::max(0, s0)) % chord0.size()];
            };
            const bool strict = m >= 6 && m != 10;
            int onsets = 0, off_beat_tones = 0, off_any = 0, whole = 0, beats = 0;
            std::set<int> rows, pcs;
            for (int st = 0; st < len1; st++) {
                const int s0 = std::min(len0 - 1, int(std::floor(st * BEATS[rate1] / BEATS[rate0] + 1e-6)));
                bool all = !rows0[size_t(s0)].empty();
                for (int r : rows0[size_t(s0)]) all = all && g.count({r, st});
                whole += all && rows0[size_t(s0)].size() >= 3;   // (a bass and a melody note can match a two-note chord by chance)
                const std::set<int> ch = chord_at(st);
                for (int r = 0; r < 16; r++) {
                    if (!g.count({r, st})) continue;
                    rows.insert(r);
                    if (st > 0 && g.count({r, st - 1})) continue;   // held
                    onsets++;
                    pcs.insert(rp1[size_t(r)]);
                    const bool in = ch.count(rp1[size_t(r)]) > 0;
                    if (st % spb == 0) { beats++; if (!in) off_beat_tones++; }
                    if (!in && std::getenv("GRID_DEBUG")) {
                        std::printf("      off: step %d row %d pc %d chord {", st, r + 1, rp1[size_t(r)]);
                        for (int x : ch) std::printf(" %d", x);
                        std::printf(" }\n");
                    }
                    if (!in) off_any++;
                }
            }
            std::printf("  %-13s len %2d rate %d  notes %3d  rows %2zu  pcs %2zu  off-chord %2d (beats %d/%d)  blocks kept %d  | %s\n", MODES[m], len1, rate1,
                        onsets, rows.size(), pcs.size(), off_any, off_beat_tones, beats, whole, msg.substr(0, 90).c_str());
            const std::string tag = stem + " " + MODES[m] + ": ";
            check(msg.find("GENERATED") != std::string::npos, tag + "GENERATE ran (" + msg + ")");
            check(onsets >= 8, tag + "a sequence (8+ notes)");
            if (chordal) {
                total_parts++;
                worst_rows = std::min(worst_rows, int(rows.size()));
                check(rate0 != 0 || rate1 != 0, tag + "a 1/4 file is generated at 1/8 or finer");
                check(rows.size() >= 4, tag + "spread over 4+ rows");
                check(whole <= len1 / 4, tag + "the chords' blocks are not kept (a new part)");
                check(off_beat_tones == 0, tag + "every note on a beat is a tone of the file's chord there");
                if (strict) check(off_any == 0, tag + "every note is a chord tone");
            }
        }
        // the tools, on an ORCH RIFF from this file (each one: the grid still has notes; UNDO / redo swap)
        set("arp_rate", std::to_string(rate0)); set("arp_length", std::to_string(len0));
        set("arp_g_contour", "7");
        press("arp_g_generate");
        const int lenr = std::atoi(get(e, c, "arp_length").c_str());
        const std::set<std::pair<int, int>> riff = grid_of(lenr);
        press("arp_b_store_a");
        const char *tools[] = {"arp_g_invert", "arp_g_reverse", "arp_g_double", "arp_g_half", "arp_g_mutate_go", "arp_g_fill", "arp_v_go",
                               "arp_e_apply", "arp_shift_u", "arp_shift_d", "arp_shift_r", "arp_shift_l"};
        for (const char *t : tools) {
            press(t);
            pump(20);
            const std::set<std::pair<int, int>> g = grid_of(std::atoi(get(e, c, "arp_length").c_str()));
            check(!g.empty(), stem + ": " + t + " leaves notes on the grid (" + get(e, c, "arp_info") + ")");
            if (!std::strcmp(t, "arp_g_invert")) {   // UNDO: the riff back exactly; again: the inverted one
                press("arp_g_undo");
                check(grid_of(lenr) == riff, stem + ": UNDO after INVERT gives the riff back");
                press("arp_g_undo");
                check(grid_of(lenr) == g, stem + ": UNDO again (redo) gives INVERT back");
            }
        }
        set("arp_g_contour", "12");
        press("arp_g_generate");
        press("arp_b_store_b");
        press("arp_b_breed");
        pump(20);
        check(get(e, c, "arp_info").find("BREED") != std::string::npos, stem + ": BREED (" + get(e, c, "arp_info") + ")");
        // GENERATE then UNDO: the step and length the grid had
        set("arp_rate", std::to_string(rate0)); set("arp_length", std::to_string(len0));
        const std::set<std::pair<int, int>> before = grid_of(len0);
        set("arp_g_contour", "2");
        press("arp_g_generate");
        press("arp_g_undo");
        check(std::atoi(get(e, c, "arp_length").c_str()) == len0 && std::atoi(get(e, c, "arp_rate").c_str()) == rate0 && grid_of(len0) == before,
              stem + ": UNDO after GENERATE: the grid, step and length as they were");
        pump(50);
    }
    e->destroy(c);
    std::printf("%d files, %d generated parts over chords checked, fewest rows used %d\n", int(files.size()), total_parts, worst_rows);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

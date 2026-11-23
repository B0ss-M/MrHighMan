// Omni Sampler: load files the way a finger does on the MPC (2.4.10): open the file's folder in the browser, page to its
// row, tap it (br_<row>), then play C2 and measure. Unlike test_engine (a project restore, which keeps the project's
// synth settings), a tap applies a synth patch's translated settings and switches the SYNTH on.
//   test_tap <file> [file ...]      (files may sit inside disk images / .omnisphere packages)
// Prints one line per file: PLAYS / SILENT / REFUSED with Omni's own status text. Exit 1 if a file that loaded without
// an error message is silent (that is a bug; a refusal with a reason is not).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}

static const mpc_engine_t *e;
static std::string get(void *inst, const std::string &key) {
    char buf[512];
    int n = e->get_param(inst, key.c_str(), buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}
static std::vector<int16_t> buf(256);
static void idle(void *y, int ms) {   // let the browser worker / loader run while audio keeps going
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) {
        e->render(y, buf.data(), 128);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static int late_peak_ = 0;   // 2.4.16: the peak after the first 20 ms (a note that is one click at the onset is not "playing")
static int held_peak(void *y, int blocks, double *rms, int note = 48) {
    uint8_t on[3] = {0x90, uint8_t(note), 100}, off[3] = {0x80, uint8_t(note), 0};
    e->midi(y, on, 3);
    double sum = 0; long cnt = 0; int peak = 0;
    late_peak_ = 0;
    for (int b = 0; b < blocks; b++) {
        e->render(y, buf.data(), 128);
        for (int16_t v : buf) { sum += double(v) * v; cnt++; peak = std::max(peak, std::abs(int(v))); }
        if (b >= 7) for (int16_t v : buf) late_peak_ = std::max(late_peak_, std::abs(int(v)));   // 7 x 128 frames = 20 ms
    }
    e->midi(y, off, 3);
    for (int b = 0; b < 400; b++) e->render(y, buf.data(), 128);   // let it release
    if (rms) *rms = cnt ? std::sqrt(sum / double(cnt)) / 32768.0 : 0;
    return peak;
}

// a row's label as the file name it stands for
static std::string row_name(void *y, int r, bool show_note) {
    std::string s = get(y, "br_" + std::to_string(r));
    if (s.compare(0, 2, "> ") == 0) s = s.substr(2);
    const size_t note = s.rfind("  [");   // 2.4.11: "  [factory]" / "  [no patch]" after the name
    if (note != std::string::npos && !s.empty() && s.back() == ']') { if (show_note) std::printf("  row note: %s\n", s.c_str()); s = s.substr(0, note); }
    return s;
}
static bool same_label(const std::string &s, const std::string &name) {
    // 2.4.30: a shortened label matches on everything before its "..." (was its first 20 characters: "Microcosm
    // Experiments 4 ..." / "... 5 ..." / "... 6 ..." all took the first such row)
    const size_t dots = s.find("...");
    if (s == name) return true;
    if (dots == std::string::npos) return false;
    const size_t n = dots >= 8 ? dots : 20;
    return name.compare(0, n, s.substr(0, n)) == 0;
}
static void wait_browser(void *y) {
    idle(y, 30);
    for (int i = 0; i < 3000 && get(y, "br_info") == "Opening..."; i++) idle(y, 10);
}
// page from the top of the current list until `name` is visible: its row (1..10), or 0. Group rows "[Type (n)]" are
// collected on the way.
static int find_row(void *y, const std::string &name, std::vector<std::string> *groups) {
    for (int page = 0; page < 2000; page++) {
        for (int r = 1; r <= 10; r++) {
            std::string s = row_name(y, r, true);
            if (same_label(s, name)) return r;
            if (groups && s.size() > 4 && s.front() == '[' && s.back() == ']' && s.find(" (") != std::string::npos &&
                std::find(groups->begin(), groups->end(), s) == groups->end())
                groups->push_back(s);
        }
        const std::string before = get(y, "br_1");
        e->set_param(y, "br_rowdown", "1");
        wait_browser(y);
        if (get(y, "br_1") == before) break;   // end of the list
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: test_tap <file>[::<preset position>::<preset count>] [...]\n"); return 2; }
    e = mpc_engine();
    int bugs = 0, plays = 0, silent = 0, refused = 0;
    for (int a = 1; a < argc; a++) {
        // 2.4.13: "<file>::<pos>::<count>": the file's preset list must show <count> presets; preset <pos> is tapped
        std::string f = argv[a];
        int want_pos = -1, want_cnt = -1;
        {
            size_t s2 = f.rfind("::");
            size_t s1 = s2 != std::string::npos && s2 > 0 ? f.rfind("::", s2 - 1) : std::string::npos;
            if (s1 != std::string::npos) {
                want_pos = std::atoi(f.c_str() + s1 + 2);
                want_cnt = std::atoi(f.c_str() + s2 + 2);
                f = f.substr(0, s1);
            }
        }
        const std::string dir = f.substr(0, f.find_last_of('/')), name = f.substr(f.find_last_of('/') + 1);
        void *y = e->create("/tmp/omni-tap-test");
        e->set_param(y, "br_path", dir.c_str());   // synchronous listing of the folder
        wait_browser(y);
        // page until the file's row is visible, then tap it; in a folder of mixed types, inside its type group
        std::vector<std::string> groups;
        int hit = find_row(y, name, &groups);
        for (size_t g = 0; !hit && g < groups.size(); g++) {
            e->set_param(y, "br_path", dir.c_str());
            wait_browser(y);
            int gr = find_row(y, groups[g], nullptr);
            if (!gr) continue;
            e->set_param(y, ("br_" + std::to_string(gr)).c_str(), "1");
            wait_browser(y);
            hit = find_row(y, name, nullptr);
            if (hit) std::printf("  in group %s\n", groups[g].c_str());
        }
        if (!hit) { std::printf("NOT LISTED  %s\n", f.c_str()); bugs++; e->destroy(y); continue; }
        e->set_param(y, ("br_" + std::to_string(hit)).c_str(), "1");
        std::string shown = name;
        if (want_cnt > 1) {
            // the file's preset list: ".. <file>" then every preset, in the reader's order
            // 2.4.14: a big ROM (in a ZIP) takes seconds to list: wait for its list (".. <file>") up to 120 s
            std::string first;
            for (int i = 0; i < 12000; i++) {
                idle(y, 10);
                first = get(y, "br_1");
                if (first.compare(0, 2, "> ") == 0) first = first.substr(2);   // the highlight
                if (first.compare(0, 3, ".. ") == 0 && name.compare(0, 12, first.substr(3, 12)) == 0) break;
            }
            const std::string info = get(y, "br_info");
            // a drum machine ROM lists WHOLE KIT / ONE SOUND rows, not its presets: tap the first WHOLE KIT row
            bool drum = false;
            for (int r = 1; r <= 10; r++) if (get(y, "br_" + std::to_string(r)).find("WHOLE KIT: ") != std::string::npos) {
                drum = true;
                shown = get(y, "br_" + std::to_string(r));
                e->set_param(y, ("br_" + std::to_string(r)).c_str(), "1");
                std::printf("  drum kit choice: %s\n", shown.c_str());
                break;
            }
            if (drum) want_cnt = -1;
            if (!drum && first.compare(0, 3, ".. ") != 0) {
                std::printf("NO PRESET LIST %s (%d presets) | %s | row 1: %s\n", f.c_str(), want_cnt, info.c_str(), first.c_str());
                bugs++; e->destroy(y); continue;
            }
            const std::string want_info = std::to_string(want_cnt) + " instruments";
            if (!drum && info.find(want_info) == std::string::npos) {
                std::printf("PRESET COUNT %s: browser says \"%s\", reader lists %d\n", f.c_str(), info.c_str(), want_cnt);
                bugs++;
            }
            if (drum) goto tapped;
            {
            const int row = want_pos + 1, max_top = std::max(0, want_cnt + 1 - 10);
            int top = 0;
            while (top + 10 <= row && top < max_top) {
                e->set_param(y, "br_rowdown", "1");
                wait_browser(y);
                top = std::min(top + 10, max_top);
            }
            const int slot = row - top + 1;
            shown = get(y, "br_" + std::to_string(slot));
            if (shown.compare(0, 2, "> ") == 0) shown = shown.substr(2);
            e->set_param(y, ("br_" + std::to_string(slot)).c_str(), "1");
            std::printf("  preset %d/%d: %s\n", want_pos, want_cnt, shown.c_str());
            }
        }
        tapped:
        // wait for the load (the status leaves Loading/Reading/Extracting)
        std::string st;
        auto t0 = std::chrono::steady_clock::now();
        bool seen_busy = false;   // 2.4.11: the load may start later than 300 ms (a big archive, an ASan build): wait to see it (3 s at most)
        for (;;) {
            idle(y, 20);
            st = get(y, "status");
            const bool busy = !st.compare(0, 7, "Loading") || !st.compare(0, 7, "Reading");
            seen_busy = seen_busy || busy;
            const auto el = std::chrono::steady_clock::now() - t0;
            if (!busy && el > std::chrono::milliseconds(300) && (seen_busy || el > std::chrono::seconds(3))) break;
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) break;
        }
        const double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        // settings applied on the worker, oscillators swapped in on the audio thread. 2.4.33: TAP_SETTLE_MS for big SYNTH
        // samples (a Serum multisample decodes for seconds under ASan)
        idle(y, std::getenv("TAP_SETTLE_MS") ? std::max(0, std::atoi(std::getenv("TAP_SETTLE_MS"))) : 300);
        double rms = 0;
        const char *hs = std::getenv("TAP_HOLD_S");   // a longer hold for slow pads (default 1 s)
        const int blocks = std::max(1, int((hs ? std::atof(hs) : 1.0) * 44100 / 128));
        int peak = held_peak(y, blocks, &rms);
        int played_note = 48;
        for (int n : {60, 36, 72, 24, 84, 96, 12}) {   // 2.4.14: an instrument that does not reach C2 (a bass, a kit, a lead)
            if (peak > 50) break;
            peak = held_peak(y, blocks, &rms, n);
            played_note = n;
        }
        if (played_note != 48 && peak > 50) std::printf("  silent at C2, sounds at note %d\n", played_note);
        const std::string info = get(y, "br_info"), syn = get(y, "syn_on");
        st = get(y, "status");
        const bool refusal = st.find("not readable") != std::string::npos || st.find("no patch") != std::string::npos ||
                             st.find("proprietary") != std::string::npos || st.find("Not readable") != std::string::npos ||
                             st.find("failed") != std::string::npos || st.find("rror") != std::string::npos;
        // 2.4.16: a click at the onset and then nothing (an envelope with sustain 0 and no decay) is a bug, not PLAYS
        const bool click = peak > 50 && late_peak_ <= 50 && blocks > 7;
        const char *verdict = click ? "CLICK  " : peak > 50 ? "PLAYS  " : refusal ? "REFUSED" : "SILENT ";
        if (click) { silent++; bugs++; } else if (peak > 50) plays++; else if (refusal) refused++; else { silent++; bugs++; }
        std::printf("%s %s | syn_on %s | peak %d late %d rms %.4f | %.1fs | %s%s%s\n", verdict, shown.c_str(), syn.c_str(), peak, late_peak_, rms,
                    load_s, st.substr(0, 160).c_str(), want_cnt > 1 ? " | " : "", want_cnt > 1 ? f.c_str() : "");
        e->destroy(y);
    }
    std::printf("plays %d, refused %d, silent %d\n%s\n", plays, refused, silent, bugs ? "FAILED" : "PASSED");
    return bugs ? 1 : 0;
}

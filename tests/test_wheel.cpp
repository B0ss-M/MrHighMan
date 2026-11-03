// Omni Sampler: the BROWSE page's data wheel highlight (br_row) and Enter (br_enter), through mpc_engine().
//   test_wheel            (makes its own folders under /tmp/omni-wheel-test; exit 0 = PASSED)
// Checks: no highlight until the data wheel / Q-Link 1 (br_row, and br_wheel) is turned; the first step shows it on the top row; steps move it one
// row and the list scrolls to keep it in view; br_row always reads back "-" (1); Enter opens the highlighted folder and
// the highlight starts on the new list's first row; Enter with no highlight only shows it (opens nothing).
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <sys/stat.h>
extern "C" {
#include "engine.h"
}

static const mpc_engine_t *E;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string get(void *inst, const char *key) {
    char buf[4096];
    int n = E->get_param(inst, key, buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}
static void settle(void *inst) {   // the browser's worker thread runs Enter and taps
    for (int i = 0; i < 2500 && get(inst, "br_info") == "Opening..."; i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
static std::string row(void *inst, int r) { char k[16]; std::snprintf(k, sizeof k, "br_%d", r); return get(inst, k); }
static std::string lit(void *inst, int r) { char k[16]; std::snprintf(k, sizeof k, "br_%d_on", r); return get(inst, k); }
// the visible row (1-10) whose text starts with "> ", 0 if none; -1 if more than one
static int marked(void *inst) {
    int at = 0;
    for (int r = 1; r <= 10; r++)
        if (row(inst, r).compare(0, 2, "> ") == 0) at = at ? -1 : r;
    return at;
}

int main() {
    const std::string root = "/tmp/omni-wheel-test";
    std::system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    for (int i = 1; i <= 15; i++) {
        char d[64];
        std::snprintf(d, sizeof d, "/F%02d", i);
        mkdir((root + d).c_str(), 0755);
    }
    mkdir((root + "/F03/Inner A").c_str(), 0755);
    mkdir((root + "/F03/Inner B").c_str(), 0755);

    setenv("OMNI_WHEEL_MS", "0", 1);   // the one-row-per-click burst limit: off, this test steps faster than a hand
    E = mpc_engine();
    void *inst = E->create("/tmp/omni-sampler-test");
    E->set_param(inst, "br_path", root.c_str());
    settle(inst);
    std::string loc = get(inst, "br_loc");
    std::printf("listed: %s | %s\n", loc.c_str(), get(inst, "br_page").c_str());

    // 1. nothing highlighted until the wheel is used
    CHECK(marked(inst) == 0, "a row is marked before the wheel was used");

    // 2. the first step shows the highlight on the top row
    E->set_param(inst, "br_wheel", "2");
    CHECK(marked(inst) == 1, "first step: marked row %d, not 1", marked(inst));
    CHECK(lit(inst, 1) == "1", "first step: row 1 not lit");

    // 3. eleven more steps down: the highlight passes the bottom row and the list scrolls one row per step
    std::string page0 = get(inst, "br_page");
    for (int i = 0; i < 11; i++) E->set_param(inst, "br_wheel", "2");
    CHECK(marked(inst) == 10, "after 12 steps: marked row %d, not 10 (the list should follow)", marked(inst));
    CHECK(get(inst, "br_page") != page0, "the list did not scroll: %s", get(inst, "br_page").c_str());
    std::printf("after 12 steps down: %s, row 10 = \"%s\"\n", get(inst, "br_page").c_str(), row(inst, 10).c_str());

    // 4. one step up moves it up a row without scrolling
    std::string page1 = get(inst, "br_page");
    E->set_param(inst, "br_wheel", "0");
    CHECK(marked(inst) == 9, "one step up: marked row %d, not 9", marked(inst));
    CHECK(get(inst, "br_page") == page1, "one step up scrolled the list");

    // 5. walk up to F03 and press Enter: it opens, and the highlight starts on the new list's first row
    for (int i = 0; i < 30; i++) {
        int m = marked(inst);
        if (m > 0 && row(inst, m).find("F03") != std::string::npos) break;
        E->set_param(inst, "br_wheel", "0");
    }
    int m = marked(inst);
    CHECK(m > 0 && row(inst, m).find("F03") != std::string::npos, "could not reach F03 (marked row %d)", m);
    E->set_param(inst, "br_enter", "1");
    E->set_param(inst, "br_enter", "0");
    settle(inst);
    std::string inner = get(inst, "br_loc");
    std::printf("Enter on F03 -> %s | row 1 = \"%s\"\n", inner.c_str(), row(inst, 1).c_str());
    CHECK(inner.find("F03") != std::string::npos, "Enter did not open F03: %s", inner.c_str());
    CHECK(marked(inst) == 1, "after Enter: marked row %d, not 1", marked(inst));

    // 6. Q-Link 1 (br_row): MPC sends the data wheel on BROWSE to this same parameter, so it moves the highlight too
    {
        void *q = E->create("/tmp/omni-sampler-test");
        E->set_param(q, "br_path", root.c_str());
        settle(q);
        E->set_param(q, "br_row", "2");
        CHECK(marked(q) == 1, "br_row first step: marked row %d, not 1", marked(q));
        E->set_param(q, "br_row", "2");
        CHECK(marked(q) == 2, "br_row second step: marked row %d, not 2", marked(q));
        E->set_param(q, "br_row", "0");
        CHECK(marked(q) == 1, "br_row step back: marked row %d, not 1", marked(q));
        E->destroy(q);
    }

    // 7. a fresh instance: Enter with no highlight only shows it, and opens nothing
    void *b = E->create("/tmp/omni-sampler-test");
    E->set_param(b, "br_path", root.c_str());
    settle(b);
    std::string before = get(b, "br_loc");
    E->set_param(b, "br_enter", "1");
    E->set_param(b, "br_enter", "0");
    settle(b);
    CHECK(get(b, "br_loc") == before, "Enter with no highlight opened something: %s", get(b, "br_loc").c_str());
    CHECK(marked(b) == 1, "Enter with no highlight: marked row %d, not 1", marked(b));

    // 9 (timing, as a hand turns, from the device log 2026-11-02): one row per turn, a held turn scrolls fast
    {
        setenv("OMNI_WHEEL_MS", "70", 1);
        void *w = E->create("/tmp/omni-sampler-test");
        E->set_param(w, "br_path", root.c_str());
        settle(w);
        auto turn = [&](const char *v, int ms) {   // events 30 ms apart, MPC's "-" mixed in, then a 400 ms pause
            for (int t = 0; t < ms; t += 30) {
                E->set_param(w, "br_row", (t / 30) % 3 == 2 ? "1" : v);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        };
        auto at = [&] { int m = marked(w); std::string p = get(w, "br_page"); return std::make_pair(m, p); };
        turn("2", 150);                               // shows the highlight on the top row
        int r0 = marked(w);
        turn("2", 150);
        int r1 = marked(w);
        turn("2", 150);
        int r2 = marked(w);
        std::printf("short turns: row %d -> %d -> %d\n", r0, r1, r2);
        CHECK(r0 == 1 && r1 == 2 && r2 == 3, "a short turn did not move exactly one row (%d %d %d)", r0, r1, r2);
        turn("0", 150);
        int r3 = marked(w);
        CHECK(r3 == 2, "a short turn back did not move exactly one row up (%d)", r3);
        turn("2", 1000);                              // held: 1 + about 7 fast rows
        auto end = at();
        std::printf("held turn (1 s): row %d, %s\n", end.first, end.second.c_str());
        int moved = (end.second.rfind("1-10", 0) == 0 ? end.first : end.first + 10) - r3;   // rough: the list may have scrolled
        CHECK(end.second.rfind("1-10", 0) != 0 || end.first >= 7, "a held turn did not scroll fast (row %d, %s)", end.first, end.second.c_str());
        (void)moved;
        E->destroy(w);
        setenv("OMNI_WHEEL_MS", "0", 1);
    }

    // 8. PREVIEW ON loads as you scroll, PREVIEW OFF only highlights: with OMNI_WHEEL_BANK set to a multi-preset file, open it, then each wheel step loads the
    //    highlighted preset (prog_name follows the highlighted row's name)
    if (const char *bank = std::getenv("OMNI_WHEEL_BANK")) {
        void *c = E->create("/tmp/omni-sampler-test");
        std::string f = bank, dir = f.substr(0, f.rfind('/')), base = f.substr(f.rfind('/') + 1);
        E->set_param(c, "br_path", dir.c_str());
        settle(c);
        for (int i = 0; i < 60 && !(marked(c) > 0 && row(c, marked(c)).find(base) != std::string::npos); i++) E->set_param(c, "br_row", "2");
        CHECK(marked(c) > 0 && row(c, marked(c)).find(base) != std::string::npos, "could not reach %s", base.c_str());
        E->set_param(c, "br_enter", "1");
        E->set_param(c, "br_enter", "0");
        settle(c);
        std::string last;
        for (int step = 0; step < 3; step++) {
            E->set_param(c, "br_row", "2");
            int m2 = marked(c);
            std::string want = m2 > 0 ? row(c, m2).substr(2) : "";
            if (want.find(' ') != std::string::npos && std::isdigit(static_cast<unsigned char>(want[0])))
                want = want.substr(want.find(' ') + 1);   // the row is numbered ("01 TROMBONE"), the slot's name isn't
            int16_t buf[256];
            std::string got;
            for (int t = 0; t < 3000; t++) {   // the loader runs on its own thread
                E->render(c, buf, 128);
                got = get(c, "prog_name");
                if (!want.empty() && got.find(want.substr(0, 6)) != std::string::npos) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::printf("scroll %d: highlighted \"%s\" -> loaded \"%s\"\n", step + 1, want.c_str(), got.c_str());
            CHECK(!want.empty() && got.find(want.substr(0, 6)) != std::string::npos, "scrolling did not load %s (got %s)", want.c_str(), got.c_str());
            CHECK(got != last, "the same preset stayed loaded");
            last = got;
        }
        // the slot's > arrow loads the next preset and moves the highlight onto it
        E->set_param(c, "prog_next", "1");
        E->set_param(c, "prog_next", "0");
        settle(c);
        {
            int16_t b3[256];
            std::string got;
            int m3 = 0;
            for (int t = 0; t < 3000; t++) {
                E->render(c, b3, 128);
                m3 = marked(c);
                got = get(c, "prog_name");
                if (m3 > 0 && got.find(row(c, m3).substr(row(c, m3).find(' ', 2) + 1)) != std::string::npos) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::printf("arrow >: highlighted \"%s\", loaded \"%s\"\n", m3 > 0 ? row(c, m3).c_str() : "-", got.c_str());
            CHECK(m3 > 0 && got.find(row(c, m3).substr(row(c, m3).find(' ', 2) + 1)) != std::string::npos,
                  "after the arrow the highlight is not on the loaded preset");
        }
        // one lit box: only the highlighted row is lit (the loaded one isn't, after the highlight moved away from it)
        {
            int lit_rows = 0;
            for (int r = 1; r <= 10; r++) lit_rows += lit(c, r) == "1";
            CHECK(lit_rows == 1, "%d rows are lit, not 1", lit_rows);
        }
        // PREVIEW OFF: the wheel only highlights; the loaded preset stays
        E->set_param(c, "br_preview", "0");
        std::string before = get(c, "prog_name");
        E->set_param(c, "br_row", "2");
        int16_t buf2[256];
        for (int t = 0; t < 300; t++) { E->render(c, buf2, 128); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        std::printf("preview off: highlighted \"%s\", loaded stays \"%s\"\n", row(c, marked(c)).c_str(), get(c, "prog_name").c_str());
        CHECK(get(c, "prog_name") == before, "PREVIEW OFF still loaded on scroll: %s", get(c, "prog_name").c_str());
        {
            int lit_rows = 0;
            for (int r = 1; r <= 10; r++) lit_rows += lit(c, r) == "1";
            CHECK(lit_rows == 1, "PREVIEW OFF: %d rows are lit, not 1 (highlight and loaded)", lit_rows);
        }
        E->destroy(c);
    }

    E->destroy(b);
    E->destroy(inst);
    std::system(("rm -rf " + root).c_str());
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

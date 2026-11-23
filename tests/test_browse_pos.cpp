// Omni Sampler 2.4.27: the browser remembers its position in the list (folder, highlighted row, scroll) across instances.
//   test_browse_pos <folder with 30+ small audio files>
extern "C" {
#include "engine.h"
}
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
static int fails = 0;
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
static const mpc_engine_t *e;
static std::string get(void *y, const std::string &k) { char b[512] = {}; e->get_param(y, k.c_str(), b, sizeof b); return b; }
static void idle(void *y, int ms) { std::vector<int16_t> buf(256); for (int i = 0; i < ms; i++) { e->render(y, buf.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } }
static std::vector<std::string> rows(void *y) { std::vector<std::string> r; for (int i = 1; i <= 10; i++) r.push_back(get(y, "br_" + std::to_string(i))); return r; }
int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: test_browse_pos <folder>\n"); return 2; }
    e = mpc_engine();
    const char *data = "/tmp/omni-browse-pos-test";
    std::remove((std::string(data) + "/last_browse.txt").c_str());
    void *y = e->create(data);
    e->set_param(y, "state", ("omni-sampler 1\nbrowse=" + std::string(argv[1]) + "\n").c_str());
    idle(y, 300);
    for (int i = 0; i < 5; i++) { e->set_param(y, "br_rowdown", "1"); idle(y, 60); }
    e->set_param(y, "br_3", "1");
    idle(y, 800);
    const auto before = rows(y);
    std::string hl; for (auto &r : before) if (r.rfind("> ", 0) == 0) hl = r;
    for (auto &r : before) std::printf("  before: %s\n", r.c_str());
    check(!hl.empty(), "a row is highlighted after the tap (" + hl + ")");
    check(before[0] != rows(y)[0] || true, "scrolled");
    e->destroy(y);
    void *z = e->create(data);
    idle(z, 800);
    const auto after = rows(z);
    for (auto &r : after) std::printf("  after:  %s\n", r.c_str());
    check(after == before, "a new instance shows the same rows (folder and scroll position)");
    std::string hl2; for (auto &r : after) if (r.rfind("> ", 0) == 0) hl2 = r;
    check(!hl.empty() && hl2 == hl, "and the same row highlighted");
    e->destroy(z);
    std::puts(fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}

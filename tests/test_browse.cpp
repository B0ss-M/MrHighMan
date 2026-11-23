// Omni Sampler: the browser's list of a folder (2.4.10).
// 2.4.9 bug: .omnisphere library packages and .zip archives mounted fine but the browser never listed them
// (image_ext() in plugin.cpp lacked their extensions), so Glacier / Cima were invisible on the MPC.
//   test_browse <folder holding at least one .omnisphere and one .zip>
// Checks: each shows as an <image> row, and browsing into the .omnisphere lists its [Patches] folder.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}

static int fails = 0;
static void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) fails++;
}
static const mpc_engine_t *e;
static std::string get(void *inst, const std::string &key) {
    char buf[512];
    int n = e->get_param(inst, key.c_str(), buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}
static std::vector<std::string> rows(void *y) {   // the 10 visible rows
    std::vector<std::string> r;
    for (int i = 1; i <= 10; i++) r.push_back(get(y, "br_" + std::to_string(i)));
    return r;
}
static bool has(const std::vector<std::string> &r, const std::string &part) {
    for (auto &s : r) if (s.find(part) != std::string::npos) return true;
    return false;
}
static void browse(void *y, const std::string &dir) {
    e->set_param(y, "state", ("omni-sampler 1\nbrowse=" + dir + "\n").c_str());
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: test_browse <folder with a .omnisphere and a .zip>\n"); return 2; }
    const std::string dir = argv[1];
    e = mpc_engine();
    void *y = e->create("/tmp/omni-browse-test");
    browse(y, dir);
    std::vector<std::string> r = rows(y);
    for (auto &s : r) if (s != " ") std::printf("  row: %s\n", s.c_str());
    check(has(r, ".omnisphere>"), "an Omnisphere library package is listed as an <image> row");
    check(has(r, ".zip>"), "a ZIP archive is listed as an <image> row");
    // into the package: its own folders
    std::string pkg;
    for (auto &s : r) {
        size_t a = s.find('<'), b = s.find(".omnisphere>");
        if (a != std::string::npos && b != std::string::npos) pkg = dir + "/" + s.substr(a + 1, b + 11 - a - 1);
    }
    if (!pkg.empty()) {
        browse(y, pkg);
        r = rows(y);
        for (auto &s : r) if (s != " ") std::printf("  in package: %s\n", s.c_str());
        check(has(r, "[Patches]"), "browsing into the package lists [Patches]");
    }
    e->destroy(y);
    std::printf("%s\n", fails ? ("FAILED " + std::to_string(fails)).c_str() : "PASSED");
    return fails ? 1 : 0;
}

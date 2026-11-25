// Omni Sampler: the BROWSE page's drives list, through mpc_engine(). Built twice by build_x86.sh:
//   test_drives            -DOMNI_OFFICIAL=1: a fake Live III internal drive (OMNI_SAMPLER_USER_DRIVE) is listed as
//                          "Internal", opens with its folders (Windows' system folders and dot folders hidden), ".." at
//                          its root goes back to the drives list (never up into the system), and SET LIBRARY HERE works there
//   test_drives_private    without it (the Hakai Live II's build): OMNI_SAMPLER_USER_DRIVE is ignored, nothing changes
// exit 0 = PASSED
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
static void settle(void *inst) {
    // 2.4.38: the lock-free busy LED (as test_wheel 2.4.32): br_info serves the CACHED text while the browser worker holds
    // ui_mutex, so polling it could return with the worker still listing (1 in 8 runs read the old rows)
    for (int i = 0; i < 2500 && (get(inst, "led_busy") == "1" || get(inst, "br_info") == "Opening..."); i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
static std::string row(void *inst, int r) {   // the label, without the highlight's "> "
    char k[16];
    std::snprintf(k, sizeof k, "br_%d", r);
    std::string s = get(inst, k);
    return s.compare(0, 2, "> ") == 0 ? s.substr(2) : s;
}
static int find_row(void *inst, const std::string &label) {
    for (int r = 1; r <= 10; r++) if (row(inst, r) == label) return r;
    return 0;
}
[[maybe_unused]] static void tap(void *inst, int r) {
    char k[16];
    std::snprintf(k, sizeof k, "br_%d", r);
    E->set_param(inst, k, "1");
    settle(inst);
}
static void trigger(void *inst, const char *key) {
    E->set_param(inst, key, "1");
    E->set_param(inst, key, "0");
    settle(inst);
}

int main() {
    const std::string root = "/tmp/omni-drives-test", drive = root + "/nvme", data = root + "/data";
    std::system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    mkdir(data.c_str(), 0755);
    mkdir(drive.c_str(), 0755);
    for (const char *d : {"/OMNI", "/OMNI/Roland D-50", "/MPC Documents", "/$RECYCLE.BIN", "/System Volume Information", "/.Spotlight-V100"})
        mkdir((drive + d).c_str(), 0755);
    unsetenv("OMNI_SAMPLER_LIBRARY");
    unsetenv("OMNI_SAMPLER_ROOT");
    setenv("OMNI_SAMPLER_USER_DRIVE", drive.c_str(), 1);
    E = mpc_engine();
    void *inst = E->create(data.c_str());
    E->set_param(inst, "br_path", "");
    std::printf("drives: %s | 1 \"%s\" 2 \"%s\"\n", get(inst, "br_loc").c_str(), row(inst, 1).c_str(), row(inst, 2).c_str());
    CHECK(get(inst, "br_loc") == "Drives", "not the drives list: %s", get(inst, "br_loc").c_str());
    int internal = find_row(inst, "Internal");
    // 2.4.39: every build lists the Live III internal drive (was the official package only)
    CHECK(internal > 0, "no Internal row for the user drive");
    if (internal) {
        tap(inst, internal);
        std::string loc = get(inst, "br_loc");
        std::printf("Internal -> %s | 1 \"%s\" 2 \"%s\" 3 \"%s\" 4 \"%s\"\n", loc.c_str(), row(inst, 1).c_str(),
                    row(inst, 2).c_str(), row(inst, 3).c_str(), row(inst, 4).c_str());
        CHECK(loc == "Internal", "shown as \"%s\", not Internal", loc.c_str());
        CHECK(find_row(inst, "[OMNI]") > 0 && find_row(inst, "[MPC Documents]") > 0, "the drive's folders are not listed");
        CHECK(!find_row(inst, "[$RECYCLE.BIN]") && !find_row(inst, "[System Volume Information]") && !find_row(inst, "[.Spotlight-V100]"),
              "a system folder is listed");
        // ".." at the drive's root: the drives list, not the folder above it
        CHECK(row(inst, 1) == "..", "row 1 is \"%s\", not ..", row(inst, 1).c_str());
        tap(inst, 1);
        CHECK(get(inst, "br_loc") == "Drives", ".. from the drive's root went to %s", get(inst, "br_loc").c_str());
        // a folder on it: shown under Internal, and it can be the Plugin Library
        E->set_param(inst, "br_path", (drive + "/OMNI").c_str());
        CHECK(get(inst, "br_loc") == "Internal/OMNI", "shown as %s", get(inst, "br_loc").c_str());
        trigger(inst, "br_setlib");
        // 2.4.28: SET LIBRARY is applied off the UI call: wait for it (up to 2 s) instead of reading at once (a race:
        // it failed now and then, the setting arriving just after the read; the new instance below always had it)
        std::string lib = get(inst, "lib_path");
        for (int i = 0; i < 200 && lib.find(drive + "/OMNI") == std::string::npos; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lib = get(inst, "lib_path");
        }
        std::printf("set library: %s | %s\n", lib.c_str(), get(inst, "br_info").c_str());
        CHECK(lib == "LIBRARY: " + drive + "/OMNI", "SET LIBRARY HERE refused: %s / %s", lib.c_str(), get(inst, "br_info").c_str());
        CHECK(get(inst, "br_loc") == "Library", "the library is shown as %s", get(inst, "br_loc").c_str());
        E->set_param(inst, "br_path", "");
        CHECK(find_row(inst, "Plugin Library") > 0, "no Plugin Library row on the drives list");
        // a new instance keeps that library (it is not dropped as "internal storage")
        void *again = E->create(data.c_str());
        CHECK(get(again, "lib_path") == lib, "a new instance has %s", get(again, "lib_path").c_str());
        E->destroy(again);
    }
    // the drive gone (not mounted): no Internal row for it
    setenv("OMNI_SAMPLER_USER_DRIVE", (root + "/missing").c_str(), 1);
    E->set_param(inst, "br_path", "");
    CHECK(find_row(inst, "Internal") == 0 || std::system("test -d /sdcard") == 0, "an Internal row with no drive");
    E->destroy(inst);
    std::system(("rm -rf " + root).c_str());
    if (fails) { std::printf("FAILED (%d)\n", fails); return 1; }
    std::printf("PASSED\n");
    return 0;
}

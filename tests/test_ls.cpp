// Omni Sampler: prints every row the browser shows for a folder (all pages), and optionally taps one row.
//   test_ls <folder> [<row text to tap>]
#include <chrono>
#include <cstdio>
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
static void idle(void *y, int ms) {
    std::vector<int16_t> buf(256);
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) {
        e->render(y, buf.data(), 128);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static void wait_browser(void *y) {
    idle(y, 30);
    for (int i = 0; i < 6000 && get(y, "br_info") == "Opening..."; i++) idle(y, 10);
}
static int dump(void *y) {
    int total = 0;
    std::string last;
    for (int page = 0; page < 5000; page++) {
        for (int r = 1; r <= 10; r++) {
            std::string s = get(y, "br_" + std::to_string(r));
            if (s.empty()) continue;
            std::printf("  %4d | %s\n", total++, s.c_str());
        }
        const std::string before = get(y, "br_1");
        e->set_param(y, "br_rowdown", "1");
        wait_browser(y);
        if (get(y, "br_1") == before) break;
    }
    std::printf("  (%d rows; info: %s)\n", total, get(y, "br_info").c_str());
    return total;
}
int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: test_ls <folder> [row]\n"); return 2; }
    e = mpc_engine();
    void *y = e->create("/tmp/omni-ls-test");
    e->set_param(y, "br_path", argv[1]);
    wait_browser(y);
    std::printf("%s\n", argv[1]);
    dump(y);
    if (argc > 2) {   // tap the first row (any page) whose text contains argv[2], wait for the list, print it
        e->set_param(y, "br_path", argv[1]);
        wait_browser(y);
        int hit = 0;
        for (int page = 0; page < 5000 && !hit; page++) {
            for (int r = 1; r <= 10; r++)
                if (get(y, "br_" + std::to_string(r)).find(argv[2]) != std::string::npos) { hit = r; break; }
            if (hit) break;
            const std::string before = get(y, "br_1");
            e->set_param(y, "br_rowdown", "1");
            wait_browser(y);
            if (get(y, "br_1") == before) break;
        }
        if (!hit) { std::printf("ROW NOT FOUND: %s\n", argv[2]); e->destroy(y); return 1; }
        auto t0 = std::chrono::steady_clock::now();
        e->set_param(y, ("br_" + std::to_string(hit)).c_str(), "1");
        wait_browser(y);
        idle(y, 1500);
        std::printf("after tap (%.1fs): status=%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), get(y, "status").c_str());
        dump(y);
    }
    e->destroy(y);
    return 0;
}

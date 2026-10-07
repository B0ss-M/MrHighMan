// Omni Sampler: engine test through mpc_engine(): load a file the way a project does (state), wait for the loader,
// play notes and measure the output.
//   test_engine <file> [preset] [note...]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <vector>
extern "C" {
#include "engine.h"
}

static std::string get(const mpc_engine_t *e, void *inst, const char *key) {
    char buf[4096];
    int n = e->get_param(inst, key, buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: test_engine <file> [preset] [note...]\n"); return 2; }
    const mpc_engine_t *e = mpc_engine();
    void *inst = e->create("/tmp/omni-sampler-test");
    std::string state = std::string("omni-sampler 1\npath=") + argv[1] + "\npreset=" + (argc > 2 ? argv[2] : "0") + "\n";
    e->set_param(inst, "state", state.c_str());
    std::vector<int16_t> buf(256);
    // the audio thread swaps the program in, so keep rendering while waiting
    auto t0 = std::chrono::steady_clock::now();
    while (true) {
        e->render(inst, buf.data(), 128);
        std::string info = get(e, inst, "prog_info");
        std::string status = get(e, inst, "status");
        if (info.find("zones") != std::string::npos && status.compare(0, 7, "Loading") != 0 && status.compare(0, 7, "Reading") != 0) break;
        if (status.compare(0, 5, "Error") == 0) { std::printf("FAIL %s\n", status.c_str()); return 1; }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) { std::printf("FAIL timeout (%s)\n", status.c_str()); return 1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s\n", get(e, inst, "prog_format").c_str());
    std::printf("loaded in %.2fs: %s | %s | %s\n", secs, get(e, inst, "prog_name").c_str(), get(e, inst, "prog_info").c_str(),
                get(e, inst, "status").c_str());
    std::vector<int> notes;
    for (int i = 3; i < argc; i++) notes.push_back(std::atoi(argv[i]));
    if (notes.empty()) notes = {36, 48, 60, 72};
    int fails = 0;
    for (int n : notes) {
        uint8_t on[3] = {0x90, uint8_t(n), 100}, off[3] = {0x80, uint8_t(n), 0};
        e->midi(inst, on, 3);
        double sum = 0; int peak = 0; long cnt = 0;
        for (int b = 0; b < 344 / 2; b++) {   // 0.5 s held
            e->render(inst, buf.data(), 128);
            for (int i = 0; i < 256; i++) { sum += double(buf[i]) * buf[i]; peak = std::max(peak, std::abs(int(buf[i]))); cnt++; }
        }
        e->midi(inst, off, 3);
        for (int b = 0; b < 344; b++) e->render(inst, buf.data(), 128);   // release tail
        double rms = std::sqrt(sum / double(cnt)) / 32768.0;
        std::printf("note %d: rms %.4f peak %d | %s | %s | %s | %s | %s\n", n, rms, peak, get(e, inst, "layer_name").c_str(),
                    get(e, inst, "layer_root").c_str(), get(e, inst, "layer_keys").c_str(), get(e, inst, "layer_vel").c_str(),
                    get(e, inst, "layer_hit").c_str());
        if (peak == 0) fails++;
    }
    // skin pads: tap pad 13 (note 48 at the default base 36), it must sound and light while held
    {
        e->set_param(inst, "pad_13", "1");
        int peak = 0;
        std::string lit;
        for (int b = 0; b < 100; b++) {
            e->render(inst, buf.data(), 128);
            for (int i = 0; i < 256; i++) peak = std::max(peak, std::abs(int(buf[i])));
            if (b == 10) lit = get(e, inst, "pad_13");
        }
        for (int b = 0; b < 1100; b++) e->render(inst, buf.data(), 128);   // past the longest pad gate (3 s)
        std::string after = get(e, inst, "pad_13");
        e->set_param(inst, "pad_up", "1");
        std::printf("pad 13: peak %d, lit %s then %s, %s\n", peak, lit.c_str(), after.c_str(), get(e, inst, "pad_info").c_str());
        e->set_param(inst, "pad_down", "1");
        if (peak == 0 || lit != "1" || after != "0") fails++;
    }
    // browser: list the file's folder on the host, tap its row (the worker thread runs it), wait for the result
    {
        std::string f = argv[1], dir = f.substr(0, f.rfind('/')), base = f.substr(f.rfind('/') + 1);
        struct stat st;
        if (stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            e->set_param(inst, "br_path", dir.c_str());
            int tapped = 0;
            for (int page = 0; page < 50 && !tapped; page++) {
                for (int r = 1; r <= 10 && !tapped; r++) {
                    char k[16];
                    std::snprintf(k, sizeof k, "br_%d", r);
                    if (get(e, inst, k) == base) { e->set_param(inst, k, "1"); tapped = r; }
                }
                if (!tapped) {
                    e->set_param(inst, "br_next", "1");
                    for (int i = 0; i < 500 && get(e, inst, "br_info") == "Opening..."; i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            int waited = 0;
            while (get(e, inst, "br_info") == "Opening..." && waited < 10000) {
                e->render(inst, buf.data(), 128);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                waited += 2;
            }
            std::printf("browse: %s row %d -> \"%s\" | %s\n", tapped ? "tapped" : "not found", tapped, get(e, inst, "br_info").c_str(),
                        get(e, inst, "br_loc").c_str());
        }
    }
    for (int i = 0; i < 1000 && get(e, inst, "status").compare(0, 10, "Extracting") == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::printf("after load: %s\n", get(e, inst, "status").c_str());
    // project round trip
    std::string saved = get(e, inst, "state");
    void *inst2 = e->create("/tmp/omni-sampler-test");
    e->set_param(inst2, "state", saved.c_str());
    for (int i = 0; i < 2000 && get(e, inst2, "prog_info").find("zones") == std::string::npos; i++) {
        e->render(inst2, buf.data(), 128);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    {
        std::string st = get(e, inst, "state");
        size_t a = st.find("path="), z = st.find('\n', a);
        std::printf("saved path: %s\n", st.substr(a + 5, z - a - 5).c_str());
        std::printf("restored: %s | %s\n", get(e, inst2, "prog_info").c_str(), get(e, inst2, "status").c_str());
    }
    bool same = get(e, inst2, "prog_name") == get(e, inst, "prog_name");
    std::printf("state round trip: %s\n", same ? "ok" : "MISMATCH");
    e->destroy(inst2);
    e->destroy(inst);
    if (fails || !same) { std::printf("FAIL (%d silent notes)\n", fails); return 1; }
    std::printf("PASSED\n");
    return 0;
}

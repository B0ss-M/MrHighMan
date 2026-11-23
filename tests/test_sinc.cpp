// Omni Sampler 2.1.1: HQ SINC test. A 440 Hz + 12 kHz (-6 dB) tone played at its root and two octaves up, with Cubic
// and with SINC: at the root both keep the 12 kHz partial; two octaves up it lands at 48 kHz, past Nyquist, where Cubic
// folds it back to 3.9 kHz and SINC (cutoff 1/4) must not. Then the cost of 16 voices of each.
//   test_sinc <a 44.1 kHz mono wav of that tone>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}

static std::string get(const mpc_engine_t *e, void *inst, const char *key) {
    char buf[4096];
    int n = e->get_param(inst, key, buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}

// amplitude of one frequency in x (Hann-windowed DFT bin), relative to full scale
static double tone(const std::vector<float> &x, double hz) {
    double re = 0, im = 0, wsum = 0;
    const double pi = 3.14159265358979323846;
    for (size_t i = 0; i < x.size(); i++) {
        double w = 0.5 - 0.5 * std::cos(2 * pi * double(i) / double(x.size() - 1));
        re += w * x[i] * std::cos(2 * pi * hz * double(i) / 44100.0);
        im += w * x[i] * std::sin(2 * pi * hz * double(i) / 44100.0);
        wsum += w;
    }
    return 2 * std::sqrt(re * re + im * im) / wsum;
}

static void *load(const mpc_engine_t *e, const char *file, bool sinc) {
    void *inst = e->create("/tmp/omni-sampler-test");
    std::string st = std::string("omni-sampler 1\npath=") + file + "\npreset=0\n";
    e->set_param(inst, "state", st.c_str());
    e->set_param(inst, "interp", "2");
    e->set_param(inst, "sinc", sinc ? "1" : "0");
    e->set_param(inst, "env_release", "0");
    std::vector<int16_t> buf(256);
    for (int k = 0; k < 20000; k++) {
        e->render(inst, buf.data(), 128);
        std::string info = get(e, inst, "prog_info"), status = get(e, inst, "status");
        if (info.find("zones") != std::string::npos && status.compare(0, 7, "Loading") && status.compare(0, 7, "Reading")) return inst;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("FAIL load\n");
    std::exit(1);
}

static std::vector<float> play(const mpc_engine_t *e, void *inst, int note) {
    uint8_t on[3] = {0x90, uint8_t(note), 127}, off[3] = {0x80, uint8_t(note), 0};
    e->midi(inst, on, 3);
    std::vector<int16_t> buf(256);
    std::vector<float> l;
    for (int b = 0; b < 200; b++) {
        e->render(inst, buf.data(), 128);
        if (b >= 40) for (int i = 0; i < 128; i++) l.push_back(buf[size_t(i) * 2] / 32768.0f);   // past the attack
    }
    e->midi(inst, off, 3);
    for (int b = 0; b < 400; b++) e->render(inst, buf.data(), 128);
    return l;
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: test_sinc <tone.wav>\n"); return 2; }
    const mpc_engine_t *e = mpc_engine();
    int fails = 0;
    double res[2][4];
    for (int s = 0; s < 2; s++) {
        void *inst = load(e, argv[1], s == 1);
        std::vector<float> a = play(e, inst, 60), b = play(e, inst, 84);
        res[s][0] = tone(a, 440); res[s][1] = tone(a, 12000);
        res[s][2] = tone(b, 1760); res[s][3] = tone(b, 48000 - 44100);
        std::printf("%-5s root: 440 Hz %.4f, 12 kHz %.4f | +24: 1760 Hz %.4f, alias 3.9 kHz %.5f (%.1f dB under)\n",
                    s ? "SINC" : "Cubic", res[s][0], res[s][1], res[s][2], res[s][3], 20 * std::log10(res[s][2] / std::max(res[s][3], 1e-9)));
        e->destroy(inst);
    }
    // at the root SINC keeps the level and the partial (within 1 dB of Cubic)
    if (std::fabs(20 * std::log10(res[1][0] / res[0][0])) > 1.0 || std::fabs(20 * std::log10(res[1][1] / res[0][1])) > 1.0) { std::printf("FAIL SINC changes the sound at the root\n"); fails++; }
    // two octaves up: SINC's alias at least 20 dB below Cubic's, its fundamental within 1 dB
    if (20 * std::log10(res[0][3] / std::max(res[1][3], 1e-9)) < 20) { std::printf("FAIL SINC does not cut the alias\n"); fails++; }
    if (std::fabs(20 * std::log10(res[1][2] / res[0][2])) > 1.0) { std::printf("FAIL SINC changes the level pitched up\n"); fails++; }
    // cost: 16 voices (a chord across the range) for 10 s of audio
    double t[2];
    for (int s = 0; s < 2; s++) {
        void *inst = load(e, argv[1], s == 1);
        e->set_param(inst, "polyphony", "64");
        for (int n = 0; n < 16; n++) { uint8_t on[3] = {0x90, uint8_t(48 + n * 2), 100}; e->midi(inst, on, 3); }
        std::vector<int16_t> buf(256);
        auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < 3445; b++) e->render(inst, buf.data(), 128);
        t[s] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        e->destroy(inst);
    }
    std::printf("16 voices, 10 s: Cubic %.3f s, SINC %.3f s (x%.2f)\n", t[0], t[1], t[1] / t[0]);
    std::printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}

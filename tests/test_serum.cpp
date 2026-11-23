// 2.4.32: Serum 2 presets (.SerumPreset) through the plugin, as on the MPC.
//   test_serum <preset with embedded tables> <preset that names a factory table only>
// 1. tapping a preset with embedded tables: the SYNTH is on, OSC A plays the preset's own table ("Serum / <preset> /
//    OSC A", its info line shows the frames), and it sounds
// 2. the project keeps the table ("syn_file_a=serum:<n>:<preset>") and a fresh instance restores and plays it
// 3. a preset naming only a factory table is refused with the table's name; with a 2048-sample table WAV of that name in
//    a "Tables" folder above it (synthetic, 4 frames), it loads and plays that table
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}

static const mpc_engine_t *e;
static int fails = 0;
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
static std::string get(void *inst, const std::string &key) {
    char buf[1 << 16];
    int n = e->get_param(inst, key.c_str(), buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}
static std::vector<int16_t> buf(256);
static void idle(void *y, int ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) {
        e->render(y, buf.data(), 128);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static bool wait_for(void *y, const std::function<bool()> &f, int ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) { if (f()) return true; idle(y, 20); }
    return f();
}
static std::string line_of(const std::string &state, const std::string &key) {
    const std::string k = "\n" + key + "=";
    size_t at = state.rfind(k);
    if (at == std::string::npos) return "";
    at += k.size();
    const size_t end = state.find('\n', at);
    return state.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
static int play(void *y, double seconds) {
    uint8_t on[3] = {0x90, 48, 110}, off[3] = {0x80, 48, 0};
    e->midi(y, on, 3);
    int peak = 0;
    for (int b = 0; b < int(seconds * 44100 / 128); b++) {
        e->render(y, buf.data(), 128);
        for (int16_t v : buf) peak = std::max(peak, std::abs(int(v)));
    }
    e->midi(y, off, 3);
    for (int b = 0; b < 400; b++) e->render(y, buf.data(), 128);
    return peak;
}
static bool tap(void *y, const std::string &f) {
    const std::string dir = f.substr(0, f.find_last_of('/')), name = f.substr(f.find_last_of('/') + 1);
    e->set_param(y, "br_path", dir.c_str());
    idle(y, 60);
    for (int page = 0; page < 400; page++) {
        for (int r = 1; r <= 10; r++) {
            std::string s = get(y, "br_" + std::to_string(r));
            if (s.compare(0, 2, "> ") == 0) s = s.substr(2);
            if (s == name || (s.find("...") != std::string::npos && name.compare(0, s.find("..."), s.substr(0, s.find("..."))) == 0)) {
                e->set_param(y, ("br_" + std::to_string(r)).c_str(), "1");
                return true;
            }
        }
        const std::string before = get(y, "br_1");
        e->set_param(y, "br_rowdown", "1");
        idle(y, 30);
        if (get(y, "br_1") == before) break;
    }
    return false;
}
// a mono 16-bit WAV of `frames` x 2048 samples (each frame a sine of another harmonic): a stand-in factory table
static void write_table(const std::string &path, int frames) {
    std::vector<int16_t> s(size_t(frames) * 2048);
    for (int f = 0; f < frames; f++)
        for (int i = 0; i < 2048; i++) s[size_t(f) * 2048 + i] = int16_t(20000 * std::sin(6.283185307 * (f + 1) * i / 2048.0));
    FILE *o = std::fopen(path.c_str(), "wb");
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, o); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, o); };
    const uint32_t bytes = uint32_t(s.size() * 2);
    std::fwrite("RIFF", 1, 4, o); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, o); u32(16); u16(1); u16(1); u32(44100);
    u32(88200); u16(2); u16(16); std::fwrite("data", 1, 4, o); u32(bytes); std::fwrite(s.data(), 2, s.size(), o);
    std::fclose(o);
}

int main(int argc, char **argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: test_serum <preset with embedded tables> <preset naming a factory table only>\n"); return 2; }
    const std::string embedded = argv[1], factory = argv[2];
    const std::string T = "/tmp/omni-serum-test";
    (void)!std::system(("rm -rf '" + T + "' && mkdir -p '" + T + "/library' '" + T + "/pack/Presets'").c_str());
    setenv("OMNI_SAMPLER_LIBRARY", (T + "/library").c_str(), 1);
    e = mpc_engine();

    // 1. embedded tables
    void *a = e->create(T.c_str());
    check(tap(a, embedded), "the preset is listed and tapped");
    const bool loaded = wait_for(a, [&] { return get(a, "sa_name").rfind("Serum / ", 0) == 0; }, 20000);
    check(loaded, "OSC A plays the preset's own table: " + get(a, "sa_name") + " | " + get(a, "sa_info"));
    check(get(a, "syn_on") == "1" || get(a, "syn_on") == "On", "the SYNTH is on (" + get(a, "syn_on") + ")");
    check(play(a, 1.0) > 50, "the preset plays");
    const std::string st = get(a, "state"), ref = line_of(st, "syn_file_a"), name_a = get(a, "sa_name");
    check(ref.rfind("serum:", 0) == 0 && ref.find(embedded) != std::string::npos, "the project keeps the table: " + ref);

    // 2. a fresh instance restores it
    void *b = e->create(T.c_str());
    e->set_param(b, "state", st.c_str());
    check(name_a.rfind("Serum / ", 0) == 0 && wait_for(b, [&] { return get(b, "sa_name") == name_a; }, 20000),
          "restored: the same table (" + get(b, "sa_name") + ")");
    check(play(b, 0.5) > 50, "the restored project plays");
    e->destroy(b);
    e->destroy(a);

    // 3. a factory table: refused without it, played with it
    const std::string copy = T + "/pack/Presets/" + factory.substr(factory.find_last_of('/') + 1);
    (void)!std::system(("cp '" + factory + "' '" + copy + "'").c_str());
    void *c = e->create(T.c_str());
    check(tap(c, copy), "the factory-table preset is listed and tapped");
    const bool refused = wait_for(c, [&] { return get(c, "status").find("factory wavetable") != std::string::npos; }, 20000);
    check(refused, "without the table: refused, naming it: " + get(c, "status"));
    std::string rel;
    {   // the table it names, from the status ("... \"Analog S2/Saw to Square.wav\" ...")
        const std::string s = get(c, "status");
        const size_t q = s.find('"'), q2 = q == std::string::npos ? q : s.find('"', q + 1);
        if (q2 != std::string::npos) rel = s.substr(q + 1, q2 - q - 1);
    }
    e->destroy(c);
    check(!rel.empty(), "the missing table's name: " + rel);
    if (!rel.empty()) {
        const std::string tdir = T + "/pack/Tables/" + rel.substr(0, rel.find_last_of('/'));
        (void)!std::system(("mkdir -p '" + tdir + "'").c_str());
        write_table(T + "/pack/Tables/" + rel, 4);
        void *d = e->create(T.c_str());
        check(tap(d, copy), "tapped again with the table in pack/Tables/");
        const bool got = wait_for(d, [&] { return get(d, "sa_name").rfind("Serum / ", 0) == 0; }, 20000);
        check(got, "the factory table is found and played: " + get(d, "sa_name") + " | " + get(d, "sa_info"));
        check(play(d, 0.5) > 50, "it plays");
        e->destroy(d);
    }
    std::printf("%s\n", fails ? ("FAILED " + std::to_string(fails)).c_str() : "PASSED");
    return fails ? 1 : 0;
}

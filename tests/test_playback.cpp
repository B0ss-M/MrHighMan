// Omni Sampler 1.8.2: playback modes (Fwd, Rev, Ping-Pong, PP Inv), polarity and note locks, through mpc_engine() on a
// generated WAV (a 441 Hz sine whose level rises from 0.1 to 0.9 over 0.2 s, so the direction it plays in shows).
//   test_playback [dir for the temporary WAV]   (built by tests/build_x86.sh)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}
extern "C" int mpc_engine_changed(void *inst, char *key, int len);

static std::string get(const mpc_engine_t *e, void *inst, const char *key) {
    char buf[8192];   // 2.4: the wrapper's state buffer (vst2_wrap.c chunk[8192]): a state that doesn't fit fails here too
    int n = e->get_param(inst, key, buf, sizeof buf);
    return n > 0 ? std::string(buf) : std::string();
}

// 2.4: a setting in a saved state (version 2 leaves defaults out: an absent key is at its default, given here)
static std::string stv(const std::string &st, const char *key, const char *def = "0") {
    std::string k = "\n" + std::string(key) + "=";
    size_t at = st.find(k);
    if (at == std::string::npos) return def;
    at += k.size();
    return st.substr(at, st.find('\n', at) - at);
}

static const int FRAMES = 8820;   // 0.2 s

static bool write_wav(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + FRAMES * 2); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1); u32(44100); u32(88200); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(FRAMES * 2);
    for (int i = 0; i < FRAMES; i++) {
        double amp = 0.1 + 0.8 * i / double(FRAMES - 1);
        u16(uint16_t(int16_t(std::lround(32767 * amp * std::sin(2 * M_PI * i / 100.0)))));
    }
    std::fclose(f);
    return true;
}

// a note held for `held` seconds, then the release; the left channel
static std::vector<float> play(const mpc_engine_t *e, void *inst, int note, double held, double total = 0) {
    std::vector<int16_t> buf(256);
    std::vector<float> out;
    uint8_t on[3] = {0x90, uint8_t(note), 100}, off[3] = {0x80, uint8_t(note), 0};
    e->midi(inst, on, 3);
    int blocks = int(held * 44100 / 128);
    const int nb = total > 0 ? int(total * 44100 / 128) : blocks + 200;   // 2.2: total = how long to record
    for (int b = 0; b < nb; b++) {
        if (b == blocks) e->midi(inst, off, 3);
        e->render(inst, buf.data(), 128);
        for (int i = 0; i < 128; i++) out.push_back(buf[2 * i]);
    }
    return out;
}
static double rms(const std::vector<float> &x, double t0, double t1) {
    size_t a = size_t(t0 * 44100), z = std::min(x.size(), size_t(t1 * 44100));
    double s = 0;
    for (size_t i = a; i < z; i++) s += double(x[i]) * x[i];
    return z > a ? std::sqrt(s / double(z - a)) : 0;
}
static double max_step(const std::vector<float> &x, double t0, double t1) {
    size_t a = size_t(t0 * 44100) + 1, z = std::min(x.size(), size_t(t1 * 44100));
    double m = 0;
    for (size_t i = a; i < z; i++) m = std::max(m, double(std::fabs(x[i] - x[i - 1])));
    return m;
}
static int crossings(const std::vector<float> &x, double t0, double t1) {
    size_t a = size_t(t0 * 44100) + 1, z = std::min(x.size(), size_t(t1 * 44100));
    int c = 0;
    for (size_t i = a; i < z; i++) c += (x[i - 1] < 0) != (x[i] < 0);
    return c;
}
static double dot(const std::vector<float> &x, const std::vector<float> &y, double t0, double t1) {
    size_t a = size_t(t0 * 44100), z = std::min(std::min(x.size(), y.size()), size_t(t1 * 44100));
    double s = 0;
    for (size_t i = a; i < z; i++) s += double(x[i]) * y[i];
    return s;
}

static std::string note_name_test(int n) {   // as the plugin names notes (C3 = 60)
    static const char *const N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(N[n % 12]) + std::to_string(n / 12 - 2);
}
static int fails = 0;
static void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}
#include "synth_routes.inc"   // 2.4.2: the synth's routing and filters (engine level)

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "/tmp";
    std::string wav = dir + "/omni_playback_test.wav";
    if (!write_wav(wav)) { std::printf("FAIL cannot write %s\n", wav.c_str()); return 1; }
    const mpc_engine_t *e = mpc_engine();
    void *inst = e->create("/tmp/omni-sampler-test");
    std::string state = "omni-sampler 1\npath=" + wav + "\npreset=0\n";
    e->set_param(inst, "state", state.c_str());
    std::vector<int16_t> buf(256);
    auto t0 = std::chrono::steady_clock::now();
    while (get(e, inst, "prog_info").find("zones") == std::string::npos) {
        e->render(inst, buf.data(), 128);
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { std::printf("FAIL load (%s)\n", get(e, inst, "status").c_str()); return 1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("loaded: %s | %s\n", get(e, inst, "prog_info").c_str(), get(e, inst, "layer_root").c_str());
    const int note = 60;   // a WAV without a root plays at its pitch on C3

    auto fwd = play(e, inst, note, 0.6);
    double fa = rms(fwd, 0.005, 0.04), fb = rms(fwd, 0.15, 0.19), ftail = rms(fwd, 0.3, 0.6);
    std::printf("fwd: start %.0f end %.0f after %.0f, max step %.0f\n", fa, fb, ftail, max_step(fwd, 0, 0.6));
    check(fa > 0 && fb > 2 * fa, "Fwd rises (plays from the start)");
    check(ftail < 1, "Fwd stops at the end of the sample");
    const double step_ref = max_step(fwd, 0, 0.19);

    e->set_param(inst, "slot1_play", "1");
    auto rev = play(e, inst, note, 0.6);
    std::printf("rev: start %.0f end %.0f\n", rms(rev, 0.005, 0.04), rms(rev, 0.15, 0.19));
    check(rms(rev, 0.005, 0.04) > 2 * rms(rev, 0.15, 0.19), "Rev falls (plays from the end)");
    check(rms(rev, 0.3, 0.6) < 1, "Rev stops at the start of the sample");
    auto tap = play(e, inst, note, 0.03);   // a pad tap: the note-off comes long before the sample's start
    std::printf("rev tap (full): 0.15-0.19 s %.0f (held %.0f)\n", rms(tap, 0.15, 0.19), rms(rev, 0.15, 0.19));
    check(rms(tap, 0.15, 0.19) > 0.9 * rms(rev, 0.15, 0.19), "Rev (Full) plays back to the start after a short tap");
    e->set_param(inst, "rev_mode", "1");   // 2.1.16: Gated, the note-off releases
    auto tapg = play(e, inst, note, 0.03);
    std::printf("rev tap (gated): 0.15-0.19 s %.0f (held %.0f)\n", rms(tapg, 0.15, 0.19), rms(rev, 0.15, 0.19));
    check(rms(tapg, 0.15, 0.19) < 0.1 * rms(rev, 0.15, 0.19), "Rev (Gated): a short tap releases");
    e->set_param(inst, "rev_mode", "0");

    e->set_param(inst, "slot1_play", "2");
    auto pp = play(e, inst, note, 0.6);
    std::printf("ping-pong: 0.25-0.35 s %.0f, 0.45-0.55 s %.0f, max step %.0f (fwd %.0f)\n", rms(pp, 0.25, 0.35),
                rms(pp, 0.45, 0.55), max_step(pp, 0, 0.6), step_ref);
    check(rms(pp, 0.25, 0.35) > 0.5 * fb && rms(pp, 0.45, 0.55) > 0.5 * fa, "Ping-Pong keeps playing while held");
    check(rms(pp, 0.25, 0.35) > rms(pp, 0.35, 0.39), "Ping-Pong comes back down (backward pass)");
    check(max_step(pp, 0, 0.6) < 1.3 * step_ref, "Ping-Pong turns without a click");
    check(rms(pp, 0.65, 0.8) < 1, "Ping-Pong stops after the release");

    e->set_param(inst, "slot1_play", "3");
    auto ppi = play(e, inst, note, 0.6);
    double d_same = dot(ppi, pp, 0.02, 0.19), d_back = dot(ppi, pp, 0.21, 0.39);
    std::printf("pp inv: forward pass . ping-pong %.3g, backward pass . ping-pong %.3g, max step %.0f\n", d_same, d_back,
                max_step(ppi, 0, 0.6));
    check(d_same > 0 && d_back < 0, "PP Inv: forward pass as is, backward pass inverted");
    check(max_step(ppi, 0, 0.6) < 1.3 * step_ref, "PP Inv turns without a click");

    e->set_param(inst, "slot1_play", "0");
    e->set_param(inst, "slot1_inv", "1");
    auto inv = play(e, inst, note, 0.6);
    double c = dot(inv, fwd, 0, 0.6) / dot(fwd, fwd, 0, 0.6);
    std::printf("invert: correlation with fwd %.3f\n", c);
    check(c < -0.98, "Invert flips the polarity");
    e->set_param(inst, "slot1_inv", "0");

    // note locks: lock 60 an octave up; 62 stays as it is; clear
    int base_x = crossings(fwd, 0.01, 0.08);
    e->set_param(inst, "lock_note", "60");
    e->set_param(inst, "lock_tune", "12");
    std::string st = get(e, inst, "state");
    check(st.find("lock60=") != std::string::npos, "the lock is saved with the project");
    auto lk = play(e, inst, 60, 0.3);
    auto other = play(e, inst, 62, 0.3);
    int lx = crossings(lk, 0.01, 0.08), ox = crossings(other, 0.01, 0.08);
    std::printf("lock: crossings 60 unlocked %d, locked %d, 62 %d\n", base_x, lx, ox);
    check(lx > 1.8 * base_x && lx < 2.2 * base_x, "the lock's tune plays (an octave up)");
    check(ox > 1.05 * base_x && ox < 1.25 * base_x, "other notes are not locked");
    {   // the project restores the lock
        void *inst2 = e->create("/tmp/omni-sampler-test");
        e->set_param(inst2, "state", st.c_str());
        for (int i = 0; i < 5000 && get(e, inst2, "prog_info").find("zones") == std::string::npos; i++) {
            e->render(inst2, buf.data(), 128);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto lk2 = play(e, inst2, 60, 0.3);
        check(std::abs(crossings(lk2, 0.01, 0.08) - lx) <= 2, "the restored project plays the lock");
        e->destroy(inst2);
    }
    e->set_param(inst, "lock_note", "60");
    e->set_param(inst, "lock_mode", "2");   // Rev, on this note only
    auto lrev = play(e, inst, 60, 0.6);
    check(rms(lrev, 0.005, 0.04) > 2 * rms(lrev, 0.15, 0.19), "a lock's playback mode overrides the slot's");
    e->set_param(inst, "lock_clear", "1");
    auto cleared = play(e, inst, 60, 0.3);
    int cx = crossings(cleared, 0.01, 0.08);
    check(std::abs(cx - base_x) <= 2 && get(e, inst, "state").find("lock60=") == std::string::npos, "Clear Lock");

    // 1.8.2: the MOD FX page. Each effect changes the sound, off is untouched, and a project keeps the settings
    auto dry = play(e, inst, 60, 0.18);
    auto with = [&](const char *key, const char *on, const char *off) {
        e->set_param(inst, key, on);
        auto w = play(e, inst, 60, 0.18);
        e->set_param(inst, key, off);
        return w;
    };
    auto corr = [&](const std::vector<float> &a) { return dot(a, dry, 0.01, 0.17) / std::sqrt(dot(a, a, 0.01, 0.17) * dot(dry, dry, 0.01, 0.17)); };
    auto again = play(e, inst, 60, 0.18);
    check(dot(again, dry, 0, 0.3) == dot(dry, dry, 0, 0.3) && max_step(again, 0, 0.3) == max_step(dry, 0, 0.3), "MOD FX off: the sound is untouched");
    e->set_param(inst, "lofi_bits", "2");
    auto lofi = with("lofi_mix", "100", "0");
    {
        std::vector<float> vals;
        for (size_t i = size_t(0.05 * 44100); i < size_t(0.15 * 44100); i++)
            if (std::find(vals.begin(), vals.end(), lofi[i]) == vals.end()) vals.push_back(lofi[i]);
        std::printf("mod fx: lo-fi 2 bit levels %zu\n", vals.size());
        check(vals.size() <= 6, "Lo-Fi at 2 bits plays a handful of levels");
    }
    auto phs = with("phs_mix", "100", "0");
    auto flg = with("flg_mix", "100", "0");
    std::printf("mod fx: correlation with dry: phaser %.3f, flanger %.3f\n", corr(phs), corr(flg));   // a sine only shifts: close values
    check(corr(phs) < 0.98 && corr(phs) > 0.2, "Phaser changes the sound");
    check(corr(flg) < 0.98 && corr(flg) > 0.2, "Flanger changes the sound");
    e->set_param(inst, "trm_shape", "1");
    e->set_param(inst, "trm_rate", "100");   // 20 Hz
    auto trm = with("trm_depth", "100", "0");
    {
        double lo = 1e9, hi = 0;
        for (double t = 0.02; t < 0.16; t += 0.005) { double v = rms(trm, t, t + 0.005) / std::max(1.0, rms(dry, t, t + 0.005)); lo = std::min(lo, v); hi = std::max(hi, v); }
        std::printf("mod fx: tremolo level %.2f .. %.2f of dry\n", lo, hi);
        check(lo < 0.2 && hi > 0.8, "Tremolo (square, full depth) cuts the sound in and out");
        check(max_step(trm, 0.02, 0.16) <= max_step(dry, 0.02, 0.16) * 1.5 + 50, "Tremolo's square has no clicks");
    }
    e->set_param(inst, "phs_mix", "55");
    e->set_param(inst, "trm_sync", "6");
    e->set_param(inst, "lofi_rate", "70");
    {
        std::string st2 = get(e, inst, "state");
        check(st2.find("phs_mix=55") != std::string::npos && st2.find("trm_sync=6") != std::string::npos
              && st2.find("lofi_rate=70") != std::string::npos, "the project saves the MOD FX settings");
        void *inst2 = e->create("/tmp/omni-sampler-test");
        e->set_param(inst2, "state", st2.c_str());
        check(get(e, inst2, "state").find("phs_mix=55") != std::string::npos && get(e, inst2, "trm_rate_display") == "1/8"
              && get(e, inst2, "lofi_rate_display") == "3.1 kHz", "the restored project has them");
        std::printf("mod fx: restored rate displays %s, %s\n", get(e, inst2, "trm_rate_display").c_str(), get(e, inst2, "lofi_rate_display").c_str());
        e->destroy(inst2);
    }

    {   // Rev follows the PLAY page's envelope: decay / sustain shape a tap, the release ends at the sample's start
        void *r = e->create("/tmp/omni-sampler-test");
        e->set_param(r, "state", state.c_str());
        for (int i = 0; i < 5000 && get(e, r, "prog_info").find("zones") == std::string::npos; i++) {
            e->render(r, buf.data(), 128);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        e->set_param(r, "slot1_play", "1");
        auto base = play(e, r, note, 0.6);
        e->set_param(r, "env_decay", "87");   // 40 ms
        e->set_param(r, "env_sustain", "0");
        auto dec = play(e, r, note, 0.03);
        std::printf("rev env: decay to 0, 0.005-0.02 s %.0f (no env %.0f), 0.12-0.19 s %.0f (no env %.0f)\n", rms(dec, 0.005, 0.02),
                    rms(base, 0.005, 0.02), rms(dec, 0.12, 0.19), rms(base, 0.12, 0.19));
        check(rms(dec, 0.005, 0.02) > 0.5 * rms(base, 0.005, 0.02) && rms(dec, 0.12, 0.19) < 0.05 * rms(base, 0.12, 0.19),
              "Rev follows Decay and Sustain");
        e->set_param(r, "env_sustain", "100");
        e->set_param(r, "env_decay", "0");
        e->set_param(r, "env_release", "110");   // 80 ms
        e->set_param(r, "rev_mode", "1");        // 2.1.16: Gated: the tap's note-off starts the 80 ms release
        auto relf = play(e, r, note, 0.03);
        std::printf("rev env (gated): release 80 ms, 0.03-0.06 s %.0f, 0.14-0.2 s %.0f (no env %.0f)\n",
                    rms(relf, 0.03, 0.06), rms(relf, 0.14, 0.2), rms(base, 0.14, 0.2));
        check(rms(relf, 0.03, 0.06) > 0.2 * rms(base, 0.03, 0.06) && rms(relf, 0.14, 0.2) < 0.05 * rms(base, 0.14, 0.2),
              "Rev (Gated) releases over the Release time after the note-off");
        e->set_param(r, "rev_mode", "0");        // the rest: Full
        auto rel = play(e, r, note, 0.03);
        std::printf("rev env: release 80 ms, 0.03-0.10 s %.0f (no env %.0f), 0.18-0.2 s %.0f (no env %.0f), max step %.0f (%.0f)\n",
                    rms(rel, 0.03, 0.10), rms(base, 0.03, 0.10), rms(rel, 0.18, 0.2), rms(base, 0.18, 0.2),
                    max_step(rel, 0, 0.3), max_step(base, 0, 0.3));
        check(rms(rel, 0.03, 0.10) > 0.9 * rms(base, 0.03, 0.10), "Rev with a release still plays on after a tap");
        check(rms(rel, 0.18, 0.2) < 0.5 * rms(base, 0.18, 0.2) && rms(rel, 0.2, 0.3) < 1, "Rev's release fades out by the start");
        check(max_step(rel, 0, 0.3) <= max_step(base, 0, 0.3), "Rev's release adds no click");
        // 2.1.16: a release longer than the sample (2 s against 0.2 s) used to start the fade at the note-on: silent
        e->set_param(r, "env_release", "322");   // about 2 s
        auto lrel = play(e, r, note, 0.03);
        std::printf("rev env: release 2 s, 0.005-0.09 s %.0f (no env %.0f), 0.2-0.3 s %.0f, max step %.0f (%.0f)\n",
                    rms(lrel, 0.005, 0.09), rms(base, 0.005, 0.09), rms(lrel, 0.2, 0.3), max_step(lrel, 0, 0.3), max_step(base, 0, 0.3));
        check(rms(lrel, 0.005, 0.09) > 0.8 * rms(base, 0.005, 0.09), "Rev with a release longer than the sample still plays");
        check(rms(lrel, 0.2, 0.3) < 1 && max_step(lrel, 0, 0.3) <= max_step(base, 0, 0.3), "...and fades out by the start, no click");
        e->set_param(r, "rev_mode", "1");        // Gated with that release: cut to end at the start
        auto lrelg = play(e, r, note, 0.03);
        std::printf("rev env (gated): release 2 s, 0.2-0.3 s %.0f, max step %.0f\n", rms(lrelg, 0.2, 0.3), max_step(lrelg, 0, 0.3));
        check(rms(lrelg, 0.2, 0.3) < 1 && max_step(lrelg, 0, 0.3) <= max_step(base, 0, 0.3), "Rev (Gated): a long release still ends at the start, no click");
        e->set_param(r, "rev_mode", "0");
        e->destroy(r);
    }

    {   // 2.2: the reverb types: each one rings on after the note and dies away; Classic is 1.0's Freeverb
        double tail[5], late[5];
        for (int t = 0; t < 5; t++) {   // 0 Off (the default), 1 Classic, 2 Plate, 3 Hall, 4 Room
            e->set_param(inst, "rev_type", std::to_string(t).c_str());
            e->set_param(inst, "rev_mix", "50");
            e->set_param(inst, "rev_size", "50");
            auto w = play(e, inst, note, 0.2, 6.0);
            tail[t] = rms(w, 0.4, 0.8); late[t] = rms(w, 5.5, 6.0);
            bool finite = true;
            for (float x : w) finite = finite && std::isfinite(x);
            std::printf("reverb type %d: 0.4-0.8 s %.0f, 5.5-6 s %.1f\n", t, tail[t], late[t]);
            if (t == 0) { check(finite && tail[0] < 1, "Reverb Off (no type lit): no reverb with the mix up"); continue; }
            check(finite && tail[t] > 30 && late[t] < 0.05 * tail[t], t == 1 ? "Reverb Classic rings on, then dies away"
                  : t == 2 ? "Reverb Plate rings on, then dies away" : t == 3 ? "Reverb Hall rings on, then dies away"
                  : "Reverb Room rings on, then dies away");
        }
        check(tail[3] > tail[4], "Hall rings longer than Room");
        e->set_param(inst, "rev_mix", "0");
        e->set_param(inst, "rev_type", "0");
        // a project saved before the types, with its reverb up, keeps its sound: Classic
        void *r = e->create("/tmp/omni-sampler-test");
        e->set_param(r, "state", "omni-sampler 1\nrev_mix=40\n");
        check(get(e, r, "state").find("rev_type=1\n") != std::string::npos, "an older project with reverb opens as Classic");
        e->set_param(r, "state", "omni-sampler 1\nrev_mix=40\nrev_type=3\n");
        check(get(e, r, "state").find("rev_type=3\n") != std::string::npos, "a project's own reverb type is kept");
        e->destroy(r);
        r = e->create("/tmp/omni-sampler-test");
        e->set_param(r, "state", "omni-sampler 1\nrev_mix=0\n");
        check(stv(get(e, r, "state"), "rev_type") == "0", "...and one without reverb stays Off");
        e->destroy(r);
    }

    {   // 2.2: the drive's types: Off leaves the sound as it is whatever the knob; each type shapes it, finite, in range
        e->set_param(inst, "drive", "0");
        e->set_param(inst, "drive_type", "0");
        auto dry = play(e, inst, note, 0.2);
        e->set_param(inst, "drive", "80");
        auto off = play(e, inst, note, 0.2);
        // (two notes are not bit-identical: free-running LFOs) the level and the peak stay the dry note's
        double pk_dry = 0, pk_off = 0;
        for (size_t i = 0; i < dry.size(); i++) { pk_dry = std::max(pk_dry, double(std::fabs(dry[i]))); pk_off = std::max(pk_off, double(std::fabs(off[i]))); }
        std::printf("drive off: rms %.0f (dry %.0f), peak %.0f (dry %.0f)\n", rms(off, 0.02, 0.2), rms(dry, 0.02, 0.2), pk_off, pk_dry);
        // (free-running LFOs move two notes' levels up to ~10% apart; drive at 80% would double the level and square the peaks)
        check(std::fabs(rms(off, 0.02, 0.2) / rms(dry, 0.02, 0.2) - 1) < 0.15 && pk_off < 1.2 * pk_dry, "Drive Off: the DRIVE knob changes nothing");
        const char *names[] = {"", "Classic", "Tape", "Tube", "Fold"};
        for (int t = 1; t <= 4; t++) {
            e->set_param(inst, "drive_type", std::to_string(t).c_str());
            auto w = play(e, inst, note, 0.2);
            bool finite = true; double mx = 0;
            for (float x : w) { finite = finite && std::isfinite(x); mx = std::max(mx, double(std::fabs(x))); }
            double corr = 0;   // the best over the drive's delay (15 samples) and a little more
            for (int lag = 0; lag < 40; lag++) {
                std::vector<float> sh(w.begin() + lag, w.end());
                corr = std::max(corr, dot(sh, dry, 0.02, 0.19) / std::sqrt(dot(sh, sh, 0.02, 0.19) * dot(dry, dry, 0.02, 0.19)));
            }
            std::printf("drive %s: correlation with dry %.3f, peak %.0f, rms %.0f (dry %.0f)\n", names[t], corr, mx, rms(w, 0.02, 0.2), rms(dry, 0.02, 0.2));
            std::string what = std::string("Drive ") + names[t] + " shapes the sound, finite and in range";
            check(finite && corr < 0.999 && corr > 0.3 && mx < 32767 && rms(w, 0.02, 0.2) > 0.5 * rms(dry, 0.02, 0.2), what.c_str());
        }
        e->set_param(inst, "drive", "0");
        e->set_param(inst, "drive_type", "0");
        void *r = e->create("/tmp/omni-sampler-test");
        e->set_param(r, "state", "omni-sampler 1\ndrive=30\n");
        check(get(e, r, "state").find("drive_type=1\n") != std::string::npos, "an older project with drive opens as Classic");
        e->destroy(r);
    }

    {   // 2.2: the delay's types: Off gives no repeats whatever the mix; each type repeats and dies away; Tape and Analog
        // differ from Classic (their drift, saturation and filters)
        e->set_param(inst, "dly_mix", "60");
        e->set_param(inst, "dly_fb", "50");
        e->set_param(inst, "dly_time", "8");   // 1/4: 0.5 s at 120 BPM
        double rep[4];
        std::vector<float> cls;
        const char *names[] = {"Off", "Classic", "Tape", "Analog"};
        for (int t = 0; t < 4; t++) {
            e->set_param(inst, "dly_type", std::to_string(t).c_str());
            auto w = play(e, inst, note, 0.15, 6.0);
            bool finite = true;
            for (float x : w) finite = finite && std::isfinite(x);
            rep[t] = rms(w, 0.5, 0.7);
            std::printf("delay %s: first repeat %.0f, 5.5-6 s %.1f\n", names[t], rep[t], rms(w, 5.5, 6.0));
            if (t == 0) { check(finite && rep[0] < 1, "Delay Off (no type lit): no repeats with the mix up"); continue; }
            std::string what = std::string("Delay ") + names[t] + " repeats, then dies away";
            check(finite && rep[t] > 100 && rms(w, 5.5, 6.0) < 0.05 * rep[t], what.c_str());
            if (t == 1) cls = w;
            else {
                double c = dot(w, cls, 0.5, 2.0) / std::sqrt(dot(w, w, 0.5, 2.0) * dot(cls, cls, 0.5, 2.0));
                std::printf("delay %s: correlation of its repeats with Classic's %.3f\n", names[t], c);
                what = std::string("Delay ") + names[t] + " sounds different from Classic";
                check(c < 0.98, what.c_str());
            }
        }
        e->set_param(inst, "dly_mix", "0");
        e->set_param(inst, "dly_type", "0");
        void *r = e->create("/tmp/omni-sampler-test");
        e->set_param(r, "state", "omni-sampler 1\ndly_mix=25\n");
        check(get(e, r, "state").find("dly_type=1\n") != std::string::npos, "an older project with delay opens as Classic");
        e->destroy(r);
    }

    {   // 2.2: the chorus's types: Off leaves the sound dry whatever the mix; the ensemble's modes widen it (left and
        // right differ) and differ from each other
        void *c = e->create("/tmp/omni-sampler-test");   // a fresh instance: nothing else modulating
        e->set_param(c, "state", state.c_str());
        for (int i = 0; i < 5000 && get(e, c, "prog_info").find("zones") == std::string::npos; i++) {
            e->render(c, buf.data(), 128);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const char *names[] = {"Off", "Classic", "Ens I", "Ens II", "Ens I+II"};
        double width[5], base = 0;
        std::vector<int16_t> buf2(256);
        for (int t = -1; t < 5; t++) {   // -1: the dry note (mix 0), the reference for Off
            e->set_param(c, "cho_mix", t < 0 ? "0" : "80");
            e->set_param(c, "cho_type", std::to_string(std::max(0, t)).c_str());
            // left and right of a held note (play() keeps the left only)
            uint8_t on[3] = {0x90, uint8_t(note), 100}, off[3] = {0x80, uint8_t(note), 0};
            e->midi(c, on, 3);
            double ll = 0, rr = 0, lr = 0; bool finite = true;
            for (int k = 0; k < 60; k++) {
                e->render(c, buf2.data(), 128);
                if (k < 10) continue;
                for (int i = 0; i < 128; i++) { double a = buf2[2 * i], b = buf2[2 * i + 1]; ll += a * a; rr += b * b; lr += a * b; finite = finite && std::isfinite(a); }
            }
            e->midi(c, off, 3);
            for (int k = 0; k < 300; k++) e->render(c, buf2.data(), 128);
            if (t < 0) { base = 1 - lr / std::sqrt(ll * rr); continue; }
            width[t] = 1 - lr / std::sqrt(ll * rr);
            std::printf("chorus %s: 1 - L/R correlation %.4f (dry %.4f), level %.0f\n", names[t], width[t], base, std::sqrt(ll / 6400));
            if (t == 0) { check(std::fabs(width[0] - base) < 0.002, "Chorus Off (no type lit): as dry, whatever the mix"); continue; }
            std::string what = std::string("Chorus ") + names[t] + " widens the sound";
            check(finite && width[t] > base + 0.05, what.c_str());
        }
        e->set_param(c, "cho_mix", "0");
        e->set_param(c, "cho_type", "0");
        e->destroy(c);
        void *r = e->create("/tmp/omni-sampler-test");
        e->set_param(r, "state", "omni-sampler 1\ncho_mix=25\n");
        check(get(e, r, "state").find("cho_type=1\n") != std::string::npos, "an older project with chorus opens as Classic");
        e->destroy(r);
    }

    {   // 2.2: compressor types and the limiter; the GR meter; an older project keeps its sound
        void *c = e->create("/tmp/omni-sampler-test");
        e->set_param(c, "state", state.c_str());
        for (int i = 0; i < 5000 && get(e, c, "prog_info").find("zones") == std::string::npos; i++) {
            e->render(c, buf.data(), 128);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        e->set_param(c, "volume", "6");   // hot, so there is something to reduce
        auto dry = play(e, c, note, 0.2);
        e->set_param(c, "comp", "80");
        auto off = play(e, c, note, 0.2);
        check(std::fabs(rms(off, 0.02, 0.2) / rms(dry, 0.02, 0.2) - 1) < 0.1 && get(e, c, "gr_meter") == "0",
              "Compressor Off (none chosen): AMOUNT changes nothing, the GR meter rests");
        const char *names[] = {"Off", "Classic", "Glue", "Punch"};
        for (int t = 1; t <= 3; t++) {
            e->set_param(c, "comp_type", std::to_string(t).c_str());
            uint8_t on[3] = {0x90, uint8_t(note), 100}, off_[3] = {0x80, uint8_t(note), 0};
            e->midi(c, on, 3);
            int gr = 0; bool finite = true; double s2 = 0; int n = 0;
            for (int k = 0; k < 60; k++) {
                e->render(c, buf.data(), 128);
                gr = std::max(gr, std::atoi(get(e, c, "gr_meter").c_str()));
                for (int i = 0; i < 256; i++) { finite = finite && std::isfinite(float(buf[size_t(i)])); if (k > 10) { s2 += double(buf[size_t(i)]) * buf[size_t(i)]; n++; } }
            }
            e->midi(c, off_, 3);
            for (int k = 0; k < 400; k++) e->render(c, buf.data(), 128);
            std::printf("compressor %s: GR meter up to %d of 127, level %.0f (dry %.0f)\n", names[t], gr, std::sqrt(s2 / n), rms(dry, 0.02, 0.2));
            std::string what = std::string("Compressor ") + names[t] + " reduces (the GR meter moves), finite";
            check(finite && gr > 0, what.c_str());
        }
        e->set_param(c, "comp_type", "0");
        // the limiter: a hot note's peaks held at -1 dBFS (0.89), where the soft clip begins
        e->set_param(c, "volume", "12");
        auto hot = play(e, c, note, 0.2);
        e->set_param(c, "limiter", "1");
        auto lim = play(e, c, note, 0.2);
        double pk_hot = 0, pk_lim = 0;
        for (float x : hot) pk_hot = std::max(pk_hot, double(std::fabs(x)));
        for (float x : lim) pk_lim = std::max(pk_lim, double(std::fabs(x)));
        std::printf("limiter: peak %.0f (without %.0f), ceiling %.0f\n", pk_lim, pk_hot, 0.89 * 32767);
        check(pk_lim <= 0.891 * 32767 && pk_hot > pk_lim, "Limiter holds the peaks at -1 dBFS");
        e->destroy(c);
        void *r = e->create("/tmp/omni-sampler-test");
        e->set_param(r, "state", "omni-sampler 1\ncomp=40\nrev_mix=20\ndrive=10\ndly_mix=30\ncho_mix=30\n");
        std::string st = get(e, r, "state");
        check(stv(st, "comp_type") == "1" && stv(st, "limiter") == "0"
              && st.find("rev_type=1\n") != std::string::npos && st.find("drive_type=1\n") != std::string::npos
              && st.find("dly_type=1\n") != std::string::npos && st.find("cho_type=1\n") != std::string::npos,
              "an older project: every effect it used opens as Classic, the limiter stays off");
        e->destroy(r);
        r = e->create("/tmp/omni-sampler-test");
        e->set_param(r, "state", "omni-sampler 1\nvolume=0\n");
        st = get(e, r, "state");
        check(stv(st, "comp_type") == "0" && stv(st, "rev_type") == "0"
              && stv(st, "dly_type") == "0" && stv(st, "cho_type") == "0"
              && stv(st, "drive_type") == "0", "...and one without effects keeps them all off");
        e->destroy(r);
    }

    {   // 2.2.3: SAMPLE_ACCURATE: the wrapper renders any 1..128 frames between MIDI events. Uneven pieces sound as
        // whole 128-frame blocks do, and a note sent between two pieces starts there, not at a block start
        auto run = [&](bool uneven, int start_at) {
            void *c = e->create("/tmp/omni-sampler-test");
            e->set_param(c, "state", state.c_str());
            for (int i = 0; i < 5000 && get(e, c, "prog_info").find("zones") == std::string::npos; i++) {
                e->render(c, buf.data(), 128);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            for (int i = 0; i < 20; i++) e->render(c, buf.data(), 128);
            std::vector<int16_t> out;
            std::vector<int16_t> b(256);
            unsigned rnd = 12345;
            int done = 0, total = 44100 / 4;
            bool sent = false;
            while (done < total) {
                int n = 128;
                if (uneven) { rnd = rnd * 1664525u + 1013904223u; n = 1 + int((rnd >> 8) % 128); }
                if (!sent && done + n > start_at) n = std::max(1, start_at - done);   // stop at the note's frame
                if (!sent && done >= start_at) {
                    uint8_t on[3] = {0x90, uint8_t(note), 100};
                    e->midi(c, on, 3);
                    sent = true;
                }
                n = std::min(n, total - done);
                e->render(c, b.data(), n);
                out.insert(out.end(), b.begin(), b.begin() + 2 * n);
                done += n;
            }
            e->destroy(c);
            return out;
        };
        auto whole = run(false, 0), pieces = run(true, 0), late = run(true, 37);
        double ew = 0, ep = 0;
        bool finite = true;
        for (size_t i = 0; i < whole.size(); i += 2) { ew += double(whole[i]) * whole[i]; ep += double(pieces[i]) * pieces[i]; }
        for (int16_t v : pieces) finite = finite && v > -32768;
        std::printf("uneven blocks: level %.4f of whole blocks\n", std::sqrt(ep / ew));
        check(finite && std::fabs(std::sqrt(ep / ew) - 1) < 0.02, "Uneven render sizes (1..128 frames) sound as whole blocks");
        int first = -1;   // the first frame that sounds, in the note sent at frame 37
        for (size_t i = 0; i < late.size(); i += 2) if (std::abs(late[i]) > 0) { first = int(i / 2); break; }
        std::printf("a note sent at frame 37 first sounds at frame %d\n", first);
        check(first >= 37 && first < 37 + 8, "A note sent between two pieces starts there (sample-accurate)");
    }

    {   // 2.3: the SYNTH layer
        const std::string lib = dir + "/omni_syn_lib";
        std::string cmd = "mkdir -p '" + lib + "/Oscillators'";
        if (std::system(cmd.c_str()) != 0) std::printf("FAIL mkdir\n");
        {   // a user's own wavetable: 2 frames of 2048 (a sine, then a square)
            FILE *f = std::fopen((lib + "/Oscillators/My Table.wav").c_str(), "wb");
            const int n = 2 * 2048;
            auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
            auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
            std::fwrite("RIFF", 1, 4, f); u32(36 + n * 2); std::fwrite("WAVEfmt ", 1, 8, f);
            u32(16); u16(1); u16(1); u32(44100); u32(88200); u16(2); u16(16);
            std::fwrite("data", 1, 4, f); u32(n * 2);
            for (int i = 0; i < n; i++) {
                double ph = (i % 2048) / 2048.0;
                double v = i < 2048 ? std::sin(2 * M_PI * ph) : (ph < 0.5 ? 0.8 : -0.8);
                u16(uint16_t(int16_t(std::lround(30000 * v))));
            }
            std::fclose(f);
        }
        {   // 2.3.2: a subfolder of single cycles (a sine, a triangle, a square; 600 samples each): a folder entry too
            cmd = "mkdir -p '" + lib + "/Oscillators/Cycles'";
            if (std::system(cmd.c_str()) != 0) std::printf("FAIL mkdir\n");
            const char *names[] = {"wave1.wav", "wave2.wav", "wave10.wav"};
            for (int w = 0; w < 3; w++) {
                FILE *f = std::fopen((lib + "/Oscillators/Cycles/" + names[w]).c_str(), "wb");
                const int n = 600;
                auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
                auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
                std::fwrite("RIFF", 1, 4, f); u32(36 + n * 2); std::fwrite("WAVEfmt ", 1, 8, f);
                u32(16); u16(1); u16(1); u32(44100); u32(88200); u16(2); u16(16);
                std::fwrite("data", 1, 4, f); u32(n * 2);
                for (int i = 0; i < n; i++) {
                    double ph = double(i) / n;
                    double v = w == 0 ? std::sin(2 * M_PI * ph) : w == 1 ? (ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph) : (ph < 0.5 ? 0.8 : -0.8);
                    u16(uint16_t(int16_t(std::lround(30000 * v))));
                }
                std::fclose(f);
            }
        }
        setenv("OMNI_SAMPLER_LIBRARY", lib.c_str(), 1);
        void *y = e->create("/tmp/omni-sampler-test-syn");
        unsetenv("OMNI_SAMPLER_LIBRARY");
        std::vector<int16_t> b2(2 * 128);
        auto wait_loaded = [&](const char *osc) {
            std::string k = std::string(osc) + "_name";
            for (int i = 0; i < 3000; i++) {
                std::string n = get(e, y, k.c_str());
                if (!n.empty() && n != "Loading...") return n;
                e->render(y, b2.data(), 128);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return std::string("(timeout)");
        };
        // render a held note; returns left and right
        auto note_lr = [&](int nn, double secs, std::vector<float> &L, std::vector<float> &R) {
            L.clear(); R.clear();
            uint8_t on[3] = {0x90, uint8_t(nn), 100}, off[3] = {0x80, uint8_t(nn), 0};
            e->midi(y, on, 3);
            for (int k = 0; k < int(secs * 44100 / 128); k++) {
                e->render(y, b2.data(), 128);
                for (int i = 0; i < 128; i++) { L.push_back(b2[size_t(2 * i)]); R.push_back(b2[size_t(2 * i + 1)]); }
            }
            e->midi(y, off, 3);
            for (int k = 0; k < 200; k++) e->render(y, b2.data(), 128);
        };
        std::vector<float> L, R;
        std::printf("synth: OSC A %s, OSC B %s\n", wait_loaded("sa").c_str(), wait_loaded("sb").c_str());
        note_lr(69, 0.3, L, R);
        check(rms(L, 0.05, 0.3) < 1, "SYNTH OFF (the default): silent");
        e->set_param(y, "syn_on", "1");
        e->set_param(y, "sa_type", "0");   // CLASSIC
        e->set_param(y, "sa_table", "0");  // Sine
        std::string nm = wait_loaded("sa");
        note_lr(69, 0.5, L, R);
        int zc = crossings(L, 0.1, 0.5);
        std::printf("synth: %s, A4: %.0f Hz (%d crossings in 0.4 s), level %.0f\n", nm.c_str(), zc / 0.8, zc, rms(L, 0.1, 0.5));
        check(nm == "Classic / Sine" && std::abs(zc / 0.8 - 440) < 6 && rms(L, 0.1, 0.5) > 500, "SYNTH ON: a sine A4 plays at 440 Hz");
        e->set_param(y, "sa_oct", "1");
        note_lr(69, 0.5, L, R);
        zc = crossings(L, 0.1, 0.5);
        check(std::abs(zc / 0.8 - 880) < 8, "OCTAVE +1: 880 Hz");
        e->set_param(y, "sa_oct", "0");
        double corr0 = dot(L, R, 0.1, 0.5) / std::sqrt(dot(L, L, 0.1, 0.5) * dot(R, R, 0.1, 0.5));
        e->set_param(y, "sa_unison", "4"); e->set_param(y, "sa_detune", "30"); e->set_param(y, "sa_spread", "100");
        note_lr(69, 0.5, L, R);
        double corr = dot(L, R, 0.1, 0.5) / std::sqrt(dot(L, L, 0.1, 0.5) * dot(R, R, 0.1, 0.5));
        std::printf("synth: unison 4 L/R correlation %.3f (one voice %.3f)\n", corr, corr0);
        check(corr0 > 0.999 && corr < 0.9, "UNISON 4 with SPREAD: a wide stereo stack");
        e->set_param(y, "sa_unison", "1");
        // a high saw stays band-limited: its energy is in its harmonics
        e->set_param(y, "sa_table", "2");  // Saw
        wait_loaded("sa");
        note_lr(100, 0.4, L, R);
        {
            double f0 = 440.0 * std::pow(2.0, (100 - 69) / 12.0), harm = 0, total = 0;
            size_t a0 = size_t(0.1 * 44100), n = 8192;
            for (size_t i = a0; i < a0 + n; i++) total += double(L[i]) * L[i];
            auto bin = [&](double f) {
                double re = 0, im = 0;
                for (size_t i = 0; i < n; i++) { double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n); re += w * L[a0 + i] * std::cos(2 * M_PI * f * i / 44100); im += w * L[a0 + i] * std::sin(2 * M_PI * f * i / 44100); }
                return (re * re + im * im);
            };
            double alias = 0;   // energy between the harmonics (where aliases land)
            for (double f = 200; f < 21000; f += 97) { double h = std::fmod(f, f0); if (h > 150 && h < f0 - 150) alias += bin(f); }
            for (int k = 1; k * f0 < 21000; k++) harm += bin(k * f0);
            std::printf("synth: saw at %.0f Hz, alias / harmonic energy %.1f dB\n", f0, 10 * std::log10(alias / harm + 1e-12));
            check(10 * std::log10(alias / harm + 1e-12) < -40, "A high saw has no audible aliasing (mip-mapped)");
            (void)total;
        }
        // the user's own wavetable, from <library>/Oscillators
        e->set_param(y, "sa_type", "1");
        e->set_param(y, "sa_table", "14");   // after the 10 factory tables and the Cycles folder (4 entries)
        nm = wait_loaded("sa");
        std::string info = get(e, y, "sa_info");
        std::printf("synth: %s | %s\n", nm.c_str(), info.c_str());
        check(nm == "Library / My Table.wav" && info.find("2 FRAMES") != std::string::npos, "A WAV in the Library's Oscillators folder loads (2 frames)");
        // 2.4.9: the info line: which folder (Factory, Cycles, the Library's own files: 3); the frame is POSITION's text
        check(info.find("FOLDER 3/3") != std::string::npos, "the info line names the folder (3 of 3)");
        e->set_param(y, "sa_pos", "0");
        check(get(e, y, "sa_pos_display") == "1/2", "POSITION 0 on a 2-frame table shows frame 1/2");
        e->set_param(y, "sa_pos", "100");
        check(get(e, y, "sa_pos_display") == "2/2", "POSITION 100 shows frame 2/2");
        const std::string info_at_100 = get(e, y, "sa_info");
        check(info_at_100 == info, "turning POSITION leaves the info line as it is (no screen rebuild on MPC)");
        note_lr(69, 0.3, L, R);
        check(rms(L, 0.1, 0.3) > 500, "...and plays (POSITION at its square frame)");
        // 2.3.2: subfolders: "Cycles/" (a wavetable of its 3 files, in natural order) then its files
        e->set_param(y, "sa_table", "10");   // the list: Cycles/, Cycles/wave1, wave2, wave10, My Table.wav
        nm = wait_loaded("sa");
        info = get(e, y, "sa_info");
        std::printf("synth: %s | %s\n", nm.c_str(), info.c_str());
        check(nm == "Cycles  (frames)" && info.find("3 FRAMES") != std::string::npos && info.find("FOLDER 2/3") != std::string::npos,
              "A folder of single cycles loads as one wavetable (3 frames), folder 2 of 3");
        e->set_param(y, "sa_table", "13");
        nm = wait_loaded("sa");
        check(nm == "Cycles / wave10.wav", "...its files list in natural order (wave10 after wave2)");
        // FOLDER: from the first factory table to the next folder (Cycles), then on to the root files (My Table.wav)
        e->set_param(y, "sa_table", "0"); wait_loaded("sa");
        e->set_param(y, "sa_folder", "1"); e->set_param(y, "sa_folder", "0");
        nm = wait_loaded("sa");
        std::printf("synth: FOLDER from Factory: %s\n", nm.c_str());
        check(nm == "Cycles  (frames)", "FOLDER jumps to the next folder");
        // 2.4.9: < FOLDER back to the factory tables' first, and from there round to the last folder (the Library's files)
        e->set_param(y, "sa_table", "12"); wait_loaded("sa");
        e->set_param(y, "sa_fprev", "1"); e->set_param(y, "sa_fprev", "0");
        nm = wait_loaded("sa");
        check(nm.rfind("Factory / ", 0) == 0 && get(e, y, "sa_table") == "0", "< FOLDER: from inside Cycles to the factory tables' first");
        e->set_param(y, "sa_fprev", "1"); e->set_param(y, "sa_fprev", "0");
        nm = wait_loaded("sa");
        check(nm == "Library / My Table.wav", "< FOLDER from the first folder: round to the last");
        // RANDOM: another entry of the same folder; RND ALL: another entry of the whole list
        bool same_folder = true, never_same = true;
        for (int i = 0; i < 12; i++) {
            e->set_param(y, "sa_table", "11"); wait_loaded("sa");
            e->set_param(y, "sa_rnd", "1"); e->set_param(y, "sa_rnd", "0");
            nm = wait_loaded("sa");
            const int t = std::atoi(get(e, y, "sa_table").c_str());
            if (t < 10 || t > 13) same_folder = false;
            if (t == 11) never_same = false;
        }
        check(same_folder && never_same, "RANDOM: another table of the same folder (Cycles), never the one playing");
        bool moved = true;
        for (int i = 0; i < 12; i++) {
            e->set_param(y, "sa_table", "3"); wait_loaded("sa");
            e->set_param(y, "sa_rndall", "1"); e->set_param(y, "sa_rndall", "0");
            wait_loaded("sa");
            const int t = std::atoi(get(e, y, "sa_table").c_str());
            if (t == 3 || t < 0 || t > 14) moved = false;
        }
        check(moved, "RND ALL: another entry of the whole list");
        e->set_param(y, "sa_table", "14");
        wait_loaded("sa");
        // the screen's bars follow the cycle
        for (int i = 0; i < 30; i++) { e->render(y, b2.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        int hi = 0, lo = 127;
        for (int bb = 1; bb <= 32; bb++) { int v = std::atoi(get(e, y, ("sa_w" + std::to_string(bb)).c_str()).c_str()); hi = std::max(hi, v); lo = std::min(lo, v); }
        std::printf("synth: OSC A bars %d..%d\n", lo, hi);
        check(hi > 100 && lo < 28, "The OSC screen's bars draw the cycle");
        // a project keeps the user's file by name
        std::string st = get(e, y, "state");
        check(st.find("syn_file_a=My Table.wav") != std::string::npos && st.find("syn_on=1") != std::string::npos, "The project saves the synth and its oscillator file");
        // 2.3.1: MIX / FILTER. A plain saw on OSC A for these
        e->set_param(y, "sa_type", "0"); e->set_param(y, "sa_table", "2"); e->set_param(y, "sa_pos", "0");
        wait_loaded("sa");
        auto hf = [&](const std::vector<float> &x) {   // the energy above ~5 kHz (a first difference) against the level
            double d = 0, t = 0;
            for (size_t i = size_t(0.1 * 44100) + 1; i < size_t(0.3 * 44100) && i < x.size(); i++) { double v = x[i] - x[i - 1]; d += v * v; t += double(x[i]) * x[i]; }
            return t > 0 ? d / t : 0;
        };
        note_lr(57, 0.3, L, R);
        const double saw_hf = hf(L), saw_lvl = rms(L, 0.1, 0.3);
        // SUB: an octave below (A2 = 110 Hz from A3) on its own
        e->set_param(y, "sa_on", "0"); e->set_param(y, "syn_sub_on", "1"); e->set_param(y, "syn_sub_level", "100");
        for (int i = 0; i < 50; i++) { e->render(y, b2.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        note_lr(57, 0.5, L, R);
        zc = crossings(L, 0.1, 0.5);
        std::printf("synth: SUB alone at A3: %.0f Hz\n", zc / 0.8);
        check(std::abs(zc / 0.8 - 110) < 4, "SUB plays an octave down (110 Hz under A3)");
        e->set_param(y, "syn_sub_oct", "1");
        note_lr(57, 0.5, L, R);
        zc = crossings(L, 0.1, 0.5);
        check(std::abs(zc / 0.8 - 55) < 4, "SUB -2 OCT: 55 Hz");
        e->set_param(y, "syn_sub_on", "0"); e->set_param(y, "syn_sub_oct", "0");
        // NOISE: each colour sounds
        e->set_param(y, "syn_noise_on", "1"); e->set_param(y, "syn_noise_level", "100");
        for (int c = 0; c < 3; c++) {
            e->set_param(y, "syn_noise_color", std::to_string(c).c_str());
            note_lr(57, 0.3, L, R);
            std::string what = std::string("NOISE ") + (c == 0 ? "white" : c == 1 ? "pink" : "crackle") + " sounds";
            std::printf("synth: noise %d level %.0f\n", c, rms(L, 0.05, 0.3));
            check(rms(L, 0.05, 0.3) > 100, what.c_str());
        }
        e->set_param(y, "syn_noise_on", "0"); e->set_param(y, "sa_on", "1");
        // FILTER: each type changes the saw; the low-passes take its highs away; the cutoff reads in Hz
        e->set_param(y, "syn_flt_on", "1"); e->set_param(y, "syn_cutoff", "45");   // ~460 Hz
        const char *ft[] = {"LP 12", "LP 24", "Ladder", "BP", "HP", "Notch", "Comb"};
        for (int t = 0; t < 7; t++) {
            e->set_param(y, "syn_flt_type", std::to_string(t).c_str());
            e->set_param(y, "syn_res", t == 6 ? "60" : "30");
            note_lr(57, 0.3, L, R);
            bool finite = true;
            for (float v : L) finite = finite && std::isfinite(v);
            double h = hf(L), lv = rms(L, 0.1, 0.3);
            std::printf("synth: filter %-6s level %.0f (saw %.0f), highs %.4f (saw %.4f)\n", ft[t], lv, saw_lvl, h, saw_hf);
            std::string what = std::string("Filter ") + ft[t] + (t < 3 ? " takes the highs away" : " changes the sound");
            bool ok = finite && lv > 30 && lv < 32767;
            if (t < 3) ok = ok && h < 0.3 * saw_hf;
            else ok = ok && (std::fabs(h - saw_hf) > 0.1 * saw_hf || std::fabs(lv - saw_lvl) > 0.1 * saw_lvl);
            check(ok, what.c_str());
        }
        std::printf("synth: cutoff 45 reads %s\n", get(e, y, "syn_cutoff_display").c_str());
        check(get(e, y, "syn_cutoff_display") == "448 Hz", "The cutoff reads in Hz");
        // ROUTING: OSC A out of the filter: the saw again
        e->set_param(y, "syn_flt_type", "1");
        e->set_param(y, "syn_fr_a", "0");
        note_lr(57, 0.3, L, R);
        check(hf(L) > 0.8 * saw_hf, "OSC A routed OUT of the filter skips it");
        e->set_param(y, "syn_fr_a", "1"); e->set_param(y, "syn_flt_on", "0");
        // OSC MOD: FM, ring and sync change the sound (B on as a sine a fifth up)
        e->set_param(y, "sb_on", "1"); e->set_param(y, "sb_type", "0"); e->set_param(y, "sb_table", "0"); e->set_param(y, "sb_semi", "7");
        wait_loaded("sb");
        note_lr(57, 0.3, L, R);
        const double ab_hf = hf(L);
        for (const char *k : {"syn_fm", "syn_ring"}) {
            e->set_param(y, k, "60");
            note_lr(57, 0.3, L, R);
            std::printf("synth: %s 60%%: highs %.4f (plain %.4f)\n", k, hf(L), ab_hf);
            check(std::fabs(hf(L) - ab_hf) > 0.1 * ab_hf, std::string(std::string(k) + " changes the sound").c_str());
            e->set_param(y, k, "0");
        }
        // sync with A a sine too (A's saw would hide what sync does to B), B a fourth up (a fifth puts every restart at
        // B's zero crossing: 1.5 cycles in, no jump to hear)
        e->set_param(y, "sa_table", "0");
        e->set_param(y, "sb_semi", "5");
        wait_loaded("sa");
        note_lr(57, 0.3, L, R);
        const double sines_hf = hf(L);
        e->set_param(y, "syn_sync", "1");
        note_lr(57, 0.3, L, R);
        std::printf("synth: sync: highs %.5f (two sines %.5f)\n", hf(L), sines_hf);
        check(hf(L) > 1.5 * sines_hf, "SYNC B TO A changes the sound (B restarts with A: its jumps add highs)");
        e->set_param(y, "syn_sync", "0"); e->set_param(y, "sb_on", "0");
        // 2.3.2: ENV / LFO. A sine on OSC A
        e->set_param(y, "sa_type", "0"); e->set_param(y, "sa_table", "0"); e->set_param(y, "sa_on", "1");
        e->set_param(y, "sb_on", "0"); e->set_param(y, "syn_flt_on", "0");
        wait_loaded("sa");
        // LFO 1 on Pitch: the pitch moves (crossings per 50 ms vary), at 6 Hz
        e->set_param(y, "syn_lfo1_shape", "0"); e->set_param(y, "syn_lfo1_rate", "56"); e->set_param(y, "syn_lfo1_depth", "20");
        e->set_param(y, "syn_lfo1_dest", "1");
        note_lr(69, 1.0, L, R);
        int cmin = 1 << 30, cmax = 0;
        for (double t0 = 0.1; t0 < 0.9; t0 += 0.05) { int c = crossings(L, t0, t0 + 0.05); cmin = std::min(cmin, c); cmax = std::max(cmax, c); }
        std::printf("synth: LFO on pitch, %s: crossings per 50 ms %d..%d\n", get(e, y, "syn_lfo1_rate_display").c_str(), cmin, cmax);
        check(cmax - cmin >= 8, "LFO 1 on PITCH: vibrato");
        e->set_param(y, "syn_lfo1_sync", "5");   // 1/4
        check(get(e, y, "syn_lfo1_rate_display") == "1/4", "A synced LFO's rate reads as its division");
        e->set_param(y, "syn_lfo1_sync", "0"); e->set_param(y, "syn_lfo1_dest", "0");
        // MOD ENV on Cutoff: a saw through LP 24, the envelope opens it then closes it
        e->set_param(y, "sa_table", "2"); wait_loaded("sa");
        e->set_param(y, "syn_flt_on", "1"); e->set_param(y, "syn_flt_type", "1"); e->set_param(y, "syn_cutoff", "30");
        e->set_param(y, "syn_me_on", "1"); e->set_param(y, "syn_me_dest", "2"); e->set_param(y, "syn_me_amt", "100");
        e->set_param(y, "syn_me_attack", "1"); e->set_param(y, "syn_me_decay", "600"); e->set_param(y, "syn_me_sustain", "0");
        note_lr(57, 1.4, L, R);
        double early = hf(std::vector<float>(L.begin(), L.begin() + int(0.3 * 44100)));
        std::vector<float> late_(L.begin() + int(1.1 * 44100) - int(0.1 * 44100), L.end());
        double latehf = hf(late_);
        std::printf("synth: mod env on cutoff: highs early %.5f, late %.5f\n", early, latehf);
        check(early > 3 * latehf, "MOD ENV on CUTOFF: opens, then closes");
        e->set_param(y, "syn_me_on", "0"); e->set_param(y, "syn_flt_on", "0");
        // a MACRO on Position A: the table moves (Basic Shapes: sine at 0, square at 100)
        e->set_param(y, "sa_type", "1"); e->set_param(y, "sa_table", "0"); e->set_param(y, "sa_pos", "0");
        wait_loaded("sa");
        note_lr(57, 0.3, L, R);
        const double pos0 = hf(L);
        e->set_param(y, "syn_macro1_dest", "4"); e->set_param(y, "syn_macro1", "100");
        note_lr(57, 0.3, L, R);
        std::printf("synth: macro on position: highs %.5f (at 0: %.5f)\n", hf(L), pos0);
        check(hf(L) > 5 * pos0, "MACRO 1 on POSITION A: moves the table");
        e->set_param(y, "syn_macro1", "0"); e->set_param(y, "syn_macro1_dest", "0");
        // the envelope screens follow the knobs
        e->set_param(y, "syn_sustain", "20");
        for (int i = 0; i < 30; i++) { e->render(y, b2.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        int s20 = std::atoi(get(e, y, "syn_ae_b14").c_str());
        e->set_param(y, "syn_sustain", "90");
        for (int i = 0; i < 30; i++) { e->render(y, b2.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        int s90 = std::atoi(get(e, y, "syn_ae_b14").c_str());
        std::printf("synth: amp env screen, a sustain bar: %d at 20%%, %d at 90%%\n", s20, s90);
        check(s90 > s20 + 50, "The AMP ENV screen follows SUSTAIN");
        e->set_param(y, "syn_sustain", "100");
        // 2.3.3: MATRIX and VOICE. A sine on OSC A, no filter
        e->set_param(y, "sa_type", "0"); e->set_param(y, "sa_table", "0"); e->set_param(y, "sa_pos", "0");
        e->set_param(y, "syn_flt_on", "0"); e->set_param(y, "syn_lfo1_dest", "0"); e->set_param(y, "syn_lfo2_dest", "0");
        wait_loaded("sa");
        // route 1: mod wheel to pitch (+12 st at full): A4 with the wheel up is A5
        e->set_param(y, "syn_mx1_src", "8"); e->set_param(y, "syn_mx1_dst", "1"); e->set_param(y, "syn_mx1_amt", "100");
        { uint8_t cc[3] = {0xB0, 1, 127}; e->midi(y, cc, 3); }
        note_lr(69, 0.5, L, R);
        zc = crossings(L, 0.1, 0.5);
        std::printf("synth: matrix wheel -> pitch: %.0f Hz\n", zc / 0.8);
        check(std::abs(zc / 0.8 - 880) < 10, "MATRIX: MOD WHEEL to PITCH (+12 st at full)");
        { uint8_t cc[3] = {0xB0, 1, 0}; e->midi(y, cc, 3); }
        e->set_param(y, "syn_mx1_src", "0");
        // a macro on two routes (pitch and level): one knob, two destinations
        e->set_param(y, "syn_mx2_src", "10"); e->set_param(y, "syn_mx2_dst", "1"); e->set_param(y, "syn_mx2_amt", "50");
        e->set_param(y, "syn_mx3_src", "10"); e->set_param(y, "syn_mx3_dst", "8"); e->set_param(y, "syn_mx3_amt", "-50");
        note_lr(69, 0.4, L, R);
        const double lvl0 = rms(L, 0.1, 0.4);
        e->set_param(y, "syn_macro1", "100");
        note_lr(69, 0.5, L, R);
        zc = crossings(L, 0.1, 0.5);
        std::printf("synth: macro 1 on two routes: %.0f Hz, level %.0f (macro at 0: %.0f)\n", zc / 0.8, rms(L, 0.1, 0.4), lvl0);
        check(std::abs(zc / 0.8 - 622) < 10 && rms(L, 0.1, 0.4) < 0.7 * lvl0, "MATRIX: one MACRO moves two destinations (pitch and level)");
        e->set_param(y, "syn_macro1", "0"); e->set_param(y, "syn_mx2_src", "0"); e->set_param(y, "syn_mx3_src", "0");
        // 2.3.3: the steppers: < > step a route's source, wrapping round
        e->set_param(y, "syn_mx4_src", "0");
        e->set_param(y, "syn_mx4_src_next", "1"); e->set_param(y, "syn_mx4_src_next", "0");
        e->set_param(y, "syn_mx4_src_next", "1"); e->set_param(y, "syn_mx4_src_next", "0");
        const bool two = get(e, y, "state").find("syn_mx4_src=2\n") != std::string::npos;
        e->set_param(y, "syn_mx4_src", "0");
        e->set_param(y, "syn_mx4_src_prev", "1"); e->set_param(y, "syn_mx4_src_prev", "0");
        const bool wrapped = get(e, y, "state").find("syn_mx4_src=18\n") != std::string::npos;
        check(two && wrapped, "A stepper's arrows step its setting (and wrap round)");
        e->set_param(y, "syn_mx4_src", "0");
        // KEY RANGE: only C4..C5 plays; TRANSPOSE +12
        e->set_param(y, "syn_key_lo", "60"); e->set_param(y, "syn_key_hi", "72");
        note_lr(48, 0.3, L, R);
        check(rms(L, 0.05, 0.3) < 1, "KEY RANGE: a key below it is silent");
        note_lr(69, 0.3, L, R);
        check(rms(L, 0.05, 0.3) > 500, "KEY RANGE: a key inside it plays");
        check(get(e, y, "syn_key_lo_display") == note_name_test(60), "The key range reads as note names");
        e->set_param(y, "syn_key_lo", "0"); e->set_param(y, "syn_key_hi", "127");
        e->set_param(y, "syn_transpose", "12");
        note_lr(57, 0.5, L, R);
        zc = crossings(L, 0.1, 0.5);
        check(std::abs(zc / 0.8 - 440) < 6, "TRANSPOSE +12: A3 plays A4");
        e->set_param(y, "syn_transpose", "0");
        // MONO: two keys, one voice (the newest)
        e->set_param(y, "syn_vmode", "1");
        {
            uint8_t on1[3] = {0x90, 57, 100}, on2[3] = {0x90, 69, 100}, off1[3] = {0x80, 57, 0}, off2[3] = {0x80, 69, 0};
            e->midi(y, on1, 3); for (int k = 0; k < 20; k++) e->render(y, b2.data(), 128);
            e->midi(y, on2, 3);
            L.clear();
            for (int k = 0; k < int(0.4 * 44100 / 128); k++) { e->render(y, b2.data(), 128); for (int i = 0; i < 128; i++) L.push_back(b2[size_t(2 * i)]); }
            zc = crossings(L, 0.05, 0.35);
            std::printf("synth: mono, A3 then A4 held: %.0f Hz\n", zc / 0.6);
            check(std::abs(zc / 0.6 - 440) < 8, "MONO: the newest key plays alone");
            e->midi(y, off2, 3); e->midi(y, off1, 3);
            for (int k = 0; k < 200; k++) e->render(y, b2.data(), 128);
        }
        // LEGATO with GLIDE: from A3 to A4 over 300 ms, the pitch rises (fewer crossings early than late)
        e->set_param(y, "syn_vmode", "2"); e->set_param(y, "syn_glide", "300");
        {
            uint8_t on1[3] = {0x90, 57, 100}, on2[3] = {0x90, 69, 100}, off1[3] = {0x80, 57, 0}, off2[3] = {0x80, 69, 0};
            e->midi(y, on1, 3); for (int k = 0; k < 40; k++) e->render(y, b2.data(), 128);
            e->midi(y, on2, 3);
            L.clear();
            for (int k = 0; k < int(0.6 * 44100 / 128); k++) { e->render(y, b2.data(), 128); for (int i = 0; i < 128; i++) L.push_back(b2[size_t(2 * i)]); }
            const int c0 = crossings(L, 0.0, 0.1), c1 = crossings(L, 0.45, 0.55);
            std::printf("synth: legato glide: %.0f Hz in its first 100 ms, %.0f Hz after it\n", c0 / 0.2, c1 / 0.2);
            check(c0 / 0.2 < 360 && std::abs(c1 / 0.2 - 440) < 12, "LEGATO + GLIDE: slides up to the new note");
            e->midi(y, off2, 3); e->midi(y, off1, 3);
            for (int k = 0; k < 200; k++) e->render(y, b2.data(), 128);
        }
        e->set_param(y, "syn_vmode", "0"); e->set_param(y, "syn_glide", "0");
        // 2.3.4: SAMPLE oscillators. Slot A gets the test WAV (a 441 Hz tone, plays at its pitch on C4), muted so only
        // the synth sounds; OSC A plays it as SAMPLE / Slot A
        {
            std::string st2 = "omni-sampler 1\npath=" + wav + "\npreset=0\nsyn_on=1\nslot1_mute=1\n";
            e->set_param(y, "state", st2.c_str());
            for (int i = 0; i < 5000 && get(e, y, "prog_info").find("zones") == std::string::npos; i++) {
                e->render(y, b2.data(), 128);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            e->set_param(y, "sa_on", "1"); e->set_param(y, "sb_on", "0"); e->set_param(y, "syn_flt_on", "0");
            e->set_param(y, "sa_type", "2"); e->set_param(y, "sa_table", "0"); e->set_param(y, "sa_pos", "0");
            nm = wait_loaded("sa");
            info = get(e, y, "sa_info");
            std::printf("synth: SAMPLE %s | %s\n", nm.c_str(), info.c_str());
            check(nm.rfind("Slot A: ", 0) == 0 && info.find("SLOT A") != std::string::npos, "SAMPLE lists Slot A (its instrument)");
            note_lr(60, 0.15, L, R);
            zc = crossings(L, 0.02, 0.15);
            std::printf("synth: SAMPLE slot A at C4: %.0f Hz, level %.0f\n", zc / 0.26, rms(L, 0.02, 0.15));
            check(std::abs(zc / 0.26 - 441) < 12 && rms(L, 0.02, 0.15) > 300, "SAMPLE / Slot A plays the slot's sample at its pitch");
            note_lr(72, 0.08, L, R);
            zc = crossings(L, 0.01, 0.08);
            std::printf("synth: SAMPLE slot A at C5: %.0f Hz\n", zc / 0.14);
            check(std::abs(zc / 0.14 - 882) < 30, "SAMPLE / Slot A follows the keys (an octave up: double)");
            // a file from the Oscillators folders as a sample
            e->set_param(y, "sa_table", "4");   // the first file after the four slots
            nm = wait_loaded("sa");
            info = get(e, y, "sa_info");
            std::printf("synth: SAMPLE file %s | %s\n", nm.c_str(), info.c_str());
            note_lr(84, 0.1, L, R);
            check(info.find("ROOT") != std::string::npos && rms(L, 0.0, 0.05) > 100, "SAMPLE plays a file from the Oscillators folders");
            e->set_param(y, "sa_type", "0"); e->set_param(y, "sa_table", "0");
            wait_loaded("sa");
        }
        // 2.3.5: MACRO engines: each one sounds, pitched (or, for String / Modal, rings) and stays finite
        e->set_param(y, "sa_type", "3"); e->set_param(y, "sa_pos", "50"); e->set_param(y, "sa_warp", "50"); e->set_param(y, "sa_spread", "50");
        {
            const char *eng[] = {"Virtual Analog", "Waveshaper", "FM 2-Op", "Formant", "Additive", "String", "Modal", "Chord", "Noise"};
            for (int k = 0; k < 9; k++) {
                e->set_param(y, "sa_table", std::to_string(k).c_str());
                nm = wait_loaded("sa");
                note_lr(57, 0.4, L, R);
                bool finite = true; double pk = 0;
                for (float x : L) { finite = finite && std::isfinite(x); pk = std::max(pk, double(std::fabs(x))); }
                std::printf("synth: MACRO %-15s level %.0f, peak %.0f  (%s)\n", eng[k], rms(L, 0.02, 0.4), pk, nm.c_str());
                std::string what = std::string("MACRO ") + eng[k] + " sounds, in range";
                check(finite && nm == std::string("Macro / ") + eng[k] && rms(L, 0.02, 0.4) > 150 && pk < 32767, what.c_str());
            }
            check(get(e, y, "sa_info").find("HARMONICS") != std::string::npos, "MACRO's info line names its three controls");
        }
        e->set_param(y, "sa_type", "0"); e->set_param(y, "sa_table", "0"); e->set_param(y, "sa_pos", "0"); e->set_param(y, "sa_warp", "0");
        wait_loaded("sa");
        // 2.3.5: the factory presets: < > steps and loads each one; every one sounds; a step resets what it doesn't set
        {
            e->set_param(y, "syn_res", "90");   // not set by the first preset after Init: the step resets it
            std::string first;
            for (int k = 0; k < 16; k++) {
                e->set_param(y, "syn_preset_next", "1"); e->set_param(y, "syn_preset_next", "0");
                std::string nm2 = get(e, y, "syn_preset_display");
                if (!k) first = nm2;
                wait_loaded("sa"); wait_loaded("sb");
                note_lr(57, 0.8, L, R);
                bool finite = true; double pk = 0;
                for (float x : L) { finite = finite && std::isfinite(x); pk = std::max(pk, double(std::fabs(x))); }
                std::printf("synth: preset %-15s level %.0f, peak %.0f\n", nm2.c_str(), rms(L, 0.0, 0.8), pk);
                std::string what = "preset " + nm2 + " sounds, in range";
                check(finite && rms(L, 0.0, 0.8) > 60 && pk < 32767, what.c_str());
                if (!k) check(get(e, y, "state").find("syn_res=90") == std::string::npos, "a preset resets the synth settings it doesn't set");
                {   // 2.4.2: a preset's LFOs are on the ROUTE keys (the hidden older destination showed no key lit)
                    const std::string st2 = get(e, y, "state");
                    std::string w2 = "preset " + nm2 + " routes its LFOs with the keys";
                    check(stv(st2, "syn_lfo1_dest") == "0" && stv(st2, "syn_lfo2_dest") == "0", w2.c_str());
                }
            }
            check(first == "Supersaw Lead" && get(e, y, "syn_preset_display") == "Init", "< > step the presets, wrapping round");
            check(get(e, y, "state").find("syn_on=1") != std::string::npos, "a preset turns the synth on");
        }
        // 2.3.6: LFO ROUTE keys: AMP is a tremolo (the level swings), FILTER moves the cutoff; RING follows the LEVELs
        {
            auto swing = [&](const std::vector<float> &x) {   // the spread of 10 ms block levels after the attack
                double lo = 1e9, hi = 0;
                for (size_t b0 = 4410; b0 + 441 <= x.size() && b0 < 4410 * 8; b0 += 441) {
                    double q = 0; for (size_t i = b0; i < b0 + 441; i++) q += double(x[i]) * x[i];
                    q = std::sqrt(q / 441); lo = std::min(lo, q); hi = std::max(hi, q);
                }
                return hi > 0 ? lo / hi : 1.0;
            };
            e->set_param(y, "syn_lfo1_rate", "70"); e->set_param(y, "syn_lfo1_depth", "100"); e->set_param(y, "syn_lfo1_shape", "0");
            note_lr(57, 0.9, L, R);
            double none = swing(L);
            e->set_param(y, "syn_lfo1_route", "1");
            note_lr(57, 0.9, L, R);
            double amp = swing(L);
            std::printf("synth: LFO route off: level min/max %.2f, AMP: %.2f\n", none, amp);
            check(none > 0.8 && amp < 0.5, "LFO 1 route AMP: a tremolo");
            e->set_param(y, "syn_lfo1_route", "0"); e->set_param(y, "syn_lfo1_depth", "50"); e->set_param(y, "syn_lfo1_rate", "50");
            e->set_param(y, "sb_on", "1"); e->set_param(y, "sa_level", "0"); e->set_param(y, "sb_level", "0"); e->set_param(y, "syn_ring", "100");
            note_lr(57, 0.3, L, R);
            double rz = rms(L, 0.0, 0.3);
            e->set_param(y, "sa_level", "80"); e->set_param(y, "sb_level", "80");
            note_lr(57, 0.3, L, R);
            std::printf("synth: RING 100 with levels 0: %.1f, at 80: %.0f\n", rz, rms(L, 0.0, 0.3));
            check(rz < 1 && rms(L, 0.0, 0.3) > 100, "RING follows the oscillators' LEVELs");
            e->set_param(y, "syn_ring", "0"); e->set_param(y, "sb_on", "0");
        }
        {   // LFO ROUTE keys: Toggles (syn_lfo<n>_rk_amp/flt/mod); a tap on the lit key turns the route Off
            auto drain = [&]() {   // what the plugin reports (the wrapper's audioMasterAutomate)
                std::string out; char k[64];
                for (int i = 0; i < 4000 && mpc_engine_changed(y, k, sizeof k) > 0; i++) { out += k; out += ' '; }
                return out;
            };
            auto lit = [&](int n) {   // the keys of LFO n as shown: "000" .. "001"
                std::string s;
                for (const char *rk : {"amp", "flt", "mod"}) s += get(e, y, ("syn_lfo" + std::to_string(n) + "_rk_" + rk).c_str());
                return s;
            };
            auto route = [&](int n) { return std::atoi(get(e, y, ("syn_lfo" + std::to_string(n) + "_route").c_str()).c_str()); };
            // a project with LFO 1 on MOD: its MOD key lit, reported
            std::string st = get(e, y, "state"), st3 = st;
            size_t at = st3.find("syn_lfo1_route=");
            if (at != std::string::npos) st3.erase(at, st3.find('\n', at) - at + 1);
            st3 += "\nsyn_lfo1_route=3\n";
            e->set_param(y, "state", st3.c_str());
            std::string rep = drain();
            check(route(1) == 3 && lit(1) == "001" && lit(2) == "000", "a project with LFO 1 route MOD loads with MOD lit");
            check(rep.find("syn_lfo1_rk_mod") != std::string::npos && rep.find("syn_lfo1_rk_amp") != std::string::npos,
                  "the route keys are reported when the route comes from a project");
            // MPC restores the keys it saved (and a stale value): not a tap
            e->set_param(y, "syn_lfo1_rk_mod", "1"); e->set_param(y, "syn_lfo1_rk_amp", "1");
            check(route(1) == 3 && lit(1) == "001", "a key restored with the project is not a tap");
            drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2100));
            // tap AMP: route Amp, AMP lit; the report's echo is no tap
            e->set_param(y, "syn_lfo1_rk_amp", "1");
            rep = drain();
            check(route(1) == 1 && lit(1) == "100", "tap AMP: route Amp, AMP lit");
            check(rep.find("syn_lfo1_route") != std::string::npos && rep.find("syn_lfo1_rk_amp") != std::string::npos &&
                  rep.find("syn_lfo1_rk_mod") != std::string::npos && rep.find("syn_lfo2_rk") == std::string::npos,
                  "a tap reports the route and that LFO's three keys");
            for (const char *rk : {"amp", "flt", "mod"})   // MPC echoes each report back with the value reported
                e->set_param(y, ("syn_lfo1_rk_" + std::string(rk)).c_str(), get(e, y, ("syn_lfo1_rk_" + std::string(rk)).c_str()).c_str());
            check(route(1) == 1 && lit(1) == "100" && drain().empty(), "the echo of a reported key is ignored");
            // tap AMP again: route Off, nothing lit
            e->set_param(y, "syn_lfo1_rk_amp", "0");
            drain();
            check(route(1) == 0 && lit(1) == "000", "tap the lit AMP: route Off, no key lit");
            e->set_param(y, "syn_lfo1_rk_amp", "0");
            check(route(1) == 0, "its echo leaves it Off");
            {   // 2.4.2: a project's older (hidden) destination doesn't outlive a tap Off
                e->set_param(y, "syn_lfo1_dest", "4"); e->set_param(y, "syn_lfo1_route", "1");
                drain();
                e->set_param(y, "syn_lfo1_rk_amp", "0");
                check(route(1) == 0 && stv(get(e, y, "state"), "syn_lfo1_dest") == "0", "tap Off: the LFO is unrouted (the older destination cleared)");
                drain();
            }
            // tap FILTER, then MOD: each switches to its own
            e->set_param(y, "syn_lfo1_rk_flt", "1");
            check(route(1) == 2 && lit(1) == "010", "tap FILTER: route Filter");
            e->set_param(y, "syn_lfo1_rk_mod", "1");
            check(route(1) == 3 && lit(1) == "001", "tap MOD with FILTER lit: route Mod");
            // LFO 2 is its own; a Q-Link / host set of the route moves the keys too
            e->set_param(y, "syn_lfo2_rk_flt", "1");
            check(route(2) == 2 && lit(2) == "010" && route(1) == 3, "LFO 2's keys route LFO 2 only");
            drain();
            e->set_param(y, "syn_lfo2_route", "1");
            rep = drain();
            check(lit(2) == "100" && rep.find("syn_lfo2_rk_amp") != std::string::npos && rep.find("syn_lfo2_rk_flt") != std::string::npos,
                  "a route set by the host lights its key, reported");
            check(get(e, y, "state").find("_rk_") == std::string::npos, "the keys are not in the plugin state");
            e->set_param(y, "state", st.c_str());   // as before
            drain();
        }
        // RESET: the synth off again
        e->set_param(y, "reset_all", "1"); e->set_param(y, "reset_all", "0");
        e->set_param(y, "reset_all", "1"); e->set_param(y, "reset_all", "0");
        check(stv(get(e, y, "state"), "syn_on") == "0", "RESET turns the synth off");
        e->destroy(y);
    }

    {   // 2.3.1: IDLE SKIP: with nothing playing the FX stop once their tails are gone; a note plays at once after
        void *c = e->create("/tmp/omni-sampler-test");
        e->set_param(c, "state", state.c_str());
        for (int i = 0; i < 5000 && get(e, c, "prog_info").find("zones") == std::string::npos; i++) {
            e->render(c, buf.data(), 128);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        e->set_param(c, "rev_type", "3"); e->set_param(c, "rev_mix", "60"); e->set_param(c, "rev_size", "90");   // a long hall
        uint8_t on[3] = {0x90, uint8_t(note), 100}, off[3] = {0x80, uint8_t(note), 0};
        e->midi(c, on, 3);
        for (int k = 0; k < 30; k++) e->render(c, buf.data(), 128);
        e->midi(c, off, 3);
        // the tail rings on (no skip while it is audible), then it goes quiet and the skip starts
        int last_sound = -1, k = 0;
        for (; k < 44100 * 20 / 128; k++) {
            e->render(c, buf.data(), 128);
            for (int i = 0; i < 256; i++) if (buf[size_t(i)] != 0) last_sound = k;
        }
        std::printf("idle skip: the hall's tail sounded for %.1f s after the note\n", last_sound * 128 / 44100.0);
        check(last_sound * 128 / 44100.0 > 2.0, "The reverb's tail rings out (no early cut)");
        e->midi(c, on, 3);
        e->render(c, buf.data(), 128);
        int nz = 0;
        for (int i = 0; i < 256; i++) nz += buf[size_t(i)] != 0;
        check(nz > 50, "After the idle skip a note sounds at once");
        e->midi(c, off, 3);
        e->destroy(c);
    }

    {   // 2.1.16: RESET (two taps) and what the engine reports changed by itself (mpc_engine_changed)
        void *r = e->create("/tmp/omni-sampler-test");
        e->set_param(r, "state", state.c_str());
        for (int i = 0; i < 5000 && get(e, r, "prog_info").find("zones") == std::string::npos; i++) {
            e->render(r, buf.data(), 128);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        char key[64];
        while (mpc_engine_changed(r, key, sizeof key) > 0) {}   // the load's tiles
        e->set_param(r, "slot1_lo", "72");   // above the WAV's keys: their tiles go dark
        e->set_param(r, "rev_mix", "70");
        e->set_param(r, "slot1_play", "2");
        e->set_param(r, "reset_all", "1"); e->set_param(r, "reset_all", "0");
        check(get(e, r, "reset_info").find("again") != std::string::npos,
              "RESET: the first tap asks for a second");
        check(get(e, r, "state").find("rev_mix=70") != std::string::npos, "RESET: one tap changes nothing");
        // the key-map tiles moved by slot1_lo (set by the host, as a LAYERS knob) are reported
        for (int i = 0; i < 40; i++) { e->render(r, buf.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        std::string seen;
        while (mpc_engine_changed(r, key, sizeof key) > 0) seen += std::string(key) + " ";
        std::printf("reported: %s\n", seen.c_str());
        check(seen.find("ltile_") != std::string::npos, "LAYERS: a changed key range reports its tiles (no page change needed)");
        e->set_param(r, "reset_all", "1"); e->set_param(r, "reset_all", "0");
        for (int i = 0; i < 2000 && get(e, r, "prog_info").find("zones") != std::string::npos; i++) {
            e->render(r, buf.data(), 128);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::string st = get(e, r, "state");
        std::printf("reset: info \"%s\", prog \"%s\"\n", get(e, r, "reset_info").c_str(), get(e, r, "prog_info").c_str());
        check(st.find("slot1_path") == std::string::npos && get(e, r, "prog_info").find("zones") == std::string::npos,
              "RESET: the slots are empty");
        check(stv(st, "rev_mix") == "0" && stv(st, "slot1_lo") == "0"
              && stv(st, "slot1_play") == "0", "RESET: the settings are back at their defaults");
        seen.clear();
        while (mpc_engine_changed(r, key, sizeof key) > 0) seen += std::string(key) + " ";
        check(seen.find("slot1_lo ") != std::string::npos && seen.find("rev_mix ") != std::string::npos,
              "RESET: the changed parameters are reported so the screen redraws");
        e->destroy(r);
    }

    syt::run_all();   // 2.4.2
    e->destroy(inst);
    std::remove(wav.c_str());
    if (fails) { std::printf("FAIL (%d)\n", fails); return 1; }
    std::printf("PASSED\n");
    return 0;
}

// Omni Sampler patch test: save a multi-slot patch, reopen it in a fresh instance, check slots + settings.
// Also: project reload keeps the project's settings over an .omni's; mono fallback leaves no pad lit.
//   test_patch <slot A file> <slot B: a preset inside a disk image> <slot C file>   (data in /tmp/omni-patch-test)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}
static const mpc_engine_t *e;
static std::vector<int16_t> buf(256);
static int fails = 0;
static std::string get(void *in, const char *k) { char b[8192] = {}; e->get_param(in, k, b, sizeof b); return b; }
static void run(void *in, int blocks) { for (int i = 0; i < blocks; i++) e->render(in, buf.data(), 128); }
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
// render until the loader is idle and status is not a transient one
static void settle(void *in, int min_ms = 300) {
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        run(in, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::string s = get(in, "status");
        bool transient = s.compare(0, 7, "Loading") == 0 || s.compare(0, 7, "Reading") == 0 || s.compare(0, 6, "Saving") == 0 ||
                         s.compare(0, 10, "Extracting") == 0 || get(in, "br_info") == "Opening...";
        if (ms > min_ms && !transient) return;
        if (ms > 120000) { std::printf("settle timeout (%s)\n", s.c_str()); return; }
    }
}
static int voices_for(void *in, int note) {
    uint8_t on[3] = {0x90, uint8_t(note), 100}, off[3] = {0x80, uint8_t(note), 0};
    e->midi(in, on, 3); run(in, 20);
    int v = std::atoi(get(in, "status").c_str());
    e->midi(in, off, 3); run(in, 800);
    return v;
}
static std::string path_dir_of(const char *p) { std::string s = p; return s.substr(0, s.rfind('/')); }
static bool tap_named(void *in, const std::string &dir, const std::string &label) {
    e->set_param(in, "br_path", dir.c_str());
    for (int page = 0; page < 20; page++) {
        for (int r = 1; r <= 10; r++) {
            std::string k = "br_" + std::to_string(r);
            if (get(in, k.c_str()) == label) { e->set_param(in, k.c_str(), "1"); settle(in); return true; }
        }
        e->set_param(in, "br_next", "1"); settle(in, 20);
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    e = mpc_engine();
    (void)!std::system("rm -rf /tmp/omni-patch-test");
    const char *data = "/tmp/omni-patch-test";

    // 1. three instruments in A, B, C with their own ranges, a keyswitch-free layer, a tweaked filter
    void *a = e->create(data);
    std::string st = std::string("omni-sampler 1\nslot1_path=") + argv[1] + "\nslot1_preset=0\nslot2_path=" + argv[2] +
                     "\nslot2_preset=0\nslot3_path=" + argv[3] + "\nslot3_preset=0\n";
    e->set_param(a, "state", st.c_str());
    settle(a, 1500);
    std::string nA = get(a, "slot1_name"), nB = get(a, "slot2_name"), nC = get(a, "slot3_name");
    std::printf("loaded: A=%s B=%s C=%s | %s\n", nA.c_str(), nB.c_str(), nC.c_str(), get(a, "status").c_str());
    e->set_param(a, "slot1_lo", "0");  e->set_param(a, "slot1_hi", "59");
    e->set_param(a, "slot2_lo", "60"); e->set_param(a, "slot2_hi", "127");
    e->set_param(a, "slot3_lo", "48"); e->set_param(a, "slot3_hi", "72"); e->set_param(a, "slot3_vol", "-6"); e->set_param(a, "slot3_tune", "12");
    e->set_param(a, "cutoff", "63");
    e->set_param(a, "mod2_amt", "40");
    int v50 = voices_for(a, 50), v66 = voices_for(a, 66);
    std::printf("before save: note 50 -> %d voices, note 66 -> %d voices\n", v50, v66);
    e->set_param(a, "patch_save", "1"); e->set_param(a, "patch_save", "0");
    settle(a, 500);
    std::string saved = get(a, "status");
    check(saved.compare(0, 11, "Saved patch") == 0, "save: " + saved);
    check(get(a, "br_info") == saved, "browse info shows it: " + get(a, "br_info"));
    std::string pdir = std::string(data) + "/library/Patches";
    (void)!std::system(("ls -R '" + pdir + "' | head -20; cat '" + pdir + "'/*.omnipatch | head -c 900; echo").c_str());
    std::string state_a = get(a, "state");
    e->destroy(a);

    // 2. a fresh instance with nothing loaded opens the patch from the browser
    void *b = e->create(data);
    std::string pname = saved.substr(saved.rfind('/') + 1);
    check(tap_named(b, pdir, pname), "patch listed in the browser: " + pname);
    settle(b, 1500);
    std::printf("opened: %s | A=%s B=%s C=%s D=%s\n", get(b, "br_info").c_str(), get(b, "slot1_name").c_str(), get(b, "slot2_name").c_str(),
                get(b, "slot3_name").c_str(), get(b, "slot4_name").c_str());
    check(get(b, "slot1_name") == nA && get(b, "slot2_name") == nB && get(b, "slot3_name") == nC && get(b, "slot4_name") == "(empty)", "slot instruments");
    check(get(b, "slot1_hi") == "59" && get(b, "slot2_lo") == "60" && get(b, "slot3_vol") == "-6" && get(b, "slot3_tune") == "12", "slot settings");
    check(get(b, "cutoff") == "63" && get(b, "mod2_amt") == "40", "sound settings (cutoff " + get(b, "cutoff") + ")");
    int w50 = voices_for(b, 50), w66 = voices_for(b, 66);
    check(w50 == v50 && w66 == v66, "plays the same: note 50 " + std::to_string(w50) + ", note 66 " + std::to_string(w66));
    // the slots now refer to the patch's .omni files: a project needs no disk image
    std::string st_b = get(b, "state");
    check(st_b.find("slot2_path=" + pdir) != std::string::npos || st_b.find("slot2_path=" + std::string(data) + "/library/Extracted") != std::string::npos,
          "slot B saved as an extracted .omni");
    e->destroy(b);

    // 3. project reload: the project's cutoff wins over the one an .omni carries
    void *c = e->create(data);
    e->set_param(c, "state", (std::string("omni-sampler 1\nslot1_path=") + argv[3] + "\nslot1_preset=0\n").c_str());
    settle(c, 800);
    e->set_param(c, "cutoff", "40");
    e->set_param(c, "br_extract", "1"); e->set_param(c, "br_extract", "0");   // SAVE TO LIBRARY: .omni with cutoff 40
    settle(c, 500);
    std::printf("save to library: %s\n", get(c, "status").c_str());
    e->set_param(c, "cutoff", "77");                                           // tweaked after saving
    std::string proj = get(c, "state");
    e->destroy(c);
    void *d = e->create(data);
    e->set_param(d, "state", proj.c_str());
    settle(d, 1000);
    check(get(d, "cutoff") == "77", "project reload keeps its cutoff (got " + get(d, "cutoff") + ", want 77)");
    // but tapping that .omni in the browser still brings its stored sound
    e->set_param(d, "cutoff", "90");
    std::string f = proj.substr(proj.find("slot1_path=") + 11); f = f.substr(0, f.find('\n'));
    check(tap_named(d, f.substr(0, f.rfind('/')), f.substr(f.rfind('/') + 1)), "tap the saved .omni");
    settle(d, 800);
    check(get(d, "cutoff") == "40", "browsing an .omni applies its settings (got " + get(d, "cutoff") + ")");

    // 4. mono mode: the fallback note leaves no key counted as down
    e->set_param(d, "voice_mode", "1");
    uint8_t on60[3] = {0x90, 60, 100}, on64[3] = {0x90, 64, 100}, off60[3] = {0x80, 60, 0}, off64[3] = {0x80, 64, 0};
    e->midi(d, on60, 3); run(d, 5); e->midi(d, on64, 3); run(d, 5); e->midi(d, off64, 3); run(d, 5); e->midi(d, off60, 3); run(d, 50);
    e->set_param(d, "pad_base", "52");
    std::string lit;
    for (int p = 1; p <= 16; p++) lit += get(d, ("pad_" + std::to_string(p)).c_str());
    check(lit.find('1') == std::string::npos, "mono fallback leaves no pad lit: " + lit);
    e->destroy(d);

    // 5. hanging notes: a note-off on another channel than its note-on; PANIC silences everything and clears held keys
    {
        void *h = e->create(data);
        e->set_param(h, "state", (std::string("omni-sampler 1\nslot1_path=") + argv[3] + "\nslot1_preset=0\n").c_str());
        settle(h, 800);
        auto v = [&] { return std::atoi(get(h, "status").c_str()); };
        auto msg = [&](uint8_t a, uint8_t b, uint8_t c) { uint8_t m[3] = {a, b, c}; e->midi(h, m, 3); };
        msg(0x90, 62, 100); run(h, 20); msg(0x81, 62, 0); run(h, 800);
        check(v() == 0, "note-on channel 1, note-off channel 2: released (" + std::to_string(v()) + " voices)");
        msg(0x90, 60, 100); run(h, 20);
        int per = v();                                         // voices one note starts (layered zones)
        msg(0x91, 60, 100); run(h, 20); msg(0x81, 60, 0); run(h, 800);
        check(per > 0 && v() == per, "the same note held on two channels: one note-off leaves the other sounding (" +
              std::to_string(v()) + " of " + std::to_string(per) + ")");
        msg(0x80, 60, 0); run(h, 800);
        check(v() == 0, "and its own note-off ends it");
        msg(0xB0, 64, 127);                                    // pedal down, keys held, no note-offs at all
        for (int n : {48, 52, 55, 60, 64}) msg(0x90, uint8_t(n), 100);
        e->set_param(h, "pad_base", "48");
        e->set_param(h, "pad_1", "1");
        run(h, 20);
        int before = v();
        e->set_param(h, "panic", "1"); e->set_param(h, "panic", "0");
        run(h, 4);                                             // ~12 ms
        std::string lit;
        for (int p = 1; p <= 16; p++) lit += get(h, ("pad_" + std::to_string(p)).c_str());
        check(before >= 5 && v() == 0 && lit.find('1') == std::string::npos,
              "PANIC: " + std::to_string(before) + " held voices silenced within 12 ms, no pad lit (" + lit + ")");
        msg(0x90, 67, 100); run(h, 20);
        int after = v();
        msg(0x80, 67, 0); run(h, 800);
        check(after >= 1 && v() == 0, "after PANIC (pedal cleared) a new note plays and releases normally");
        e->destroy(h);
    }

    // 6. (with a 4th argument: a folder of more than 10 instruments, e.g. MV-8000 .MV0 patches) the browser list scrolls by
    // rows: the scroll fader by a drag (top = the start), by single-row nudges (data wheel, Q-Link), the page buttons keep it
    // in step, and a row tapped after scrolling loads that row
    if (argc > 4) {
        void *b = e->create(data);
        e->set_param(b, "br_path", argv[4]);
        std::string pg = get(b, "br_page");
        int n = std::atoi(pg.substr(pg.rfind(' ') + 1).c_str()), m = n - 10;
        auto row1 = [&] { return get(b, "br_1"); };
        std::string first = row1();
        auto rng = [&](int a) { return std::to_string(a) + "-" + std::to_string(a + 9) + " of " + std::to_string(n); };
        auto wait = [&] { for (int i = 0; i < 500 && get(b, "br_info") == "Opening..."; i++) std::this_thread::sleep_for(std::chrono::milliseconds(2)); };
        check(n > 10 && pg == "1-10 of " + std::to_string(n), "a long folder: " + pg);
        // 1.7.3: the simple scroll: a row button or a Q-Link click is always exactly one row, whatever the list's length
        std::string second = get(b, "br_2");
        e->set_param(b, "br_rowdown", "1"); e->set_param(b, "br_rowdown", "0"); wait();
        check(get(b, "br_page") == rng(2) && row1() == second, "row down: one row (" + get(b, "br_page") + ")");
        e->set_param(b, "br_rowup", "1"); e->set_param(b, "br_rowup", "0"); wait();
        e->set_param(b, "br_rowup", "1"); e->set_param(b, "br_rowup", "0"); wait();
        check(get(b, "br_page") == rng(1) && row1() == first, "row up: one row, and stops at the top");
        for (int k = 0; k < 3; k++) e->set_param(b, "br_row", "2");   // three Q-Link clicks clockwise
        check(get(b, "br_page") == rng(4) && get(b, "br_row") == "1", "Q-Link: three clicks = three rows, back in the middle (" + get(b, "br_page") + ")");
        e->set_param(b, "br_row", "1");
        check(get(b, "br_page") == rng(4), "Q-Link: the middle value does nothing");
        for (int k = 0; k < 5; k++) e->set_param(b, "br_row", "0");
        check(get(b, "br_page") == rng(1), "Q-Link anticlockwise: back up, stops at the top");
        for (int k = 0; k < n + 5; k++) e->set_param(b, "br_row", "2");
        check(get(b, "br_page") == std::to_string(m + 1) + "-" + std::to_string(n) + " of " + std::to_string(n), "and stops at the last row");
        e->set_param(b, "br_prev", "1"); e->set_param(b, "br_prev", "0"); wait();
        check(get(b, "br_page") == rng(std::max(1, m + 1 - 10)), "page up still moves ten rows (" + get(b, "br_page") + ")");
        {   // 1.7.3: the SELECTED box: a long name in full over three lines; a short one in the middle line
            std::string full = get(b, "br_info"), l1 = get(b, "br_sel1"), l2 = get(b, "br_sel2"), l3 = get(b, "br_sel3");
            check(l1 == " " && l2 == full && l3 == " ", "SELECTED, short text in the middle: [" + l2 + "]");
        }
        for (int k = 0; k < n + 5; k++) e->set_param(b, "br_row", "2");
        std::string label = row1();
        e->set_param(b, "br_1", "1");
        settle(b, 1500);
        std::string stem = label.substr(0, label.rfind('.'));
        check(get(b, "prog_name").find(stem.substr(stem.find('_') + 1)) != std::string::npos || get(b, "br_info").find("instrument") != std::string::npos,
              "tapping row 1 after scrolling loads that row (" + label + " -> " + get(b, "prog_name") + ")");
        e->destroy(b);
    }
    if (argc > 5) {   // an MV-8000 drum kit (its pads from A0) lands on MPC's pad 1 (C1, note 36)
        void *k = e->create(data);
        e->set_param(k, "state", (std::string("omni-sampler 1\nslot1_path=") + argv[5] + "\nslot1_preset=0\n").c_str());
        settle(k, 1000);
        auto msg = [&](uint8_t a, uint8_t b2, uint8_t c) { uint8_t mm[3] = {a, b2, c}; e->midi(k, mm, 3); };
        msg(0x90, 36, 100); run(k, 30);
        int on36 = std::atoi(get(k, "status").c_str());
        std::string nm = get(k, "layer_name");
        msg(0x80, 36, 0); run(k, 600);
        check(on36 > 0 && get(k, "prog_format").find("MV-8000") != std::string::npos, "MV-8000 kit: pad 1 (note 36) plays " + nm);
        // 1.7.3 KEY SHIFT: slot A moved down an octave plays the same sound, at the same pitch, from C0 (note 24)
        auto hit = [&](int note) {   // peak and a checksum of the first 40 blocks of a note (fresh reverb/voices)
            msg(0xB0, 123, 0); run(k, 800);
            std::vector<int16_t> b(256); long long sum = 0; int pk = 0;
            msg(0x90, uint8_t(note), 100);
            for (int i = 0; i < 40; i++) { e->render(k, b.data(), 128); for (int16_t x : b) { sum += std::abs(int(x)); pk = std::max(pk, std::abs(int(x))); } }
            msg(0x80, uint8_t(note), 0);
            return std::make_pair(pk, sum);
        };
        e->set_param(k, "rev_mix", "0");
        auto ref = hit(36);
        e->set_param(k, "slot1_hi", "60");   // a closed range moves with the shift
        e->set_param(k, "slot1_shift", "-12");
        auto moved = hit(24), above = hit(61 - 12 + 1);
        check(ref.first > 0 && moved.first == ref.first && std::llabs(moved.second - ref.second) <= ref.second / 100,
              "key shift -12: note 24 plays what note 36 did (peak " + std::to_string(moved.first) + " vs " + std::to_string(ref.first) + ")");
        check(get(k, "slot1_lo") == "0" && std::atoi(get(k, "slot1_hi").c_str()) == 48 && above.first == 0,
              "the range moves with it: low stays open, high 60 -> " + get(k, "slot1_hi") + ", nothing above it");
        check(get(k, "slot1_shift_display") == "-12 keys", "shown as " + get(k, "slot1_shift_display"));
        e->set_param(k, "slot1_shift", "0");
        check(std::atoi(get(k, "slot1_hi").c_str()) == 60 && hit(36).first == ref.first, "back to 0: range and pad 1 restored");
        e->destroy(k);
    }

    // 7. single cycles and wavetables (1.7): files written here. A 2048-frame saw plays at the key's pitch, band-limited; a
    // 4-cycle wavetable (Serum "clm " chunk: sine, triangle, saw, square) morphs with WT POSITION, and keeps its cycles
    // through SAVE TO LIBRARY; a long folder name shows both ends in the browser
    {
        auto wav = [](const std::string &path, const std::vector<int16_t> &smp, const std::string &clm) {
            std::string body = "WAVE", fmt;
            auto u32 = [](std::string &o, uint32_t v) { for (int i = 0; i < 4; i++) o += char(v >> (8 * i) & 0xFF); };
            auto u16 = [](std::string &o, uint16_t v) { o += char(v & 0xFF); o += char(v >> 8); };
            body += "fmt "; u32(body, 16); u16(body, 1); u16(body, 1); u32(body, 44100); u32(body, 88200); u16(body, 2); u16(body, 16);
            if (!clm.empty()) { std::string c = clm; if (c.size() & 1) c += '\0'; body += "clm "; u32(body, uint32_t(clm.size())); body += c; }
            body += "data"; u32(body, uint32_t(smp.size() * 2));
            for (int16_t v : smp) u16(body, uint16_t(v));
            std::string f = "RIFF"; u32(f, uint32_t(body.size())); f += body;
            FILE *o = std::fopen(path.c_str(), "wb"); std::fwrite(f.data(), 1, f.size(), o); std::fclose(o);
        };
        const int N = 2048;
        std::vector<int16_t> saw, table;
        for (int i = 0; i < N; i++) saw.push_back(int16_t((2.0 * i / N - 1) * 20000));
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < N; i++) {
                double ph = double(i) / N;
                double v = k == 0 ? std::sin(2 * M_PI * ph) : k == 1 ? 4 * std::fabs(ph - 0.5) - 1 : k == 2 ? 2 * ph - 1 : ph < 0.5 ? 1 : -1;
                table.push_back(int16_t(v * 20000));
            }
        std::string dir = std::string(data) + "/wt-test";
        (void)!std::system(("mkdir -p '" + dir + "/A Folder With A Very Long Name That Goes On And On Past The Row - Volume 12'").c_str());
        wav(dir + "/saw.wav", saw, "");
        wav(dir + "/table.wav", table, "<!>2048 01000000 wavetable");
        // render a note, return (strongest frequency in Hz via zero crossings of a band-pass-free signal, h3/h1)
        auto play = [&](void *h, int note, double &h1, double &h3, double f) {
            std::vector<int16_t> buf(256);
            std::vector<double> x;
            uint8_t on[3] = {0x90, uint8_t(note), 100}, off[3] = {0x80, uint8_t(note), 0};
            e->midi(h, on, 3);
            for (int b = 0; b < 200; b++) { e->render(h, buf.data(), 128); if (b >= 40) for (int i = 0; i < 128; i++) x.push_back(buf[size_t(i) * 2]); }
            e->midi(h, off, 3); run(h, 200);
            auto goertzel = [&](double fr) {
                double w = 2 * M_PI * fr / 44100, c = 2 * std::cos(w), s1 = 0, s2 = 0;
                for (double v : x) { double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
                return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
            };
            h1 = goertzel(f); h3 = goertzel(3 * f);
            return goertzel(f) > 4 * goertzel(f * 1.0595) && goertzel(f) > 4 * goertzel(f / 1.0595);   // the fundamental, in tune
        };
        void *w = e->create(data);
        e->set_param(w, "state", ("omni-sampler 1\nslot1_path=" + dir + "/saw.wav\nslot1_preset=0\n").c_str());
        settle(w, 800);
        double h1, h3;
        bool tuned = play(w, 69, h1, h3, 440.0);
        check(get(w, "prog_format") == "FORMAT: SINGLE CYCLE" && tuned, "a 2048-frame saw is a single cycle, A3 plays 440 Hz (" + get(w, "prog_format") + ")");
        e->set_param(w, "state", ("omni-sampler 1\nslot1_path=" + dir + "/table.wav\nslot1_preset=0\nwt_pos=0\n").c_str());
        settle(w, 800);
        play(w, 57, h1, h3, 220.0);
        double sine = h3 / h1;
        e->set_param(w, "wt_pos", "100");
        play(w, 57, h1, h3, 220.0);
        double square = h3 / h1;
        check(get(w, "prog_format").find("WAVETABLE (4 CYCLES)") != std::string::npos && sine < 0.02 && square > 0.25 && square < 0.4,
              "wavetable: position 0 a sine (h3/h1 " + std::to_string(sine) + "), 100 a square (" + std::to_string(square) + ")");
        e->set_param(w, "br_extract", "1"); e->set_param(w, "br_extract", "0");
        settle(w, 800);
        std::string st = get(w, "state");
        std::string saved = st.substr(st.find("slot1_path=") + 11); saved = saved.substr(0, saved.find('\n'));
        e->destroy(w);
        void *w2 = e->create(data);
        e->set_param(w2, "state", st.c_str());
        settle(w2, 800);
        e->set_param(w2, "wt_pos", "100");
        play(w2, 57, h1, h3, 220.0);
        check(saved.find(".omni") != std::string::npos && h3 / h1 > 0.25, "saved to the library and reopened, still a wavetable (" + saved.substr(saved.rfind('/') + 1) + ")");
        e->set_param(w2, "br_path", dir.c_str());
        std::string rows;
        for (int r = 1; r <= 4; r++) rows += get(w2, ("br_" + std::to_string(r)).c_str()) + " | ";
        check(rows.find("...") != std::string::npos && rows.find("Volume 12]") != std::string::npos && rows.find("[A Folder With") != std::string::npos,
              "a long folder name keeps both ends: " + rows);
        for (int r = 1; r <= 4; r++)   // tapped: the SELECTED box shows its whole name, wrapped
            if (get(w2, ("br_" + std::to_string(r)).c_str()).find("[A Folder With") != std::string::npos) {
                e->set_param(w2, ("br_" + std::to_string(r)).c_str(), "1");
                settle(w2, 50);
                break;
            }
        std::string l1 = get(w2, "br_sel1"), l2 = get(w2, "br_sel2"), l3 = get(w2, "br_sel3");
        check(l1 + " " + l2 == "A Folder With A Very Long Name That Goes On And On Past The Row - Volume 12" && l3 == " " && l1.size() <= 54,
              "SELECTED shows the full folder name: [" + l1 + "] [" + l2 + "] [" + l3 + "]");
        e->destroy(w2);
    }

    // 8. (1.7.1) the PLAY envelope faders show the loaded preset's own envelope; moved, they change every zone by as much;
    // a project keeps the moved values; another preset loaded fresh shows its own again
    {
        (void)!std::system("rm -rf /tmp/omni-env-test");
        void *v = e->create("/tmp/omni-env-test");
        auto tail = [&](void *h) {   // seconds a note rings after its note-off (held 0.3 s)
            std::vector<int16_t> buf(256);
            uint8_t on[3] = {0x90, 60, 100}, off[3] = {0x80, 60, 0};
            e->midi(h, on, 3); run(h, 103); e->midi(h, off, 3);
            int b = 0;
            for (; b < 3000; b++) { e->render(h, buf.data(), 128); if (std::atoi(get(h, "status").c_str()) == 0 && b > 2) break; }
            return b * 128 / 44100.0;
        };
        e->set_param(v, "br_path", path_dir_of(argv[3]).c_str());
        bool tapped = tap_named(v, path_dir_of(argv[3]), argv[3] + path_dir_of(argv[3]).size() + 1);
        settle(v, 800);
        std::string ad = get(v, "env_attack_display"), dd = get(v, "env_decay_display"), sd = get(v, "env_sustain_display"), rd = get(v, "env_release_display");
        check(tapped && ad == "10 ms" && dd == "300 ms" && sd == "3 %" && rd == "400 ms",
              "a preset loaded shows its envelope: A " + ad + " D " + dd + " S " + sd + " R " + rd);
        double t0 = tail(v);
        char u[32];
        std::snprintf(u, sizeof u, "%g", 1000.0 * std::cbrt(2000.0 / 60000.0));   // 2 s on the release fader
        e->set_param(v, "env_release", u);
        double t1 = tail(v);
        check(get(v, "env_release_display") == "2.00 s" && t0 < 0.7 && t1 > 1.6 && t1 < 2.6,
              "release moved to 2 s: notes ring " + std::to_string(t1) + " s (were " + std::to_string(t0) + " s)");
        std::string st = get(v, "state");
        void *r = e->create("/tmp/omni-env-test");
        e->set_param(r, "state", st.c_str());
        settle(r, 800);
        double t2 = tail(r);
        check(get(r, "env_release_display") == "2.00 s" && t2 > 1.6 && t2 < 2.6, "the project keeps it: " + get(r, "env_release_display") + ", rings " + std::to_string(t2) + " s");
        bool t3 = tap_named(r, path_dir_of(argv[1]), argv[1] + path_dir_of(argv[1]).size() + 1);
        settle(r, 1500);
        check(t3 && get(r, "env_release_display") != "2.00 s" && std::atof(get(r, "env_sustain").c_str()) > 0,
              "another preset loaded shows its own: A " + get(r, "env_attack_display") + " D " + get(r, "env_decay_display") + " S " +
              get(r, "env_sustain_display") + " R " + get(r, "env_release_display"));
        e->destroy(r);
        e->destroy(v);
    }

    // 9. (1.7.2, with a 6th argument: a folder of NI kits with a previews/ folder, e.g. Battery Kits) tapping a kit plays its
    // preview at once (even when the kit's samples are missing); PANIC stops it; with PREVIEW off a tap plays nothing
    if (argc > 6) {
        void *b = e->create("/tmp/omni-preview-test");
        std::string dir = argv[6], kit;
        e->set_param(b, "br_path", dir.c_str());
        for (int r = 1; r <= 10 && kit.empty(); r++) {   // the first kit row
            std::string l = get(b, ("br_" + std::to_string(r)).c_str());
            if (l.find(".nbkt") != std::string::npos || l.find(".mxgrp") != std::string::npos) kit = l;
        }
        auto loud = [&](int blocks) { std::vector<int16_t> buf(256); int pk = 0; for (int k = 0; k < blocks; k++) { e->render(b, buf.data(), 128); for (int16_t x : buf) pk = std::max(pk, std::abs(int(x))); } return pk; };
        bool tapped = !kit.empty() && tap_named(b, dir, kit);
        std::string info = get(b, "br_info");
        int pk = loud(60);
        check(tapped && pk > 500 && info.find("preview playing") != std::string::npos,
              "tapping " + kit + " plays its preview (peak " + std::to_string(pk) + "): " + info);
        e->set_param(b, "panic", "1"); e->set_param(b, "panic", "0");
        loud(4);
        check(loud(20) == 0, "PANIC stops the preview");
        e->set_param(b, "br_preview", "0");
        tap_named(b, dir, kit);
        loud(4);
        check(loud(20) == 0 && get(b, "br_info").find("preview playing") == std::string::npos, "PREVIEW OFF: a tap plays nothing");
        e->destroy(b);
    }

    std::printf(fails ? "FAILED %d\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

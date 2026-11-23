// Omni Sampler 2.4.12: MUTATE / BREED (SYNTH > VOICE) and the GRID's BREED (ARP > GEN), through the plugin as the MPC
// drives it. MUTATE: only the continuous sound settings move, within range, more for WILD than SUBTLE; never an
// oscillator's type / wave, the output level or the voice. UNDO / again: exact. BREED: each section from A or B whole,
// or (continuous values) between them; refused without both parents; the child plays. GRID BREED: every bar is A's or
// B's, both used.
//   test_mutate        (run with OMNI_ARP_SEED=<n> to repeat a run)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "engine.h"
}

static const mpc_engine_t *e;
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static std::string get(void *y, const char *key) { char b[65536]; int n = e->get_param(y, key, b, sizeof b); return n > 0 ? std::string(b) : ""; }
static void press(void *y, const char *key) { e->set_param(y, key, "1"); e->set_param(y, key, "0"); }
static std::vector<int16_t> buf(256);
// (with real time passing: the synth's oscillators load on a worker thread)
static void idle(void *y, int blocks) { for (int i = 0; i < blocks; i++) { e->render(y, buf.data(), 128); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } }
// the synth's settings as the project keeps them (state: key=value lines)
static std::map<std::string, double> synth(void *y) {
    std::map<std::string, double> m;
    const std::string st = get(y, "state");
    size_t at = 0;
    while (at < st.size()) {
        size_t nl = st.find('\n', at);
        if (nl == std::string::npos) nl = st.size();
        const std::string line = st.substr(at, nl - at);
        at = nl + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const bool syn = (!k.compare(0, 4, "syn_") && k != "syn_preset" && k.compare(0, 9, "syn_file_")) ||
                         (k.size() > 3 && k[0] == 's' && (k[1] == 'a' || k[1] == 'b') && k[2] == '_');
        if (syn) m[k] = std::atof(line.c_str() + eq + 1);
    }
    return m;
}
// (the state keeps only what differs from the default: a missing key = at its default, NAN here)
using Syn = std::map<std::string, double>;
static double val(const Syn &m, const std::string &k) { auto it = m.find(k); return it == m.end() ? NAN : it->second; }
static bool same(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }
static std::set<std::string> keys(const Syn &a, const Syn &b, const Syn &c = {}) {
    std::set<std::string> s;
    for (auto *m : {&a, &b, &c}) for (auto &kv : *m) s.insert(kv.first);
    return s;
}
static double peak_of_note(void *y) {
    idle(y, 600);   // (a mutated / bred patch: its oscillators reload first; 2.4.26: 60 blocks was too short under ASan, a race not a bug)
    uint8_t on[3] = {0x90, 48, 110}, off[3] = {0x80, 48, 0};
    e->midi(y, on, 3);
    int pk = 0;
    for (int b = 0; b < 350; b++) { e->render(y, buf.data(), 128); for (int16_t v : buf) pk = std::max(pk, std::abs(int(v))); }
    e->midi(y, off, 3);
    idle(y, 300);
    return pk;
}
static const char *const NEVER[] = {"sa_type", "sa_table", "sb_type", "sb_table", "sa_on", "sb_on", "syn_level", "syn_poly", "syn_vmode",
                                    "syn_on", "sa_unison", "sb_unison", "sa_oct", "sb_oct", "syn_flt_type", "syn_lfo1_shape", "syn_mx1_src",
                                    "syn_lfo3_shape", "syn_lfo4_shape", "syn_mx9_src", "syn_mx16_dst", "syn_me2_dest"};   // 2.4.17

int main() {
    e = mpc_engine();
    void *y = e->create("/tmp/omni-mutate-test");
    idle(y, 20);
    // a synth patch: SYNTH on, two oscillators, the filter, an LFO route
    const char *patch[][2] = {{"syn_on", "1"}, {"sa_on", "1"}, {"sa_level", "70"}, {"sa_pos", "30"}, {"sa_detune", "20"}, {"sb_on", "1"},
                              {"sb_level", "60"}, {"sb_table", "3"}, {"syn_flt_on", "1"}, {"syn_cutoff", "60"}, {"syn_res", "30"},
                              {"syn_attack", "10"}, {"syn_decay", "400"}, {"syn_sustain", "70"}, {"syn_release", "500"},
                              {"syn_mx1_src", "1"}, {"syn_mx1_dst", "2"}, {"syn_mx1_amt", "30"}, {"syn_fm", "10"}};
    for (auto &p : patch) e->set_param(y, p[0], p[1]);
    idle(y, 50);
    const auto base = synth(y);
    check(val(base, "syn_on") == 1 && val(base, "sa_level") == 70, "a synth patch set up");
    check(peak_of_note(y) > 200, "it plays");

    // MUTATE at each amount: how many / how far, the protected settings never move, UNDO exact, again = forward
    double moved_sum[3] = {0, 0, 0};
    bool kept = true, in_range = true, undo_ok = true, redo_ok = true, plays = true;
    for (int lvl = 0; lvl < 3; lvl++) {
        char lv[4]; std::snprintf(lv, sizeof lv, "%d", lvl);
        e->set_param(y, "mut_amount", lv);
        for (int rep = 0; rep < 10; rep++) {
            const auto before = synth(y);
            press(y, "mut_go");
            idle(y, 4);
            const auto after = synth(y);
            for (const char *k : NEVER) if (!same(val(after, k), val(before, k))) { kept = false; std::printf("  %s moved\n", k); }
            for (const std::string &k : keys(before, after)) {
                const double b = val(before, k), x = val(after, k);
                if (std::isnan(b) || std::isnan(x)) continue;
                const bool t = k.find("attack") != std::string::npos || k.find("decay") != std::string::npos || k.find("release") != std::string::npos;
                moved_sum[lvl] += std::fabs(x - b) / (t ? std::max(1.0, b) : 100.0);
                if (!std::isfinite(x) || x < -100 || x > 10000) { in_range = false; std::printf("  %s = %g\n", k.c_str(), x); }
            }
            if (!std::isnan(val(after, "syn_cutoff")) && val(after, "syn_cutoff") < 20 - 1e-6) in_range = false;
            if (rep == 9 && peak_of_note(y) <= 50) plays = false;
            press(y, "mut_undo");
            idle(y, 4);
            if (synth(y) != before) undo_ok = false;
            press(y, "mut_undo");   // again: forward
            idle(y, 4);
            if (synth(y) != after) redo_ok = false;
            press(y, "mut_undo");
            idle(y, 4);
        }
    }
    std::printf("     MUTATE distance over 10 presses: SUBTLE %.2f, MEDIUM %.2f, WILD %.2f\n", moved_sum[0], moved_sum[1], moved_sum[2]);
    check(moved_sum[0] > 0 && moved_sum[0] < moved_sum[1] && moved_sum[1] < moved_sum[2], "MUTATE: SUBTLE < MEDIUM < WILD");
    check(kept, "MUTATE never touches the waves, the output level or the voice");
    check(in_range, "MUTATE keeps every value finite and in range (the cutoff never shut)");
    check(undo_ok && redo_ok, "UNDO restores the patch exactly; again goes forward");
    check(plays, "a mutated patch plays");
    check(synth(y) == base, "after UNDO the patch is as it was");

    // BREED
    press(y, "mut_breed");
    idle(y, 4);
    check(get(y, "status").find("STORE A and STORE B first") != std::string::npos && synth(y) == base, "BREED without parents: refused, says why");
    press(y, "mut_store_a");
    const auto A = synth(y);
    const char *other[][2] = {{"sa_level", "40"}, {"sa_pos", "80"}, {"sa_detune", "5"}, {"sb_table", "5"}, {"sb_level", "90"},
                              {"syn_cutoff", "85"}, {"syn_res", "10"}, {"syn_attack", "300"}, {"syn_release", "1500"}, {"syn_mx1_amt", "-60"},
                              {"syn_fm", "40"}, {"syn_lfo1_rate", "80"}};
    for (auto &p : other) e->set_param(y, p[0], p[1]);
    idle(y, 4);
    press(y, "mut_store_b");
    const auto B = synth(y);
    bool child_ok = true, both = false, osc_whole = true, plays_b = true;
    int from_a_total = 0, from_b_total = 0;
    for (int rep = 0; rep < 20; rep++) {
        press(y, "mut_breed");
        idle(y, 4);
        const auto C = synth(y);
        int fa = 0, fb = 0;
        for (const std::string &k : keys(A, B, C)) {
            const double a = val(A, k), b = val(B, k), c = val(C, k);
            if (same(c, a) && same(c, b)) continue;
            if (same(c, a)) fa++;
            else if (same(c, b)) fb++;
            else if (std::isnan(a) || std::isnan(b) || std::isnan(c)) continue;   // (a default: its number is not known here)
            else if (!(c >= std::min(a, b) - 1e-4 && c <= std::max(a, b) + 1e-4)) { child_ok = false; std::printf("  %s = %g (A %g, B %g)\n", k.c_str(), c, a, b); }
        }
        if (!same(val(C, "sb_table"), val(A, "sb_table")) && !same(val(C, "sb_table"), val(B, "sb_table"))) osc_whole = false;   // a wave: one parent's
        from_a_total += fa; from_b_total += fb;
        if (fa && fb) both = true;
        if (rep % 5 == 4 && peak_of_note(y) <= 50) plays_b = false;
    }
    std::printf("     BREED x20: %d values from A, %d from B (others between)\n", from_a_total, from_b_total);
    check(child_ok, "BREED: every value is A's, B's or between them");
    check(both, "BREED: a child of both parents");
    check(osc_whole, "BREED: an oscillator's wave comes whole from one parent");
    check(plays_b, "a bred patch plays");
    press(y, "mut_undo");
    idle(y, 4);

    // the GRID's BREED: two patterns (4 bars of 1/16), a child bar by bar
    e->set_param(y, "arp_length", "64"); e->set_param(y, "arp_rate", "2"); e->set_param(y, "arp_tie", "0");
    auto grid = [&]() {   // the pattern as the state keeps it
        const std::string st = get(y, "state");
        const size_t a = st.find("\narp_pattern=");
        if (a == std::string::npos) return std::string();
        const size_t b = st.find('\n', a + 1);
        return st.substr(a + 13, b - a - 13);
    };
    e->set_param(y, "arp_g_contour", "2");
    press(y, "arp_g_generate");
    press(y, "arp_b_store_a");
    const std::string ga = grid();
    e->set_param(y, "arp_g_contour", "7");
    press(y, "arp_g_generate");
    press(y, "arp_b_store_b");
    const std::string gb = grid();
    check(!ga.empty() && !gb.empty() && ga != gb, "GRID: two different patterns stored as A and B");
    bool bars_ok = true, mixed = false;
    // arp_pattern: 6 hex characters a step (mask 4, velocity 2); a 1/16 bar = 16 steps
    auto bar_of = [](const std::string &g, int b) { std::string s = g.substr(size_t(std::min<size_t>(g.size(), size_t(b) * 96)), 96); s.resize(96, '0'); return s; };
    for (int rep = 0; rep < 10; rep++) {
        press(y, "arp_b_breed");
        const std::string gc = grid();
        int a_bars = 0, b_bars = 0;
        for (int b = 0; b < 4; b++) {
            const std::string c = bar_of(gc, b), ba = bar_of(ga, b), bb = bar_of(gb, b);
            const bool isa = c.substr(0, 96) == ba, isb = c.substr(0, 96) == bb;
            // (velocities can be overwritten by the step store: compare the note masks)
            bool ma = true, mb = true;
            for (int s = 0; s < 16; s++) { if (c.substr(size_t(s) * 6, 4) != ba.substr(size_t(s) * 6, 4)) ma = false; if (c.substr(size_t(s) * 6, 4) != bb.substr(size_t(s) * 6, 4)) mb = false; }
            if (!(isa || isb || ma || mb)) bars_ok = false;
            a_bars += ma; b_bars += mb;
        }
        if (a_bars && b_bars) mixed = true;
    }
    std::printf("     GRID BREED: %s\n", get(y, "arp_info").c_str());
    check(bars_ok, "GRID BREED: every bar is A's or B's");
    check(mixed, "GRID BREED: the child has bars of both");
    e->destroy(y);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

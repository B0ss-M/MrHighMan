// 2.4.37: GENERATE's MC-909 styles (mc909_arp.hpp): every style becomes grid notes (rows = its notes low to high, ties
// held), the Style mode writes a style as stored (LENGTH, step, TIE; UNDO puts them back), another mode takes the style as
// its material, and the Style mode without a style changes nothing.
//   g++ -std=gnu++17 -g -O1 -fsanitize=address,undefined -I<src> -I<wrapper> -DPLUGIN_CPP='"<src>/plugin.cpp"' test_mc909_style.cpp <prebuilt .o> -lpthread
#include PLUGIN_CPP
static int fails = 0;
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }
static std::string msg_of(Omni *o) { std::lock_guard<std::mutex> lock(o->syn_mutex); return o->arp_msg; }

int main() {
    // 1. every style
    int good = 0;
    for (int id = 1; id <= 128; id++) {
        StyleGrid g;
        bool ok = mc909_style_grid(id, g) && g.len >= 2 && g.len <= 32 && !g.notes.empty();
        for (const GNote &n : g.notes)
            ok = ok && n.row >= 0 && n.row < 16 && n.s >= 0 && n.len >= 1 && n.s + n.len <= g.len && n.vel >= 1 && n.vel <= 127 && n.chord;
        const int rate = mc909_style_rate(id);
        ok = ok && rate >= 0 && rate < Arp::RATES;
        if (ok) good++; else std::printf("  style %d (%s) bad\n", id, mv8800_arp::STYLE_NAMES[id - 1]);
    }
    check(good == 128, "all 128 styles make grid notes (" + std::to_string(good) + ")");
    StyleGrid none;
    check(!mc909_style_grid(0, none) && !mc909_style_grid(129, none), "style 0 / 129 refused");
    // 2. two styles against their bytes: 001 "1/4" = rows 72 (step 1, vel 127) and 84 (step 2, vel 110); 016 "1/16 ..."
    //    record 04 03 FF 00 | 3C 7F 80 80 80 | 3E 00 64 00 00 | 40 00 00 5A 00: row 60 held 4 steps (ties)
    StyleGrid a, b;
    mc909_style_grid(1, a);
    check(a.len == 2 && a.notes.size() == 2 && a.notes[0].s == 0 && a.notes[0].row == 0 && a.notes[0].vel == 127 &&
          a.notes[1].s == 1 && a.notes[1].row == 1 && a.notes[1].vel == 110, "style 1 (1/4) = its two notes");
    mc909_style_grid(16, b);
    bool held = false;
    for (const GNote &n : b.notes) if (n.row == 0 && n.s == 0 && n.len == 4 && n.vel == 127) held = true;
    check(b.len == 4 && held && b.mask[0] == 1 && b.mask[3] == (1 | 0), "style 16: the first note held over its 3 ties");
    check(mc909_style_rate(1) == 0 && mc909_style_rate(4) == 4 && mc909_style_rate(5) == 2 && mc909_style_rate(8) == 3 && mc909_style_rate(98) == 2,
          "step from the name (1/4, 1/12 = 1/8 T, 1/16, 1/32, others 1/16)");

    // 3. the plugin: Style mode writes the style as stored; UNDO puts the grid, LENGTH and step back
    (void)!std::system("rm -rf /tmp/omni-mc909-test");
    const mpc_engine_t *e = mpc_engine();
    void *y = e->create("/tmp/omni-mc909-test");
    Omni *o = static_cast<Omni *>(y);
    Arp &arp = o->sampler.arp;
    e->set_param(y, "arp_length", "16"); e->set_param(y, "arp_rate", "1");
    apply_settings(o);
    uint16_t before[64];
    for (int s = 0; s < 64; s++) before[s] = arp.mask[s].load();
    e->set_param(y, "arp_g_contour", std::to_string(int(AM_STYLE)).c_str());
    arp_gen(o, "generate");   // no style chosen
    bool same = true;
    for (int s = 0; s < 64; s++) same = same && arp.mask[s].load() == before[s];
    check(same && msg_of(o).find("pick an OMNI STYLE") != std::string::npos, "Style mode without a style: grid unchanged, says so");
    StyleGrid t;
    mc909_style_grid(111, t);   // TANGO 2: 16 steps, 6 rows
    e->set_param(y, "arp_g_style", "111");
    arp_gen(o, "generate");
    bool exact = arp_len(o) == t.len && int(num(o, "arp_rate") + .5f) == 2 && num(o, "arp_tie") > .5f;
    for (int s = 0; s < t.len; s++) exact = exact && arp.mask[s].load() == t.mask[s];
    check(exact, "Style mode: the grid = TANGO 2 as stored (16 steps of 1/16, TIE on)");
    check(msg_of(o).find("111 TANGO 2") != std::string::npos, "message names the style: " + msg_of(o));
    arp_gen(o, "undo");
    bool back = arp_len(o) == 16 && int(num(o, "arp_rate") + .5f) == 1;
    for (int s = 0; s < 64; s++) back = back && arp.mask[s].load() == before[s];
    check(back, "UNDO: grid, LENGTH 16 and step 1/8 back");

    // 4. another mode takes the style as its material (a chordal style: SALSA 1, 10 rows)
    e->set_param(y, "arp_g_contour", "12");   // Arpeggio
    e->set_param(y, "arp_g_style", "99");
    arp_gen(o, "generate");
    int notes = 0;
    for (int s = 0; s < arp_len(o); s++) notes += __builtin_popcount(arp.mask[s].load());
    check(notes > 0 && msg_of(o).find("Omni Style SALSA 1") != std::string::npos, "Arpeggio from Omni Style SALSA 1: " + msg_of(o));
    e->set_param(y, "arp_g_contour", "6");   // Ostinato, a one-line style (style 24: 16 steps, 1 row)
    e->set_param(y, "arp_g_style", "24");
    arp_gen(o, "generate");
    notes = 0;
    for (int s = 0; s < arp_len(o); s++) notes += __builtin_popcount(arp.mask[s].load());
    check(notes > 0 && msg_of(o).find("from Omni Style") != std::string::npos, "Ostinato from a one-row style: " + msg_of(o));
    e->destroy(y);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

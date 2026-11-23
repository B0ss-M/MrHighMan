// F7 (proposed): compact "arp_x" state. Includes plugin.cpp (PLUGIN_CPP) for arp_x_text / get_state / set_state.
#include PLUGIN_CPP
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static uint32_t rs = 12345;
static int rnd(int n) { rs = rs * 1664525u + 1013904223u; return int((rs >> 8) % unsigned(n)); }
static std::string line_of(const std::string &st, const char *key) {
    const size_t a = st.find(std::string("\n") + key + "=");
    if (a == std::string::npos) return "";
    const size_t b = st.find('\n', a + 1);
    return st.substr(a + 1, b - a - 1);
}
static bool same_cells(Arp &x, Arp &y, int *bad) {
    *bad = 0;
    for (int i = 0; i < Arp::STEPS * 16; i++)
        if (x.ngate[i] != y.ngate[i] || x.nvel[i] != y.nvel[i] || x.nskip[i] != y.nskip[i] || std::max(1, int(x.nratch[i])) != std::max(1, int(y.nratch[i])) || x.noct[i] != y.noct[i] || x.nslide[i] != y.nslide[i]) (*bad)++;
    return *bad == 0;
}
static void fill(Arp &a, int mode) {   // mode 0: gate on every cell (varying), 1: random everything, 2: sparse
    for (int s = 0; s < 64; s++) { a.mask[s].store(0xFFFF); a.vel[s].store(100); }
    for (int i = 0; i < 1024; i++) {
        if (mode == 0) a.ngate[i].store(uint8_t(10 + (i * 7) % 91));
        else if (mode == 1) { a.ngate[i].store(uint8_t(1 + rnd(100))); a.nvel[i].store(uint8_t(1 + rnd(200))); a.nskip[i].store(uint8_t(rnd(100))); a.nratch[i].store(uint8_t(1 + rnd(4))); a.noct[i].store(int8_t(rnd(5) - 2)); a.nslide[i].store(uint8_t(rnd(2))); }
        else if (mode == 3) { a.ngate[i].store(uint8_t(1 + rnd(100))); a.nvel[i].store(uint8_t(1 + rnd(200))); a.nratch[i].store(uint8_t(1 + rnd(4))); a.noct[i].store(int8_t(rnd(5) - 2)); a.nslide[i].store(uint8_t(rnd(2))); }
        else if (i % 37 == 0) { a.ngate[i].store(55); a.nvel[i].store(uint8_t(80 + i % 100)); a.nratch[i].store(3); a.noct[i].store(-1); a.nslide[i].store(1); }
    }
}
int main() {
    (void)!std::system("rm -rf /tmp/omni-f7-test");
    const mpc_engine_t *e = mpc_engine();
    for (int mode = 0; mode < 4; mode++) {
        void *y = e->create("/tmp/omni-f7-test");
        Omni *o = static_cast<Omni *>(y);
        e->set_param(y, "arp_length", "64");
        fill(o->sampler.arp, mode);
        const std::string st = get_state(o);
        const std::string gl = line_of(st, "arp_gates"), nl = line_of(st, "arp_nx"), xl = line_of(st, "arp_x");
        std::printf("mode %d: state %zu bytes (limit %zu), arp_gates %zu, arp_nx %zu, arp_x %zu, warning '%s'\n", mode, st.size(), STATE_MAX, gl.size(), nl.size(), xl.size(), o->state_warning.c_str());
        if (mode == 0) check(st.size() <= STATE_MAX && o->state_warning.empty() && (!gl.empty() || !xl.empty()), "dense gates (all 1024 cells): saved whole, no warning");
        if (mode == 2) check(st.size() <= STATE_MAX && o->state_warning.empty(), "sparse expression: saved whole");
        if (mode == 3) check(st.size() <= STATE_MAX && o->state_warning.empty(), "DENSE 64x16: gate, velocity, ratchet, octave, slide on every cell: saved whole, no warning");
        if (mode == 1) check(st.size() <= STATE_MAX && !o->state_warning.empty(), "all six fields on every cell (cannot fit): under the limit, with a warning");
        // round trip into a fresh instance
        void *z = e->create("/tmp/omni-f7-test");
        Omni *p = static_cast<Omni *>(z);
        e->set_param(z, "state", st.c_str());
        int bad = 0;
        const bool same = same_cells(o->sampler.arp, p->sampler.arp, &bad);
        std::printf("  round trip: %d cells differ\n", bad);
        if (mode != 1) check(same, "round trip restores every cell exactly");
        else check(bad < 1024, "worst case: partial, not all lost (the early steps are exact)");
        e->destroy(z); e->destroy(y);
    }
    {   // backward compatibility: a project saved before (arp_gates + arp_nx lines) still loads exactly
        void *y = e->create("/tmp/omni-f7-test");
        Omni *o = static_cast<Omni *>(y);
        fill(o->sampler.arp, 2);
        const std::string legacy = "omni-sampler 2\narp_pattern=" + arp_pattern_text(o) + "\narp_gates=" + arp_gates_text(o) + "\narp_nx=" + arp_nx_text(o) + "\n";
        void *z = e->create("/tmp/omni-f7-test");
        Omni *p = static_cast<Omni *>(z);
        e->set_param(z, "state", legacy.c_str());
        int bad = 0;
        same_cells(o->sampler.arp, p->sampler.arp, &bad);
        std::printf("legacy state (%zu bytes): %d cells differ\n", legacy.size(), bad);
        check(bad == 0 && legacy.find("arp_x") == std::string::npos, "a project with arp_gates / arp_nx lines (written by 2.4.29 and before) loads exactly");
        e->destroy(z); e->destroy(y);
    }
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

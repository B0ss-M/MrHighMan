// F5 regression: an instrument / patch's saved sound settings must carry no "arp_*" key (the GRID belongs to the project).
// Includes plugin.cpp so it can reach sound_param() / patch_param() / settings_json() (they live in its anonymous namespace).
//   g++ -std=gnu++17 -g -O1 -fsanitize=address,undefined -I<src> -I<wrapper> -DPLUGIN_CPP='"<src>/plugin.cpp"' test_grid_f5.cpp <prebuilt .o> -lpthread
#include PLUGIN_CPP
static int fails = 0;
static void check(bool ok, const char *what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
int main() {
    static const char *const ARP[] = {"arp_on", "arp_rate", "arp_scale", "arp_root", "arp_page", "arp_mode", "arp_order", "arp_tie", "arp_length", "arp_latch", "arp_octave", "arp_swing"};
    bool all_out = true;
    for (const char *k : ARP) if (sound_param(k) || patch_param(k)) { std::printf("  still saved: %s\n", k); all_out = false; }
    check(all_out, "sound_param / patch_param refuse every arp_* key");
    check(sound_param("cutoff"), "ordinary sound settings (cutoff) are still saved");
    (void)!std::system("rm -rf /tmp/omni-f5-test");
    const mpc_engine_t *e = mpc_engine();
    void *y = e->create("/tmp/omni-f5-test");
    e->set_param(y, "arp_on", "1"); e->set_param(y, "arp_scale", "5"); e->set_param(y, "arp_rate", "3");
    const std::string js = settings_json(static_cast<Omni *>(y)), pj = settings_json(static_cast<Omni *>(y), patch_param);
    std::printf("settings_json: %zu bytes, patch: %zu bytes\n", js.size(), pj.size());
    check(js.find("\"arp_") == std::string::npos && pj.find("\"arp_") == std::string::npos, "settings_json (instrument and patch) holds no arp_* key with ARP on");
    check(js.find("\"cutoff\"") != std::string::npos, "settings_json still holds cutoff");
    e->destroy(y);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

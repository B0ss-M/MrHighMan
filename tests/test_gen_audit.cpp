// 2.4.40 audit (the user: "ensure all functions work; the motif for open, closed etc. don't seem to have much impact on the
// sequence"): every GENERATE mode on its own melody, on Omni Styles and on a loaded MIDI file, in SEQ and ARP and two keys;
// then each motif tool (INVERT, REVERSE, DOUBLE, HALF, MUTATE, FILL) and each VOICE mode on the result. For each action:
// how much of the grid it changed (cells: a step's row on / off, the step's velocity, a note's octave) and its message.
//   g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -Isrc -I<wrapper> -DPLUGIN_CPP='"src/plugin.cpp"' test_gen_audit.cpp <objs> -lpthread
#include PLUGIN_CPP
#include <map>

static std::string msg_of(Omni *o) { std::lock_guard<std::mutex> lock(o->syn_mutex); return o->arp_msg; }
struct Grid { uint16_t mask[64]; uint8_t vel[64]; int8_t oct[64 * 16]; };
static Grid grab(Omni *o) {
    Grid g;
    Arp &a = o->sampler.arp;
    for (int s = 0; s < 64; s++) { g.mask[s] = a.mask[s].load(); g.vel[s] = a.vel[s].load(); }
    for (int i = 0; i < 64 * 16; i++) g.oct[i] = int8_t(a.noct[i].load());
    return g;
}
static int changed(const Grid &x, const Grid &y, int len) {
    int n = 0;
    for (int s = 0; s < len; s++) {
        n += __builtin_popcount(unsigned(x.mask[s] ^ y.mask[s]));
        if (x.mask[s] && x.vel[s] != y.vel[s]) n++;
        for (int r = 0; r < 16; r++) if ((x.mask[s] >> r & 1) && x.oct[s * 16 + r] != y.oct[s * 16 + r]) n++;
    }
    return n;
}
static int notes(const Grid &g, int len) { int n = 0; for (int s = 0; s < len; s++) n += __builtin_popcount(g.mask[s]); return n; }

int main(int argc, char **argv) {
    const std::string midi = argc > 1 ? argv[1] : "";
    (void)!std::system("rm -rf /tmp/omni-gen-audit && mkdir -p /tmp/omni-gen-audit/lib/MIDI");
    if (!midi.empty()) (void)!std::system(("cp '" + midi + "' /tmp/omni-gen-audit/lib/MIDI/a.mid").c_str());
    const mpc_engine_t *e = mpc_engine();
    void *y = e->create("/tmp/omni-gen-audit/data");
    Omni *o = static_cast<Omni *>(y);
    { std::lock_guard<std::mutex> lock(o->lib_mutex); o->lib_choice = "/tmp/omni-gen-audit/lib"; }
    const char *TOOLS[] = {"invert", "reverse", "double", "half", "mutate", "fill"};
    std::map<std::string, std::pair<int, int>> stat;   // action -> (runs, runs that changed nothing)
    std::map<std::string, std::string> example;        // action -> a message of a no-change run
    int runs = 0;
    for (int mode = 0; mode < 2; mode++)
        for (int key = 0; key < 2; key++)
            for (int mat = 0; mat < 5; mat++) {   // own melody, styles 98 BOSSA NOVA / 111 TANGO 2 / 24 one row, MIDI file
                if (mat == 4 && midi.empty()) continue;
                for (int contour = 0; contour < AM_COUNT; contour++) {
                    if (contour == AM_STYLE && (mat == 0 || mat == 4)) continue;
                    e->set_param(y, "arp_mode", std::to_string(mode).c_str());
                    e->set_param(y, "arp_scale", key ? "2" : "1");   // Minor / Major
                    e->set_param(y, "arp_root", key ? "9" : "0");    // A / C
                    e->set_param(y, "arp_length", "16"); e->set_param(y, "arp_rate", "2");
                    e->set_param(y, "arp_g_style", mat == 1 ? "98" : mat == 2 ? "111" : mat == 3 ? "24" : "0");
                    { std::lock_guard<std::mutex> lock(o->syn_mutex); o->arp_src.set = false; o->arp_loaded.clear(); }
                    if (mat == 4) { { std::lock_guard<std::mutex> lock(o->syn_mutex); o->arp_want = "a.mid"; } arp_load_now(o); }
                    e->set_param(y, "arp_g_contour", std::to_string(contour).c_str());
                    apply_settings(o);
                    arp_gen(o, "generate");
                    const int len = arp_len(o);
                    const Grid g0 = grab(o);
                    const std::string tag = std::string(mode ? "ARP" : "SEQ") + (key ? " Am " : " C ") +
                                            (mat == 0 ? "own" : mat == 4 ? "midi" : "style" + std::to_string(mat)) + " " + ARP_MODE_NAMES[contour];
                    runs++;
                    if (notes(g0, len) == 0) { stat["generate"].second++; example["generate"] = tag + ": " + msg_of(o); }
                    stat["generate"].first++;
                    for (const char *t : TOOLS) {
                        arp_gen(o, t);
                        const int c = changed(g0, grab(o), arp_len(o));
                        auto &st = stat[t];
                        st.first++;
                        if (!c) { st.second++; example[t] = tag + ": " + msg_of(o); }
                        arp_gen(o, "undo");
                    }
                    for (int v = 0; v < 6; v++) {
                        e->set_param(y, "arp_v_mode", std::to_string(v).c_str());
                        arp_gen(o, "voice");
                        const int c = changed(g0, grab(o), arp_len(o));
                        const std::string k = std::string("voice ") + VOICE_MODE_NAMES[v];
                        auto &st = stat[k];
                        st.first++;
                        if (!c) { st.second++; example[k] = tag + ": " + msg_of(o); }
                        arp_gen(o, "undo");
                    }
                }
            }
    std::printf("%d generated patterns\n", runs);
    for (const auto &kv : stat)
        std::printf("%-22s %4d runs, %4d changed nothing (%3d %%)%s%s\n", kv.first.c_str(), kv.second.first, kv.second.second,
                    kv.second.first ? 100 * kv.second.second / kv.second.first : 0, example.count(kv.first) ? "  e.g. " : "",
                    example.count(kv.first) ? example[kv.first].substr(0, 140).c_str() : "");
    e->destroy(y);
    return 0;
}

// Omni Sampler 2.4.6: the MPC keygroup program (.xpm) written beside a saved instrument's WAVs. An instrument is saved
// (write_omni), its .xpm read back with the MPC reader, and every zone compared: keys, velocities, root, tune, level,
// loop, one shot.   test_xpm <instrument file> [preset] [out dir]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include "../src/formats/format.hpp"
#include "../src/formats/omni_native.hpp"
using namespace omni;
static int fails = 0;
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: test_xpm <instrument file> [preset] [out dir]\n"); return 2; }
    register_all();
    const int preset = argc > 2 ? std::atoi(argv[2]) : 0;
    const std::string out = argc > 3 ? argv[3] : "/tmp/omni-xpm-test";
    if (std::system(("rm -rf '" + out + "'").c_str()) != 0) return 2;
    Vfs vfs;
    Location loc = vfs.resolve(argv[1]);
    const FormatReader *r = find_reader(*loc.volume, loc.inner);
    if (!r) { std::printf("FAIL no reader\n"); return 1; }
    Instrument in = r->load(loc.volume, loc.inner, preset);
    std::string omni = write_omni(in, {}, out, argv[1], preset, "", "");
    std::string stem = omni.substr(out.size() + 1, omni.size() - out.size() - 1 - 5);
    std::string xpm = out + "/" + stem + " Samples/" + stem + ".xpm";
    Location xl = vfs.resolve(xpm);
    const FormatReader *xr = find_reader(*xl.volume, xl.inner);
    check(xr != nullptr, "the .xpm is written and recognised: " + xpm);
    if (!xr) return 1;
    Instrument back = xr->load(xl.volume, xl.inner, 0);
    size_t with_sample = 0;
    for (auto &z : in.zones) if (z.sample) with_sample++;
    check(back.zones.size() == std::min<size_t>(with_sample, 512), "every zone comes back (" + std::to_string(back.zones.size()) + " of " + std::to_string(with_sample) + ")");
    check(back.warnings.empty() || back.warnings[0].find("missing") == std::string::npos, "no missing samples");
    // match zones by (key range, velocity range, root): several can share them (round robin): compare as multisets
    std::multimap<std::tuple<int, int, int, int, int>, const Zone *> want;
    for (auto &z : in.zones) if (z.sample) want.insert({{z.key_lo, z.key_hi, std::max(1, z.vel_lo), std::min(127, z.vel_hi), z.root}, &z});   // velocity 0 = 1 (no note-on has 0)
    int bad_keys = 0, bad_tune = 0, bad_gain = 0, bad_loop = 0, bad_shot = 0;
    for (auto &b : back.zones) {
        auto it = want.find({b.key_lo, b.key_hi, std::max(1, b.vel_lo), b.vel_hi, b.root});
        if (it == want.end()) {
            std::printf("     back: keys %d-%d vel %d-%d root %d (%s)\n", b.key_lo, b.key_hi, b.vel_lo, b.vel_hi, b.root, b.name.c_str());
            bad_keys++; continue;
        }
        const Zone &a = *it->second;
        if (std::fabs(a.tune - b.tune) > 0.011) bad_tune++;
        const double ga = std::max(-21.0, std::min(6.0, a.gain_db + in.gain_db));
        if (std::fabs(ga - (b.gain_db + back.gain_db)) > 0.05) bad_gain++;
        const bool la = !a.loops.empty() && !a.one_shot, lb = !b.loops.empty();
        if (la != lb || (la && (a.loops[0].start != b.loops[0].start))) bad_loop++;
        if (a.one_shot != b.one_shot) bad_shot++;
        want.erase(it);
    }
    for (auto &w : want) std::printf("     unmatched: keys %d-%d vel %d-%d root %d (%s)\n", w.second->key_lo, w.second->key_hi, w.second->vel_lo, w.second->vel_hi, w.second->root, w.second->name.c_str());
    check(bad_keys == 0, "keys, velocities and roots match (" + std::to_string(bad_keys) + " differ)");
    check(bad_tune == 0, "tuning matches (" + std::to_string(bad_tune) + " differ)");
    check(bad_gain == 0, "levels match within 0.05 dB (" + std::to_string(bad_gain) + " differ)");
    check(bad_loop == 0, "loops match (" + std::to_string(bad_loop) + " differ)");
    check(bad_shot == 0, "one shots match (" + std::to_string(bad_shot) + " differ)");
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

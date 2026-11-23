// Omni Sampler 2.4.35: Serum 2 mapping regressions (testdata/serum2_map: 4 factory presets + the files they need).
//   usage: test_serum_map <testdata/serum2_map>
// The 2.4.34 bugs this guards: source 1 (Mod Wheel) played as the amp envelope; source 2 (Env 1, the amp envelope)
// dropped; a route's aux source ignored (Ambient Piano: LFO 2 x wheel on OSC A's level = silence); default filter type;
// an FX-only (routing None) layer played dry; every source under -40 dB refused (The Even Odds).
#include "formats/format.hpp"
#include "formats/serum_sample.hpp"
#include "formats/synth_patch.hpp"
#include <cmath>
#include <cstdio>
#include <map>
using namespace omni;

static int fails = 0;
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; }

struct Loaded { std::map<std::string, float> v; std::map<std::string, std::string> text; std::string info, err; };
static Loaded load(const std::string &path) {
    Loaded l;
    try {
        Vfs fs;
        Location loc = fs.resolve(path);
        const FormatReader *r = find_reader(*loc.volume, loc.inner);
        if (!r) { l.err = "no reader"; return l; }
        Instrument in = r->load(loc.volume, loc.inner, 0);
        if (!in.warnings.empty()) l.info = in.warnings[0];
        for (auto &kv : in.settings) {
            if (!kv.first.empty() && kv.first[0] == '@') {
                const size_t eq = kv.first.find('=');
                l.text[kv.first.substr(1, eq - 1)] = kv.first.substr(eq + 1);
            } else l.v[kv.first] = kv.second;
        }
    } catch (const std::exception &e) { l.err = e.what(); }
    return l;
}
// the MATRIX routes as "src>dst"
static std::vector<std::pair<int, int>> routes(const Loaded &l) {
    std::vector<std::pair<int, int>> r;
    for (int n = 1; n <= 16; n++) {
        auto s = l.v.find("syn_mx" + std::to_string(n) + "_src"), d = l.v.find("syn_mx" + std::to_string(n) + "_dst");
        if (s != l.v.end() && d != l.v.end() && s->second != 0) r.push_back({int(s->second), int(d->second)});
    }
    return r;
}

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("usage: test_serum_map <serum2_map>\n"); return 2; }
    register_all();
    const std::string P = std::string(argv[1]) + "/Presets/Factory/";

    Loaded fl = load(P + "Woodwind/WIND - Flute.SerumPreset");
    check(fl.err.empty(), "Flute loads " + fl.err);
    check(fl.text["syn_file_a"].rfind("serumsmp:0:", 0) == 0, "Flute OSC A = the flute multisample: " + fl.text["syn_file_a"]);
    check(fl.text["syn_file_b"].rfind("serumsmp:2:", 0) == 0, "Flute OSC B = Aztec Pipe (Betty is an FX-only layer): " + fl.text["syn_file_b"]);
    check(fl.v["syn_flt_on"] == 1 && fl.v["syn_flt_type"] == SynthPatch::LP12, "Flute filter on, MgL12 default = LP12");
    check(fl.v["syn_fenv"] > 50 && std::fabs(fl.v["syn_fe_attack"] - 1100) < 1, "Flute: Env 1 (amp) -> cutoff on the FILTER ENV with the amp env's times");
    for (auto &r : routes(fl)) check(r.first != SynthPatch::S_AMPENV || r.second != SynthPatch::D_CUTOFF, "no duplicate amp env route on the cutoff");

    Loaded ap = load(P + "Piano/PN - Ambient Piano.SerumPreset");
    check(ap.err.empty(), "Ambient Piano loads " + ap.err);
    check(ap.v["sa_level"] > 60, "Ambient Piano OSC A level as stored (65): " + std::to_string(ap.v["sa_level"]));
    bool lfo_on_level = false, vel_cut = false;
    for (auto &r : routes(ap)) {
        if ((r.first == SynthPatch::S_LFO1 || r.first == SynthPatch::S_LFO2) && (r.second == SynthPatch::D_BALANCE || r.second == SynthPatch::D_LEVEL)) lfo_on_level = true;
        if (r.first == SynthPatch::S_VEL && r.second == SynthPatch::D_CUTOFF) vel_cut = true;
    }
    check(!lfo_on_level, "Ambient Piano: the wheel-scaled LFO 2 on OSC A's level is not applied (wheel at 0)");
    check(vel_cut, "Ambient Piano: velocity (source 16) -> cutoff kept");
    check(ap.info.find("mod wheel") != std::string::npos, "the info line counts the wheel-scaled route");

    Loaded pd = load(P + "Piano/PN - Piano Dance.SerumPreset");
    check(pd.err.empty(), "Piano Dance loads " + pd.err);
    check(pd.v["syn_fenv"] > 50, "Piano Dance: Env 1 (amp) -> cutoff on the FILTER ENV (2.4.34 dropped source 2)");
    bool wheel_as_amp = false;
    for (auto &r : routes(pd)) if (r.first == SynthPatch::S_AMPENV) wheel_as_amp = true;
    check(!wheel_as_amp, "Piano Dance: no amp-env route invented from the mod wheel");
    bool env_level_b = false;
    for (auto &r : routes(pd)) if (r.first == SynthPatch::S_MODENV && r.second == SynthPatch::D_LEVEL_B) env_level_b = true;
    check(env_level_b && pd.v["sb_level"] < 1, "Piano Dance: OSC B faded in by Env 2 = stored 0 + MOD ENV -> LEVEL B (2.4.35)");

    Loaded eo = load(P + "Bass/Reese/BA - The Even Odds.SerumPreset");
    check(eo.err.empty(), "The Even Odds loads (all sources under -40 dB raised) " + eo.err);
    check(eo.v["sa_level"] > 40, "The Even Odds OSC A audible: " + std::to_string(eo.v["sa_level"]));

    // the noise sample / multisample loaders (files in the fixture): the flute's 15 zones and the FX-only sample
    try {
        Vfs fs;
        SynthSamplePtr s = serum_sample(fs, P + "Woodwind/WIND - Flute.SerumPreset", 0);
        check(s->zones.size() == 15, "Flute multisample: 15 zones (" + std::to_string(s->zones.size()) + ")");
        bool stop = false;
        try { serum_sample(fs, P + "Woodwind/WIND - Flute.SerumPreset", 0, [&] { return stop = true; }); } catch (const ParseError &) {}
        check(stop, "a load can be cancelled");
    } catch (const std::exception &e) { check(false, std::string("Flute multisample: ") + e.what()); }

    std::printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}

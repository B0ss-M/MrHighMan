// 2.4.13: Vital's wavetables and LFO shapes (formats/vital_table.cpp) over whole folders of Vital files.
//   test_vital <folder> [<folder> ...]   every .vitaltable, .vitallfo and .vital under them (a .vitalbank: unzip it first)
// Checks: every file reads; each table renders finite, not silent, 1 or 64 frames; each LFO shape stays in -1..1; known
// shapes come out as they should (Saw Down falls, Sine is a sine, a square holds two levels); the worst render time.
#include <dirent.h>
#include <sys/stat.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "core/parse.hpp"
#include "formats/vital_table.hpp"

using namespace omni;

static int fails = 0;
static void fail(const std::string &what) { std::printf("FAIL %s\n", what.c_str()); fails++; }

static void walk(const std::string &d, std::vector<std::string> &out) {
    DIR *dir = opendir(d.c_str());
    if (!dir) return;
    while (dirent *e = readdir(dir)) {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        std::string p = d + "/" + n;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) walk(p, out);
        else out.push_back(p);
    }
    closedir(dir);
}
static bool ends(const std::string &s, const char *e) { size_t n = std::strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }
static std::string slurp(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static bool check_table(const std::string &what, const std::vector<float> &c, int frames) {
    if (frames != 1 && frames != VITAL_MAX_FRAMES) { fail(what + ": frames " + std::to_string(frames)); return false; }
    if (c.size() != size_t(frames) * VITAL_CYCLE) { fail(what + ": size"); return false; }
    double pk = 0;
    for (float v : c) { if (!std::isfinite(v)) { fail(what + ": not finite"); return false; } pk = std::max(pk, double(std::fabs(v))); }
    if (pk < 1e-4) { fail(what + ": silent"); return false; }
    return true;
}
static bool check_lfo(const std::string &what, const VitalLine &l) {
    const std::vector<float> t = vital_lfo_table(l);
    if (t.size() != size_t(VITAL_LFO_TAB) + 1 || t.back() != t.front()) { fail(what + ": table size / guard"); return false; }
    for (float v : t) if (!std::isfinite(v) || v < -1.0001f || v > 1.0001f) { fail(what + ": out of -1..1"); return false; }
    return true;
}

int main(int argc, char **argv) {
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) walk(argv[i], files);
    int tables = 0, lfos = 0, presets = 0, ptables = 0, plfos = 0, multi = 0;
    double worst = 0;
    std::string worst_name;
    for (const std::string &f : files) {
        if (ends(f, ".vitaltable")) {
            std::vector<float> c; int frames = 0; std::string name, err;
            const double t0 = now_s();
            if (!vital_wavetable_from_text(slurp(f), 0, VITAL_MAX_FRAMES, c, frames, name, err)) { fail(f + ": " + err); continue; }
            const double dt = now_s() - t0;
            if (dt > worst) { worst = dt; worst_name = f; }
            if (check_table(f, c, frames)) tables++;
            multi += frames > 1;
        } else if (ends(f, ".vitallfo")) {
            VitalLine l; std::string err;
            if (!vital_lfo_from_text(slurp(f), 0, l, err)) { fail(f + ": " + err); continue; }
            if (!check_lfo(f, l)) continue;
            lfos++;
            const std::string base = f.substr(f.find_last_of('/') + 1);
            const std::vector<float> t = vital_lfo_table(l);
            if (base == "Saw Down.vitallfo") {   // falls from the top to the bottom
                if (!(t[0] > 0.95f && t[VITAL_LFO_TAB - 1] < -0.95f)) fail("Saw Down: not a falling ramp");
                for (int i = 1; i < VITAL_LFO_TAB; i++) if (t[size_t(i)] > t[size_t(i - 1)] + 1e-6f) { fail("Saw Down: rises at " + std::to_string(i)); break; }
            }
            if (base == "Saw Up.vitallfo" && !(t[0] < -0.95f && t[VITAL_LFO_TAB - 1] > 0.95f)) fail("Saw Up: not a rising ramp");
            if (base == "Triangle.vitallfo") {   // one peak and one trough, half a cycle apart
                int imax = 0, imin = 0;
                for (int i = 0; i < VITAL_LFO_TAB; i++) { if (t[size_t(i)] > t[size_t(imax)]) imax = i; if (t[size_t(i)] < t[size_t(imin)]) imin = i; }
                if (t[size_t(imax)] < 0.95f || t[size_t(imin)] > -0.95f || std::abs(std::abs(imax - imin) - VITAL_LFO_TAB / 2) > 8) fail("Triangle: not a triangle");
            }
            if (base == "Square.vitallfo") {
                int hi = 0, lo = 0;
                for (int i = 0; i < VITAL_LFO_TAB; i++) { hi += t[size_t(i)] > 0.95f; lo += t[size_t(i)] < -0.95f; }
                if (hi < 100 || lo < 100) fail("Square: not two levels (" + std::to_string(hi) + " high, " + std::to_string(lo) + " low)");
            }
            if (base == "Sin.vitallfo") {   // drawn with curves: close to a sine of some phase
                double best = 1e9;
                for (int s = 0; s < VITAL_LFO_TAB; s++) {
                    double e = 0;
                    for (int i = 0; i < VITAL_LFO_TAB; i++) e = std::max(e, std::fabs(t[size_t(i)] - std::sin(2 * M_PI * (i + s) / VITAL_LFO_TAB)));
                    best = std::min(best, e);
                }
                std::printf("Sin.vitallfo: max error from a sine %.3f\n", best);
                if (best > 0.15) fail("Sine: not a sine");
            }
        } else if (ends(f, ".vital")) {
            const std::string text = slurp(f);
            Json j;
            try { j = parse_json(text); } catch (const std::exception &e) { fail(f + ": " + e.what()); continue; }
            presets++;
            const Json &s = j["settings"];
            for (int o = 1; o <= 3; o++) {
                if (s["osc_" + std::to_string(o) + "_on"].as_num() < 0.5) continue;
                const Json &w = s["wavetables"];
                if (w.type != Json::Array || size_t(o - 1) >= w.arr.size()) continue;
                std::vector<float> c; int frames = 0; std::vector<std::string> notes;
                const double t0 = now_s();
                if (!vital_wavetable_render(w.arr[size_t(o - 1)], VITAL_MAX_FRAMES, c, frames, &notes)) { fail(f + " osc " + std::to_string(o) + ": no table"); continue; }
                const double dt = now_s() - t0;
                if (dt > worst) { worst = dt; worst_name = f + " osc " + std::to_string(o); }
                if (check_table(f + " osc " + std::to_string(o), c, frames)) ptables++;
            }
            const Json &ls = s["lfos"];
            for (size_t i = 0; ls.type == Json::Array && i < ls.arr.size(); i++) {
                VitalLine l;
                if (!vital_line_parse(ls.arr[i], l)) { fail(f + " lfo " + std::to_string(i + 1) + ": no shape"); continue; }
                if (check_lfo(f + " lfo " + std::to_string(i + 1), l)) plfos++;
            }
        }
    }
    {   // references
        int n = 0; std::string p;
        if (!vital_ref_parse("vital:2:/a/b c.vital", "vital", n, p) || n != 2 || p != "/a/b c.vital") fail("ref parse vital");
        if (!vital_ref_parse("vlfo:8:/x.vitalbank/y.vital", "vlfo", n, p) || n != 8 || p != "/x.vitalbank/y.vital") fail("ref parse vlfo");
        if (vital_ref_parse("Vital/Saw.vitaltable", "vital", n, p) || vital_ref_parse("vital::/x", "vital", n, p) || vital_ref_parse("vital:1:", "vital", n, p))
            fail("ref parse: a plain path taken as a reference");
    }
    std::printf("tables %d (%d with frames), LFO files %d, presets %d: %d oscillator tables, %d LFO shapes; slowest %.3f s (%s)\n",
                tables, multi, lfos, presets, ptables, plfos, worst, worst_name.c_str());
    std::printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

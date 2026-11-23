// Omni Sampler: Initial Audio Heat Up 3 presets (.heatUpPreset) and their sample folders (2.1.2). Measured 2026-11-05 on
// three STREET CINEMA expansion leads (INVASION, INFINITE, ITS HERE); facts only:
//   - The preset is XML: <PluginState preset= bank= crex= (the expansion) ... intr= ...> with ~200 normalised (0..1)
//     settings, the synth layers' envelopes, LFOs, matrix, arpeggiator (<SeqPreset>) and effects.
//   - intr= is the preset's sample folder name XORed with the repeating key "HEATUP" (byte by byte; the XML escapes
//     what that makes of quotes). The folder sits beside the preset ("stcinec lead-invasion" for INVASION).
//   - The folder holds 01.ignitex .. 24.ignitex: plain FLAC (24-bit stereo, 14.8 s, no loops) whose STREAMINFO sample
//     rate is 0; every frame header says 44100. Pitch-tracked: sample n plays MIDI 24 + 3 (n - 1), C0 .. A5 in minor
//     thirds, so each covers its root and the keys either side (the first and last reach the keyboard's ends).
//   - LINE (drill_II_02_brass) has 14 samples whose measured roots are MIDI 48 through 87 in minor thirds.
//     Its original zone boundaries are unknown. Use the same nearest-root mapping as the leads.
//   - Read here (2.4.10, _toolcache/omni-fixes/translator/HEATUP_MAPPING.md): the samples, their roots and ranges, the
//     master amp envelope matt/mdec/msus/mrel (smAtt.. belong to the atmosphere/noise layer: earlier versions used them
//     by mistake), the filter (fType/fcut/fres) and its envelope (fEAm + fatt.., or an ADSR -> Filter Cutoff matrix
//     row), velocity / mod wheel -> cutoff (Expression rows) and ADSR -> Master Pitch. Times: t = 10 x^2 s (measured).
//     Not read: mono/glide (global in Omni), LFOs (LFO 1 is a CC74-gated sub oscillator; LFO 2 a wheel vibrato), the
//     atmosphere layer (its samples are not shipped), drive, arpeggiator and effects; the preset's own volume.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include "../core/audio.hpp"
#include "../core/parse.hpp"
#include "format.hpp"


namespace omni {
namespace {


thread_local std::string g_folder;   // the sample folder find_beside looks for


std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}


bool is_folder(Volume &vol, const std::string &dir) {
    return !g_folder.empty() && lower(path_name(dir)) == lower(g_folder) && vol.is_dir(dir);
}


std::string unxor(const std::string &s) {
    static const char KEY[] = "HEATUP";
    std::string out(s);
    for (size_t i = 0; i < out.size(); i++) out[i] = char(out[i] ^ KEY[i % 6]);
    return out;
}


// the rate of a FLAC frame header's code (RFC 9639 table); 0 = in the header's tail or unknown
int flac_rate_code(int c) {
    static const int R[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    return c >= 0 && c < 12 ? R[c] : 0;
}


// a .ignitex as decodable FLAC: STREAMINFO's sample rate filled in from the first frame header (44100 when unclear)
PcmPtr decode_ignitex(const BlobPtr &f) {
    std::vector<uint8_t> b = f->all();
    if (b.size() < 42 || std::memcmp(b.data(), "fLaC", 4)) throw ParseError("Heat Up: not a FLAC sample");
    const size_t si = 8;   // STREAMINFO after "fLaC" and its block header
    int rate = int(b[si + 10]) << 12 | int(b[si + 11]) << 4 | b[si + 12] >> 4;
    if (rate == 0) {
        size_t p = 4;
        bool last = false;
        while (!last && p + 4 <= b.size()) {   // skip the metadata blocks to the first frame
            last = b[p] & 0x80;
            p += 4 + (size_t(b[p + 1]) << 16 | size_t(b[p + 2]) << 8 | b[p + 3]);
        }
        rate = p + 3 < b.size() && b[p] == 0xFF && (b[p + 1] & 0xFE) == 0xF8 ? flac_rate_code(b[p + 2] & 15) : 0;
        if (rate == 0) rate = 44100;
        b[si + 10] = uint8_t(rate >> 12);
        b[si + 11] = uint8_t(rate >> 4);
        b[si + 12] = uint8_t((rate & 15) << 4 | (b[si + 12] & 15));
    }
    return decode_audio(*make_mem_blob(std::move(b)));
}


double num(const XmlNode *x, const char *a, double def) {   // a finite attribute, else def
    const double v = x ? x->attr_num(a, def) : def;
    return std::isfinite(v) ? v : def;
}


double env_time(double x) { x = clampd(x, 0, 1); return 10.0 * x * x; }   // seconds (HEATUP_MAPPING.md 3.1)


Envelope adsr(const XmlNode *x, const char *a, const char *d, const char *s, const char *r) {
    Envelope e;
    e.set = true;
    e.attack = env_time(num(x, a, 0));
    e.decay = env_time(num(x, d, 0));
    e.sustain = clampd(num(x, s, 1), 0, 1);
    e.release = env_time(num(x, r, 0));
    return e;
}


Envelope adsr_n(const XmlNode *x, int n) {   // the matrix's ADSR 1 / ADSR 2 (e1At.. / e2At..)
    return n == 1 ? adsr(x, "e1At", "e1De", "e1Su", "e1Re") : adsr(x, "e2At", "e2De", "e2Su", "e2Re");
}


struct Preset {
    std::string name, bank, expansion, folder;
    Envelope amp, pitch_env;
    Filter filter;
    double pitch_depth = 0;
    std::vector<std::string> skipped;
};


Preset read_preset(VolumePtr vol, const std::string &path) {
    std::vector<uint8_t> raw = vol->open(path)->all(size_t(16) << 20);
    std::unique_ptr<XmlNode> xp = parse_xml(std::string(raw.begin(), raw.end()));
    const XmlNode *x = xp.get();
    if (!x || x->name != "PluginState") throw ParseError("Heat Up: no <PluginState>");
    Preset p;
    p.name = x->attr("preset", path_stem(path));
    p.bank = x->attr("bank", "");
    p.expansion = x->attr("crex", "");
    p.folder = unxor(x->attr("intr", ""));
    p.amp = adsr(x, "matt", "mdec", "msus", "mrel");
    if (!x->has_attr("matt")) p.amp.release = 0.25;   // (no master envelope stored: a short release)

    // the matrix: 32 rows, 4 per source (1/2 ADSR, 3/4 LFO, 5..8 Expression 1..4); modAmt in real units
    struct Row { int src, dest; double amt; };
    std::vector<Row> rows;
    if (const XmlNode *m = x->child("Matrix"))
        for (int r = 0; r < 32; r++) {
            const std::string k = "Row" + std::to_string(r);
            if (num(m, (k + "enabled").c_str(), 0) < 0.5) continue;
            const int src = int(num(m, (k + "source").c_str(), 0)), dest = int(num(m, (k + "dest").c_str(), 0));
            if (src > 0 && dest > 0) rows.push_back({src, dest, clampd(num(m, (k + "modAmt").c_str(), 0), -48, 48)});
        }
    auto exp_source = [&](int e) {   // Expression e's controller: 1 velocity, 2 aftertouch, 3 mod wheel, 5 MPE slide
        const XmlNode *sh = x->child("ex" + std::to_string(e) + "Sh");
        return sh ? int(num(sh, "Source", 0)) : 0;
    };
    auto exp_reach = [&](int e) {   // the expression curve's value at full travel (amps="a,b": b)
        const XmlNode *sh = x->child("ex" + std::to_string(e) + "Sh");
        const std::string a = sh ? sh->attr("amps") : "";
        const size_t c = a.find_last_of(',');
        const double v = c == std::string::npos ? 1 : std::atof(a.c_str() + c + 1);
        return std::isfinite(v) ? clampd(v, -1, 1) : 1.0;
    };

    // filter: fType steps of 0.25 = NT, BP, HP, LP, Off (0.75 LP, 1.0 Off: measured); cutoff 20 * 1000^x Hz [G]
    const double ft = num(x, "fType", 0.75), fc = clampd(num(x, "fcut", 1), 0, 1);
    double depth = 2 * clampd(num(x, "fEAm", 0.5), 0, 1) - 1;
    if (std::fabs(depth) < 0.02 || fc >= 0.999) depth = 0;   // (the -0.13 default offset on an open filter: ignored)
    Envelope fenv = adsr(x, "fatt", "fdec", "fsus", "frel");
    if (depth == 0)   // no own envelope amount: an ADSR -> Filter Cutoff row (ORBIT, BALLAZ)
        for (const Row &r : rows)
            if ((r.src == 1 || r.src == 2) && r.dest == 7) { depth = clampd(r.amt, -1, 1); fenv = adsr_n(x, r.src); break; }
    const bool open = fc >= 0.999, shut = fc < 0.02 && depth == 0;   // (open: no filter at all, saves CPU)
    if (ft <= 0.875 && !open && !shut) {
        Filter &f = p.filter;
        f.type = ft > 0.625 ? FilterType::LowPass : ft > 0.375 ? FilterType::HighPass : ft > 0.125 ? FilterType::BandPass : FilterType::BandReject;
        f.poles = 2;
        f.cutoff = std::min(MAX_CUTOFF_HZ, 20 * std::pow(1000.0, fc));
        f.resonance = clampd(num(x, "fres", 0), 0, 1);
        if (depth != 0) { f.env_depth = clampd(depth * 0.9966, -1, 1); f.env = fenv; }   // HU 1 = 11,959 cents
        for (const Row &r : rows) {   // Expression -> Filter Cutoff (velocity / mod wheel)
            if (r.dest != 7 || r.src < 5 || r.src > 8) continue;
            const int c = exp_source(r.src - 4);
            if (c == 1) f.vel_depth = clampd(r.amt * (1 - fc) * 11959.0 / 9600.0, -1, 1);
            else if (c == 3) f.modwheel_depth = clampd(r.amt * exp_reach(r.src - 4) * (1 - fc) * 0.9966, -1, 1);
        }
    }
    for (const Row &r : rows)   // ADSR -> Master Pitch (semitones): a pitch drop / scoop
        if ((r.src == 1 || r.src == 2) && r.dest == 20 && std::fabs(r.amt) > 1e-3) {
            p.pitch_depth = clampd(r.amt * 100 / MAX_ENVELOPE_DEPTH, -1, 1);
            p.pitch_env = adsr_n(x, r.src);
            break;
        }
    if (num(x, "pmon", 0) > 0.5) p.skipped.push_back(num(x, "glid", 0) > 0.001 ? "mono + glide" : "mono");
    for (const Row &r : rows)
        if (r.src == 4 && r.dest == 20) { p.skipped.push_back("vibrato"); break; }
    if (num(x, "noOn", 0) > 0.5) p.skipped.push_back("atmosphere layer");
    return p;
}


}  // namespace


std::string heatup_sample_folder(const std::vector<uint8_t> &head) {
    std::string s(head.begin(), head.end());
    if (s.find("<PluginState") == std::string::npos) return "";
    size_t a = s.find(" intr=\"");
    if (a == std::string::npos) return "";
    a += 7;
    size_t b = s.find('"', a);
    if (b == std::string::npos) return "";
    std::string v, raw = s.substr(a, b - a);   // the XML's escapes undone, then the XOR
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] != '&') { v += raw[i]; continue; }
        size_t e = raw.find(';', i);
        if (e == std::string::npos) { v += raw[i]; continue; }
        std::string ent = raw.substr(i + 1, e - i - 1);
        if (ent == "quot") v += '"';
        else if (ent == "amp") v += '&';
        else if (ent == "lt") v += '<';
        else if (ent == "gt") v += '>';
        else if (ent == "apos") v += '\'';
        else if (!ent.empty() && ent[0] == '#') v += char(ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::strtol(ent.c_str() + 2, nullptr, 16) : std::atoi(ent.c_str() + 1));
        else { v += raw.substr(i, e - i + 1); }
        i = e;
    }
    return unxor(v);
}


namespace {


bool probe_heatup(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    static const char T[] = "<PluginState";
    return std::search(h, h + n, T, T + sizeof T - 1) != h + n;
}


std::vector<PresetInfo> list_heatup(VolumePtr vol, const std::string &path) {
    return {{read_preset(vol, path).name, 0}};
}


Instrument load_heatup(VolumePtr vol, const std::string &path, int) {
    Preset p = read_preset(vol, path);
    if (p.folder.empty()) throw ParseError("Heat Up: this preset names no samples (a synth-only preset)");
    std::string dir, found;
    if (find_ci(*vol, path_join(path_dir(path), p.folder), found) && vol->is_dir(found)) dir = found;
    else {
        static std::string cache;
        g_folder = p.folder;
        dir = find_beside(*vol, path, is_folder, cache);
        g_folder.clear();
    }
    if (dir.empty()) throw ParseError("Heat Up: sample folder \"" + p.folder + "\" not found beside the preset");
    std::vector<std::string> files;
    for (const DirEntry &e : vol->list(dir))
        if (!e.dir && path_ext(e.name) == "ignitex") files.push_back(e.name);
    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
        int x = std::atoi(a.c_str()), y = std::atoi(b.c_str());
        return x != y ? x < y : a < b;
    });
    if (files.empty()) throw ParseError("Heat Up: no .ignitex samples in \"" + p.folder + "\"");


    const bool line_brass = lower(p.folder) == "drill_ii_02_brass";
    if (line_brass) {
        if (files.size() != 14)
            throw ParseError("Heat Up: drill_II_02_brass requires samples 01.ignitex through 14.ignitex");
        for (size_t i = 0; i < files.size(); ++i) {
            const std::string number = std::to_string(i + 1);
            const std::string expected = (i < 9 ? "0" : "") + number + ".ignitex";
            if (lower(files[i]) != expected)
                throw ParseError("Heat Up: drill_II_02_brass expected " + expected + ", found " + files[i]);
        }
    }


    Instrument inst;
    inst.name = p.name;
    inst.format = "Heat Up 3";
    inst.library = p.expansion;
    inst.category = p.bank;
    if (line_brass)
        inst.warnings.push_back("Heat Up: LINE uses measured roots (MIDI 48-87) and estimated key ranges");
    else if (files.size() != 24)
        inst.warnings.push_back(std::to_string(files.size()) + " samples (24 expected): using fallback root mapping");
    std::string skipped;
    for (const std::string &k : p.skipped) skipped += ", " + k;
    inst.warnings.push_back("Heat Up: samples, amp + filter envelopes, filter and pitch envelope read (not read: LFOs, effects, arp" + skipped + ")");
    const int first_root = line_brass ? 48 : 24;
    const int n = int(files.size());
    for (int i = 0; i < n; i++) {
        std::string fp = path_join(dir, files[size_t(i)]);
        auto ref = std::make_shared<SampleRef>();
        ref->name = p.name + " " + path_stem(files[size_t(i)]);
        ref->key = "heatup:" + fp;
        ref->decode = [vol, fp]() { return decode_ignitex(vol->open(fp)); };
        Zone z;
        z.name = ref->name;
        z.sample = ref;
        z.root = n == 1 ? 60 : clampi(first_root + 3 * i, 0, 127);
        z.key_lo = i == 0 ? 0 : clampi(z.root - 1, 0, 127);
        z.key_hi = i == n - 1 ? 127 : clampi(z.root + 1, 0, 127);
        z.amp_env = p.amp;
        z.filter = p.filter;
        z.pitch_env_depth = p.pitch_depth;
        if (p.pitch_depth != 0) z.pitch_env = p.pitch_env;
        inst.zones.push_back(std::move(z));
    }
    finish_instrument(inst);
    return inst;
}


}  // namespace


void register_heatup() {
    register_reader({"Heat Up 3 preset", "heatuppreset", probe_heatup, list_heatup, load_heatup});
}


}  // namespace omni

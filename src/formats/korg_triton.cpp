// Omni Sampler: Korg TRITON content dumps ("triton-content-cards-v1"): the TRITON's ROM and EXB-PCM sample banks as
// FLAC files plus JSON cards for every sample, multisample and program, listed by CSV manifests. Two files open:
//   programs/manifest.csv      the programs, one preset each: both oscillators' multisamples (high / low by velocity),
//                              velocity zones, octave / transpose / tune, levels, pan, amp EG, velocity, bend range
//   multisamples/manifest.csv  the multisamples, one preset each, as the keyboard maps them
// The browser shows programs/manifest.csv as a folder, "TRITON by Category": a view (TritonView) with a folder per
// TRITON category (program_catalog/manifest.csv) holding a small .tpg entry per program, which loads that program.
// Loaded programs carry their category, so SAVE TO LIBRARY files them in Extracted/TRITON/<category>/.
// What the dump's numbers turned out to mean (measured against its audio and the TRITON PCG banks it ships with):
//   - The FLACs say 44.1 kHz but hold 48 kHz audio (the TRITON's rate): at 44.1 every sample is 147 cents flat (the ROM's
//     synth basses measure -145..-152 cents). They play at 48 kHz.
//   - A zone's tune_cents is not a pitch offset: without it the samples sound at their root key (97 of 284 cleanly
//     pitched zones within 35 cents, 7 with it added, 4 with it subtracted). Not applied.
//   - A zone's level is 64 = unity, like a KMP level: x (1 + (level - 64) / 63).
//   - The program cards' common.oscillator_mode is read from the program name's 11th character, not the mode. The real
//     mode (PCG program byte 204, low 2 bits: 0 single, 1 double, 2 drums) was recovered for 1720 of the 1792 programs by
//     matching each to its record in the dump's PCG banks (name and both oscillators' velocity bytes): TRITON_MODES.
//     The others ('?') play both oscillators.
//   - The amp EG bytes match the PCG's (program bytes 354-361): start level, attack time, attack level, decay time,
//     break point level, slope time, sustain level, release time; levels and times 0..99. The filter fields don't line up
//     with the PCG, so the filter is left to the sampler's own.
// Drum kit programs are not listed: their kits play the TRITON's drum samples, and the dump has no drum sample table.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include "../core/audio.hpp"
#include "../core/parse.hpp"
#include "format.hpp"

namespace omni {
namespace {

constexpr int TRITON_RATE = 48000;

// Oscillator mode per program, by bank and program number: '0' single, '1' double, '2' drums, '?' not matched
struct BankModes { const char *bank; const char *modes; };
const BankModes TRITON_MODES[] = {
    {"INT-A", "10112111111111111111210110111111111121111111101111112011111111111111201111111010111111111011111110112111111010111111210111111111"},
    {"INT-B", "11112111111111111110200110111110111121111110011011112111111101011010211111111001111111111111100011112111111111111110211111111011"},
    {"INT-C", "11112111111111111110211110111111111021111111111111112101111110101111010111101110111121111111101111112111111111111111210111111011"},
    {"INT-D", "11112011111110101111111111111111111111111111111111111101111111111110111111111111111111011111101011111111001111111111110111111111"},
    {"INT-E", "???????????????????????????????0000000000000000000000000000000000000000000000?????????????00000000000000000000??????????????????"},
    {"EXB-A", "11111110111111011110010101111111111011111111111111111111111111101111111111111100111101111111110111111111110101111111111111111111"},
    {"EXB-B", "11111101111111100111110111111111111111111101111111111101110011011111111111110001011111111101011111111111111111111011111111110111"},
    {"EXB-C", "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000222220"},
    {"EXB-D", "1121001111101211101100110001111111101111111110111111111111111111111111111111011100111111111211111111110011111111111012222222222?"},
    {"EXB-E", "11101011111111110121111111110111110100010000111101100110011011111111011111110111111111111111111111011111111111111111011111010012"},
    {"EXB-F", "10111110001111112011111111111111111111111111111111111111111100011111110001001000011001111111111111111010000001000011110011111101"},
    {"EXB-G", "11111010101101111111111111111111111111111111111111111111011111100000010111111111111111111100000000000000000011110110111111222222"},
    {"EXB-H", "1111111?1111111111??1111?111??1?111021111111111111112101111110101111010111101110111121111111101111112111111111111111210111111011"},
    {"EXB-I", "1111111111111111222111101011111111011111111011111101111101111110?111111111111111111111111111111111111111111111111111222222?22111"},
};

char program_mode(const std::string &bank, int index) {
    for (auto &b : TRITON_MODES)
        if (bank == b.bank && index >= 0 && index < 128) return b.modes[index];
    return '?';
}

const char PROGRAMS_HEAD[] = "program_id,section,content_name,bank,bank_name,index,name,primary_multisample_id";
const char MULTISAMPLES_HEAD[] = "multisample_id,bank,bank_name,index,name,zones,json";

bool starts_with(const uint8_t *h, size_t n, const char *s) {
    size_t l = std::strlen(s);
    if (n >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF) { h += 3; n -= 3; }   // a UTF-8 BOM
    return n >= l && !std::memcmp(h, s, l);
}

// A CSV file as rows of fields (quoted fields, "" inside quotes, CRLF or LF)
std::vector<std::vector<std::string>> read_csv(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(size_t(64) << 20);
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cur;
    bool quoted = false, any = false;
    size_t i = (d.size() >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) ? 3 : 0;
    for (; i < d.size(); i++) {
        char c = char(d[i]);
        if (quoted) {
            if (c == '"') {
                if (i + 1 < d.size() && d[i + 1] == '"') { cur += '"'; i++; }
                else quoted = false;
            } else cur += c;
        } else if (c == '"') { quoted = true; any = true; }
        else if (c == ',') { row.push_back(cur); cur.clear(); any = true; }
        else if (c == '\r') {}
        else if (c == '\n') {
            if (any || !cur.empty()) { row.push_back(cur); rows.push_back(row); }
            row.clear(); cur.clear(); any = false;
        } else { cur += c; any = true; }
    }
    if (any || !cur.empty()) { row.push_back(cur); rows.push_back(row); }
    return rows;
}

int column(const std::vector<std::string> &head, const char *name) {
    for (size_t i = 0; i < head.size(); i++) if (head[i] == name) return int(i);
    throw ParseError(std::string("TRITON manifest without a ") + name + " column");
}

const std::string &field(const std::vector<std::string> &row, int col) {
    static const std::string none;
    return col >= 0 && size_t(col) < row.size() ? row[size_t(col)] : none;
}

int to_int(const std::string &s, int def = 0) {
    try { return s.empty() ? def : std::stoi(s); } catch (const std::exception &) { return def; }
}

Json read_json(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(size_t(16) << 20);
    return parse_json(std::string(d.begin(), d.end()));
}

// The dump's root: two folders up from a manifest (<root>/programs/manifest.csv)
std::string dump_root(const std::string &manifest) { return path_dir(path_dir(manifest)); }

std::string rel(const std::string &root, const std::string &p) {
    std::string t = trim(p);
    return root.empty() ? t : path_join(root, t);
}

// --- multisamples ------------------------------------------------------------------------------------------------

// Samples are decoded once per load and shared between the oscillators that use them
struct Loader {
    VolumePtr vol;
    std::string root;
    std::map<std::string, SampleRefPtr> refs;
    std::map<std::string, Json> cards;
    std::map<std::string, std::string> ms_paths;   // multisample_id -> its card
    int missing = 0;

    SampleRefPtr sample(const std::string &audio) {
        SampleRefPtr &r = refs[audio];
        if (r) return r;
        r = std::make_shared<SampleRef>();
        r->name = path_stem(audio);
        r->key = "triton:" + std::to_string(reinterpret_cast<uintptr_t>(vol.get())) + ":" + rel(root, audio);
        VolumePtr v = vol;
        std::string p = rel(root, audio);
        r->decode = [v, p]() -> PcmPtr {
            PcmPtr src = decode_audio(*v->open(p));
            auto pcm = std::const_pointer_cast<Pcm>(src);   // just decoded: nothing else holds it
            pcm->rate = TRITON_RATE;
            return pcm;
        };
        return r;
    }

    // the sample's card (loop) beside its audio: samples/audio/<bank>/<id>.flac -> samples/by_bank/<bank>/<id>.json
    const Json &sample_card(const std::string &audio) {
        auto it = cards.find(audio);
        if (it != cards.end()) return it->second;
        Json j;
        std::string card = audio;
        size_t at = card.find("samples/audio/");
        if (at != std::string::npos) card.replace(at, 14, "samples/by_bank/");
        if (path_ext(card) == "flac") card = card.substr(0, card.size() - 4) + "json";
        try { j = read_json(*vol, rel(root, card)); } catch (const std::exception &) {}
        return cards.emplace(audio, std::move(j)).first->second;
    }

    std::string ms_card(int bank, int index) {
        if (ms_paths.empty()) {
            auto rows = read_csv(*vol, rel(root, "multisamples/manifest.csv"));
            if (rows.empty()) throw ParseError("empty TRITON multisample manifest");
            int c_id = column(rows[0], "multisample_id"), c_json = column(rows[0], "json");
            for (size_t i = 1; i < rows.size(); i++) ms_paths[field(rows[i], c_id)] = field(rows[i], c_json);
        }
        char id[32];
        std::snprintf(id, sizeof id, "b%02d_ms%03d", bank, index);
        auto it = ms_paths.find(id);
        return it == ms_paths.end() ? std::string() : it->second;
    }

    // The multisample's zones, each given the oscillator's settings in tmpl; false when the dump lacks it
    bool add_multisample(const std::string &card, const Zone &tmpl, std::vector<Zone> &out, std::string *name = nullptr) {
        Json ms;
        try { ms = read_json(*vol, rel(root, card)); } catch (const std::exception &) { return false; }
        if (name) *name = ms["name"].as_str();
        const Json &zones = ms["zones"];
        bool any = false;
        for (size_t i = 0; i < zones.arr.size(); i++) {
            const Json &zj = zones[i];
            const Json &sj = zj["sample"];
            std::string audio = sj["audio"].type == Json::String ? sj["audio"].as_str() : sj["audio"]["path"].as_str();
            if (audio.empty() || !sj["available"].as_bool(true)) { missing++; continue; }
            Zone z = tmpl;
            z.name = ms["name"].as_str() + " " + std::to_string(i + 1);
            z.key_lo = zj["key_range"]["low"].as_int(0);
            z.key_hi = zj["key_range"]["high"].as_int(127);
            z.root = zj["root_key"].as_int(60);
            int level = zj["level"].as_int(64);
            z.gain_db += 20.0 * std::log10(std::max(0.01, 1.0 + (level - 64) / 63.0));
            z.sample = sample(audio);
            const Json &sc = sample_card(audio);
            const Json &loop = sc["loop"];
            int64_t frames = int64_t(sc["audio"]["frames"].as_num(0));
            if (loop["enabled"].as_bool()) {
                Loop l;
                l.start = int64_t(loop["start_frame"].as_num(0));
                l.end = int64_t(loop["end_frame"].as_num(0));
                if (l.end > l.start && (frames <= 0 || l.end < frames)) z.loops.push_back(l);
            }
            out.push_back(std::move(z));
            any = true;
        }
        return any;
    }
};

bool probe_multisamples(const std::string &, const uint8_t *h, size_t n, uint64_t) { return starts_with(h, n, MULTISAMPLES_HEAD); }

std::vector<PresetInfo> list_multisamples(VolumePtr vol, const std::string &path) {
    auto rows = read_csv(*vol, path);
    if (rows.empty()) return {};
    int c_bank = column(rows[0], "bank_name"), c_name = column(rows[0], "name"), c_zones = column(rows[0], "zones");
    std::vector<PresetInfo> out;
    for (size_t i = 1; i < rows.size(); i++) {
        if (to_int(field(rows[i], c_zones), 1) <= 0) continue;
        out.push_back(PresetInfo{field(rows[i], c_bank) + " " + field(rows[i], c_name), int(i)});
    }
    return out;
}

Instrument load_multisample(VolumePtr vol, const std::string &path, int index) {
    auto rows = read_csv(*vol, path);
    if (index < 1 || size_t(index) >= rows.size()) throw ParseError("no such TRITON multisample");
    Loader ld;
    ld.vol = vol;
    ld.root = dump_root(path);
    Instrument inst;
    inst.format = "Korg TRITON multisample";
    inst.name = field(rows[size_t(index)], column(rows[0], "name"));
    if (!ld.add_multisample(field(rows[size_t(index)], column(rows[0], "json")), Zone{}, inst.zones))
        throw ParseError("the dump has no samples for this multisample");
    if (ld.missing) inst.warnings.push_back(std::to_string(ld.missing) + " samples not in the dump");
    finish_instrument(inst);
    return inst;
}

// --- categories ----------------------------------------------------------------------------------------------------

// program_id -> its TRITON category ("Keyboard", "Woodwind-Reed": '/' would split a folder name); empty when the dump
// has no catalog
std::map<std::string, std::string> read_categories(Volume &vol, const std::string &root) {
    std::map<std::string, std::string> out;
    std::vector<std::vector<std::string>> rows;
    try { rows = read_csv(vol, rel(root, "program_catalog/manifest.csv")); } catch (const std::exception &) { return out; }
    if (rows.empty()) return out;
    int c_id = column(rows[0], "program_id"), c_cat = column(rows[0], "category_name");
    for (size_t i = 1; i < rows.size(); i++) {
        std::string cat = trim(field(rows[i], c_cat));
        for (char &c : cat) if (c == '/' || c == '\\') c = '-';
        if (!cat.empty()) out.emplace(field(rows[i], c_id), cat);
    }
    return out;
}

const char TPG_HEAD[] = "TRITON PROGRAM ";   // a .tpg entry: "TRITON PROGRAM <manifest row>\n"

// programs/manifest.csv seen as category folders. Every other path is the dump's own, read through the parent volume,
// so a program loads from here exactly as from the manifest.
class TritonView : public Volume {
public:
    TritonView(VolumePtr parent, const std::string &manifest) : parent_(std::move(parent)), root_(dump_root(manifest)) {
        auto rows = read_csv(*parent_, manifest);
        if (rows.empty()) throw ParseError("empty TRITON program manifest");
        auto cats = read_categories(*parent_, root_);
        int c_id = column(rows[0], "program_id"), c_bank = column(rows[0], "bank_name"), c_index = column(rows[0], "index"),
            c_name = column(rows[0], "name");
        dirs_[""];
        for (size_t i = 1; i < rows.size(); i++) {
            const std::string &bank = field(rows[i], c_bank), &name = field(rows[i], c_name);
            int index = to_int(field(rows[i], c_index), -1);
            if (program_mode(bank, index) == '2' || !name.compare(0, 12, "InitlProgram")) continue;   // as list_programs
            auto it = cats.find(field(rows[i], c_id));
            std::string cat = it == cats.end() ? "Other" : it->second;
            char num[8];
            std::snprintf(num, sizeof num, "%03d", index);
            std::string file = bank + num + " " + name;
            for (char &c : file) if (c == '/' || c == '\\') c = '-';
            file += ".tpg";
            auto &dir = dirs_[cat];
            if (dir.empty()) dirs_[""].push_back(DirEntry{cat, true, 0});
            dir.push_back(DirEntry{file, false, 32});
            files_[path_join(cat, file)] = int(i);
        }
    }
    std::string kind() const override { return "Korg TRITON categories"; }
    bool view() const override { return true; }
    std::vector<DirEntry> list(const std::string &dir) override {
        auto it = dirs_.find(dir);
        return it != dirs_.end() ? it->second : parent_->list(rel(root_, dir));
    }
    BlobPtr open(const std::string &path) override {
        auto it = files_.find(path);
        if (it == files_.end()) return parent_->open(rel(root_, path));
        std::string s = TPG_HEAD + std::to_string(it->second) + "\n";
        return make_mem_blob(std::vector<uint8_t>(s.begin(), s.end()));
    }
    bool is_dir(const std::string &path) override { return dirs_.count(path) || (!files_.count(path) && parent_->is_dir(rel(root_, path))); }
    bool exists(const std::string &path) override { return dirs_.count(path) || files_.count(path) || parent_->exists(rel(root_, path)); }

private:
    VolumePtr parent_;
    std::string root_;
    std::map<std::string, std::vector<DirEntry>> dirs_;   // "" -> categories, category -> its programs (immutable)
    std::map<std::string, int> files_;                    // "<category>/<entry>.tpg" -> manifest row
};

bool probe_view(const std::string &lower_name, const uint8_t *h, size_t n, uint64_t) {
    return lower_name == "manifest.csv" && starts_with(h, n, PROGRAMS_HEAD);
}
VolumePtr mount_view_blob(BlobPtr) { throw ParseError("TRITON by Category opens from the dump's folder"); }
VolumePtr mount_view(VolumePtr parent, const std::string &inner) { return std::make_shared<TritonView>(parent, inner); }

// --- programs ------------------------------------------------------------------------------------------------------

// TRITON EG time 0..99 to seconds. Korg doesn't publish the curve: an exponential fit, 0 = instant, 50 = 0.23 s,
// 64 = 0.8 s, 99 = 17 s
double eg_time(int v) { return v <= 0 ? 0.0 : 0.003 * std::exp(clampi(v, 0, 99) * 0.087); }

// Amp EG: start level -> attack level (attack time) -> break point (decay time) -> sustain (slope time); release.
// The sampler's envelope is DAHDSR, peaking at 1: the higher of the attack and break levels is the peak.
Envelope amp_eg(const Json &a) {
    double st = a["amp_eg_start_level"].as_num(0), al = a["amp_eg_attack_level"].as_num(99),
           bp = a["amp_eg_break_point_level"].as_num(99), su = a["amp_eg_sustain_level"].as_num(99);
    double at = eg_time(a["amp_eg_attack_time"].as_int(0)), dt = eg_time(a["amp_eg_decay_time"].as_int(0)),
           sl = eg_time(a["amp_eg_slope_time"].as_int(0)), rt = eg_time(a["amp_eg_release_time"].as_int(0));
    Envelope e;
    e.set = true;
    double peak = std::max(1.0, std::max(al, bp));
    e.start_level = clampd(st / peak, 0, 1);
    e.sustain = clampd(su / peak, 0, 1);
    e.release = rt;
    if (bp > al + 2) {                    // a swell: the attack runs on to the break point
        e.attack = at + dt;
        e.decay = std::fabs(bp - su) > 2 ? sl : 0;
    } else if (std::fabs(al - bp) <= 2) { // no fall to the break point: it holds, then slopes to the sustain
        e.attack = at;
        e.hold = std::fabs(bp - su) > 2 ? dt : 0;
        e.decay = std::fabs(bp - su) > 2 ? sl : dt;
    } else if (std::fabs(bp - su) <= 2) { // the slope does nothing: one decay
        e.attack = at;
        e.decay = dt;
    } else {                              // two falling stages in one
        e.attack = at;
        e.decay = dt + sl;
    }
    return e;
}

std::map<std::string, int> osc_params(const Json &osc) {
    std::map<std::string, int> p;
    for (const Json *list : {&osc["oscillator_basic"]["parameters"], &osc["pitch_controls"]["trace"]})
        for (auto &e : list->arr) {
            std::string n = e["name"].as_str();
            size_t u = n.find('_');
            if (u != std::string::npos && !n.compare(0, 3, "osc")) n = n.substr(u + 1);   // "osc1_osc_high_bank" -> "osc_high_bank"
            p[n] = e["value"].as_int();
        }
    return p;
}

bool probe_programs(const std::string &, const uint8_t *h, size_t n, uint64_t) { return starts_with(h, n, PROGRAMS_HEAD); }

std::vector<PresetInfo> list_programs(VolumePtr vol, const std::string &path) {
    auto rows = read_csv(*vol, path);
    if (rows.empty()) return {};
    int c_bank = column(rows[0], "bank_name"), c_index = column(rows[0], "index"), c_name = column(rows[0], "name");
    std::vector<PresetInfo> out;
    for (size_t i = 1; i < rows.size(); i++) {
        const std::string &bank = field(rows[i], c_bank), &name = field(rows[i], c_name);
        int index = to_int(field(rows[i], c_index), -1);
        if (program_mode(bank, index) == '2' || !name.compare(0, 12, "InitlProgram")) continue;   // drum kits, empty slots
        char num[8];
        std::snprintf(num, sizeof num, "%03d", index);
        out.push_back(PresetInfo{bank + num + " " + name, int(i)});
    }
    return out;
}

Instrument load_program(VolumePtr vol, const std::string &path, int index) {
    auto rows = read_csv(*vol, path);
    if (index < 1 || size_t(index) >= rows.size()) throw ParseError("no such TRITON program");
    const auto &row = rows[size_t(index)];
    std::string bank = field(row, column(rows[0], "bank_name"));
    int pnum = to_int(field(row, column(rows[0], "index")), -1);
    char mode = program_mode(bank, pnum);
    if (mode == '2') throw ParseError("drum kit programs are not supported: the dump has no drum sample table");
    Loader ld;
    ld.vol = vol;
    ld.root = dump_root(path);
    Json prog = read_json(*vol, rel(ld.root, field(row, column(rows[0], "json"))));
    Instrument inst;
    inst.format = "Korg TRITON program";
    inst.name = prog["name"].as_str(field(row, column(rows[0], "name")));
    inst.library = "TRITON";
    {
        auto cats = read_categories(*vol, ld.root);
        auto it = cats.find(field(row, column(rows[0], "program_id")));
        inst.category = it == cats.end() ? "Other" : it->second;
    }
    const Json &oscs = prog["oscillators"];
    size_t n_osc = mode == '0' ? 1 : std::min<size_t>(2, oscs.arr.size());
    int absent = 0;
    for (size_t o = 0; o < n_osc && o < oscs.arr.size(); o++) {
        const Json &osc = oscs[o];
        auto p = osc_params(osc);
        auto get = [&](const char *k, int def) { auto it = p.find(k); return it == p.end() ? def : it->second; };
        const Json &amp = osc["amp"]["fields"];
        Zone t;
        t.group = 0;
        const Json &pitch = osc["pitch"];
        t.tune = pitch["octave_semitones"].as_num(0) + pitch["transpose_semitones"].as_num(0) + pitch["tune_cents"].as_num(0) / 100.0;
        int amp_level = amp["amp_level"].as_int(127);
        t.gain_db = 20.0 * std::log10(std::max(1, amp_level) / 127.0);
        int pan = osc["amp"]["pan_raw"].as_int(64);
        t.pan = pan <= 0 ? 0 : clampd((pan - 64) / 63.0, -1, 1);   // 0 = random: centred
        t.amp_vel_depth = clampd(amp["amp_velocity_intensity"].as_num(0) / 99.0, 0, 1);
        t.amp_env = amp_eg(amp);
        t.bend_up = 100 * get("osc_pitch_bend_plus_x", 2);
        t.bend_down = 100 * get("osc_pitch_bend_minus_x", -2);
        // the oscillator's velocity zone (double mode), split between its low and high multisample
        int vlo = 1, vhi = 127;
        if (n_osc > 1) { vlo = clampi(get("osc_velocity_zone_bottom", 1), 1, 127); vhi = clampi(get("osc_velocity_zone_top", 127), vlo, 127); }
        int sw = clampi(get("osc_velocity_multisample_switch_low_to_high", 1), 1, 128);   // velocities from here play the high one
        struct Part { const char *which; int lo, hi; };
        for (Part part : {Part{"high", std::max(vlo, sw), vhi}, Part{"low", vlo, std::min(vhi, sw - 1)}}) {
            if (part.lo > part.hi) continue;
            std::string w = part.which;
            int msb = get(("osc_" + w + "_bank").c_str(), 0), msn = get(("osc_" + w + "_sample_no").c_str(), 0);
            Zone z = t;
            z.vel_lo = part.lo;
            z.vel_hi = part.hi;
            int level = get(("osc_" + w + "_level").c_str(), 127);
            z.gain_db += 20.0 * std::log10(std::max(1, level) / 127.0);
            z.reverse = get(("osc_" + w + "_reverse").c_str(), 0) != 0;
            std::string card = ld.ms_card(msb, msn);
            if (card.empty() || !ld.add_multisample(card, z, inst.zones)) absent++;
        }
    }
    if (absent) inst.warnings.push_back(std::to_string(absent) + " multisamples not in the dump (sampler RAM or user banks)");
    if (ld.missing) inst.warnings.push_back(std::to_string(ld.missing) + " samples not in the dump");
    if (mode == '?' && n_osc > 1) inst.warnings.push_back("oscillator mode not known: both oscillators play");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("the dump has no samples for this program");
    return inst;
}

bool probe_tpg(const std::string &, const uint8_t *h, size_t n, uint64_t) { return starts_with(h, n, TPG_HEAD); }

std::vector<PresetInfo> list_tpg(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

// a program entry of the category view: the program of that manifest row (the view reads the dump through)
Instrument load_tpg(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->head(64);
    int row = std::atoi(std::string(d.begin(), d.end()).c_str() + std::strlen(TPG_HEAD));
    return load_program(vol, "programs/manifest.csv", row);
}

}  // namespace

void register_korg_triton() {
    register_image_type({"TRITON by Category", probe_view, mount_view_blob, mount_view, "csv"});
    register_reader({"Korg TRITON program", "tpg", probe_tpg, list_tpg, load_tpg});
    register_reader({"Korg TRITON programs", "csv", probe_programs, list_programs, load_program});
    register_reader({"Korg TRITON multisamples", "csv", probe_multisamples, list_multisamples, load_multisample});
}

}  // namespace omni

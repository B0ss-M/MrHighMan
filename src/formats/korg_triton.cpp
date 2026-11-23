// Omni Sampler: Korg TRITON content dumps ("triton-content-cards-v1"): the TRITON's ROM and EXB-PCM sample banks as
// FLAC files plus JSON cards for every sample, multisample and program, listed by CSV manifests. Two files open:
//   programs/manifest.csv      the programs, one preset each: both oscillators' multisamples (high / low by velocity),
//                              velocity zones, octave / transpose / tune, levels, pan, amp EG, velocity, bend range
//   multisamples/manifest.csv  the multisamples, one preset each, as the keyboard maps them
// The browser shows programs/manifest.csv as a folder, "TRITON by Category": a view (TritonView) with a folder per
// TRITON category (program_catalog/manifest.csv) holding a small .tpg entry per program, which loads that program.
// Loaded programs carry their category, so SAVE TO LIBRARY files them in Extracted/TRITON/<category>/.
// The view also has "GM" and "GM Variations": the TRITON's GM bank (256 programs), which the dump lacks. It is read from
// the TRITON OS into korg_triton_gm.inc (tests/gen_triton_gm.py, which lists the PCG program offsets) and plays the
// dump's ROM multisamples.
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
//   - The amp EG bytes match the PCG's (program bytes 356-363): start level, attack time, attack level, decay time,
//     break point level, slope time, sustain level, release time; levels and times 0..99. The filter fields don't line up
//     with the PCG, so the filter is left to the sampler's own.
// Drum kit programs are not listed: their kits play the TRITON's drum samples, and the dump has no drum sample table.
#include <algorithm>
#include "triton_extreme_pcm.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
#include "triton_classic.inc"
#include "triton_rom.inc"

struct Loader {
    BlobPtr extreme;
    std::shared_ptr<ClassicRom> classic;
    std::string extreme_id;
    VolumePtr vol;
    std::string root;
    std::map<std::string, SampleRefPtr> refs;
    std::map<std::string, Json> cards;
    std::map<std::string, std::string> ms_paths;   // multisample_id -> its card
    int missing = 0;
    // bank 1 (sampler RAM: a PCG's KSC) multisample n into out, each zone given tmpl's settings; null = none
    std::function<bool(int n, const Zone &tmpl, std::vector<Zone> &out)> ram;

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

// --- the GM bank ---------------------------------------------------------------------------------------------------

// The TRITON's GM programs aren't in the dump; they are in its OS, read out once into korg_triton_gm.inc. They play the
// ROM multisamples (bank 0), which are.
struct TritonGm {
    const char *name;
    int mode;   // 0 single, 1 double
    // high ms, level, low ms, level, velocity switch, zone bottom, top, pitch, tune, bend +, -, amp level, amp
    // velocity, amp EG x 8, pan
    int16_t osc[2][22];
};
#include "korg_triton_gm.inc"

constexpr int GM_FIRST_VARIATION = 128;   // 0..127: GM 1..128; then the variations

std::string gm_entry(int k) {
    char num[16];
    std::snprintf(num, sizeof num, "%03d", k < GM_FIRST_VARIATION ? k + 1 : k - GM_FIRST_VARIATION + 1);
    std::string file = std::string(k < GM_FIRST_VARIATION ? "GM" : "GMv") + num + " " + TRITON_GM[k].name;
    for (char &c : file) if (c == '/' || c == '\\') c = '-';
    return file;
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

const char TPG_HEAD[] = "TRITON PROGRAM ";   // a .tpg entry: "TRITON PROGRAM <manifest row or G<n>>\n"

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
            files_[path_join(cat, file)] = std::to_string(i);
        }
        // the GM bank, from the OS: it needs only the dump's ROM multisamples
        if (parent_->exists(rel(root_, "multisamples/manifest.csv")))
            for (int k = 0; k < int(sizeof TRITON_GM / sizeof TRITON_GM[0]); k++) {
                std::string cat = k < GM_FIRST_VARIATION ? "GM" : "GM Variations", file = gm_entry(k) + ".tpg";
                auto &dir = dirs_[cat];
                if (dir.empty()) dirs_[""].push_back(DirEntry{cat, true, 0});
                dir.push_back(DirEntry{file, false, 32});
                files_[path_join(cat, file)] = "G" + std::to_string(k);
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
        std::string s = TPG_HEAD + it->second + "\n";
        return make_mem_blob(std::vector<uint8_t>(s.begin(), s.end()));
    }
    bool is_dir(const std::string &path) override { return dirs_.count(path) || (!files_.count(path) && parent_->is_dir(rel(root_, path))); }
    bool exists(const std::string &path) override { return dirs_.count(path) || files_.count(path) || parent_->exists(rel(root_, path)); }

private:
    VolumePtr parent_;
    std::string root_;
    std::map<std::string, std::vector<DirEntry>> dirs_;   // "" -> categories, category -> its programs (immutable)
    std::map<std::string, std::string> files_;            // "<category>/<entry>.tpg" -> manifest row, or "G<n>" GM program
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
// eg: start level, attack time, attack level, decay time, break point level, slope time, sustain level, release time
Envelope amp_eg(const int eg[8]) {
    Envelope e;
    e.set = true;
    e.start_level = clampd(eg[0] / 99.0, 0, 1);
    e.attack = eg_time(eg[1]);
    e.attack_level = clampd(eg[2] / 99.0, 0, 1);
    e.decay = eg_time(eg[3]);
    e.break_level = clampd(eg[4] / 99.0, 0, 1);
    e.slope_time = eg_time(eg[5]);
    e.sustain = clampd(eg[6] / 99.0, 0, 1);
    e.release = eg_time(eg[7]);
    return e;
}

// One oscillator's settings, from a program card or the GM table
struct Osc {
    int ms_bank[2] = {0, 0}, ms[2] = {0, 0}, level[2] = {127, 127};   // [0] high, [1] low multisample
    bool offset[2] = {};
    bool reverse[2] = {false, false};
    int vel_switch = 1, vel_lo = 1, vel_hi = 127;   // velocities from vel_switch play the high multisample
    double tune = 0;                                // semitones
    int bend_up = 2, bend_down = -2, amp_level = 127, amp_vel = 0, pan = 64;
    int eg[8] = {0, 0, 99, 0, 99, 0, 99, 0};
};

// The oscillator's zones into inst; double: the oscillator's velocity zone applies. Counts multisamples the dump lacks.
void add_osc(Loader &ld, Instrument &inst, const Osc &o, bool dbl, int &absent) {
    if (o.amp_level <= 0) return;   // 2.4.27 (velocity audit #2): amp level 0 is silent on the TRITON (was -43 dB)
    Zone t;
    t.group = 0;
    t.tune = o.tune;
    t.gain_db = 20.0 * std::log10(std::max(1, o.amp_level) / 127.0);
    t.pan = o.pan <= 0 ? 0 : clampd((o.pan - 64) / 63.0, -1, 1);   // 0 = random: centred
    t.amp_vel_depth = clampd(o.amp_vel / 99.0, 0, 1);
    t.amp_env = amp_eg(o.eg);
    t.bend_up = 100 * o.bend_up;
    t.bend_down = 100 * o.bend_down;
    int vlo = 1, vhi = 127;
    if (dbl) { vlo = clampi(o.vel_lo, 1, 127); vhi = clampi(o.vel_hi, vlo, 127); }
    // 2.4.27: TRITON Extreme Parameter Guide 1-2c: "Notes played with a velocity stronger than this value will be sounded
    // by the High multisample" [1...127]: High = switch + 1 and up, Low = up to the switch (was off by one: High from it)
    // A switch of 1 is how the factory programs say "always High": their Low slot is then a leftover (ROM banks 2 / 3 /
    // 5 / 127 or unused entries, 44 programs in the Extreme preload), reachable only at velocity 1: High plays everywhere
    int sw = clampi(o.vel_switch, 1, 127);
    const int lo[2] = {std::max(vlo, sw == 1 ? 1 : sw + 1), vlo}, hi[2] = {vhi, sw == 1 ? 0 : std::min(vhi, sw)};
    for (int w = 0; w < 2; w++) {
        if (lo[w] > hi[w]) continue;
        if (o.level[w] <= 0) continue;   // 2.4.27 (velocity audit #2): a multisample at level 0 plays nothing (was -43 dB)
        Zone z = t;
        z.vel_lo = lo[w];
        z.vel_hi = hi[w];
        z.gain_db += 20.0 * std::log10(std::max(1, o.level[w]) / 127.0);
        z.reverse = o.reverse[w];
        if (o.ms_bank[w] == 1) {
            if (!ld.ram || !ld.ram(o.ms[w], z, inst.zones)) absent++;
            continue;
        }
        if (add_extreme_rom(ld.extreme, ld.classic, ld.extreme_id, o.ms_bank[w], o.ms[w], o.offset[w], z, inst.zones)) continue;
        if ((ld.extreme || ld.classic) && ld.root.empty()) { ++absent; continue; }
        std::string card = ld.ms_card(o.ms_bank[w], o.ms[w]);
        if (card.empty() || !ld.add_multisample(card, z, inst.zones)) absent++;
    }
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
        const Json &amp = osc["amp"]["fields"], &pitch = osc["pitch"];
        Osc s;
        for (int w = 0; w < 2; w++) {
            std::string k = w ? "osc_low_" : "osc_high_";
            s.ms_bank[w] = get((k + "bank").c_str(), 0);
            s.ms[w] = get((k + "sample_no").c_str(), 0);
            s.level[w] = get((k + "level").c_str(), 127);
            s.reverse[w] = get((k + "reverse").c_str(), 0) != 0;
        }
        s.vel_switch = get("osc_velocity_multisample_switch_low_to_high", 1);
        s.vel_lo = get("osc_velocity_zone_bottom", 1);
        s.vel_hi = get("osc_velocity_zone_top", 127);
        s.tune = pitch["octave_semitones"].as_num(0) + pitch["transpose_semitones"].as_num(0) + pitch["tune_cents"].as_num(0) / 100.0;
        s.bend_up = get("osc_pitch_bend_plus_x", 2);
        s.bend_down = get("osc_pitch_bend_minus_x", -2);
        s.amp_level = amp["amp_level"].as_int(127);
        s.amp_vel = amp["amp_velocity_intensity"].as_int(0);
        s.pan = osc["amp"]["pan_raw"].as_int(64);
        const char *eg_keys[8] = {"amp_eg_start_level", "amp_eg_attack_time", "amp_eg_attack_level", "amp_eg_decay_time",
                                  "amp_eg_break_point_level", "amp_eg_slope_time", "amp_eg_sustain_level", "amp_eg_release_time"};
        for (int e = 0; e < 8; e++) s.eg[e] = amp[eg_keys[e]].as_int(s.eg[e]);
        add_osc(ld, inst, s, n_osc > 1, absent);
    }
    if (absent) inst.warnings.push_back(std::to_string(absent) + " multisamples not in the dump (sampler RAM or user banks)");
    if (ld.missing) inst.warnings.push_back(std::to_string(ld.missing) + " samples not in the dump");
    if (mode == '?' && n_osc > 1) inst.warnings.push_back("oscillator mode not known: both oscillators play");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("the dump has no samples for this program");
    return inst;
}

Instrument load_gm(VolumePtr vol, const std::string &root, int k) {
    if (k < 0 || k >= int(sizeof TRITON_GM / sizeof TRITON_GM[0])) throw ParseError("no such TRITON GM program");
    const TritonGm &g = TRITON_GM[k];
    Loader ld;
    ld.vol = vol;
    ld.root = root;
    Instrument inst;
    inst.format = "Korg TRITON program";
    inst.name = g.name;
    inst.library = "TRITON";
    inst.category = "GM";
    int absent = 0;
    for (int o = 0; o <= g.mode; o++) {
        const int16_t *v = g.osc[o];
        Osc s;
        s.ms[0] = v[0];
        s.level[0] = v[1];
        s.ms[1] = v[2];
        s.level[1] = v[3];
        s.vel_switch = v[4];
        s.vel_lo = v[5];
        s.vel_hi = v[6];
        s.tune = v[7] + v[8] / 100.0;
        s.bend_up = v[9];
        s.bend_down = v[10];
        s.amp_level = v[11];
        s.amp_vel = v[12];
        for (int e = 0; e < 8; e++) s.eg[e] = v[13 + e];
        s.pan = v[21];
        add_osc(ld, inst, s, g.mode == 1, absent);
    }
    if (absent) inst.warnings.push_back(std::to_string(absent) + " multisamples not in the dump");
    if (ld.missing) inst.warnings.push_back(std::to_string(ld.missing) + " samples not in the dump");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("the dump has no samples for this program");
    return inst;
}

// --- PCG banks -----------------------------------------------------------------------------------------------------

// A TRITON .PCG ("KORG", product 0x50; the TRITON LE / TR write 0x63): big-endian chunks (id, u32 size, body) from 0x10,
// PCG1 holding DIV1, INI1, PRG1, CMB1, DKT1, ARP1 ...; PRG1 holds one PBK1 per program bank: u32 count, u32 record
// size, u32 bank (0 - 4 = A - E, 0x2000n = H + n), then the records. A TRITON / Studio / Extreme program is 540 bytes
// (laid out in tests/gen_triton_gm.py); an LE / TR one 444 bytes, the same fields 96 bytes earlier from the
// oscillators on. Multisample banks (OSC block bytes 232 / 236, checked on 14,322 oscillators against the dump's
// program cards): 0 ROM, 1 sampler RAM (the KSC saved with the PCG), 2 - 10 EXB-PCM01 - 09, the dump's bank numbers.
// Drum kit programs are not played (the dump has no drum sample table).
constexpr size_t PCG_PROG = 540, PCG_PROG_LE = 444, LE_SHIFT = 96;

bool is_triton_pcg(const uint8_t *h, size_t n) {
    return n >= 0x18 && !std::memcmp(h, "KORG", 4) && (h[4] == 0x50 || h[4] == 0x63) && !std::memcmp(h + 0x10, "PCG1", 4);
}

struct PcgProg { std::string bank, name; int index = 0, mode = 0; std::vector<uint8_t> rec; size_t shift = 0; };

std::vector<PcgProg> read_pcg(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(size_t(32) << 20);
    if (!is_triton_pcg(d.data(), d.size())) throw ParseError("not a Korg TRITON .PCG");
    auto u32 = [&](size_t o) { return o + 4 <= d.size() ? be32(&d[o]) : 0u; };
    std::vector<PcgProg> out;
    // the chunks of [o, end): id -> body offset, size
    auto each = [&](size_t o, size_t end, const std::function<void(const char *, size_t, size_t)> &f) {
        while (o + 8 <= end) {
            size_t size = u32(o + 4);
            if (o + 8 + size > end) break;
            f(reinterpret_cast<const char *>(&d[o]), o + 8, size);
            o += 8 + size;
        }
    };
    each(0x10, d.size(), [&](const char *id, size_t o, size_t size) {
        if (std::memcmp(id, "PCG1", 4)) return;
        each(o, o + size, [&](const char *id2, size_t o2, size_t size2) {
            if (std::memcmp(id2, "PRG1", 4)) return;
            each(o2, o2 + size2, [&](const char *id3, size_t o3, size_t size3) {
                if (std::memcmp(id3, "PBK1", 4) || size3 < 12) return;
                size_t count = u32(o3), rs = u32(o3 + 4);
                uint32_t bank = u32(o3 + 8);
                if (rs != PCG_PROG && rs != PCG_PROG_LE) return;
                char b[8];
                std::snprintf(b, sizeof b, "%c", bank >= 0x20000 ? 'H' + int(bank & 0xFF) : 'A' + int(bank & 0xFF));
                for (size_t i = 0; i < count && 12 + (i + 1) * rs <= size3; i++) {
                    const uint8_t *r = &d[o3 + 12 + i * rs];
                    PcgProg p;
                    p.bank = b;
                    p.index = int(i);
                    p.name = trim(std::string(reinterpret_cast<const char *>(r), strnlen(reinterpret_cast<const char *>(r), 16)));
                    p.shift = rs == PCG_PROG ? 0 : LE_SHIFT;
                    p.mode = r[204 - p.shift] & 3;
                    p.rec.assign(r, r + rs);
                    out.push_back(std::move(p));
                }
            });
        });
    });
    return out;
}

bool pcg_listed(const PcgProg &p) {
    return p.mode != 2 && !p.name.empty() && p.name.compare(0, 11, "InitialProg") && p.name.compare(0, 12, "InitlProgram");
}

bool is_dump(Volume &vol, const std::string &dir) {
    return vol.exists(path_join(dir, "multisamples/manifest.csv")) && vol.exists(path_join(dir, "samples/manifest.csv"));
}
std::string cached_dump;

// The sampler RAM a PCG was saved with: the KSC of the same name beside it (or in a folder of that name), else the
// only KSC beside it when the PCG is the only one there (a sample library's ALL_INST.KSC)
std::string find_pcg_ksc(Volume &vol, const std::string &pcg) {
    std::string dir = path_dir(pcg), stem = path_stem(pcg), found;
    if (path_ext(stem) == "pcg") stem = path_stem(stem);   // "X.PCG.PCG"
    for (const std::string &p : {path_join(dir, stem + ".KSC"), path_join(dir, stem + "/" + stem + ".KSC")})
        if (find_ci(vol, p, found) && !vol.is_dir(found)) return found;
    std::string only;
    int pcgs = 0, kscs = 0;
    try {
        for (const DirEntry &e : vol.list(dir)) {
            if (e.dir) continue;
            std::string ext = path_ext(e.name);
            if (ext == "pcg") pcgs++;
            else if (ext == "ksc") { kscs++; only = path_join(dir, e.name); }
        }
    } catch (const std::exception &) {}
    return pcgs == 1 && kscs == 1 ? only : "";
}

bool probe_pcg(const std::string &, const uint8_t *h, size_t n, uint64_t) { return is_triton_pcg(h, n); }

std::vector<PresetInfo> list_pcg(VolumePtr vol, const std::string &path) {
    std::vector<PresetInfo> out;
    std::vector<PcgProg> ps = read_pcg(*vol, path);
    for (size_t i = 0; i < ps.size(); i++) {
        if (!pcg_listed(ps[i])) continue;
        char num[8];
        std::snprintf(num, sizeof num, "%03d ", ps[i].index);
        out.push_back(PresetInfo{ps[i].bank + num + ps[i].name, int(i)});
    }
    return out;
}

Instrument load_pcg(VolumePtr vol, const std::string &path, int index) {
    std::vector<PcgProg> ps = read_pcg(*vol, path);
    if (index < 0 || size_t(index) >= ps.size()) throw ParseError("no such TRITON program");
    const PcgProg &p = ps[size_t(index)];
    if (p.mode == 2) throw ParseError("drum kit programs are not supported: the dump has no drum sample table");
    Loader ld;
    ld.vol = vol;
    ld.root = find_beside(*vol, path, is_dump, cached_dump);
    std::string bin;
    if (find_ci(*vol, path_join(path_dir(path), "TritonExtremePcm.bin"), bin)) {
        auto blob = vol->open(bin);
        if (blob->size() == 222822400) {
            const auto signature = blob->head(16);
            const uint8_t expected[] = {0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x1,0x2,0x4,0x8,0x10,0x20,0x40,0x80,0x0};
            if (signature.size() == sizeof expected && !std::memcmp(signature.data(), expected, sizeof expected)) {
                ld.extreme = blob;
                ld.extreme_id = "triton-extreme:" + std::to_string(reinterpret_cast<uintptr_t>(vol.get())) + ":" + bin;
            }
        }
    }
    if (!ld.extreme) {
        ld.classic = find_classic_rom(*vol, path_dir(path));
        ld.extreme_id = "triton-classic:" + std::to_string(reinterpret_cast<uintptr_t>(vol.get())) + ":" + path_dir(path);
    }
    if (ld.root.empty() && !ld.extreme && !ld.classic)
        throw ParseError("TRITON ROM samples not found: put the TRITON content dump (its multisamples/ and samples/ folders) "
                         "beside the .PCG or in the Plugin Library");
    std::string ksc = find_pcg_ksc(*vol, path);
    const FormatReader *kr = ksc.empty() ? nullptr : find_reader(*vol, ksc);
    std::vector<int> kmp_line;   // RAM multisample n = the script's n-th KMP; the KSC reader takes the script line
    if (kr) {
        std::vector<uint8_t> d = vol->open(ksc)->head(1 << 20);
        std::string cur;
        int line = 0;
        auto take = [&] {
            cur = trim(cur);
            if (!cur.empty() && cur[0] != '#') { if (path_ext(cur) == "kmp") kmp_line.push_back(line); line++; }
            cur.clear();
        };
        for (uint8_t c : d) { if (c == '\r' || c == '\n') take(); else cur += char(c); }
        take();
    }
    std::vector<std::string> ram_warnings;
    ld.ram = [&](int n, const Zone &t, std::vector<Zone> &out) {
        if (!kr || n < 0 || n >= int(kmp_line.size())) return false;
        Instrument m;
        try { m = kr->load(vol, ksc, kmp_line[size_t(n)]); } catch (const std::exception &e) { ram_warnings.push_back(e.what()); return false; }
        for (Zone z : m.zones) {
            z.group = t.group;
            z.tune += t.tune;
            z.reverse = z.reverse != t.reverse;
            z.gain_db += t.gain_db;
            z.pan = clampd(z.pan + t.pan, -1, 1);
            // 2.4.27 (velocity audit #3): keep the KMP's own velocity layers inside the oscillator's range (was: overwritten,
            // so every layer of a multi-layer KMP played at once)
            z.vel_lo = std::max(z.vel_lo, t.vel_lo);
            z.vel_hi = std::min(z.vel_hi, t.vel_hi);
            if (z.vel_lo > z.vel_hi) continue;
            z.amp_vel_depth = t.amp_vel_depth;
            z.amp_env = t.amp_env;
            z.bend_up = t.bend_up;
            z.bend_down = t.bend_down;
            out.push_back(std::move(z));
        }
        return !m.zones.empty();
    };
    const uint8_t *r = p.rec.data();
    auto s8 = [](uint8_t b) { return int(int8_t(b)); };
    Instrument inst;
    inst.format = "Korg TRITON PCG";
    inst.name = p.name;
    inst.library = "TRITON";
    int absent = 0;
    for (int o = 0; o <= (p.mode == 1 ? 1 : 0); o++) {
        const uint8_t *osc = r + (230 - p.shift) + o * 154;
        Osc s;
        s.offset[0] = (osc[0] & 0x80) != 0;
        s.offset[1] = (osc[4] & 0x80) != 0;
        s.ms[0] = be16(osc) & 0x3fff;
        s.reverse[0] = (osc[0] & 0x40) != 0;
        s.ms_bank[0] = osc[2];
        s.level[0] = osc[3];
        s.ms[1] = be16(osc + 4) & 0x3fff;
        s.reverse[1] = (osc[4] & 0x40) != 0;
        s.ms_bank[1] = osc[6];
        s.level[1] = osc[7];
        s.vel_switch = osc[9];
        s.vel_lo = osc[10];
        s.vel_hi = osc[11];
        s.tune = s8(osc[32]) * 12 + s8(osc[33]) + (s8(osc[34]) * 256 + osc[35]) / 100.0;
        s.bend_up = s8(osc[46]);
        s.bend_down = s8(osc[47]);
        s.amp_level = osc[116];
        s.amp_vel = s8(osc[117]);
        for (int e = 0; e < 8; e++) s.eg[e] = osc[126 + e];
        s.pan = osc[149] & 0x7F;
        add_osc(ld, inst, s, p.mode == 1, absent);
    }
    if (absent) inst.warnings.push_back(std::to_string(absent) + (ksc.empty() ? " multisamples not available (sampler RAM: no KSC beside the PCG; or not in the dump)"
                                                                             : " multisamples not available (not in the KSC or the dump)"));
    for (auto &w : ram_warnings) inst.warnings.push_back(w);
    if (ld.missing) inst.warnings.push_back(std::to_string(ld.missing) + " samples not in the dump");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("nothing to play in " + p.name + ": its multisamples are not available");
    return inst;
}

bool probe_tpg(const std::string &, const uint8_t *h, size_t n, uint64_t) { return starts_with(h, n, TPG_HEAD); }

std::vector<PresetInfo> list_tpg(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

// a program entry of the category view: the program of that manifest row, or "G<n>" a GM program (the view reads the
// dump through)
Instrument load_tpg(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->head(64);
    std::string s(d.begin(), d.end());
    const char *arg = s.c_str() + std::min(s.size(), std::strlen(TPG_HEAD));
    if (*arg == 'G') return load_gm(vol, "", std::atoi(arg + 1));
    return load_program(vol, "programs/manifest.csv", std::atoi(arg));
}

}  // namespace

void register_korg_triton() {
    register_image_type({"TRITON by Category", probe_view, mount_view_blob, mount_view, "csv"});
    register_reader({"Korg TRITON program", "tpg", probe_tpg, list_tpg, load_tpg});
    register_reader({"Korg TRITON programs", "csv", probe_programs, list_programs, load_program});
    register_reader({"Korg TRITON multisamples", "csv", probe_multisamples, list_multisamples, load_multisample});
    register_reader({"Korg TRITON PCG", "pcg", probe_pcg, list_pcg, load_pcg});
}

}  // namespace omni

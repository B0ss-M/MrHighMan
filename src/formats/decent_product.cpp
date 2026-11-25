// Omni Sampler 2.4.38: Decent Sampler PRODUCTS (.dsproduct). A sealed product starts "DECENTSAMPLER001" and is encrypted
// (its preset and, in some products, its samples: reports/DECENT_SAMPLER_1.43_ANALYSIS.md). Omni never decrypts it: each
// is listed as "<name> (sealed)" and says so when opened.
//
// Paper Drums (the user, 2026-11-24): its 12 presets are sealed but its samples are plain WAVs, so a bundle with Paper
// Drums' sample layout (SAMPLES/KICKS/1_KICK.wav ...) also lists "Paper Drums kit (from the samples)" on its
// Default.dsproduct (or, without one, its first product by name): built from paper_drums_assets.hpp (the user's list),
// on the GM drum notes (the user: "GM drum map"; Paper Drums' own key map is unknown):
//   each articulation on one key, its 11 takes round robin, the dry and room mics as two zones of each take (room -6 dB
//   [G]), the snare buzz as a quiet round-robin layer on every key (-18 dB [G]: Paper Drums' buzz "triggered by all keys"),
//   one-shots; the closed / pedal / open hats choke each other. The IR reverbs are not used (Omni has no convolution).
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "../core/audio.hpp"
#include "format.hpp"
#include "paper_drums_assets.hpp"

namespace omni {
namespace {

constexpr char SEAL[] = "DECENTSAMPLER001";
constexpr int KIT = 1 << 20;   // the kit's preset index (the product itself is 0)

bool probe_product(const std::string &, const uint8_t *head, size_t n, uint64_t) {
    return n >= 16 && !std::memcmp(head, SEAL, 16);
}

std::string stem(const std::string &path) {
    std::string n = path_name(path);
    const size_t dot = n.rfind('.');
    return dot == std::string::npos ? n : n.substr(0, dot);
}

bool paper_drums_bundle(Volume &vol, const std::string &dir) {
    return vol.exists(path_join(dir, "SAMPLES/KICKS/1_KICK.wav")) && vol.exists(path_join(dir, "SAMPLES/ROOM/1_KICKROOM.wav"));
}

// the product that carries the kit: Default.dsproduct, else the first .dsproduct by name (one kit per bundle)
bool carries_kit(Volume &vol, const std::string &path) {
    const std::string dir = path_dir(path);
    if (!paper_drums_bundle(vol, dir)) return false;
    std::vector<std::string> products;
    for (const DirEntry &e : vol.list(dir)) {
        if (e.dir || e.name.empty() || e.name[0] == '.') continue;
        std::string l = e.name;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (l.size() > 10 && l.compare(l.size() - 10, 10, ".dsproduct") == 0) products.push_back(e.name);
    }
    if (products.empty()) return false;
    std::sort(products.begin(), products.end());
    const auto def = std::find(products.begin(), products.end(), "Default.dsproduct");
    return path_name(path) == (def != products.end() ? *def : products.front());
}

std::vector<PresetInfo> list_product(VolumePtr vol, const std::string &path) {
    std::vector<PresetInfo> out;
    if (carries_kit(*vol, path)) out.push_back({"Paper Drums kit (from the samples)", KIT});
    out.push_back({stem(path) + " (sealed)", 0});
    return out;
}

// GM drum notes for Paper Drums' 25 articulations [G: the percussion above 53 are the nearest GM sounds]
int gm_note(std::string_view a) {
    static const std::pair<const char *, int> MAP[] = {
        {"KICKS", 36}, {"SIDESTICKS", 37}, {"SNARES", 38}, {"CLAPS", 39}, {"ROLLS", 40}, {"HATS_CLOSED", 42},
        {"LOWTOMS", 43}, {"HATS_PEDAL", 44}, {"HATS_OPEN", 46}, {"HITOMS", 48}, {"RIDES", 51}, {"RIDE_BELLS", 53},
        {"TAMB_METAL", 54}, {"CAJON_HIGH", 60}, {"CAJON_LOW", 61}, {"HIGHHD", 63}, {"LOWHD", 64}, {"TONGUEDRUM", 65},
        {"GLASS_LOW", 72}, {"GLASS_MED", 73}, {"GLASS_HIGH", 74}, {"STICK", 75}, {"BLOCK_HIGH", 76}, {"BLOCK_LOW", 77},
        {"FINGER_CYMBAL", 81}};
    for (const auto &m : MAP) if (a == m.first) return m.second;
    return -1;
}

Instrument load_kit(VolumePtr vol, const std::string &path) {
    const std::string dir = path_dir(path);
    Instrument inst;
    inst.name = "Paper Drums";
    inst.format = "Decent Sampler (Paper Drums samples)";
    inst.library = "Paper Drums";
    std::map<std::string, SampleRefPtr> refs;
    int missing = 0;
    std::string first_missing;
    auto ref = [&](std::string_view rel) -> SampleRefPtr {
        const std::string p = path_join(dir, std::string(rel));
        auto it = refs.find(p);
        if (it != refs.end()) return it->second;
        if (!vol->exists(p)) { if (!missing++) first_missing = std::string(rel); return refs[p] = nullptr; }
        return refs[p] = file_sample_ref(vol, p);
    };
    constexpr unsigned TAKES = 11;
    constexpr int HATS = 1;   // choke group
    std::vector<int> keys;
    auto zone = [&](SampleRefPtr s, int key, unsigned take, double gain_db, const std::string &name) {
        Zone z;
        z.name = name;
        z.sample = std::move(s);
        z.key_lo = z.key_hi = z.root = key;
        z.key_tracking = 0;
        z.one_shot = true;
        z.gain_db = gain_db;
        z.play_logic = PlayLogic::RoundRobin;
        z.seq_position = int(take);
        z.seq_length = int(TAKES);
        return z;
    };
    // takes follow the file names: the drums count 1..11, the percussion 0..10 (BLOCK_HIGH.wav, BLOCK_HIGH1.wav ...):
    // each articulation's round robin starts at its lowest take
    std::map<std::string_view, unsigned> first_take;
    for (const auto &h : paper_drums::hits) {
        auto it = first_take.find(h.articulation);
        if (it == first_take.end() || h.take < it->second) first_take[h.articulation] = h.take;
    }
    for (const auto &h : paper_drums::hits) {
        const int key = gm_note(h.articulation);
        const unsigned take = h.take - first_take[h.articulation] + 1;   // 1..11
        if (key < 0 || take < 1 || take > TAKES) continue;
        const std::string name = std::string(h.articulation) + " " + std::to_string(take);
        const bool hat = h.articulation.compare(0, 5, "HATS_") == 0;
        if (SampleRefPtr s = ref(h.dry)) {
            Zone z = zone(s, key, take, 0.0, name);
            if (hat) z.exclusive_group = HATS;
            inst.zones.push_back(std::move(z));
        }
        if (SampleRefPtr s = ref(h.room)) {
            Zone z = zone(s, key, take, -6.0, name + " room");
            if (hat) z.exclusive_group = HATS;
            inst.zones.push_back(std::move(z));
        }
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    }
    // the snare buzz on every key, its own round robin (takes 1..11 of the buzz list)
    const unsigned nbuzz = unsigned(sizeof(paper_drums::buzz) / sizeof(paper_drums::buzz[0]));
    for (int key : keys)
        for (unsigned t = 1; t <= nbuzz && t <= TAKES; t++)
            if (SampleRefPtr s = ref(paper_drums::buzz[t - 1])) inst.zones.push_back(zone(s, key, t, -18.0, "SNARE_BUZZ " + std::to_string(t)));
    if (inst.zones.empty()) throw ParseError("Paper Drums: none of the samples found, e.g. " + first_missing);
    if (missing) inst.warnings.push_back("Paper Drums: " + std::to_string(missing) + " sample(s) not found, e.g. " + first_missing);
    inst.warnings.push_back("Paper Drums: built from the samples (the presets are sealed); GM drum keys, the IR reverbs not used");
    finish_instrument(inst);
    return inst;
}

Instrument load_product(VolumePtr vol, const std::string &path, int index) {
    if (index == KIT && carries_kit(*vol, path)) return load_kit(vol, path);
    throw ParseError("Decent Sampler: \"" + stem(path) + "\" is a sealed (encrypted) Decent Sampler product: it plays only in Decent Sampler");
}

}  // namespace

void register_decent_product() {
    register_reader({"Decent Sampler product", "dsproduct", probe_product, list_product, load_product});
}

}  // namespace omni

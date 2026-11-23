// Omni Sampler: Native Instruments Battery 1-3 kits (.kit, XML "BatteryPatch"). Each SampleSlot is a cell: label, key
// range, root, volume (linear), pan (0..1), mute group and its Sample layers (file + velocity range + layer volume).
// The WAVs sit in "<kit> samples" beside the kit (Battery's "save kit with samples") or next to it. Cells play as
// drum one-shots at their own pitch; Battery's envelope / modulation fields are not read.
#include <cmath>
#include <cstring>
#include "../core/parse.hpp"
#include "format.hpp"

namespace omni {
namespace {

std::vector<PresetInfo> list_kit(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

double lin_db(double v) { return v <= 1e-6 ? -120 : 20 * std::log10(v); }

// A sample by file name: in "<kit> samples", then beside the kit; exact name first, then case-insensitive
std::string find_sample(Volume &vol, const std::string &dir, const std::string &kit_stem, const std::string &file) {
    std::string name = path_name(file);   // Battery may store a path; the name is what travels with the kit
    for (const std::string &d : {path_join(dir, kit_stem + " samples"), dir}) {
        std::string p = path_join(d, name);
        if (vol.exists(p)) return p;
        try {
            for (const DirEntry &e : vol.list(d))
                if (!e.dir && lower(e.name) == lower(name)) return path_join(d, e.name);
        } catch (const std::exception &) {}
    }
    return "";
}

Instrument load_kit(VolumePtr vol, const std::string &path, int) {
    std::vector<uint8_t> d = vol->open(path)->all(8 << 20);
    auto root = parse_xml(std::string(d.begin(), d.end()));
    if (!root || root->name != "BatteryPatch") throw ParseError("not a Battery kit");
    Instrument inst;
    inst.format = "NI Battery kit";
    inst.name = path_stem(path);
    double kit_db = lin_db(root->attr_num("volume", 1));
    std::string dir = path_dir(path), stem = path_stem(path);
    for (const XmlNode *slot : root->all("SampleSlot")) {
        int lo = clampi(int(slot->attr_num("lowKey", 36)), 0, 127), hi = clampi(int(slot->attr_num("highKey", lo)), 0, 127);
        int rk = clampi(int(slot->attr_num("rootKey", lo)), 0, 127);
        double pitch = slot->attr_num("pitch", 1);   // a playback ratio; 1 = as recorded
        double cell_db = lin_db(slot->attr_num("volume", 1));
        double pan = clampd(slot->attr_num("pan", 0.5) * 2 - 1, -1, 1);
        int mute = int(slot->attr_num("muteGroup", 0));
        std::string label = slot->attr("label");
        int group = int(inst.groups.size());
        inst.groups.push_back(Group{label, Trigger::Attack});
        for (const XmlNode *s : slot->all("Sample")) {
            std::string file = s->attr("file");
            if (file.empty()) continue;
            std::string p = find_sample(*vol, dir, stem, file);
            if (p.empty()) { inst.warnings.push_back("missing sample " + file); continue; }
            Zone z;
            z.group = group;
            z.name = label.empty() ? path_stem(file) : label;
            z.sample = file_sample_ref(vol, p);
            z.key_lo = std::min(lo, hi); z.key_hi = std::max(lo, hi);
            z.vel_lo = clampi(int(s->attr_num("lowVelo", 0)), 0, 127);
            z.vel_hi = clampi(int(s->attr_num("highVelo", 127)), 0, 127);
            z.root = rk;
            z.key_tracking = lo == hi ? 0 : 1;   // a one-key cell plays as recorded; a key range is pitched
            if (pitch > 0 && std::fabs(pitch - 1) > 1e-6) z.tune = 12 * std::log2(pitch);
            z.gain_db = kit_db + cell_db + lin_db(s->attr_num("layerVolume", 1));
            z.pan = pan;
            z.exclusive_group = mute > 0 ? mute : 0;
            z.one_shot = true;
            inst.zones.push_back(std::move(z));
        }
    }
    finish_instrument(inst);
    return inst;
}

bool probe_kit(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    std::string head(reinterpret_cast<const char *>(h), std::min<size_t>(n, 512));
    return head.find("<BatteryPatch") != std::string::npos;
}

}  // namespace

void register_ni_battery() {
    register_reader({"NI Battery kit", "kit", probe_kit, list_kit, load_kit});
}

}  // namespace omni

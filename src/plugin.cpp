// Omni Sampler: mpc_engine() glue: parameters, the file browser, project state. The sampler and loader do the work.
//
// Threads: set_param/get_param come from the host (UI thread; get_param of "<key>_on" and of numeric params also from
// the audio thread inside the wrapper's housekeeping), midi/render from the audio thread, loading on the loader
// thread. Browser state is guarded by ui_mutex; everything the audio thread may read is atomic.
#include <algorithm>
#include <chrono>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <sys/stat.h>
extern "C" {
#include "engine.h"
}
#include "engine/loader.hpp"
#include "engine/sampler.hpp"
#include "formats/format.hpp"
#include "formats/omni_native.hpp"

using namespace omni;

namespace {

constexpr int ROWS = 10;

enum class RowKind { Up, Drive, Folder, Image, File, Preset };
struct Row {
    std::string label, path;
    RowKind kind;
    int preset = 0;
};

// Numeric parameters: key, default (in parameter units). Order is irrelevant here; params.json fixes VST indices.
struct NumParam { const char *key; float def; };
const NumParam NUM_PARAMS[] = {
    {"volume", 0}, {"pan", 0}, {"transpose", 0}, {"tune", 0},
    {"cutoff", 100}, {"resonance", 0}, {"filter_type", 0}, {"filter_vel", 0},
    {"attack", 0}, {"decay", 100}, {"sustain", 100}, {"release", 0},
    {"vel_sens", 100}, {"bend", 0}, {"polyphony", 48}, {"voice_mode", 0}, {"glide", 0}, {"interp", 2},
    {"rev_mix", 0}, {"rev_size", 60}, {"rev_damp", 40}, {"drive", 0},
    {"mem_limit", 512}, {"prog_change", 1}, {"pad_vel", 100}, {"pad_base", 36}, {"auto_extract", 1},
    {"target_slot", 0}, {"layer_mode", 0}, {"ks_base", 24}, {"auto_loop", 0},
    {"slot1_lo", 0}, {"slot1_hi", 127}, {"slot1_vol", 0}, {"slot1_tune", 0}, {"slot1_mute", 0},
    {"slot2_lo", 0}, {"slot2_hi", 127}, {"slot2_vol", 0}, {"slot2_tune", 0}, {"slot2_mute", 0},
    {"slot3_lo", 0}, {"slot3_hi", 127}, {"slot3_vol", 0}, {"slot3_tune", 0}, {"slot3_mute", 0},
    {"slot4_lo", 0}, {"slot4_hi", 127}, {"slot4_vol", 0}, {"slot4_tune", 0}, {"slot4_mute", 0},

    {"lfo1_wave", 0}, {"lfo1_rate", 50}, {"lfo1_sync", 0}, {"lfo1_retrig", 0},
    {"lfo2_wave", 1}, {"lfo2_rate", 35}, {"lfo2_sync", 0}, {"lfo2_retrig", 0},
    {"mod1_src", 1}, {"mod1_dst", 1}, {"mod1_amt", 0},
    {"mod2_src", 2}, {"mod2_dst", 2}, {"mod2_amt", 0},
    {"mod3_src", 3}, {"mod3_dst", 2}, {"mod3_amt", 0},
    {"mod4_src", 6}, {"mod4_dst", 2}, {"mod4_amt", 0},
    {"mod5_src", 8}, {"mod5_dst", 5}, {"mod5_amt", 0},
    {"mod6_src", 7}, {"mod6_dst", 2}, {"mod6_amt", 0},
    {"mod7_src", 4}, {"mod7_dst", 4}, {"mod7_amt", 0},
    {"mod8_src", 1}, {"mod8_dst", 4}, {"mod8_amt", 0},

};
constexpr int NUM_COUNT = int(sizeof NUM_PARAMS / sizeof NUM_PARAMS[0]);

struct Omni {
    Sampler sampler;
    Vfs vfs;
    Loader loader{sampler, vfs};
    std::string data_dir;
    std::atomic<float> values[NUM_COUNT];

    std::mutex ui_mutex;
    std::string dir;                 // full path of the folder or image shown; "" = the drives list
    std::string preset_file;
    std::string sel_path;            // the row last tapped (file or preset): highlighted until another is tapped
    // the Plugin Library's location: the user's choice (an SD card or SSD folder, saved in <data_dir>/library_path.txt
    // for every project), else <data_dir>/library on the internal storage
    std::mutex lib_mutex;
    std::string lib_choice;
    int sel_preset = -1;
    // diagnostics: with /sdcard/omni-debug present, the state is written to /tmp/omni-debug.txt every second
    std::thread diag_thread;
    std::atomic<bool> diag_quit{false};
    // the skin's displays, computed by the diag/display thread (30 Hz), read anywhere (audio thread included)
    std::atomic<uint8_t> disp_wave[48]{}, disp_ptile[32]{}, disp_ltile[32]{};         // non-empty: listing the presets of this file
    std::vector<Row> rows;
    int page = 0;
    std::string info_text;           // the last tapped item
    std::string browse_msg;
    std::atomic<int> row_on[ROWS];
    std::atomic<int> trig_on[16];   // one per TRIGGERS entry
    // the skin's 16 pads: a tap queues a note (set_param), the audio thread plays it with a fixed gate (render)
    std::atomic<int> pad_req[16];
    int pad_gate[16] = {}, pad_note[16] = {};   // audio thread only
    std::atomic<int> pad_base_rev{0};
    std::atomic<int> pad_panic{0};       // PANIC: the audio thread drops the pads' pending notes and gates     // trigger buttons held (the wrapper releases them): their highlight
    // browser actions run on their own thread: listing folders, mounting images and reading a file's presets can
    // take a while, and set_param must return at once whichever thread the host calls it from
    std::thread br_thread;
    std::mutex br_qm;
    std::condition_variable br_cv;
    std::deque<std::pair<std::string, std::string>> br_q;
    bool br_quit = false;
    std::atomic<int> br_busy{0};
    std::atomic<uint32_t> ui_rev{0};

    Omni() {
        for (int i = 0; i < NUM_COUNT; i++) values[i].store(NUM_PARAMS[i].def);
        for (auto &r : row_on) r.store(0);
        for (auto &t : trig_on) t.store(0);
        for (auto &p : pad_req) p.store(0);
    }
};

int num_index(const char *key) {
    for (int i = 0; i < NUM_COUNT; i++) if (!std::strcmp(NUM_PARAMS[i].key, key)) return i;
    return -1;
}

float num(Omni *o, const char *key) { int i = num_index(key); return i < 0 ? 0 : o->values[i].load(); }

std::string default_library(Omni *o) { return path_join(o->data_dir, "library"); }
bool host_dir(const std::string &p) { struct stat st; return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
// the chosen library when its drive is there, else the internal one
std::string library_dir(Omni *o) {
    std::lock_guard<std::mutex> lock(o->lib_mutex);
    return host_dir(o->lib_choice) ? o->lib_choice : default_library(o);
}
std::string library_choice(Omni *o) {
    std::lock_guard<std::mutex> lock(o->lib_mutex);
    return o->lib_choice;
}

// LFO rate knob 0..100 -> 0.02..20 Hz (logarithmic)
float lfo_hz(float v) { return 0.02f * std::pow(1000.0f, std::max(0.0f, std::min(100.0f, v)) / 100.0f); }

void apply_settings(Omni *o) {
    Settings &s = o->sampler.settings;
    s.volume_db.store(num(o, "volume"));
    s.pan.store(num(o, "pan") / 100.0f);
    s.transpose.store(num(o, "transpose"));
    s.tune_cents.store(num(o, "tune"));
    s.cutoff_hz.store(20.0f * std::pow(1000.0f, num(o, "cutoff") / 100.0f));
    s.resonance.store(num(o, "resonance") / 100.0f);
    s.filter_type.store(num(o, "filter_type"));
    s.filter_vel.store(num(o, "filter_vel") / 100.0f);
    s.attack_add.store(num(o, "attack") / 1000.0f);
    s.decay_scale.store(num(o, "decay") / 100.0f);
    s.sustain_scale.store(num(o, "sustain") / 100.0f);
    s.release_add.store(num(o, "release") / 1000.0f);
    s.vel_sens.store(num(o, "vel_sens") / 100.0f);
    s.bend_range.store(num(o, "bend"));
    s.polyphony.store(int(num(o, "polyphony")));
    s.voice_mode.store(int(num(o, "voice_mode")));
    s.glide_s.store(num(o, "glide") / 1000.0f);
    s.interpolation.store(int(num(o, "interp")));
    s.reverb_mix.store(num(o, "rev_mix") / 100.0f);
    s.reverb_size.store(num(o, "rev_size") / 100.0f);
    s.reverb_damp.store(num(o, "rev_damp") / 100.0f);
    s.drive.store(num(o, "drive") / 100.0f);
    for (int i = 0; i < 2; i++) {
        char k[24];
        std::snprintf(k, sizeof k, "lfo%d_wave", i + 1); s.lfo_wave[i].store(int(num(o, k) + 0.5f));
        std::snprintf(k, sizeof k, "lfo%d_sync", i + 1); s.lfo_sync[i].store(int(num(o, k) + 0.5f));
        std::snprintf(k, sizeof k, "lfo%d_retrig", i + 1); s.lfo_retrig[i].store(int(num(o, k) + 0.5f));
        std::snprintf(k, sizeof k, "lfo%d_rate", i + 1); s.lfo_rate[i].store(lfo_hz(num(o, k)));
    }
    for (int i = 0; i < MOD_SLOTS; i++) {
        char k[24];
        std::snprintf(k, sizeof k, "mod%d_src", i + 1); s.mod_src[i].store(int(num(o, k) + 0.5f));
        std::snprintf(k, sizeof k, "mod%d_dst", i + 1); s.mod_dst[i].store(int(num(o, k) + 0.5f));
        std::snprintf(k, sizeof k, "mod%d_amt", i + 1); s.mod_amt[i].store(num(o, k) / 100.0f);
    }
    for (int i = 0; i < 4; i++) {
        char k[24];
        std::snprintf(k, sizeof k, "slot%d_lo", i + 1); int lo = int(num(o, k) + 0.5f);
        std::snprintf(k, sizeof k, "slot%d_hi", i + 1); int hi = int(num(o, k) + 0.5f);
        s.slot_lo[i].store(std::min(lo, hi)); s.slot_hi[i].store(std::max(lo, hi));
        std::snprintf(k, sizeof k, "slot%d_vol", i + 1); s.slot_gain[i].store(std::pow(10.0f, num(o, k) / 20.0f));
        std::snprintf(k, sizeof k, "slot%d_tune", i + 1); s.slot_tune[i].store(num(o, k));
        std::snprintf(k, sizeof k, "slot%d_mute", i + 1); s.slot_mute[i].store(num(o, k) > 0.5f ? 1 : 0);
    }
    s.layer_mode.store(int(num(o, "layer_mode") + 0.5f));
    s.ks_base.store(int(num(o, "ks_base") + 0.5f));
    o->loader.set_target(int(num(o, "target_slot") + 0.5f));
    o->loader.set_auto_loop(num(o, "auto_loop") > 0.5f);
    o->loader.set_budget_mb(int(num(o, "mem_limit")));
    o->loader.set_extract(int(num(o, "auto_extract") + 0.5f), path_join(library_dir(o), "Extracted"));
}

// ---------------------------------------------------------------------------------------------------------------
// browser


bool is_dir_host(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool listable_ext(const std::string &ext) {
    if (ext.empty()) return false;
    for (auto &f : formats()) {
        std::string exts = std::string(" ") + f.exts + " ";
        if (exts.find(" " + ext + " ") != std::string::npos) return true;
    }
    return false;
}

bool image_ext(const std::string &ext) {
    static const char *exts[] = {"iso", "img", "bin", "cdr", "hda", "hdf", "hds", "out", "sdk", "hfe", "imd", "dsk",
                                 "ima", "raw", "toc", "e3", "eiv", "e4", "ei", "em", "emu", "akai", "dmg", "nrg", "mdf"};
    for (auto *e : exts) if (ext == e) return true;
    return false;
}

std::string short_path(Omni *o, const std::string &p) {
    std::string lib = library_dir(o);
    if (p.compare(0, lib.size(), lib) == 0) return "Library" + p.substr(lib.size());
    if (p.compare(0, 7, "/sdcard") == 0) return "Internal" + p.substr(7);
    if (p.compare(0, 6, "/media") == 0) return p.substr(6);
    return p;
}

void rebuild_on(Omni *o) {
    LoadedInfo li = o->loader.info();
    for (int r = 0; r < ROWS; r++) {
        size_t i = size_t(o->page * ROWS + r);
        int on = 0;
        if (i < o->rows.size()) {
            const Row &row = o->rows[i];
            // the tapped row, or what is loaded (an extracted instrument: its source)
            const std::string &lp = li.source.empty() ? li.path : li.source;
            int lpre = li.source.empty() ? li.preset : li.source_preset;
            if (row.kind == RowKind::Preset) on = (row.path == o->sel_path && row.preset == o->sel_preset) || (row.path == lp && row.preset == lpre);
            else if (row.kind == RowKind::File) on = row.path == o->sel_path || row.path == lp;
        }
        o->row_on[r].store(on);
    }
    o->ui_rev.fetch_add(1);
}

void list_drives(Omni *o) {
    o->rows.clear();
    o->dir.clear();
    o->preset_file.clear();
    o->page = 0;
    o->rows.push_back({"Plugin Library", library_dir(o), RowKind::Drive});
    if (is_dir_host("/sdcard")) o->rows.push_back({"Internal", "/sdcard", RowKind::Drive});
    std::vector<DirEntry> media;
    try { media = o->vfs.list("/media"); } catch (const std::exception &) {}
    for (auto &m : media) if (m.dir) o->rows.push_back({m.name, "/media/" + m.name, RowKind::Drive});
    const char *extra = std::getenv("OMNI_SAMPLER_ROOT");   // tests and desktop builds
    if (extra && *extra) o->rows.push_back({extra, extra, RowKind::Drive});
    rebuild_on(o);
}

void list_folder(Omni *o, const std::string &dir) {
    std::vector<DirEntry> entries;
    try {
        entries = o->vfs.list(dir);
    } catch (const std::exception &e) {
        o->browse_msg = e.what();
        o->ui_rev.fetch_add(1);
        return;
    }
    o->dir = dir;
    o->preset_file.clear();
    o->page = 0;
    o->rows.clear();
    o->rows.push_back({"..", path_dir(dir), RowKind::Up});
    for (auto &e : entries) {
        if (e.name.empty() || e.name[0] == '.') continue;   // hidden files, macOS "._" resource forks
        std::string full = path_join(dir, e.name), ext = path_ext(e.name);
        if (e.dir) o->rows.push_back({"[" + e.name + "]", full, RowKind::Folder});
        else if (listable_ext(ext) || ext == "omnipatch") o->rows.push_back({e.name, full, RowKind::File});
        else if (image_ext(ext)) o->rows.push_back({"<" + e.name + ">", full, RowKind::Image});
    }
    o->browse_msg.clear();
    rebuild_on(o);
}

void list_presets(Omni *o, const std::string &file, const std::vector<PresetInfo> &presets) {
    o->preset_file = file;
    o->page = 0;
    o->rows.clear();
    o->rows.push_back({".. " + path_name(file), o->dir, RowKind::Up});
    for (size_t i = 0; i < presets.size(); i++) o->rows.push_back({presets[i].name, file, RowKind::Preset, int(i)});
    rebuild_on(o);
}

void load_patch(Omni *o, const std::string &path);

void tap_row(Omni *o, int index) {
    if (index < 0 || index >= int(o->rows.size())) return;
    Row row = o->rows[size_t(index)];
    switch (row.kind) {
    case RowKind::Up:
        if (!o->preset_file.empty()) { list_folder(o, o->dir); break; }
        if (o->dir.empty() || o->dir == "/" || row.path.empty()) list_drives(o);
        else {
            // leaving a drive root goes back to the drives list
            bool root = false;
            for (const char *r : {"/sdcard", "/media"}) if (o->dir == r) root = true;
            if (o->dir == library_dir(o) || root || (o->dir.compare(0, 7, "/media/") == 0 && std::count(o->dir.begin(), o->dir.end(), '/') == 2))
                list_drives(o);
            else list_folder(o, row.path);
        }
        break;
    case RowKind::Drive:
        if (row.path == library_dir(o)) mkdir(row.path.c_str(), 0755);
        list_folder(o, row.path);
        break;
    case RowKind::Folder:
    case RowKind::Image:
        list_folder(o, row.path);
        if (row.kind == RowKind::Image && o->dir != row.path) o->info_text = "Not a disk image this sampler reads";
        break;
    case RowKind::File: {
        if (path_ext(row.path) == "omnipatch") {
            o->sel_path = row.path;
            o->sel_preset = 0;
            load_patch(o, row.path);
            rebuild_on(o);
            break;
        }
        std::vector<PresetInfo> presets;
        try {
            Location loc = o->vfs.resolve(row.path);
            const FormatReader *r = find_reader(*loc.volume, loc.inner);
            if (!r) { o->info_text = "Not readable: " + path_name(row.path); break; }
            presets = r->list(loc.volume, loc.inner);
            o->info_text = std::string(r->name) + ": " + std::to_string(presets.size()) + (presets.size() == 1 ? " instrument" : " instruments");
        } catch (const std::exception &e) {
            o->info_text = e.what();
            break;
        }
        o->sel_path = row.path;
        o->sel_preset = 0;
        if (presets.size() > 1) list_presets(o, row.path, presets);
        else if (presets.size() == 1) o->loader.request(row.path, 0);
        rebuild_on(o);
        break;
    }
    case RowKind::Preset:
        o->sel_path = row.path;
        o->sel_preset = row.preset;
        o->loader.request(row.path, row.preset);
        rebuild_on(o);
        break;
    }
    o->ui_rev.fetch_add(1);
}

// Step through the presets of the loaded file, then on through the loadable files of its folder.
void step_program(Omni *o, int delta) {
    LoadedInfo li = o->loader.info();
    if (li.path.empty()) return;
    int n = int(li.presets.size());
    // an extracted instrument steps through its source (the disk image's presets / folder) when that is still there
    if (!li.source.empty()) {
        bool reachable = false;
        try { reachable = o->vfs.resolve(li.source).volume->exists(o->vfs.resolve(li.source).inner); } catch (const std::exception &) {}
        if (reachable) { li.path = li.source; li.preset = li.source_preset; n = std::max(1, li.source_presets); }
    }
    if (n > 1) {
        int p = (li.preset + delta + n) % n;
        o->loader.request(li.path, p);
        return;
    }
    std::vector<DirEntry> entries;
    try { entries = o->vfs.list(path_dir(li.path)); } catch (const std::exception &) { return; }
    std::vector<std::string> files;
    for (auto &e : entries) if (!e.dir && listable_ext(path_ext(e.name))) files.push_back(path_join(path_dir(li.path), e.name));
    if (files.empty()) return;
    auto it = std::find(files.begin(), files.end(), li.path);
    int i = it == files.end() ? 0 : int(it - files.begin());
    i = (i + delta + int(files.size())) % int(files.size());
    o->loader.request(files[size_t(i)], 0);
}

// ---------------------------------------------------------------------------------------------------------------
// instrument settings: the sound parameters an extracted instrument remembers (not the slots, pads or system)

bool sound_param(const char *key) {
    static const char *const NOT[] = {"mem_limit", "prog_change", "auto_extract", "pad_vel", "pad_base", "target_slot", "layer_mode",
                                      "ks_base", "auto_loop"};
    for (const char *n : NOT) if (!std::strcmp(key, n)) return false;
    return std::strncmp(key, "slot", 4) != 0;
}

// a patch also keeps the slots' key ranges, levels and tuning and the layer mode
bool patch_param(const char *key) {
    if (sound_param(key)) return true;
    if (!std::strcmp(key, "layer_mode") || !std::strcmp(key, "ks_base")) return true;
    return !std::strncmp(key, "slot", 4) && std::isdigit(static_cast<unsigned char>(key[4]));
}

std::string settings_json(Omni *o, bool (*want)(const char *) = sound_param) {
    std::string s = "{";
    char buf[96];
    for (int i = 0; i < NUM_COUNT; i++) {
        if (!want(NUM_PARAMS[i].key)) continue;
        std::snprintf(buf, sizeof buf, "%s\"%s\":%g", s.size() > 1 ? "," : "", NUM_PARAMS[i].key, o->values[i].load());
        s += buf;
    }
    return s + "}";
}

// loader thread, after an instrument carrying settings loaded: values are atomics; MPC picks them up through the
// "live" parameter polling
void apply_saved_settings(Omni *o, const Settings_kv &kv) {
    for (auto &p : kv) {
        if (!sound_param(p.first.c_str())) continue;
        int i = num_index(p.first.c_str());
        if (i >= 0) o->values[i].store(p.second);
    }
    apply_settings(o);
}

// browser worker (ui_mutex held): a patch's settings, then its instruments into their slots (without the settings
// their own .omni files carry: the patch's win); slots the patch does not use are cleared
void load_patch(Omni *o, const std::string &path) {
    Patch p;
    try { p = read_patch(path); } catch (const std::exception &e) { o->info_text = path_name(path) + ": " + e.what(); return; }
    if (p.layers.empty()) { o->info_text = "This patch has no instruments"; return; }
    for (auto &kv : p.settings) {
        if (!patch_param(kv.first.c_str())) continue;
        int i = num_index(kv.first.c_str());
        if (i >= 0) o->values[i].store(kv.second);
    }
    apply_settings(o);
    bool used[SLOTS] = {};
    int missing = 0;
    for (auto &l : p.layers) {
        if (used[l.slot]) continue;
        used[l.slot] = true;
        struct stat st;
        if (stat(l.file.c_str(), &st) != 0) missing++;
        o->loader.request(l.file, l.preset, l.slot, false);
    }
    for (int s = 0; s < SLOTS; s++) if (!used[s]) o->loader.request("", 0, s, false);
    int n = int(p.layers.size());
    o->info_text = "Patch " + p.name + ": " + std::to_string(n) + (n == 1 ? " instrument" : " instruments");
    if (missing) o->info_text += ", " + std::to_string(missing) + " missing";
}

// key ranges for the loaded slots: the keyboard divided evenly, in slot order
void auto_split(Omni *o) {
    std::vector<int> loaded;
    for (int i = 0; i < SLOTS; i++) if (!o->loader.slot_info(i).path.empty()) loaded.push_back(i);
    if (loaded.size() < 2) {
        std::lock_guard<std::mutex> lock(o->ui_mutex);
        o->browse_msg = "Auto split: load sounds into two or more slots first";
        o->ui_rev.fetch_add(1);
        return;
    }
    size_t n = loaded.size();
    for (size_t k = 0; k < n; k++) {
        char key[24];
        std::snprintf(key, sizeof key, "slot%d_lo", loaded[k] + 1);
        o->values[num_index(key)].store(float(128 * k / n));
        std::snprintf(key, sizeof key, "slot%d_hi", loaded[k] + 1);
        o->values[num_index(key)].store(float(128 * (k + 1) / n - 1));
    }
    o->values[num_index("layer_mode")].store(0);
    apply_settings(o);
}

// ---------------------------------------------------------------------------------------------------------------
// state: "key=value" lines

std::string get_state(Omni *o) {
    std::string s = "omni-sampler 1\n";
    for (int i = 0; i < SLOTS; i++) {
        LoadedInfo li = o->loader.slot_info(i);
        if (li.path.empty()) continue;
        s += "slot" + std::to_string(i + 1) + "_path=" + li.path + "\n";
        s += "slot" + std::to_string(i + 1) + "_preset=" + std::to_string(li.preset) + "\n";
    }
    {
        std::lock_guard<std::mutex> lock(o->ui_mutex);
        s += "browse=" + (o->preset_file.empty() ? o->dir : o->preset_file) + "\n";
    }
    char buf[64];
    for (int i = 0; i < NUM_COUNT; i++) {
        std::snprintf(buf, sizeof buf, "%g", o->values[i].load());
        s += std::string(NUM_PARAMS[i].key) + "=" + buf + "\n";
    }
    return s;
}

void set_state(Omni *o, const char *text) {
    std::string path, browse;
    int preset = 0;
    std::string spath[SLOTS];
    int spreset[SLOTS] = {};
    const char *p = text;
    while (*p) {
        const char *e = std::strchr(p, '\n');
        std::string line(p, e ? size_t(e - p) : std::strlen(p));
        p = e ? e + 1 : p + line.size();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "path") path = v;   // projects before 1.4: one instrument, slot A
        else if (k == "preset") preset = std::atoi(v.c_str());
        else if (k.size() > 6 && k.compare(0, 4, "slot") == 0 && isdigit((unsigned char)k[4]) && k.compare(5, 5, "_path") == 0) spath[(k[4] - '1') & 3] = v;
        else if (k.size() > 6 && k.compare(0, 4, "slot") == 0 && isdigit((unsigned char)k[4]) && k.compare(5, 7, "_preset") == 0) spreset[(k[4] - '1') & 3] = std::atoi(v.c_str());
        else if (k == "browse") browse = v;
        else { int i = num_index(k.c_str()); if (i >= 0) o->values[i].store(float(std::atof(v.c_str()))); }
    }
    apply_settings(o);
    if (!path.empty() && spath[0].empty()) { spath[0] = path; spreset[0] = preset; }
    // "" clears the slot; the project's own settings win over those an .omni carries
    for (int i = 0; i < SLOTS; i++) o->loader.request(spath[i], spreset[i], i, false);
    if (path.empty()) path = spath[int(num(o, "target_slot") + 0.5f) & 3];
    std::lock_guard<std::mutex> lock(o->ui_mutex);
    std::string folder = browse.empty() ? path_dir(path) : browse;
    if (!folder.empty() && o->vfs.is_container(folder)) list_folder(o, folder);
}

// ---------------------------------------------------------------------------------------------------------------
// engine interface

void browse_worker(Omni *o);
std::string get_state(Omni *o);

// the skin's waveform bars and keyboard maps from the loaded program, the last played zone and the slot settings
void update_displays(Omni *o) {
    MergedInfo m = o->loader.merged();
    Settings &s = o->sampler.settings;
    int zi = o->sampler.last_zone.load();
    bool have = o->sampler.last_zone_prog.load() == m.serial && zi >= 0 && size_t(zi) < m.zones.size();
    if (!have) {   // nothing played yet: the target slot's first zone
        int t = o->loader.target();
        for (size_t i = 0; i < m.zones.size(); i++) if (m.zones[i].slot == t) { zi = int(i); have = true; break; }
    }
    for (int b = 0; b < 48; b++) o->disp_wave[b].store(have ? uint8_t(m.zones[size_t(zi)].wave[b] / 2) : 0);
    bool ks = s.layer_mode.load() == 1;
    for (int t = 0; t < 32; t++) {
        int k0 = t * 4, k1 = t * 4 + 3, play = 0, mask = 0;
        for (size_t i = 0; i < m.zones.size(); i++) {
            const ZoneBrief &z = m.zones[i];
            if (z.key_hi < k0 || z.key_lo > k1) continue;
            int sl = z.slot & 3;
            bool sounding = !s.slot_mute[sl].load() &&
                            (ks ? sl == o->sampler.active_slot.load() : !(s.slot_hi[sl].load() < k0 || s.slot_lo[sl].load() > k1));
            if (sounding) play = std::max(play, have && int(i) == zi ? 2 : 1);
            bool in_range = ks || !(s.slot_hi[sl].load() < k0 || s.slot_lo[sl].load() > k1);
            if (in_range && !s.slot_mute[sl].load()) mask |= 1 << sl;
        }
        o->disp_ptile[t].store(uint8_t(play));
        o->disp_ltile[t].store(uint8_t(mask));
    }
}

void apply_saved_settings(Omni *o, const Settings_kv &kv);

void diag_loop(Omni *o) {
    int tick = 0;
    while (!o->diag_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        update_displays(o);
        if (++tick % 30) continue;
        struct stat st;
        if (stat("/sdcard/omni-debug", &st) != 0) continue;
        Settings &s = o->sampler.settings;
        std::string t = get_state(o);
        char b[256];
        for (int k = 0; k < MOD_SLOTS; k++) {
            std::snprintf(b, sizeof b, "engine slot %d: src %d dst %d amt %.2f\n", k + 1, s.mod_src[k].load(), s.mod_dst[k].load(), s.mod_amt[k].load());
            t += b;
        }
        std::snprintf(b, sizeof b, "engine lfo: rate %.3f/%.3f Hz wave %d/%d sync %d/%d out %.3f/%.3f voices %d\n", s.lfo_rate[0].load(),
                      s.lfo_rate[1].load(), s.lfo_wave[0].load(), s.lfo_wave[1].load(), s.lfo_sync[0].load(), s.lfo_sync[1].load(),
                      o->sampler.lfo_out[0].load(), o->sampler.lfo_out[1].load(), o->sampler.active_voices.load());
        t += b;
        t += "status=" + o->loader.status() + "\n";
        FILE *f = std::fopen("/tmp/omni-debug.txt.new", "w");
        if (!f) continue;
        std::fwrite(t.data(), 1, t.size(), f);
        std::fclose(f);
        std::rename("/tmp/omni-debug.txt.new", "/tmp/omni-debug.txt");
    }
}

void *create(const char *data_dir) {
    register_all();
    Omni *o = new Omni();
    o->data_dir = data_dir && *data_dir ? data_dir : "/tmp/omni-sampler";
    mkdir(o->data_dir.c_str(), 0755);
    {   // the user's library location, if one was chosen
        FILE *f = std::fopen(path_join(o->data_dir, "library_path.txt").c_str(), "r");
        if (f) {
            char b[1024] = {};
            if (std::fgets(b, sizeof b, f)) o->lib_choice = trim(b);
            std::fclose(f);
        }
    }
    mkdir(default_library(o).c_str(), 0755);
    apply_settings(o);
    {
        std::lock_guard<std::mutex> lock(o->ui_mutex);
        list_drives(o);
    }
    o->loader.get_settings = [o] { return settings_json(o); };
    o->loader.apply_settings = [o](const Settings_kv &kv) { apply_saved_settings(o, kv); };
    o->br_thread = std::thread(browse_worker, o);
    o->diag_thread = std::thread(diag_loop, o);
    return o;
}

void destroy(void *inst) {
    Omni *o = static_cast<Omni *>(inst);
    {
        std::lock_guard<std::mutex> lock(o->br_qm);
        o->br_quit = true;
    }
    o->br_cv.notify_all();
    if (o->br_thread.joinable()) o->br_thread.join();
    o->diag_quit.store(true);
    if (o->diag_thread.joinable()) o->diag_thread.join();
    delete o;
}

void midi(void *inst, const uint8_t *msg, int len) {
    Omni *o = static_cast<Omni *>(inst);
    // program change: the n-th preset of the loaded file, loaded by the loader thread (no locks here)
    if (len >= 2 && (msg[0] & 0xF0) == 0xC0) { if (num(o, "prog_change") > 0.5f) o->loader.request_program(msg[1] & 0x7F); return; }
    o->sampler.midi(msg, len);
}

bool is_trigger_on(const char *val) { return std::atof(val) > 0.5f; }

// the trigger buttons, latched while held so the button lights
constexpr int TRIGGER_COUNT = 16;
const char *const TRIGGERS[TRIGGER_COUNT] = {"prog_prev", "prog_next", "br_prev", "br_next", "br_up", "br_drives", "br_library",
                                             "br_refresh", "pad_down", "pad_up", "br_extract", "slot_clear", "auto_split",
                                             "br_setlib", "patch_save", "panic"};
int trigger_index(const char *key) {
    for (int i = 0; i < TRIGGER_COUNT; i++) if (!std::strcmp(key, TRIGGERS[i])) return i;
    return -1;
}

void set_param(void *inst, const char *key, const char *val) {
    Omni *o = static_cast<Omni *>(inst);
    if (!std::strcmp(key, "state")) { set_state(o, val); return; }
    int ni = num_index(key);
    if (ni >= 0) { o->values[ni].store(float(std::atof(val))); apply_settings(o); return; }
    int ti = trigger_index(key);
    if (ti >= 0) o->trig_on[ti].store(is_trigger_on(val) ? 1 : 0);
    if (!std::strncmp(key, "pad_", 4) && std::isdigit(static_cast<unsigned char>(key[4]))) {
        int n = std::atoi(key + 4) - 1;
        if (n >= 0 && n < 16 && is_trigger_on(val)) o->pad_req[n].store(1);
        return;
    }
    if ((!std::strcmp(key, "pad_up") || !std::strcmp(key, "pad_down")) && is_trigger_on(val)) {
        int pb = num_index("pad_base");
        float b = o->values[pb].load() + (key[4] == 'u' ? 16.0f : -16.0f);
        o->values[pb].store(std::max(0.0f, std::min(112.0f, b)));
        o->pad_base_rev.fetch_add(1);
        return;
    }
    if (!std::strcmp(key, "br_extract")) { if (is_trigger_on(val)) o->loader.save(); return; }
    if (!std::strcmp(key, "panic")) { if (is_trigger_on(val)) { o->pad_panic.store(1); o->sampler.panic(); } return; }
    if (!std::strcmp(key, "patch_save")) { if (is_trigger_on(val)) o->loader.save_patch(settings_json(o, patch_param)); return; }
    if (!std::strcmp(key, "br_setlib")) {
        if (!is_trigger_on(val)) return;
        std::lock_guard<std::mutex> lock(o->br_qm);
        o->br_q.emplace_back("setlib", "1");
        o->br_busy.store(1);
        o->br_cv.notify_one();
        return;
    }
    if (!std::strcmp(key, "slot_clear")) { if (is_trigger_on(val)) o->loader.request("", 0); return; }
    if (!std::strcmp(key, "auto_split")) { if (is_trigger_on(val)) auto_split(o); return; }
    bool prog = !std::strcmp(key, "prog_prev") || !std::strcmp(key, "prog_next");
    if (!prog && std::strncmp(key, "br_", 3)) return;
    const char *k = prog ? key : key + 3;   // the worker's action: "prog_prev"/"prog_next", or the br_ suffix
    if (!std::strcmp(k, "path")) {   // tests, state: synchronous
        std::lock_guard<std::mutex> lock(o->ui_mutex);
        if (*val) list_folder(o, val); else list_drives(o);
        return;
    }
    bool row = std::isdigit(static_cast<unsigned char>(*k)) && !std::strchr(k, '_');
    if (!row && !is_trigger_on(val)) return;
    {
        std::lock_guard<std::mutex> lock(o->br_qm);
        if (o->br_q.size() >= 32) return;
        o->br_q.emplace_back(k, val);
        o->br_busy.store(1);
    }
    o->ui_rev.fetch_add(1);
    o->br_cv.notify_one();
}

// one browser action (the worker thread, ui_mutex held)
void browse_action(Omni *o, const std::string &key) {
    const char *k = key.c_str();
    if (!std::strcmp(k, "prog_prev") || !std::strcmp(k, "prog_next")) {   // may list the folder for the next file
        step_program(o, k[5] == 'p' ? -1 : 1);
        return;
    }
    {
        if (std::isdigit(static_cast<unsigned char>(*k))) {
            tap_row(o, o->page * ROWS + std::atoi(k) - 1);
            return;
        }
        int pages = std::max(1, (int(o->rows.size()) + ROWS - 1) / ROWS);
        if (!std::strcmp(k, "prev") && o->page > 0) { o->page--; rebuild_on(o); }
        else if (!std::strcmp(k, "next") && o->page + 1 < pages) { o->page++; rebuild_on(o); }
        else if (!std::strcmp(k, "up")) { if (!o->rows.empty() && o->rows[0].kind == RowKind::Up) tap_row(o, 0); else list_drives(o); }
        else if (!std::strcmp(k, "drives")) list_drives(o);
        else if (!std::strcmp(k, "library")) list_folder(o, library_dir(o));
        else if (!std::strcmp(k, "setlib")) {
            // the folder shown becomes the Plugin Library (the drives list: back to the internal one)
            std::string dir = o->preset_file.empty() ? o->dir : "";
            bool ok = dir.empty();
            if (!ok) try { ok = o->vfs.resolve(dir).volume->kind() == "Folder" && host_dir(dir); } catch (const std::exception &) {}
            if (!ok) o->browse_msg = "Open a folder on a drive (not inside a disk image), then SET LIBRARY HERE";
            else {
                {
                    std::lock_guard<std::mutex> lock(o->lib_mutex);
                    o->lib_choice = dir;
                }
                FILE *f = std::fopen(path_join(o->data_dir, "library_path.txt").c_str(), "w");
                if (f) { std::fputs(dir.c_str(), f); std::fclose(f); }
                apply_settings(o);
                o->browse_msg = dir.empty() ? "Plugin Library: internal storage" : "Plugin Library: " + short_path(o, dir);
                if (o->dir.empty()) list_drives(o);
            }
        }
        else if (!std::strcmp(k, "refresh")) { o->vfs.forget(); if (o->dir.empty()) list_drives(o); else list_folder(o, o->dir); }
    }
}

void browse_worker(Omni *o) {
    for (;;) {
        std::pair<std::string, std::string> a;
        {
            std::unique_lock<std::mutex> lock(o->br_qm);
            o->br_cv.wait(lock, [o] { return o->br_quit || !o->br_q.empty(); });
            if (o->br_quit) return;
            a = std::move(o->br_q.front());
            o->br_q.pop_front();
        }
        {
            std::lock_guard<std::mutex> lock(o->ui_mutex);
            try { browse_action(o, a.first); }
            catch (const std::exception &e) { o->browse_msg = e.what(); }
        }
        {
            std::lock_guard<std::mutex> lock(o->br_qm);
            if (o->br_q.empty()) o->br_busy.store(0);
        }
        o->ui_rev.fetch_add(1);
    }
}

int put(char *buf, int len, const std::string &s) {
    if (len <= 0) return 0;
    std::snprintf(buf, size_t(len), "%s", s.empty() ? " " : s.c_str());
    return int(std::strlen(buf)) > 0 ? int(std::strlen(buf)) : 1;
}

std::string mb(int64_t bytes) {
    char b[32];
    std::snprintf(b, sizeof b, bytes >= (100 << 20) ? "%.0f MB" : "%.1f MB", double(bytes) / 1048576.0);
    return b;
}

// MPC's octave numbering: note 60 = C3
std::string note_name(int n) {
    static const char *names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    n = std::max(0, std::min(127, n));
    return std::string(names[n % 12]) + std::to_string(n / 12 - 2);
}

int get_param(void *inst, const char *key, char *buf, int len) {
    Omni *o = static_cast<Omni *>(inst);
    // audio-thread safe lookups first: list highlights and the status bit
    if (!std::strncmp(key, "br_", 3) && std::isdigit(static_cast<unsigned char>(key[3]))) {
        const char *us = std::strchr(key + 3, '_');
        if (us && !std::strcmp(us, "_on")) {
            int r = std::atoi(key + 3) - 1;
            return std::snprintf(buf, size_t(len), "%d", r >= 0 && r < ROWS ? o->row_on[r].load() : 0);
        }
    }
    if (!std::strcmp(key, "pad_info_on")) return std::snprintf(buf, size_t(len), "%d", o->pad_base_rev.load() & 1);
    if (!std::strncmp(key, "pad_", 4) && std::isdigit(static_cast<unsigned char>(key[4]))) {   // lit while its key is down
        int n = std::atoi(key + 4) - 1, note = int(num(o, "pad_base")) + n;
        return std::snprintf(buf, size_t(len), "%d", n >= 0 && n < 16 && note < 128 && o->sampler.key_down[note].load() ? 1 : 0);
    }
    if (!std::strncmp(key, "layer_", 6) && std::strstr(key, "_on"))
        return std::snprintf(buf, size_t(len), "%d", int((o->sampler.layer_events.load() + o->loader.revision.load()) & 1));
    if (!std::strncmp(key, "slot", 4) && std::strstr(key, "_name_on"))
        return std::snprintf(buf, size_t(len), "%d", int(o->loader.revision.load() & 1));
    if (!std::strcmp(key, "status_on") || !std::strcmp(key, "prog_name_on") || !std::strcmp(key, "prog_info_on") ||
        !std::strcmp(key, "prog_format_on"))
        return std::snprintf(buf, size_t(len), "%d", int((o->loader.revision.load() + o->ui_rev.load()) & 1));
    if (!std::strcmp(key, "lib_path_on")) return std::snprintf(buf, size_t(len), "%d", int(o->ui_rev.load() & 1));
    if (!std::strcmp(key, "br_info_on"))   // also shows the loader's errors: follows its revision too
        return std::snprintf(buf, size_t(len), "%d", int((o->ui_rev.load() + o->loader.revision.load()) & 1));
    if (!std::strcmp(key, "br_loc_on") || !std::strcmp(key, "br_page_on"))
        return std::snprintf(buf, size_t(len), "%d", int(o->ui_rev.load() & 1));
    if (!std::strncmp(key, "wave_", 5) && isdigit((unsigned char)key[5])) {
        int i = std::atoi(key + 5) - 1;
        return std::snprintf(buf, size_t(len), "%d", i >= 0 && i < 48 ? o->disp_wave[i].load() : 0);
    }
    if ((!std::strncmp(key, "ptile_", 6) || !std::strncmp(key, "ltile_", 6)) && isdigit((unsigned char)key[6])) {
        int i = std::atoi(key + 6) - 1;
        if (i < 0 || i >= 32) return std::snprintf(buf, size_t(len), "0");
        return std::snprintf(buf, size_t(len), "%d", key[0] == 'p' ? o->disp_ptile[i].load() : o->disp_ltile[i].load());
    }
    if (std::strstr(key, "_display") && (!std::strncmp(key, "slot", 4) || !std::strncmp(key, "ks_base", 7))) {
        std::string k(key, std::strstr(key, "_display") - key);
        int ni = num_index(k.c_str());
        if (ni >= 0 && (k.find("_lo") != std::string::npos || k.find("_hi") != std::string::npos || k == "ks_base"))
            return put(buf, len, note_name(int(o->values[ni].load() + 0.5f)));
        if (ni >= 0 && k.find("_vol") != std::string::npos) return std::snprintf(buf, size_t(len), "%+.0f dB", o->values[ni].load());
        if (ni >= 0 && k.find("_tune") != std::string::npos) return std::snprintf(buf, size_t(len), "%+.0f st", o->values[ni].load());
    }
    if (!std::strcmp(key, "cutoff_display")) {
        float hz = 20.0f * std::pow(1000.0f, num(o, "cutoff") / 100.0f);
        return hz >= 1000 ? std::snprintf(buf, size_t(len), "%.1f kHz", hz / 1000) : std::snprintf(buf, size_t(len), "%.0f Hz", hz);
    }
    if (!std::strcmp(key, "lfo1_rate_display") || !std::strcmp(key, "lfo2_rate_display")) {
        static const char *const DIV[] = {"", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T", "1/4.", "1/8.", "1/16."};
        int sync = int(num(o, key[3] == '1' ? "lfo1_sync" : "lfo2_sync") + 0.5f);
        if (sync > 0 && sync < 15) return put(buf, len, DIV[sync]);
        float hz = lfo_hz(num(o, key[3] == '1' ? "lfo1_rate" : "lfo2_rate"));
        return std::snprintf(buf, size_t(len), hz < 1 ? "%.2f Hz" : "%.1f Hz", hz);
    }
    if (!std::strcmp(key, "bend_display")) {
        int b = int(std::lround(num(o, "bend")));
        return b ? std::snprintf(buf, size_t(len), "%d st", b) : put(buf, len, "Inst");
    }
    int ni = num_index(key);
    if (ni >= 0) return std::snprintf(buf, size_t(len), "%g", o->values[ni].load());
    if (!std::strcmp(key, "state")) return put(buf, len, get_state(o));
    int ti = trigger_index(key);
    if (ti >= 0) return std::snprintf(buf, size_t(len), "%d", o->trig_on[ti].load());

    if (!std::strncmp(key, "slot", 4) && isdigit((unsigned char)key[4]) && !std::strcmp(key + 5, "_name")) {
        int i = (key[4] - '1') & 3;
        LoadedInfo li = o->loader.slot_info(i);
        return put(buf, len, li.path.empty() ? "(empty)" : li.name);
    }
    if (!std::strcmp(key, "prog_name")) {
        LoadedInfo li = o->loader.info();
        if (li.path.empty()) return put(buf, len, std::string("Slot ") + char('A' + o->loader.target()) + " empty - open BROWSE");
        std::string n = std::string(1, char('A' + o->loader.target())) + ": " + li.name;
        if (li.presets.size() > 1) n = std::to_string(li.preset + 1) + "/" + std::to_string(li.presets.size()) + " " + n;
        return put(buf, len, n);
    }
    if (!std::strcmp(key, "prog_info")) {
        LoadedInfo li = o->loader.info();
        if (li.path.empty()) return put(buf, len, "SFZ SF2 Akai E-mu Kontakt and disk images");
        return put(buf, len, li.format + " - " + std::to_string(li.zones) + " zones, " + std::to_string(li.samples) +
                             " samples, " + mb(li.bytes));
    }
    if (!std::strcmp(key, "pad_info")) {
        int b = int(num(o, "pad_base"));
        return put(buf, len, "PADS " + note_name(b) + " - " + note_name(std::min(127, b + 15)));
    }
    if (!std::strcmp(key, "lib_path")) {
        std::string c = library_choice(o);
        if (c.empty()) return put(buf, len, "LIBRARY: internal storage");
        if (!host_dir(c)) return put(buf, len, "LIBRARY: " + c + " (not connected: using internal)");
        return put(buf, len, "LIBRARY: " + c);
    }
    if (!std::strcmp(key, "prog_format")) {
        LoadedInfo li = o->loader.info();
        std::string f = li.path.empty() ? "NO INSTRUMENT" : li.format;
        for (char &c : f) c = char(std::toupper(static_cast<unsigned char>(c)));
        return put(buf, len, "FORMAT: " + f);
    }
    if (!std::strncmp(key, "layer_", 6)) {
        MergedInfo m = o->loader.merged();
        int zi = o->sampler.last_zone.load();
        bool have = o->sampler.last_zone_prog.load() == m.serial && zi >= 0 && size_t(zi) < m.zones.size();
        const char *k = key + 6;
        if (!have) return put(buf, len, !std::strcmp(k, "name") ? (m.zones.empty() ? "Load an instrument on the BROWSE tab" : "Play a pad or key") : " ");
        const ZoneBrief &z = m.zones[size_t(zi)];
        if (!std::strcmp(k, "name"))
            return put(buf, len, std::string("ACTIVE LAYER ") + char('A' + (z.slot & 3)) + ": " + (z.name.empty() ? "zone " + std::to_string(zi + 1) : z.name));
        if (!std::strcmp(k, "root")) return put(buf, len, "ROOT: " + note_name(z.root));
        if (!std::strcmp(k, "keys")) return put(buf, len, "KEYS: " + note_name(z.key_lo) + " - " + note_name(z.key_hi));
        if (!std::strcmp(k, "vel")) return put(buf, len, "VEL: " + std::to_string(std::max(1, z.vel_lo)) + " - " + std::to_string(z.vel_hi));
        if (!std::strcmp(k, "hit")) return put(buf, len, "HIT: " + std::to_string(o->sampler.last_vel.load()));
        return put(buf, len, " ");
    }
    if (!std::strcmp(key, "status")) {
        std::string s = o->loader.status();
        if (s.empty()) {
            int v = o->sampler.active_voices.load();
            s = v ? std::to_string(v) + " voices" : "Ready";
        }
        return put(buf, len, s);
    }
    std::lock_guard<std::mutex> lock(o->ui_mutex);
    if (!std::strncmp(key, "br_", 3)) {
        const char *k = key + 3;
        if (std::isdigit(static_cast<unsigned char>(*k))) {
            size_t i = size_t(o->page * ROWS + std::atoi(k) - 1);
            return put(buf, len, i < o->rows.size() ? o->rows[i].label : " ");
        }
        if (!std::strcmp(k, "loc")) {
            if (!o->preset_file.empty()) return put(buf, len, short_path(o, o->preset_file));
            return put(buf, len, o->dir.empty() ? "Drives" : short_path(o, o->dir));
        }
        if (!std::strcmp(k, "page")) {
            int pages = std::max(1, (int(o->rows.size()) + ROWS - 1) / ROWS);
            return put(buf, len, std::to_string(o->page + 1) + " / " + std::to_string(pages));
        }
        if (!std::strcmp(k, "info")) {
            if (o->br_busy.load()) return put(buf, len, "Opening...");
            std::string st = o->loader.status();   // a failed load is shown here, where the tap was made
            if (st.compare(0, 6, "Error:") == 0 || st.compare(0, 7, "Extract") == 0 || st.compare(0, 4, "Save") == 0 ||
                st.compare(0, 7, "Nothing") == 0)
                return put(buf, len, st);
            return put(buf, len, o->browse_msg.empty() ? o->info_text : o->browse_msg);
        }
        return std::snprintf(buf, size_t(len), "0");
    }
    return 0;
}

void render(void *inst, int16_t *out, int frames) {
    Omni *o = static_cast<Omni *>(inst);
    // skin pads: MPC's momentary buttons release on their own, so a tap holds the note for a fixed time: 0.6 s, or
    // the sound's attack plus half a second (slow strings and pads would barely start otherwise), at most 3 s
    if (o->pad_panic.exchange(0))
        for (int n = 0; n < 16; n++) { o->pad_gate[n] = 0; o->pad_req[n].store(0); }
    for (int n = 0; n < 16; n++) {
        if (o->pad_gate[n] > 0) {
            o->pad_gate[n] -= frames;
            if (o->pad_gate[n] <= 0) { uint8_t off[3] = {0x80, uint8_t(o->pad_note[n]), 0}; o->sampler.midi(off, 3); }
        }
        if (o->pad_req[n].exchange(0)) {
            if (o->pad_gate[n] > 0) { uint8_t off[3] = {0x80, uint8_t(o->pad_note[n]), 0}; o->sampler.midi(off, 3); }
            int note = std::min(127, int(num(o, "pad_base")) + n);
            int vel = std::max(1, std::min(127, int(num(o, "pad_vel"))));
            o->pad_note[n] = note;
            float gate = std::max(0.6f, std::min(3.0f, o->sampler.slowest_attack.load() + 0.5f));
            o->pad_gate[n] = int(gate * 44100);
            uint8_t on[3] = {0x90, uint8_t(note), uint8_t(vel)};
            o->sampler.midi(on, 3);
        }
    }
    o->sampler.render(out, frames);
}

// the host transport (audio thread, before render): tempo-synced LFOs
void transport(void *inst, double ppq, double tempo, int playing, int ppq_valid) {
    static_cast<Omni *>(inst)->sampler.transport(ppq, tempo, playing, ppq_valid);
}

const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, nullptr, transport};

}  // namespace

extern "C" const mpc_engine_t *mpc_engine(void) { return &ENGINE; }

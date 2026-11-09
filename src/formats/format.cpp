// Omni Sampler: reader registry and shared reader helpers.
#include "format.hpp"
#include "omni_native.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>

namespace omni {

namespace {
std::vector<FormatReader> &storage() {
    static std::vector<FormatReader> r;
    return r;
}
bool has_ext(const char *exts, const std::string &ext) {
    if (ext.empty()) return false;
    std::istringstream s(exts);
    std::string e;
    while (s >> e) if (e == ext) return true;
    return false;
}
}  // namespace

void register_reader(const FormatReader &reader) { storage().push_back(reader); }

// ---- shared sample folders (M1 ROM dumps / disk, Wavestation ROM dumps) ----

namespace {
std::mutex dirs_mutex;
std::vector<std::string> extra_dirs;   // host folders to search too (the Plugin Library)

// a folder that `is`: <dir> itself or a folder up to `depth` levels below it
bool search_folder(Volume &vol, const std::string &dir, int depth, IsFolder is, std::string &found) {
    if (is(vol, dir)) { found = dir; return true; }
    if (depth <= 0) return false;
    std::vector<DirEntry> list;
    try { list = vol.list(dir); } catch (const std::exception &) { return false; }
    if (list.size() > 4000) return false;
    for (const DirEntry &e : list)
        if (e.dir && !e.name.empty() && e.name[0] != '.' && search_folder(vol, path_join(dir, e.name), depth - 1, is, found)) return true;
    return false;
}
}  // namespace

void set_search_dirs(const std::vector<std::string> &dirs) {
    std::lock_guard<std::mutex> lock(dirs_mutex);
    extra_dirs = dirs;
}

std::string find_beside(Volume &vol, const std::string &file, IsFolder is, std::string &cache) {
    std::vector<std::string> extra;
    {
        std::lock_guard<std::mutex> lock(dirs_mutex);
        if (!cache.empty() && vol.kind() == "Folder" && is(vol, cache)) return cache;
        extra = extra_dirs;
    }
    std::string found;
    std::string dir = path_dir(file), below;
    for (int up = 0; up < 8 && !found.size(); up++) {
        if (is(vol, dir)) { found = dir; break; }
        std::vector<DirEntry> list;
        try { list = vol.list(dir); } catch (const std::exception &) {}
        if (list.size() <= 4000)
            for (const DirEntry &e : list)
                if (e.dir && e.name != below && !e.name.empty() && e.name[0] != '.' && search_folder(vol, path_join(dir, e.name), 1, is, found)) break;
        if (dir.empty() || !found.empty()) break;
        below = path_name(dir);
        dir = path_dir(dir);
    }
    if (found.empty() && vol.kind() == "Folder")
        for (const std::string &lib : extra) {
            std::string inner = lib;
            while (!inner.empty() && inner[0] == '/') inner.erase(0, 1);
            if (!inner.empty() && vol.is_dir(inner) && search_folder(vol, inner, 3, is, found)) break;
        }
    if (!found.empty() && vol.kind() == "Folder") {
        std::lock_guard<std::mutex> lock(dirs_mutex);
        cache = found;
    }
    return found;
}
const std::vector<FormatReader> &formats() { return storage(); }
size_t reader_probe_bytes() { return 4096; }

void register_all() {
    static std::once_flag once;
    std::call_once(once, [] {
        // Order matters where extensions overlap: content probes decide, first match wins.
        register_sfz();
        register_sf2();
        register_akai();
        register_emu();
        register_ni();
        register_roland();
        register_reason();
        register_ensoniq();
        register_korg();
        register_korg_triton();
        register_korg_trinity();
        register_korg_m1();
        register_korg_ws();
        register_ensoniq_sd1();
        register_yamaha();
        register_logic();
        register_omni_native();
        register_nxs_klp();
        register_nxs();
        register_wav_folder();
        register_images();
    });
}

const FormatReader *find_reader(Volume &vol, const std::string &path) {
    std::string ln = lower(path_name(path)), ext = path_ext(path);
    std::vector<uint8_t> head;
    uint64_t size = 0;
    bool have_head = false;
    for (auto &f : formats()) {
        bool ext_ok = has_ext(f.exts, ext);
        if (!ext_ok && f.exts[0]) continue;
        if (!f.probe) { if (ext_ok) return &f; continue; }
        if (!have_head) {
            try { BlobPtr b = vol.open(path); head = b->head(reader_probe_bytes()); size = b->size(); } catch (const std::exception &) { return nullptr; }
            have_head = true;
        }
        if (f.probe(ln, head.data(), head.size(), size)) return &f;
    }
    return nullptr;
}

double velocity_to_db(int value) { return value <= 0 ? -96.0 : 40.0 * std::log10(value / 127.0); }

void finish_instrument(Instrument &inst) {
    std::vector<Zone> kept;
    for (auto &z : inst.zones) {
        if (!z.sample) continue;
        z.key_lo = clampi(z.key_lo, 0, 127);
        z.key_hi = clampi(z.key_hi, 0, 127);
        if (z.key_hi < z.key_lo) std::swap(z.key_lo, z.key_hi);
        z.vel_lo = clampi(z.vel_lo, 0, 127);
        z.vel_hi = clampi(z.vel_hi, 0, 127);
        if (z.vel_hi < z.vel_lo) std::swap(z.vel_lo, z.vel_hi);
        z.root = clampi(z.root, 0, 127);
        z.pan = clampd(z.pan, -1, 1);
        if (z.group < 0 || z.group >= int(inst.groups.size())) z.group = 0;
        for (auto &l : z.loops) if (l.end < l.start) std::swap(l.start, l.end);
        kept.push_back(std::move(z));
    }
    inst.zones.swap(kept);
    if (inst.groups.empty()) inst.groups.push_back(Group{});
    // round robin: a sequence position without a length means "one of N" over the zones sharing the slot
    for (auto &z : inst.zones) {
        if (z.play_logic != PlayLogic::RoundRobin || z.seq_length > 1) continue;
        int maxpos = 1;
        for (auto &o : inst.zones)
            if (o.play_logic == PlayLogic::RoundRobin && o.key_lo <= z.key_hi && o.key_hi >= z.key_lo &&
                o.vel_lo <= z.vel_hi && o.vel_hi >= z.vel_lo) maxpos = std::max(maxpos, o.seq_position);
        z.seq_length = maxpos;
    }
    if (inst.name.empty()) inst.name = "Instrument";
}

}  // namespace omni

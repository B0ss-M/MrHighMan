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

// OMNI_PUBLIC (2.3.0, user 2026-11-24): the public build reads 1.8.1's formats + serum, vital, heatup, purity,
// kurzweil_k2000, roland_jv880, ableton, alesis_drum, bliss, decent_sampler, directwave, ensoniq_kt, ensoniq_mr, tracker,
// rom_map, drum_rom. The others are guarded below and their files are not in the public source.
#ifndef OMNI_PUBLIC
#define OMNI_PUBLIC 0
#endif
#if OMNI_PUBLIC
// the public build has no Omnisphere or UJAM readers (their files are left out): the browser's Omnisphere note and the
// ARP's UJAM phrases find nothing (the code that asks for them stays, for a later public version)
std::string omnisphere_note(VolumePtr, const std::string &) { return ""; }
std::vector<UjamMidi> ujam_midi_files(VolumePtr, const std::string &) { return {}; }
#endif
void register_all() {
    static std::once_flag once;
    std::call_once(once, [] {
        // Order matters where extensions overlap: content probes decide, first match wins.
        register_sfz();
        register_sf2();
        register_decent_sampler();   // 2.4.30: .dspreset (extension + "<DecentSampler" probe)
        register_directwave();   // extension-only matches (.dwp / .adv .adg / .prt_omn .mlt_omn .zmap): ahead of the
        register_ableton();      // probe-only ROM readers, which could otherwise take a file of a ROM's size
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_omnisphere();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_omnisphere_factory_db();   // 2.4.14: .db (content probe "<FileSystem>"): the banks were invisible in the browser
#endif
        register_vital();   // 2.4.11
        register_serum();   // 2.4.32: .serumpreset + "XferJson" probe
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
        register_drum_rom();
        register_alesis_drum();
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_korg_dw8000();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_roland_d70();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_roland_d110();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_roland_d50();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_roland_mt32();
#endif
        register_tracker();
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_roland_sc55();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_roland_sc88();
#endif
        register_roland_jv880();
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_roland_jd800();
#endif
        register_ensoniq_kt();
        register_ensoniq_mr();
        register_kurzweil_k2000();
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_augmented_strings();   // 2.4.21
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_augmented_synth();     // 2.4.22
#endif
        register_yamaha();
        register_logic();
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_maize();
#endif
        register_bliss();
        register_heatup();
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_nxs_klp();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_nxs();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_srx_world();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_ujam_blob();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_ujam_patch();   // 2.4.28: UJAM .patch presets (SE CHOIR)
#endif
        register_purity();
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_kawai_k1();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_kawai_k5();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_kawai_k5000();
#endif
#if !OMNI_PUBLIC   // 2.3 public build: not in the public reader set (build_public.sh leaves its file out)
        register_dx7();   // 2.4.14: after every other .syx reader (content probes: theirs decide first)
#endif
        register_rom_map();
        register_omni_native();
        register_wav_folder();
        register_images();
    });
}

const FormatReader *find_reader(Volume &vol, const std::string &path) {
    std::string ln = lower(path_name(path)), ext = path_ext(path);
    std::vector<uint8_t> head;
    uint64_t size = 0;
    bool have_head = false;
    size_t read_limit = 0;
    for (auto &f : formats()) {
        bool ext_ok = has_ext(f.exts, ext);
        if (!ext_ok && f.exts[0]) continue;
        if (!f.probe) { if (ext_ok) return &f; continue; }
        const size_t needed = f.probe_bytes ? f.probe_bytes : reader_probe_bytes();
        // (a reader's larger probe is read only when the whole file fits in it: MT-32 control ROMs are 64 / 128 KB; any
        // bigger file keeps the shared head, so browsing a folder costs no more than before)
        if (!have_head || (needed > read_limit && size <= needed)) {
            try {
                BlobPtr b = vol.open(path);
                size = b->size();
                const size_t want = f.probe_bytes && size > f.probe_bytes ? reader_probe_bytes() : needed;
                if (!have_head || want > read_limit) { head = b->head(want); read_limit = want; }
            } catch (const std::exception &) { return nullptr; }
            have_head = true;
        }
        if (f.probe(ln, head.data(), head.size(), size)) return &f;
    }
    return nullptr;
}

double velocity_to_db(int value) { return value <= 0 ? -96.0 : 40.0 * std::log10(value / 127.0); }

Loop chip_loop(int64_t start, double length, int64_t last) {
    Loop l;
    l.start = start;
    l.end = std::min(last, start + std::max<int64_t>(2, std::llround(length)) - 1);
    if (length > 0 && l.end > l.start) l.tune = 12.0 * std::log2(double(l.end - l.start + 1) / length);
    if (std::fabs(l.tune) < 1e-9) l.tune = 0;
    return l;
}

int16_t la32_sample(uint8_t hi, uint8_t lo) {
    static const std::vector<int16_t> amp = [] {
        std::vector<int16_t> t(32768);
        for (int m = 0; m < 32768; m++) t[size_t(m)] = int16_t(std::lround(32767.0 * std::exp2((m - 32787) / 2048.0)));
        return t;
    }();
    // Magnitude bits, top down: lo bit 6, hi bits 6..0, lo bits 5..0, lo bit 7 (always 0 on the ROMs)
    const int m = (lo & 0x40) << 8 | (hi & 0x7F) << 7 | (lo & 0x3F) << 1 | (lo & 0x80) >> 7;
    const int16_t a = amp[size_t(m)];
    return (hi & 0x80) ? int16_t(-a) : a;
}

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

// Omni Sampler: loader thread (see loader.hpp).
#include "loader.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include "../formats/omni_native.hpp"
#include "autoloop.hpp"

namespace omni {

// 2.3 public (user, 2026-11-24): what SAVE / Auto Extract / a patch keep of a preset. Sample formats keep their samples
// (an .omni + WAVs); presets that need ROMs, .bin files or a synth's own library, and NI / Logic / Reason / Decent
// Sampler presets (their samples are separate files they point to) keep a REFERENCE .omni: settings + where the sounds
// come from, no samples. The private build keeps samples for everything.
#ifndef OMNI_PUBLIC
#define OMNI_PUBLIC 0
#endif
bool saves_reference(const std::string &reader) {
#if OMNI_PUBLIC
    // (a K2000 .krz file carries its own samples: saved with them; the K2000 ROM sets are references)
    static const char *const REF[] = {"Kurzweil K2000 ROM", "Roland JV-", "Roland XV-3080", "Roland Cloud", "Omni ROM map",
                                      "Drum machine ROM", "Alesis drum ROM", "Korg M1", "Korg TRINITY", "Korg TRITON",
                                      "Korg Wavestation", "Ensoniq SD-1", "Ensoniq SQ-1", "Ensoniq KT-76", "Ensoniq MR",
                                      "Serum 2", "Vital", "Heat Up", "LUXONIX Purity", "NI ", "Logic EXS", "Reason ",
                                      "Decent Sampler"};
    for (const char *p : REF) if (reader.compare(0, std::strlen(p), p) == 0) return true;
#else
    (void)reader;
#endif
    return false;
}

// A forward loop's crossfade (Kontakt, Maschine, SFZ loop_crossfade, .omni), baked into the audio once at load as the
// hardware samplers did: the last xf frames before the loop end blend into the xf frames before the loop start, so the
// jump back is seamless and the voice still plays plain data (the audio thread's fast path, no per-sample mixing).
// The blended copy is the zone's own (never the decode cache's), shared by the zones with the same loop. Returns the
// bytes the copies add; the zones' crossfades are then 0 (done). Copies that would exceed max_added_bytes keep the
// original PCM and source crossfade metadata, and are reported as skipped.
int64_t bake_loop_crossfades(Instrument &inst, std::vector<PcmPtr> &pcm,
                             int64_t max_added_bytes, int *skipped) {
    std::map<std::tuple<const Pcm *, int64_t, int64_t, int64_t>, PcmPtr> baked;
    int64_t added = 0;
    for (size_t i = 0; i < inst.zones.size(); i++) {
        Zone &z = inst.zones[i];
        if (!pcm[i] || z.loops.empty() || z.reverse || z.one_shot) continue;
        Loop &l = z.loops[0];
        if (l.type != LoopType::Forward || l.crossfade <= 0) continue;
        const Pcm &src = *pcm[i];
        int64_t ls = std::max<int64_t>(0, l.start), le = std::min<int64_t>(l.end + 1, src.frames());
        int64_t xf = std::min({l.crossfade, ls, le - ls});   // needs xf frames before the loop start
        if (xf < 2) continue;
        auto key = std::make_tuple(pcm[i].get(), ls, le, xf);
        auto it = baked.find(key);
        if (it == baked.end()) {
            auto out = std::make_shared<Pcm>(src);
            const int64_t copy_bytes = int64_t(out->bytes());
            if (copy_bytes > max_added_bytes - added) {
                if (skipped) ++*skipped;
                continue;
            }
            const int ch = out->channels;
            int16_t *d = out->data.data();
            for (int64_t k = 0; k < xf; k++) {
                // equal power: the two ends are different stretches of the sound, mostly uncorrelated
                float w = float(k + 1) / float(xf + 1);
                float gin = std::sqrt(w), gout = std::sqrt(1.0f - w);
                for (int c = 0; c < ch; c++) {
                    size_t a = size_t(le - xf + k) * size_t(ch) + size_t(c), b = size_t(ls - xf + k) * size_t(ch) + size_t(c);
                    float v = float(d[a]) * gout + float(d[b]) * gin;
                    d[a] = int16_t(std::lround(std::max(-32768.0f, std::min(32767.0f, v))));
                }
            }
            added += int64_t(out->bytes());
            it = baked.emplace(key, out).first;
        }
        pcm[i] = it->second;
        l.crossfade = 0;   // it is in the audio now: an .omni saved from this keeps the blend, not a second one
    }
    return added;
}

Loader::Loader(Sampler &sampler, Vfs &vfs) : sampler_(sampler), vfs_(vfs) {
    thread_ = std::thread([this] { run(); });
}

Loader::~Loader() { shutdown(); }

void Loader::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Loader::request(const std::string &path, int preset, int slot, bool use_settings) {
    if (slot < 0 || slot >= SLOTS) slot = target_.load();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // only the newest request for a slot matters
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [slot](const Request &r) { return r.slot == slot; }), queue_.end());
        queue_.push_back({path, preset, slot, use_settings});
        slot_serial_[slot].fetch_add(1);
    }
    cv_.notify_all();
}

void Loader::reload() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int s = 0; s < SLOTS; s++) {
            if (std::any_of(queue_.begin(), queue_.end(), [s](const Request &r) { return r.slot == s; })) continue;
            if (loading_.slot == s) queue_.push_back(loading_);   // it may have read the old setting already
            else if (!info_[s].path.empty()) queue_.push_back({info_[s].path, info_[s].preset, s, false, true});
            else continue;
            slot_serial_[s].fetch_add(1);
        }
    }
    cv_.notify_all();
}

LoadedInfo Loader::info() { return slot_info(target_.load()); }

Loader::Missing Loader::missing(int slot) {   // 2.4.9
    std::lock_guard<std::mutex> lock(mutex_);
    return slot >= 0 && slot < SLOTS ? missing_[slot] : Missing();
}

LoadedInfo Loader::slot_info(int slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    return slot >= 0 && slot < SLOTS ? info_[slot] : LoadedInfo();
}

std::shared_ptr<const LoadedInfo> Loader::slot_ref(int slot) {   // 2.4.11
    static const std::shared_ptr<const LoadedInfo> none = std::make_shared<LoadedInfo>();
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot < 0 || slot >= SLOTS) return none;
    if (!snap_[slot]) snap_[slot] = std::make_shared<const LoadedInfo>(info_[slot]);
    return snap_[slot];
}

std::shared_ptr<const MergedInfo> Loader::merged_ref() {   // 2.4.11
    std::lock_guard<std::mutex> lock(mutex_);
    return merged_;
}

MergedInfo Loader::merged() {
    std::lock_guard<std::mutex> lock(mutex_);
    return *merged_;
}

std::string Loader::status() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Loader::set_status(const std::string &s) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ == s) return;
        status_ = s;
    }
    revision.fetch_add(1);
}

// 2.4.14: a host path's file is there: the path itself, or a disk image holding the rest of it (as plugin.cpp
// host_reachable): a folder above it that exists does not count
static bool path_there(const std::string &p) {
    struct stat st;
    if (p.empty()) return false;
    if (stat(p.c_str(), &st) == 0) return true;
    for (size_t cut = p.rfind('/'); cut != std::string::npos && cut > 0; cut = p.rfind('/', cut - 1)) {
        const std::string pre = p.substr(0, cut);
        if (stat(pre.c_str(), &st) == 0) return S_ISREG(st.st_mode);
    }
    return false;
}

void Loader::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!quit_) {
        cv_.wait_for(lock, std::chrono::milliseconds(50), [this] { return quit_ || !queue_.empty() || save_req_.load() || patch_req_.load(); });
        // free programs the audio thread let go of
        lock.unlock();
        while (Program *old = sampler_.take_retired()) delete old;
        lock.lock();
        if (quit_) break;
        // a save waits until the loads queued before it are done (it saves what they load)
        if (queue_.empty() && save_req_.exchange(false)) {
            lock.unlock();
            extract(target_.load(), true);
            lock.lock();
            continue;
        }
        if (queue_.empty() && patch_req_.exchange(false)) {
            lock.unlock();
            write_patch();
            lock.lock();
            continue;
        }
        // program change: the target slot's next preset (of its source, for an extracted instrument)
        int pc = queue_.empty() ? program_req_.exchange(-1) : -1;
        if (pc >= 0) {
            int t = target_.load();
            const LoadedInfo &li = info_[t];
            bool from_source = !li.source.empty();
            int count = from_source ? li.source_presets : int(li.presets.size());
            if (!li.path.empty() && pc < count) {
                queue_.push_back({from_source ? li.source : li.path, pc, t, true});
                slot_serial_[t].fetch_add(1);
            }
        }
        if (queue_.empty()) continue;
        Request r = queue_.front();
        queue_.pop_front();
        loading_ = r;
        uint32_t serial = slot_serial_[r.slot].load();
        lock.unlock();
        busy_.store(true);
        try {
            load(r, serial);
        } catch (const std::exception &e) {
            set_status(std::string("Error: ") + e.what());
            std::fprintf(stderr, "Omni Sampler: loading %s failed: %s\n", r.path.c_str(), e.what());
            // 2.4.9: an empty slot remembers what it was asked for (the user's report: a project's ISO instrument that
            // failed to load once was gone from the project at its next save)
            std::lock_guard<std::mutex> l2(mutex_);
            if (r.slot >= 0 && r.slot < SLOTS && !r.path.empty() && slot_serial_[r.slot].load() == serial && info_[r.slot].path.empty())
                missing_[r.slot] = Missing{r.path, r.preset, e.what(), path_there(r.path)};   // 2.4.14: and whether its file is there
        }
        progress_.store(-1);
        busy_.store(false);
        revision.fetch_add(1);
        lock.lock();
        loading_.slot = -1;
    }
}

// the peak outline of [start, stop) in WAVE_BINS bins, 0..255
static void outline(const Pcm &p, int64_t start, int64_t stop, uint8_t *out) {
    int64_t frames = p.frames();
    stop = stop > 0 ? std::min(stop, frames) : frames;
    start = std::max<int64_t>(0, std::min(start, stop));
    int64_t n = stop - start;
    for (int b = 0; b < WAVE_BINS; b++) {
        int64_t a = start + n * b / WAVE_BINS, e = start + n * (b + 1) / WAVE_BINS;
        int64_t step = std::max<int64_t>(1, (e - a) / 512);   // sample at most ~512 points per bin
        int peak = 0;
        for (int64_t i = a; i < e; i += step)
            for (int c = 0; c < p.channels && c < 2; c++) peak = std::max(peak, std::abs(int(p.data[size_t(i) * p.channels + c])));
        out[b] = uint8_t(std::min(255, peak * 255 / 32767));
    }
}

void Loader::load(const Request &r, uint32_t serial) {
    const int slot = r.slot;
    auto superseded = [&] { return slot_serial_[slot].load() != serial || quit_.load(); };
    if (r.path.empty()) {   // clear the slot (already empty: nothing to do, no program swap)
        if (slots_[slot].inst.zones.empty() && slot_info(slot).path.empty()) {   // (2.4.9: and forget a missing one)
            std::lock_guard<std::mutex> lock(mutex_);
            missing_[slot] = Missing();
            return;
        }
        slots_[slot] = Slot();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            info_[slot] = LoadedInfo();
            snap_[slot].reset();
            missing_[slot] = Missing();   // 2.4.9
        }
        publish(serial, slot);
        set_status("");
        return;
    }
    progress_.store(0);
    set_status("Reading " + path_name(r.path));
    Location loc;
    const FormatReader *reader = nullptr;
    std::vector<PresetInfo> presets;
    int req_preset = r.preset;
    // an extracted instrument written by an older version that read its format wrongly: read it again from its
    // source (if that is still there) and rewrite it in place, keeping its settings
    std::string refresh;
    if (ends_with_ci(r.path, ".omni") && omni_stale(r.path)) {
        std::string src = omni_source_of(r.path);
        size_t nl = src.find('\n');
        try {
            loc = vfs_.resolve(src.substr(0, nl));
            reader = find_reader(*loc.volume, loc.inner);
            if (reader) presets = reader->list(loc.volume, loc.inner);
            req_preset = std::atoi(src.c_str() + nl + 1);
            if (!presets.empty() && req_preset >= 0 && req_preset < int(presets.size())) refresh = r.path;
        } catch (const std::exception &) {}
        if (refresh.empty()) { reader = nullptr; presets.clear(); req_preset = r.preset; }
    }
    // 2.4.35: a reference .omni (no samples): its source is read, the .omni stays the slot's file (its settings apply)
    std::string ref_src;
    if (refresh.empty() && ends_with_ci(r.path, ".omni") && omni_is_reference(r.path)) {
        std::string src = omni_source_of(r.path);
        size_t nl = src.find('\n');
        if (nl == std::string::npos || nl == 0) throw ParseError(path_name(r.path) + " names no source");
        ref_src = src.substr(0, nl);
        try {
            loc = vfs_.resolve(ref_src);
            reader = find_reader(*loc.volume, loc.inner);
        } catch (const std::exception &) { reader = nullptr; }
        if (!reader) throw ParseError("its sounds are not here: " + ref_src);
        presets = reader->list(loc.volume, loc.inner);
        req_preset = std::atoi(src.c_str() + nl + 1);
    }
    if (refresh.empty() && ref_src.empty()) {
        loc = vfs_.resolve(r.path);
        reader = find_reader(*loc.volume, loc.inner);
        if (!reader) throw ParseError("no reader for " + path_name(r.path));
        presets = reader->list(loc.volume, loc.inner);
    }
    if (presets.empty()) throw ParseError("no instruments in " + path_name(r.path));
    int preset = req_preset < 0 || req_preset >= int(presets.size()) ? 0 : req_preset;
    Instrument inst = reader->load(loc.volume, loc.inner, presets[size_t(preset)].index);
    if (inst.format.empty()) inst.format = reader->name;
    if (superseded()) return;

    std::vector<PcmPtr> pcm(inst.zones.size());
    std::map<SampleRef *, PcmPtr> decoded;
    // the budget covers every slot: what the other slots hold counts against it
    int64_t budget = int64_t(budget_mb_.load()) << 20, used = 0, others = 0;
    {
        std::map<const Pcm *, int> seen;
        for (int s = 0; s < SLOTS; s++)
            if (s != slot)
                for (auto &p : slots_[s].pcm)
                    if (p && seen.emplace(p.get(), 0).second) others += int64_t(p->bytes());
    }
    budget -= others;
    // 2.4.11 (audit M4, user OK): the slot's own old sound is still in memory (its Program plays until the swap): with
    // it the peak reached about twice the limit (an out-of-memory kill on the 32-bit Live II). When the new samples
    // and the old would not fit together, the old sound is let go first (the slot is silent while the rest loads).
    int64_t old_bytes = 0;
    {
        std::map<const Pcm *, int> seen;
        for (int s = 0; s < SLOTS; s++)
            if (s != slot) for (auto &p : slots_[s].pcm) if (p) seen.emplace(p.get(), 0);
        for (auto &p : slots_[slot].pcm)
            if (p && seen.emplace(p.get(), 0).second) old_bytes += int64_t(p->bytes());
    }
    auto release_old = [&] {
        slots_[slot] = Slot();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            info_[slot] = LoadedInfo();
            snap_[slot].reset();
        }
        publish(serial, slot);
        old_bytes = 0;
    };
    int unique = 0, failed = 0, skipped = 0;
    std::vector<std::string> warnings = inst.warnings;
    std::map<SampleRef *, int> distinct;
    for (auto &z : inst.zones) distinct.emplace(z.sample.get(), 0);
    size_t done = 0;
    for (size_t i = 0; i < inst.zones.size(); i++) {
        if (superseded()) return;   // a newer request for this slot: abandon this one
        SampleRef *ref = inst.zones[i].sample.get();
        auto it = decoded.find(ref);
        if (it == decoded.end()) {
            PcmPtr p;
            auto c = cache_.find(ref->key);
            if (c != cache_.end()) p = c->second.lock();
            if (!p) {
                int64_t estimate = ref->frames * std::max(1, ref->channels) * 2;
                if (old_bytes > 0 && used + estimate + old_bytes > budget) release_old();
                if (used + estimate > budget) skipped++;
                else {
                    try {
                        p = ref->decode();
                    } catch (const std::exception &e) {
                        failed++;
                        if (warnings.size() < 20) warnings.push_back(ref->name + ": " + e.what());
                    }
                    if (p && used + int64_t(p->bytes()) > budget) { skipped++; p = nullptr; }
                }
                if (p) cache_[ref->key] = p;
            }
            if (p) { used += int64_t(p->bytes()); unique++; }
            it = decoded.emplace(ref, p).first;
            done++;
            int pct = int(done * 100 / std::max<size_t>(1, distinct.size()));
            if (pct != progress_.load()) {
                progress_.store(pct);
                char buf[96];
                std::snprintf(buf, sizeof buf, "Loading %d%%", pct);
                set_status(buf);
            }
        }
        pcm[i] = it->second;
    }
    for (auto c = cache_.begin(); c != cache_.end();) { if (c->second.expired()) c = cache_.erase(c); else ++c; }
    if (skipped) warnings.insert(warnings.begin(), std::to_string(skipped) + " samples over the memory limit");
    if (failed) warnings.insert(warnings.begin(), std::to_string(failed) + " samples failed to decode");
    int skipped_crossfades = 0;
    used += bake_loop_crossfades(inst, pcm, std::max<int64_t>(0, budget - used), &skipped_crossfades);
    if (skipped_crossfades)
        warnings.push_back(std::to_string(skipped_crossfades) + " loop crossfades over the memory limit");
    const bool auto_looped = auto_loop_.load();
    int loops_added = auto_looped ? auto_loop(inst, pcm) : 0;

    LoadedInfo li;
    li.path = refresh.empty() ? r.path : refresh;
    li.preset = preset;
    li.presets = presets;
    li.name = inst.name;
    li.format = inst.format;
    li.reader = reader->name;
    li.zones = int(inst.zones.size());
    li.samples = unique;
    li.bytes = used;
    li.loops_added = loops_added;
    li.warnings = warnings;
    // where it came from: a disk image (the volume is a mounted image), or an .omni recording its source
    bool in_image = refresh.empty() && ref_src.empty() && loc.volume->kind() != "Folder";
    Settings_kv settings;
    if (!in_image && ends_with_ci(li.path, ".omni")) {
        std::string src = omni_source_of(li.path);
        size_t nl = src.find('\n');
        if (nl != std::string::npos && nl > 0) {
            li.source = src.substr(0, nl);
            li.source_preset = std::atoi(src.c_str() + nl + 1);
            li.source_presets = std::max(1, li.source_preset + 1);
        }
        settings = omni_settings_of(li.path);
    }
    if (settings.empty() && !inst.settings.empty()) settings = inst.settings;   // 2.4.10: a translated synth patch
    for (auto &kv : settings)   // 2.4.13: a text setting ("@key=value", synth_patch.hpp) may name its own file as {path}
        if (!kv.first.empty() && kv.first[0] == '@') {
            const size_t at = kv.first.find("{path}");
            if (at != std::string::npos) kv.first.replace(at, 6, ref_src.empty() ? li.path : ref_src);   // (a reference: its source)
        }
    if (in_image) li.source_presets = int(presets.size());
    {   // the reference envelope: the zone that plays middle C at a medium velocity, else the first zone
        const Zone *ref = nullptr;
        for (auto &z : inst.zones) if (z.key_lo <= 60 && z.key_hi >= 60 && z.vel_lo <= 100 && z.vel_hi >= 100) { ref = &z; break; }
        if (!ref && !inst.zones.empty()) ref = &inst.zones[0];
        if (ref && ref->amp_env.set) {
            li.env_attack = ref->amp_env.attack; li.env_decay = ref->amp_env.decay;
            li.env_sustain = ref->amp_env.sustain; li.env_release = std::max(0.004, ref->amp_env.release);
        }
    }
    slots_[slot].inst = std::move(inst);
    slots_[slot].pcm = std::move(pcm);
    slots_[slot].container = in_image && loc.inner.size() < r.path.size() ? r.path.substr(0, r.path.size() - loc.inner.size() - 1)
                                                                          : path_dir(li.path);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_[slot] = li;
        snap_[slot].reset();
        missing_[slot] = Missing();   // 2.4.9
    }
    if (!publish(serial, slot)) return;
    bool had_settings = !settings.empty() && r.use_settings;
    if (had_settings && apply_settings) apply_settings(settings);
    if (loaded && !r.reload) loaded(slot, r.use_settings, had_settings);   // a reload: the same preset, faders as they are
    std::string note = warnings.empty() ? "" : warnings[0];
    if (slots_[slot].inst.zones.empty()) {   // 2.4.10: a synth patch (translated, or a saved .omni of one) says so
        bool synth = false;
        for (auto &kv : settings) if (kv.first == "syn_on" && kv.second > 0.5f) synth = true;
        note = !synth ? "This preset has no samples (an empty or credits preset)"
                      : (note.empty() ? "Synth patch: played by Omni's SYNTH engine" : note);
    }
    // shown with 0 too, so it's clear Auto Loop ran and found nothing to loop
    if (note.empty() && auto_looped) note = "Auto Loop: " + std::to_string(loops_added) + (loops_added == 1 ? " zone looped" : " zones looped");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = note;
    }
    revision.fetch_add(1);
    // a preset from inside a disk image: keep it as its own instrument file, so the project needs only that
    // (not from a view such as the TRITON categories: those presets stay on the drive, SAVE TO LIBRARY keeps one)
    // (2.3 public: not for a preset that saves a reference: the project points at its source)
    if (in_image && !loc.volume->view() && extract_mode_.load() == 1 && !slots_[slot].inst.zones.empty() && !superseded() && !saves_reference(li.reader))
        extract(slot, false);
    if (!refresh.empty() && !superseded()) {
        std::string json;
        for (auto &kv : settings) {
            char num[32];
            std::snprintf(num, sizeof num, "%.9g", double(kv.second));
            json += (json.empty() ? "{" : ",") + std::string("\"") + kv.first + "\":" + num;
        }
        if (!json.empty()) json += "}";
        try {
            write_omni(slots_[slot].inst, slots_[slot].pcm, path_dir(refresh), li.source, li.source_preset, json, refresh);
        } catch (const std::exception &) {}   // read-only library: it still plays right from the source this time
    }
}

bool Loader::publish(uint32_t serial, int slot) {
    auto prog = std::unique_ptr<Program>(new Program());
    MergedInfo mi;
    std::map<const Pcm *, size_t> outline_of;   // pcm + range already outlined: reuse
    prog->inst.groups.clear();
    prog->inst.name = slots_[target_.load()].inst.name;
    for (int s = 0; s < SLOTS; s++) {
        const Slot &sl = slots_[s];
        if (sl.inst.zones.empty()) continue;
        int goff = int(prog->inst.groups.size());
        for (auto &g : sl.inst.groups) prog->inst.groups.push_back(g);
        prog->inst.polyphony = std::max(prog->inst.polyphony, sl.inst.polyphony);
        for (size_t i = 0; i < sl.inst.zones.size(); i++) {
            Zone z = sl.inst.zones[i];
            z.slot = s;
            z.group += goff;
            z.gain_db += sl.inst.gain_db;
            prog->inst.zones.push_back(z);
            PcmPtr p = i < sl.pcm.size() ? sl.pcm[i] : nullptr;
            prog->pcm.push_back(p);
            ZoneBrief b;
            b.name = z.name; b.root = z.root; b.key_lo = z.key_lo; b.key_hi = z.key_hi; b.vel_lo = z.vel_lo; b.vel_hi = z.vel_hi; b.slot = s;
            if (p) outline(*p, z.start, z.stop, b.wave);
            mi.zones.push_back(b);
        }
    }
    if (prog->inst.groups.empty()) prog->inst.groups.push_back(Group{});
    mi.serial = prog->serial = ++prog_serial_;
    prog->index();   // here, not on the audio thread at the swap (it allocates)
    Program *raw = prog.release();
    while (!sampler_.set_program(raw)) {   // the audio thread has not taken the previous one yet
        if (slot_serial_[slot].load() != serial || quit_.load()) { delete raw; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        while (Program *old = sampler_.take_retired()) delete old;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        merged_ = std::make_shared<const MergedInfo>(std::move(mi));
    }
    revision.fetch_add(1);
    return true;
}

void Loader::extract(int slot, bool with_settings) {
    LoadedInfo li = slot_info(slot);
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dir = extract_dir_;
    }
    const Slot &sl = slots_[slot];
    // (2.3 public: a preset that saves a reference needs no zones: a synth preset plays on the SYNTH)
    if (li.path.empty() || (sl.inst.zones.empty() && !saves_reference(li.reader))) { set_status("Nothing to save"); return; }
    if (dir.empty()) { set_status("No Plugin Library: set one on an SD card or SSD (BROWSE)"); return; }
    std::string settings = with_settings && get_settings ? get_settings() : "";
    bool is_omni = ends_with_ci(li.path, ".omni");
    // an extracted instrument is rewritten in place (its samples stay); anything else gets a new file
    std::string source = is_omni ? li.source : li.path;
    int source_preset = is_omni ? li.source_preset : li.preset;
    std::string out_dir = is_omni ? path_dir(li.path)
                                  : path_join(dir, path_stem(sl.container).empty() ? "Instruments" : path_stem(sl.container));
    if (!is_omni && !sl.inst.library.empty()) {   // a library with categories: Extracted/<library>/<category>/
        out_dir = path_join(dir, omni_file_safe(sl.inst.library));
        if (!sl.inst.category.empty()) out_dir = path_join(out_dir, omni_file_safe(sl.inst.category));
    }
    set_status((with_settings ? "Saving " : "Extracting ") + li.name);
    std::string out;
    // 2.3 public: a reference (its own .omni rewritten as a reference; a new one for a preset that saves one)
    const bool as_ref = is_omni ? omni_is_reference(li.path) : saves_reference(li.reader);
    try {
        out = as_ref ? write_omni_reference(li.name, li.format, out_dir, source, source_preset, settings, is_omni ? li.path : std::string())
                     : write_omni(sl.inst, sl.pcm, out_dir, source, source_preset, settings, is_omni ? li.path : std::string());
    } catch (const std::exception &e) {
        set_status(std::string("Save failed: ") + e.what());
        return;
    }
    if (adopt(slot, li.path, out, source, source_preset)) set_status("Saved to Plugin Library: " + path_name(path_dir(out)) + "/" + path_name(out));
}

// the slot now refers to the .omni written from it (what a project saves); false if something else loaded meanwhile
bool Loader::adopt(int slot, const std::string &was, const std::string &out, const std::string &source, int source_preset) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        LoadedInfo &in = info_[slot];
        if (in.path != was) return false;
        snap_[slot].reset();
        in.source = source;
        in.source_preset = source_preset;
        if (in.source_presets <= 0) in.source_presets = int(in.presets.size());
        in.path = out;
        in.preset = 0;
        in.presets = {PresetInfo{in.name, 0}};
        in.extracted_to = out;
    }
    revision.fetch_add(1);
    return true;
}

void Loader::write_patch() {
    std::string dir, settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dir = extract_dir_.empty() ? std::string() : path_join(path_dir(extract_dir_), "Patches");
        settings = patch_settings_;
    }
    std::vector<int> loaded;
    std::string name;
    for (int s = 0; s < SLOTS; s++) {
        LoadedInfo li = slot_info(s);
        if (li.path.empty() || (slots_[s].inst.zones.empty() && !saves_reference(li.reader))) continue;   // (2.3 public: references too)
        loaded.push_back(s);
        name += (name.empty() ? "" : " + ") + li.name;
    }
    if (loaded.empty()) { set_status("Nothing to save: load instruments into the slots first"); return; }
    if (dir.empty()) { set_status("No Plugin Library: set one on an SD card or SSD (BROWSE)"); return; }
    // a new file each time: "<A> + <B>", numbered when that name is taken
    std::string base = omni_file_safe(name);
    name = base;
    struct stat st;
    for (int k = 2; stat(path_join(dir, name + ".omnipatch").c_str(), &st) == 0; k++) name = base + " (" + std::to_string(k) + ")";
    set_status("Saving patch " + name);
    std::vector<PatchLayer> layers;
    std::string out;
    try {
        for (int s : loaded) {
            LoadedInfo li = slot_info(s);
            PatchLayer pl;
            pl.slot = s;
            pl.name = li.name;
            pl.file = li.path;
            // an .omni on a drive is used as it is; anything else (a file's preset, a preset inside a disk image) is
            // written next to the patch, so the patch keeps working without its source
            bool host_omni = ends_with_ci(li.path, ".omni") && stat(li.path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
            if (!host_omni) {
                pl.file = saves_reference(li.reader)   // 2.3 public: no samples for a ROM / synth / NI / Logic / Reason / Decent preset
                    ? write_omni_reference(li.name, li.format, path_join(dir, name + " Instruments"), li.path, li.preset)
                    : write_omni(slots_[s].inst, slots_[s].pcm, path_join(dir, name + " Instruments"), li.path, li.preset);
                adopt(s, li.path, pl.file, li.path, li.preset);
            }
            layers.push_back(pl);
        }
        out = omni::write_patch(dir, name, layers, settings);
    } catch (const std::exception &e) {
        set_status(std::string("Save failed: ") + e.what());
        return;
    }
    set_status("Saved patch to Plugin Library: Patches/" + path_name(out));
}

}  // namespace omni

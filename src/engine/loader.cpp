// Omni Sampler: loader thread (see loader.hpp).
#include "loader.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sys/stat.h>
#include "../formats/omni_native.hpp"
#include "autoloop.hpp"

namespace omni {

Loader::Loader(Sampler &sampler, Vfs &vfs) : sampler_(sampler), vfs_(vfs) {
    thread_ = std::thread([this] { run(); });
}

Loader::~Loader() {
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

LoadedInfo Loader::info() { return slot_info(target_.load()); }

LoadedInfo Loader::slot_info(int slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    return slot >= 0 && slot < SLOTS ? info_[slot] : LoadedInfo();
}

MergedInfo Loader::merged() {
    std::lock_guard<std::mutex> lock(mutex_);
    return merged_;
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
        uint32_t serial = slot_serial_[r.slot].load();
        lock.unlock();
        busy_.store(true);
        try {
            load(r, serial);
        } catch (const std::exception &e) {
            set_status(std::string("Error: ") + e.what());
            std::fprintf(stderr, "Omni Sampler: loading %s failed: %s\n", r.path.c_str(), e.what());
        }
        progress_.store(-1);
        busy_.store(false);
        revision.fetch_add(1);
        lock.lock();
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
        if (slots_[slot].inst.zones.empty() && slot_info(slot).path.empty()) return;
        slots_[slot] = Slot();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            info_[slot] = LoadedInfo();
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
    if (refresh.empty()) {
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
    int loops_added = auto_loop_.load() ? auto_loop(inst, pcm) : 0;

    LoadedInfo li;
    li.path = refresh.empty() ? r.path : refresh;
    li.preset = preset;
    li.presets = presets;
    li.name = inst.name;
    li.format = inst.format;
    li.zones = int(inst.zones.size());
    li.samples = unique;
    li.bytes = used;
    li.loops_added = loops_added;
    li.warnings = warnings;
    // where it came from: a disk image (the volume is a mounted image), or an .omni recording its source
    bool in_image = refresh.empty() && loc.volume->kind() != "Folder";
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
    if (in_image) li.source_presets = int(presets.size());
    slots_[slot].inst = std::move(inst);
    slots_[slot].pcm = std::move(pcm);
    slots_[slot].container = in_image && loc.inner.size() < r.path.size() ? r.path.substr(0, r.path.size() - loc.inner.size() - 1)
                                                                          : path_dir(li.path);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_[slot] = li;
    }
    if (!publish(serial, slot)) return;
    if (!settings.empty() && apply_settings && r.use_settings) apply_settings(settings);
    std::string note = warnings.empty() ? "" : warnings[0];
    if (slots_[slot].inst.zones.empty()) note = "This preset has no samples (an empty or credits preset)";
    if (note.empty() && loops_added) note = "Auto Loop: " + std::to_string(loops_added) + " zones looped";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = note;
    }
    revision.fetch_add(1);
    // a preset from inside a disk image: keep it as its own instrument file, so the project needs only that
    if (in_image && extract_mode_.load() == 1 && !slots_[slot].inst.zones.empty() && !superseded()) extract(slot, false);
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
    Program *raw = prog.release();
    while (!sampler_.set_program(raw)) {   // the audio thread has not taken the previous one yet
        if (slot_serial_[slot].load() != serial || quit_.load()) { delete raw; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        while (Program *old = sampler_.take_retired()) delete old;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        merged_ = std::move(mi);
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
    if (li.path.empty() || sl.inst.zones.empty() || dir.empty()) { set_status("Nothing to save"); return; }
    std::string settings = with_settings && get_settings ? get_settings() : "";
    bool is_omni = ends_with_ci(li.path, ".omni");
    // an extracted instrument is rewritten in place (its samples stay); anything else gets a new file
    std::string source = is_omni ? li.source : li.path;
    int source_preset = is_omni ? li.source_preset : li.preset;
    std::string out_dir = is_omni ? path_dir(li.path)
                                  : path_join(dir, path_stem(sl.container).empty() ? "Instruments" : path_stem(sl.container));
    set_status((with_settings ? "Saving " : "Extracting ") + li.name);
    std::string out;
    try {
        out = write_omni(sl.inst, sl.pcm, out_dir, source, source_preset, settings, is_omni ? li.path : std::string());
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
        if (li.path.empty() || slots_[s].inst.zones.empty()) continue;
        loaded.push_back(s);
        name += (name.empty() ? "" : " + ") + li.name;
    }
    if (loaded.empty() || dir.empty()) { set_status("Nothing to save: load instruments into the slots first"); return; }
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
                pl.file = write_omni(slots_[s].inst, slots_[s].pcm, path_join(dir, name + " Instruments"), li.path, li.preset);
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

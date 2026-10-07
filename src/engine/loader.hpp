// Omni Sampler: the loader thread. Parses a requested file into one of the instrument slots (A-D), decodes its
// samples within a memory budget, merges the loaded slots into one Program and hands it to the Sampler. Only the
// newest request per slot matters; an older one still decoding for that slot is abandoned.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include "../core/vfs.hpp"
#include "../formats/format.hpp"
#include "sampler.hpp"

namespace omni {

constexpr int SLOTS = 4;
constexpr int WAVE_BINS = 48;

struct ZoneBrief {
    std::string name;
    int root = 60, key_lo = 0, key_hi = 127, vel_lo = 0, vel_hi = 127, slot = 0;
    uint8_t wave[WAVE_BINS] = {};      // peak outline of its sample (0..255), for the display
};

// One slot's instrument
struct LoadedInfo {
    std::string path;                  // file loaded (full path), "" = empty slot
    int preset = 0;
    std::vector<PresetInfo> presets;   // of that file
    std::string name, format;
    int zones = 0, samples = 0, loops_added = 0;
    int64_t bytes = 0;
    std::vector<std::string> warnings;
    // an instrument extracted from a disk image (an .omni file): where it came from, for program change and stepping
    std::string source;                // "" = loaded from the file itself
    int source_preset = 0, source_presets = 0;
    std::string extracted_to;          // set when this load was just extracted (the .omni written)
};

// The merged program the sampler plays (all slots)
struct MergedInfo {
    uint32_t serial = 0;               // = Program::serial
    std::vector<ZoneBrief> zones;      // per zone, in the program's order
};

using Settings_kv = std::vector<std::pair<std::string, float>>;

class Loader {
public:
    Loader(Sampler &sampler, Vfs &vfs);
    ~Loader();
    // Load path's preset into slot (-1: the target slot). "" clears the slot.
    void request(const std::string &path, int preset, int slot = -1);
    void request_program(int preset) { program_req_.store(preset); }   // audio thread: lock-free, the target slot
    void set_budget_mb(int mb) { budget_mb_.store(mb); }
    void set_target(int slot) { target_.store(slot < 0 || slot >= SLOTS ? 0 : slot); }
    int target() const { return target_.load(); }
    void set_auto_loop(bool on) { auto_loop_.store(on); }
    // Extraction: mode 0 off, 1 = presets loaded from inside a disk image are written to dir (an .omni each) and the
    // slot then refers to that file (what a project saves). save() writes the target slot now, with the settings.
    void set_extract(int mode, const std::string &dir) { extract_mode_.store(mode); std::lock_guard<std::mutex> l(mutex_); extract_dir_ = dir; }
    void save() { save_req_.store(true); cv_.notify_all(); }
    // The plugin's sound settings, saved into extracted instruments (JSON object text), and applied when an
    // instrument file carrying them loads. Set once before use.
    std::function<std::string()> get_settings;
    std::function<void(const Settings_kv &)> apply_settings;

    // Snapshots for the UI (copies under lock)
    LoadedInfo info();                 // the target slot
    LoadedInfo slot_info(int slot);
    MergedInfo merged();
    std::string status();              // "" when idle
    int progress() const { return progress_.load(); }   // 0..100 while loading, -1 idle
    std::atomic<uint32_t> revision{0}; // bumps whenever info(), merged() or status() changes
    bool busy() const { return busy_.load(); }

private:
    struct Request { std::string path; int preset; int slot; };
    struct Slot {
        Instrument inst;
        std::vector<PcmPtr> pcm;
        std::string container;         // the disk image (or folder) it came from
    };

    Sampler &sampler_;
    Vfs &vfs_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> quit_{false};
    std::deque<Request> queue_;
    std::atomic<uint32_t> slot_serial_[SLOTS]{};
    std::atomic<int> budget_mb_{512};
    std::atomic<int> progress_{-1};
    std::atomic<int> target_{0};
    std::atomic<bool> auto_loop_{false};
    uint32_t prog_serial_ = 0;         // loader thread only
    std::atomic<bool> busy_{false};
    std::atomic<int> program_req_{-1};
    LoadedInfo info_[SLOTS];
    MergedInfo merged_;
    std::string status_;
    std::map<std::string, std::weak_ptr<const Pcm>> cache_;
    std::atomic<int> extract_mode_{1};
    std::atomic<bool> save_req_{false};
    std::string extract_dir_;
    Slot slots_[SLOTS];                // loader thread only

    void run();
    void load(const Request &r, uint32_t serial);
    bool publish(uint32_t serial, int slot);   // merge the slots and hand the program over; false if superseded
    void extract(int slot, bool with_settings);
    void set_status(const std::string &s);
};

}  // namespace omni

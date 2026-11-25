// Omni Sampler 2.4.33: the FX BANK expansion slots B / C / D (see fx_expansion.hpp).
// 2.4.35 (user, 11-22 / 11-23): B = Roland hardware 236 (XV-3080 80, SC-88 Pro 91, VP-9000 58, JD-990 7), C = other
// hardware 186 (Wavestation AD 56, Z-FX 53, Stellaris 48, K2000 28, SX-240 1): hardware_fx.hpp; the SC-88 / SC-88VL
// System + Delay rows are left out (the same recipes as the SC-88 Pro's). D = Camel 891 (camel_fx.hpp).
#include "fx_expansion.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#ifndef OMNI_PUBLIC
#define OMNI_PUBLIC 0
#endif
#if !OMNI_PUBLIC   // the public 2.3 build ships bank A only: slots B-D stay EMPTY (build_public.sh leaves these files out)
#include "camel_fx.hpp"
#include "hardware_fx.hpp"
#endif
#include "mv8800_bank_a.hpp"
#include "mv8800_fx.hpp"   // 2.4.37: the MV-8800 MFX (private: slot B after the Roland hardware; 2.4.39 public: bank A)

namespace omni {

namespace {
std::atomic<FxExpansionFactory> g_factory{nullptr};

// 2.4.37 (user): the MV-8800 MFX as programs "MV <effect>", each at its .MVE defaults; the page's MIX blends dry / wet (as
// CAMEL). 2.4.39: built from a list (effect index, dry): private slot B leaves the Live II's heavy ones out (bay_hidden),
// public bank A lists all 30 light-first and plays the heavy ones dry on 32-bit ARM ("64-bit MPC only")
class MvBank : public FxExpansion {
    omnisampler::mv8800::Processor fx_;
    std::vector<float> dl_, dr_;
    std::vector<unsigned> idx_;
    std::vector<bool> dry_;
    std::vector<std::string> names_;
    float mix_ = .5f;
    double bpm_ = 120, rate_ = 0;
    int sel_ = 0;
public:
    explicit MvBank(const std::vector<std::pair<unsigned, bool>> &list) {
        for (const auto &e : list) {
            if (e.first >= omnisampler::mv8800::count) continue;
            idx_.push_back(e.first);
            dry_.push_back(e.second);
            names_.push_back(std::string("MV ") + omnisampler::mv8800::programs[e.first].name + (e.second ? " (64-bit MPC only)" : ""));
        }
    }
    const char *title() const override { return "MV-8800"; }
    int count() const override { return int(idx_.size()); }
    const char *name(int p) const override { return p >= 1 && p <= count() ? names_[size_t(p - 1)].c_str() : ""; }
    void prepare(double sample_rate, int max_block) override {
        if (rate_ != sample_rate) { fx_.prepare(unsigned(sample_rate + .5)); rate_ = sample_rate; if (sel_ > 0) select(sel_); }
        dl_.assign(size_t(std::max(1, max_block)), 0.f);
        dr_.assign(size_t(std::max(1, max_block)), 0.f);
    }
    void select(int p) override {
        if (p < 1 || p > count()) return;
        sel_ = p;
        if (!dry_[size_t(p - 1)]) fx_.load(static_cast<omnisampler::mv8800::Effect>(idx_[size_t(p - 1)]));
    }
    void set(const omnisampler::fxbank::Controls &c) override { mix_ = std::isfinite(c.mix) ? std::max(0.f, std::min(1.f, c.mix)) : .5f; }
    void tempo(double bpm) override { bpm_ = bpm; }
    void process(float *l, float *r, int frames) override {
        if (sel_ <= 0 || dry_[size_t(sel_ - 1)] || frames <= 0 || mix_ <= 0 || dl_.empty()) return;
        for (int done = 0; done < frames;) {
            const int n = std::min(frames - done, int(dl_.size()));
            float *a = l + done, *b = r + done;
            std::memcpy(dl_.data(), a, sizeof(float) * size_t(n));
            std::memcpy(dr_.data(), b, sizeof(float) * size_t(n));
            fx_.process(a, b, unsigned(n), bpm_);
            if (mix_ < 1)
                for (int i = 0; i < n; i++) { a[i] = dl_[size_t(i)] + mix_ * (a[i] - dl_[size_t(i)]); b[i] = dr_[size_t(i)] + mix_ * (b[i] - dr_[size_t(i)]); }
            done += n;
        }
    }
    void reset() override { fx_.reset(); }
};

// public bank A: light ones (they run on the Live II) first, then the heavy ones (dry on 32-bit ARM)
std::vector<std::pair<unsigned, bool>> mv_bank_a_list() {
    std::vector<std::pair<unsigned, bool>> out;
#if defined(__arm__) && !defined(__aarch64__)
    const bool arm32 = true;
#else
    const bool arm32 = false;
#endif
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < omnisampler::mv8800::count; i++)
            if (mv_heavy_armv7(omnisampler::mv8800::programs[i].name) == (pass == 1)) out.push_back({i, pass == 1 && arm32});
    return out;
}

#if !OMNI_PUBLIC
namespace hw = omnisampler::hardware;

// 2.4.35 (user): on 32-bit ARM (the Live II) the programs its device bench measured at or over 2.5 % of a core are left
// out (tools/gen_bay_heavy.py from reports/bench-2435/live2-armv7.tsv); the Live III (arm64) and the desktop keep all
#include "bay_heavy_armv7.inc"
bool bay_hidden(char slot, const char *name) {
#if defined(__arm__) && !defined(__aarch64__)
    for (const auto &h : BAY_HEAVY_ARMV7) if (h.slot == slot && !std::strcmp(h.name, name)) return true;
#else
    (void)slot; (void)name;
#endif
    return false;
}

// one hardware rack for slots B and C (about 5.6 MB of lines: only one slot plays at a time; the sampler selects again
// whenever the slot changes, and the wrapper sets its controls again after every select)
struct HardwareShared {
    hw::Rack rack;
    double rate = 0;
};

// the page's 13 controls as a hardware stage's: TONE = the filter / EQ frequency (100 Hz .. 10 kHz), DEPTH = its Q,
// LOW / MID / HIGH over the graphic / spectrum bands, TIME spread over the four taps [G: the mapping is Omni's]
hw::Controls hw_controls(const omnisampler::fxbank::Controls &c) {
    hw::Controls h;
    static_cast<omnisampler::fxbank::Controls &>(h) = c;
    const float tone = std::isfinite(c.tone) ? std::max(0.f, std::min(1.f, c.tone)) : .55f;
    const float depth = std::isfinite(c.depth) ? std::max(0.f, std::min(1.f, c.depth)) : .65f;
    h.frequency = 100.f * std::pow(100.f, tone);
    h.q = .5f + 4.f * depth;
    const float lo = c.lowDb, mid = c.midDb, hi = c.highDb;
    const float g[8] = {lo, lo, .5f * (lo + mid), mid, mid, .5f * (mid + hi), hi, hi};
    for (int i = 0; i < 8; i++) h.gainDb[size_t(i)] = g[i];
    h.gainDb[0] = lo;
    for (int k = 0; k < 4; k++) h.tapMs[size_t(k)] = c.timeMs * float(k + 1) / 4.f;
    return h;
}

class HardwareBank : public FxExpansion {
    std::shared_ptr<HardwareShared> sh_;
    const char *title_;
    std::vector<size_t> ids_;          // catalog rows, in the slot's order
    std::vector<std::string> names_;
    hw::Controls c_;
    int sel_ = 0;
    void apply() { for (unsigned k = 0; k < sh_->rack.stages(); k++) sh_->rack.set(k, c_); }
public:
    // banks: the catalog bank names this slot takes, in order; short: their OLED prefixes
    HardwareBank(std::shared_ptr<HardwareShared> sh, const char *title, char slot, const std::vector<std::pair<const char *, const char *>> &banks)
        : sh_(std::move(sh)), title_(title) {
        size_t n = 0;
        const hw::Program *t = hw::programs(n);
        for (const auto &b : banks)
            for (size_t i = 0; i < n && ids_.size() < size_t(MAX_PROGRAMS); i++)
                if (!std::strcmp(t[i].bank, b.first) && t[i].count) {
                    std::string name = std::string(b.second) + " " + t[i].label;
                    if (bay_hidden(slot, name.c_str())) continue;
                    ids_.push_back(i);
                    names_.push_back(std::move(name));
                }
    }
    const char *title() const override { return title_; }
    int count() const override { return int(ids_.size()); }
    const char *name(int p) const override { return p >= 1 && p <= count() ? names_[size_t(p - 1)].c_str() : ""; }
    void prepare(double sample_rate, int) override {
        if (sh_->rate != sample_rate) { sh_->rack.prepare(float(sample_rate)); sh_->rate = sample_rate; }
    }
    void select(int p) override {
        if (p < 1 || p > count()) return;
        sel_ = p;
        if (sh_->rack.select(ids_[size_t(p - 1)])) apply();   // (a stage's select resets its controls)
    }
    void set(const omnisampler::fxbank::Controls &c) override { c_ = hw_controls(c); apply(); }
    void process(float *l, float *r, int frames) override { if (sel_ > 0 && frames > 0) sh_->rack.process(l, r, size_t(frames)); }
    void reset() override { if (sel_ > 0) { sh_->rack.reset(); apply(); } }
    void note_on() override { if (sel_ > 0) sh_->rack.noteOn(); }
};

class CamelBank : public FxExpansion {
    omnisampler::camel::Processor fx_;
    std::vector<float> dl_, dr_;     // the dry signal (for the page's MIX), made in prepare()
    float mix_ = .5f;
    double bpm_ = 120;
    int sel_ = 0;
    std::vector<unsigned> idx_;   // the presets this CPU plays (2.4.35: all but the Live II's over-limit ones)
public:
    CamelBank() {
        for (unsigned i = 0; i < omnisampler::camel::CAMEL_COUNT; i++)
            if (!bay_hidden('D', omnisampler::camel::CAMEL_PRESETS[i].name)) idx_.push_back(i);
    }
    const char *title() const override { return "CAMEL"; }
    int count() const override { return int(idx_.size()); }
    const char *name(int p) const override { return p >= 1 && p <= count() ? omnisampler::camel::CAMEL_PRESETS[idx_[size_t(p - 1)]].name : ""; }
    void prepare(double sample_rate, int max_block) override {
        fx_.prepare(float(sample_rate));
        dl_.assign(size_t(std::max(1, max_block)), 0.f);
        dr_.assign(size_t(std::max(1, max_block)), 0.f);
    }
    void select(int p) override { if (p >= 1 && p <= count() && fx_.load(idx_[size_t(p - 1)])) sel_ = p; }
    void set(const omnisampler::fxbank::Controls &c) override { mix_ = std::isfinite(c.mix) ? std::max(0.f, std::min(1.f, c.mix)) : .5f; }
    void tempo(double bpm) override { bpm_ = bpm; }
    void process(float *l, float *r, int frames) override {
        if (sel_ <= 0 || frames <= 0 || mix_ <= 0) return;
        for (int done = 0; done < frames;) {   // in blocks of the dry buffers
            const int n = std::min(frames - done, int(dl_.size()));
            float *a = l + done, *b = r + done;
            std::memcpy(dl_.data(), a, sizeof(float) * size_t(n));
            std::memcpy(dr_.data(), b, sizeof(float) * size_t(n));
            fx_.process(a, b, unsigned(n), bpm_);
            if (mix_ < 1)
                for (int i = 0; i < n; i++) { a[i] = dl_[size_t(i)] + mix_ * (a[i] - dl_[size_t(i)]); b[i] = dr_[size_t(i)] + mix_ * (b[i] - dr_[size_t(i)]); }
            done += n;
        }
    }
    void reset() override { fx_.reset(); }
};

// 2.4.37: slot B = the Roland hardware programs (unchanged numbers 1..n), then the MV-8800 programs
class RolandSlot : public FxExpansion {
    std::unique_ptr<FxExpansion> hw_, mv_;
    FxExpansion *on_ = nullptr;   // the bank of the selected program
public:
    RolandSlot(std::unique_ptr<FxExpansion> hw, std::unique_ptr<FxExpansion> mv) : hw_(std::move(hw)), mv_(std::move(mv)) {}
    const char *title() const override { return "ROLAND HW"; }
    int count() const override { return hw_->count() + mv_->count(); }
    const char *name(int p) const override { return p <= hw_->count() ? hw_->name(p) : mv_->name(p - hw_->count()); }
    void prepare(double sample_rate, int max_block) override { hw_->prepare(sample_rate, max_block); mv_->prepare(sample_rate, max_block); }
    void select(int p) override {
        if (p < 1 || p > count()) return;
        if (p <= hw_->count()) { hw_->select(p); on_ = hw_.get(); }
        else { mv_->select(p - hw_->count()); on_ = mv_.get(); }
    }
    void set(const omnisampler::fxbank::Controls &c) override { hw_->set(c); mv_->set(c); }
    void process(float *l, float *r, int frames) override { if (on_) on_->process(l, r, frames); }
    void reset() override { if (on_) on_->reset(); }
    void tempo(double bpm) override { hw_->tempo(bpm); mv_->tempo(bpm); }
    void note_on() override { if (on_) on_->note_on(); }
};

std::unique_ptr<FxExpansion> mv_slot_b() {
    std::vector<std::pair<unsigned, bool>> list;
    for (unsigned i = 0; i < omnisampler::mv8800::count; i++)
        if (!bay_hidden('B', (std::string("MV ") + omnisampler::mv8800::programs[i].name).c_str())) list.push_back({i, false});
    return std::make_unique<MvBank>(list);
}

const std::vector<std::pair<const char *, const char *>> &roland_banks() {
    static const std::vector<std::pair<const char *, const char *>> b = {
        {"XV-3080 MFX", "XV"}, {"SC-88 Pro System", "88P"}, {"SC-88 Pro Delay", "88P"}, {"SC-88 Pro Insert", "88P"},
        {"VP-9000 MULTI", "VP"}, {"VP-9000 CHORUS", "VP"}, {"VP-9000 REVERB", "VP"}, {"JD-990", "JD990"}};
    return b;
}
const std::vector<std::pair<const char *, const char *>> &other_banks() {
    static const std::vector<std::pair<const char *, const char *>> b = {
        {"Wavestation AD", "WSAD"}, {"Z-FX", "ZFX"}, {"Stellaris", "STELL"}, {"K2000", "K2K"}, {"SX-240", "SX240"}};
    return b;
}
#endif  // !OMNI_PUBLIC
}  // namespace

void make_fx_expansions(std::unique_ptr<FxExpansion> out[3]) {
    if (FxExpansionFactory f = g_factory.load()) {
        for (int k = 0; k < 3; k++) out[k] = f(k + 1);
        return;
    }
#if OMNI_PUBLIC
    for (int k = 0; k < 3; k++) out[k] = nullptr;   // empty slots
#else
    auto sh = std::make_shared<HardwareShared>();
    out[0] = std::make_unique<RolandSlot>(std::make_unique<HardwareBank>(sh, "ROLAND HW", 'B', roland_banks()), mv_slot_b());   // 2.4.37: + MV
    out[1] = std::make_unique<HardwareBank>(sh, "HARDWARE", 'C', other_banks());
    out[2] = std::make_unique<CamelBank>();
#endif
}

std::unique_ptr<FxExpansion> make_fx_expansion(int slot) {
    if (FxExpansionFactory f = g_factory.load()) return f(slot);
#if OMNI_PUBLIC
    (void)slot;
    return nullptr;
#else
    switch (slot) {
    case 1: return std::make_unique<RolandSlot>(std::make_unique<HardwareBank>(std::make_shared<HardwareShared>(), "ROLAND HW", 'B', roland_banks()), mv_slot_b());
    case 2: return std::make_unique<HardwareBank>(std::make_shared<HardwareShared>(), "HARDWARE", 'C', other_banks());
    case 3: return std::make_unique<CamelBank>();
    default: return nullptr;
    }
#endif
}

std::unique_ptr<FxExpansion> make_mv_bank_a() { return std::make_unique<MvBank>(mv_bank_a_list()); }

void set_fx_expansion_factory(FxExpansionFactory f) { g_factory.store(f); }

}  // namespace omni

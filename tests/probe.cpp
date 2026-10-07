// Omni Sampler: desktop probe. Lists a folder or disk image, or prints what a reader makes of a file.
//   probe <path>                 list (folder / image) or presets (file)
//   probe <path> <preset> [-d]   zones of one preset; -d also decodes every sample (peak, frames)
//   probe <path> -all [-d]       every preset of a file
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include "../src/formats/format.hpp"

using namespace omni;

static const char *loop_name(LoopType t) { return t == LoopType::Forward ? "fwd" : t == LoopType::Backward ? "bwd" : "alt"; }

static void dump(VolumePtr vol, const std::string &inner, const FormatReader *r, int preset, bool decode) {
    Instrument inst = r->load(vol, inner, preset);
    std::printf("instrument \"%s\" format=%s zones=%zu groups=%zu poly=%d gain=%.2f\n", inst.name.c_str(),
                inst.format.c_str(), inst.zones.size(), inst.groups.size(), inst.polyphony, inst.gain_db);
    for (auto &w : inst.warnings) std::printf("  warning: %s\n", w.c_str());
    std::map<SampleRef *, PcmPtr> done;
    for (auto &z : inst.zones) {
        std::printf("  zone \"%s\" g%d key %d-%d root %d vel %d-%d tune %+.3f gain %+.2f pan %+.2f kt %.2f start %lld stop %lld",
                    z.name.c_str(), z.group, z.key_lo, z.key_hi, z.root, z.vel_lo, z.vel_hi, z.tune, z.gain_db, z.pan,
                    z.key_tracking, (long long)z.start, (long long)z.stop);
        for (auto &l : z.loops) std::printf(" loop %s %lld-%lld%s", loop_name(l.type), (long long)l.start, (long long)l.end, l.until_release ? " sus" : "");
        if (z.one_shot) std::printf(" oneshot");
        if (z.reverse) std::printf(" reverse");
        if (z.exclusive_group) std::printf(" excl %d", z.exclusive_group);
        if (z.trigger != Trigger::Attack) std::printf(" trig %d", int(z.trigger));
        if (z.play_logic != PlayLogic::Always) std::printf(" rr %d/%d", z.seq_position, z.seq_length);
        if (z.amp_env.set) std::printf(" env a%.3f h%.3f d%.3f s%.2f r%.3f", z.amp_env.attack, z.amp_env.hold, z.amp_env.decay, z.amp_env.sustain, z.amp_env.release);
        if (z.filter.type != FilterType::None) std::printf(" filter %d/%dp %.0fHz res %.2f env %.2f", int(z.filter.type), z.filter.poles, z.filter.cutoff, z.filter.resonance, z.filter.env_depth);
        std::printf(" sample \"%s\" %dHz %dch %lld", z.sample->name.c_str(), z.sample->rate, z.sample->channels, (long long)z.sample->frames);
        if (decode) {
            auto it = done.find(z.sample.get());
            if (it == done.end()) {
                PcmPtr p;
                try { p = z.sample->decode(); } catch (const std::exception &e) { std::printf(" DECODE ERROR %s", e.what()); }
                it = done.emplace(z.sample.get(), p).first;
            }
            if (it->second) {
                const Pcm &p = *it->second;
                int peak = 0;
                double sum = 0;
                for (int16_t s : p.data) { peak = std::max(peak, std::abs(int(s))); sum += double(s) * s; }
                std::printf(" -> %dHz %dch %lld frames peak %d rms %.0f", p.rate, p.channels, (long long)p.frames(), peak,
                            p.data.empty() ? 0.0 : std::sqrt(sum / double(p.data.size())));
            }
        }
        std::printf("\n");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: probe <path> [preset|-all] [-d]\n"); return 2; }
    register_all();
    Vfs vfs;
    std::string path = argv[1];
    bool decode = argc > 2 && !std::strcmp(argv[argc - 1], "-d");
    try {
        if (vfs.is_container(path)) {
            for (auto &e : vfs.list(path)) std::printf("%s %12llu %s\n", e.dir ? "d" : "-", (unsigned long long)e.size, e.name.c_str());
            return 0;
        }
        Location loc = vfs.resolve(path);
        const FormatReader *r = find_reader(*loc.volume, loc.inner);
        if (!r) { std::printf("no reader\n"); return 1; }
        std::vector<PresetInfo> presets = r->list(loc.volume, loc.inner);
        std::printf("reader: %s, %zu presets\n", r->name, presets.size());
        if (argc == 2 || (argc == 3 && decode)) {
            for (size_t i = 0; i < presets.size(); i++) std::printf("  %zu: %s\n", i, presets[i].name.c_str());
            if (presets.size() == 1) dump(loc.volume, loc.inner, r, presets[0].index, decode);
            return 0;
        }
        if (!std::strcmp(argv[2], "-all")) {
            for (auto &p : presets) dump(loc.volume, loc.inner, r, p.index, decode);
            return 0;
        }
        int i = std::atoi(argv[2]);
        if (i < 0 || i >= int(presets.size())) { std::printf("no preset %d\n", i); return 1; }
        dump(loc.volume, loc.inner, r, presets[size_t(i)].index, decode);
    } catch (const std::exception &e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
    return 0;
}

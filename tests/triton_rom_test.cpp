#include <algorithm>
#include "../src/formats/format.hpp"
#include <cassert>
#include <iostream>
#include <set>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    omni::register_korg_triton();
    omni::Vfs fs;
    auto location = fs.resolve(argv[1]);
    auto reader = omni::find_reader(*location.volume, location.inner);
    assert(reader);
    size_t programs = 0, partial = 0, silent = 0;
    std::set<std::string> decoded;
    for (const auto &preset : reader->list(location.volume, location.inner)) {
        auto inst = reader->load(location.volume, location.inner, preset.index);
        if (!inst.warnings.empty()) ++partial;
        for (const auto &zone : inst.zones) {
            assert(zone.sample && zone.key_lo <= zone.key_hi);
            for (const auto &loop : zone.loops)
                assert(loop.start >= 0 && loop.start <= loop.end && loop.end < zone.sample->frames);
            if (!decoded.insert(zone.sample->key).second) continue;
            auto audio = zone.sample->decode();
            assert(audio->frames() == zone.sample->frames);
            if (std::none_of(audio->data.begin(), audio->data.end(), [](int16_t x){ return x != 0; })) ++silent;
        }
        ++programs;
    }
    assert(programs == 1287 && partial == 0 && silent == 0);
    std::cout << programs << " listed programs loaded, " << partial << " with warnings, "
              << decoded.size() << " distinct sample references decoded, " << silent << " silent.\n";
}

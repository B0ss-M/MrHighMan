#include <cstdio>
#include <memory>
#include "../src/engine/loader.hpp"

using namespace omni;

static int fails = 0;
static void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

int main() {
    auto pcm = std::make_shared<Pcm>();
    pcm->channels = 1;
    pcm->data.assign(256, 0);
    Zone z;
    z.sample = std::make_shared<SampleRef>();
    z.loops.push_back(Loop{LoopType::Forward, 32, 191, false, 8, 0});
    Instrument inst;
    inst.zones.push_back(z);
    std::vector<PcmPtr> decoded{pcm};
    int skipped = 0;

    const int64_t added0 = bake_loop_crossfades(inst, decoded, 0, &skipped);
    check(added0 == 0 && skipped == 1, "a crossfade copy over the remaining budget is refused");
    check(decoded[0] == pcm, "the original PCM remains playable when the copy is refused");
    check(inst.zones[0].loops[0].crossfade == 8, "a refused crossfade remains in source metadata");

    skipped = 0;
    const int64_t added = bake_loop_crossfades(inst, decoded, int64_t(pcm->bytes()), &skipped);
    check(added == int64_t(pcm->bytes()) && skipped == 0, "one fitting crossfade copy is admitted");
    check(decoded[0] != pcm, "the fitted copy replaces the uncrossfaded PCM");
    check(inst.zones[0].loops[0].crossfade == 0, "the baked copy is not baked twice");
    check(added + int64_t(pcm->bytes()) <= int64_t(2 * pcm->bytes()),
          "the copy accounting stays within PCM plus admitted copy");
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}

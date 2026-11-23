// Included inside the Triton reader's private namespace.
struct PianoRegion {
    int high_key, root, fine_cents;
    uint32_t start, alternate, loop, end;
};
#include "triton_piano_map.inc"

bool add_extreme_piano(const BlobPtr &rom, const std::string &identity, int instrument,
                       bool offset, const Zone &tmpl, std::vector<Zone> &out) {
    if (!rom || instrument < 0 || instrument >= 4) return false;
    int low = 0;
    for (int split = 0; split < 26; ++split) {
        const auto &r = piano_regions[instrument * 26 + split];
        const uint64_t byte = 0x2a80000ull + 2ull * r.start;
        const uint32_t frames = r.end - r.start;
        Zone z = tmpl;
        z.key_lo = low;
        z.key_hi = r.high_key;
        low = r.high_key + 1;
        z.root = r.root;
        z.tune += r.fine_cents / 100.0;
        z.start = offset ? r.alternate - r.start : 0;
        Loop loop;
        loop.start = r.loop - r.start;
        loop.end = frames - 1;
        if (!z.reverse) z.loops.push_back(loop);
        auto ref = std::make_shared<SampleRef>();
        ref->key = identity + ":" + std::to_string(byte);
        ref->name = std::string("A.Piano-BD ") + (instrument < 2 ? "ff " : "mf ") + (instrument % 2 ? "R" : "L");
        ref->rate = 35000; // Effective rate estimated from piano fundamentals against the root keys.
        ref->frames = frames;
        ref->channels = 1;
        ref->decode = [rom, byte, frames]() -> PcmPtr {
            auto data = rom->bytes(byte, 2 * size_t(frames));
            triton_extreme::decode_span(byte, data.data(), data.size());
            auto pcm = std::make_shared<Pcm>();
            pcm->rate = 35000;
            pcm->channels = 1;
            pcm->data.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                const int word = data[2*i] + 256 * data[2*i+1];
                pcm->data[i] = int16_t(word < 32768 ? word : word - 65536);
            }
            return pcm;
        };
        z.name = ref->name;
        z.sample = std::move(ref);
        out.push_back(std::move(z));
    }
    return true;
}

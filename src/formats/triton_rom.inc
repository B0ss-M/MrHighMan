// Private implementation of the Extreme BIN-backed multisample reader.
struct RomSplit {
    int low, high, root, fine, level, flags;
    uint32_t start, alternate, loop, end;
    int pitch;
};
struct RomMap {
    int bank, number, first, count;
    const char *name;
    int mode;
};
#include "triton_rom_map.inc"

bool add_extreme_rom(const BlobPtr &rom, const std::shared_ptr<ClassicRom> &classic, const std::string &identity, int bank, int number,
                     bool offset, const Zone &tmpl, std::vector<Zone> &out) {
    if (!rom && !classic) return false;
    auto map = std::lower_bound(std::begin(rom_maps), std::end(rom_maps), std::make_pair(bank, number),
        [](const RomMap &m, const std::pair<int,int> &key) {
            return std::make_pair(m.bank, m.number) < key;
        });
    if (map == std::end(rom_maps) || map->bank != bank || map->number != number) return false;
    const auto initial = out.size();
    for (int i = 0; i < map->count; ++i) {
        const auto &r = rom_splits[map->first + i];
        if (r.end <= r.start) continue;
        const uint64_t byte = 2ull * r.start;
        const size_t frames = r.end - r.start;
        const int64_t physical = classic ? classic->locate(byte, frames * 2ull) : int64_t(byte);
        if (physical < 0) { out.resize(initial); return false; }
        if (rom && byte + frames * 2ull > rom->size()) throw ParseError("TRITON sample exceeds BIN");
        Zone z = tmpl;
        z.key_lo = r.low;
        z.key_hi = r.high;
        z.root = r.root;
        z.tune += r.fine / 100.0;
        // 2.4.27 (velocity audit #8): the split's level (Korg s8 -99..+99, amplitude 1 + level / 99, as KMP RLP1 in korg.cpp):
        // was never applied, so key splits and the mf / ff layers played at the wrong relative loudness
        z.gain_db += 20.0 * std::log10(std::max(0.01, 1.0 + r.level / 99.0));
        if (offset && r.alternate >= r.start && r.alternate < r.end) z.start = r.alternate - r.start;
        if (!z.reverse && !(r.flags & 0xa0) && r.loop >= r.start && r.loop < r.end) {
            Loop loop;
            loop.start = r.loop - r.start;
            loop.end = frames - 1;
            z.loops.push_back(loop);
        }
        const int rate = int(std::lround(48000 * std::exp2(r.pitch / 1536.0)));
        auto ref = std::make_shared<SampleRef>();
        ref->key = identity + ":" + std::to_string(byte) + ":" + std::to_string(frames) + ":" + std::to_string(rate);
        ref->name = map->name;
        ref->frames = frames;
        ref->rate = rate;
        ref->channels = 1;
        ref->decode = [rom, classic, physical, frames, rate]() -> PcmPtr {
            std::vector<uint8_t> data(frames * 2);
            if (classic) classic->read(physical, data.data(), data.size());
            else {
                rom->read(physical, data.data(), data.size());
                triton_extreme::decode_span(physical, data.data(), data.size());
            }
            auto pcm = std::make_shared<Pcm>();
            pcm->rate = rate;
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
    return out.size() > initial;
}

// Omni Sampler: Akai S612 sounds (.s612: "S612" + the S612's SysEx dump body). The 7-bit dump has a 32-byte
// parameter block sent as 7-bit low / 1-bit high pairs, one odd byte, then the whole 32,256-word wave memory as
// 7-bit pairs: 12-bit offset-binary samples in the top of 14 bits. Parameter byte 4 is the scan mode (1 = one-shot).
// The S612 clocks its memory from the key played: 16 kHz at C4 (key 60), up an octave per octave (manual, table 1),
// whatever rate the sound was recorded at, so every sound maps as 16 kHz with root 60 and plays at its true pitch.
// The splice (loop) point and lever positions are not decoded: sounds play start to end, without a loop.
#include <cstdlib>
#include <cstring>
#include "format.hpp"

namespace omni {
namespace {

constexpr size_t kParams = 4, kData = 69, kFrames = 32256;

std::vector<PresetInfo> list_s612(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

PcmPtr decode_s612(const std::vector<uint8_t> &d) {
    auto p = std::make_shared<Pcm>();
    p->rate = 16000;
    p->channels = 1;
    for (size_t i = kData; i + 1 < d.size() && p->data.size() < kFrames; i += 2)
        p->data.push_back(int16_t(((d[i] & 0x7F) | (d[i + 1] & 0x7F) << 7) * 4 - 32768));
    return p;
}

Instrument load_s612(VolumePtr vol, const std::string &path, int) {
    BlobPtr b = vol->open(path);
    std::vector<uint8_t> d = b->all(1 << 20);
    if (d.size() < kData + 2) throw ParseError("not an Akai S612 sound");
    int mode = (d[kParams + 8] & 0x7F) | (d[kParams + 9] & 1) << 7;
    PcmPtr pcm = decode_s612(d);
    // the memory past the sound holds the recorder's idle level: stop at the last sample that moves
    int64_t end = int64_t(pcm->data.size());
    while (end > 1 && std::abs(pcm->data[size_t(end - 1)]) < 256) end--;
    auto r = std::make_shared<SampleRef>();
    r->name = path_stem(path);
    r->rate = pcm->rate;
    r->channels = 1;
    r->frames = int64_t(pcm->data.size());
    r->key = "s612:" + path;
    r->decode = [pcm]() { return pcm; };
    Instrument inst;
    inst.format = "Akai S612";
    inst.name = path_stem(path);
    Zone z;
    z.name = inst.name;
    z.sample = r;
    z.root = 60;
    z.stop = end;
    z.one_shot = mode == 1;
    inst.zones.push_back(std::move(z));
    finish_instrument(inst);
    return inst;
}

bool probe_s612(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    return n >= 4 && !std::memcmp(h, "S612", 4) && size >= kData + 2;
}

}  // namespace

void register_akai_s612() {
    register_reader({"Akai S612", "s612", probe_s612, list_s612, load_s612});
}

}  // namespace omni

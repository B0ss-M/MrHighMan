// Nexus 4.5 KLP1 multisamples. The block codec is known; this reader maps
// the observed 16 stereo-pair layout using measured pitches.
#include "format.hpp"

namespace omni {
namespace {

constexpr uint64_t BLOCK_START = 0x698;
constexpr size_t HEADER_SIZE = 32;
constexpr size_t BLOCK_COUNT = 32;
constexpr uint32_t MAX_FRAMES = 1000000;
constexpr int RATE = 44100;

struct Block {
    uint64_t offset;
    uint32_t frames, groups, packed_size, tail_frames;
};

bool probe(const std::string &, const uint8_t *head, size_t count, uint64_t size) {
    return count >= 20 && size > BLOCK_START && std::memcmp(head, "NEXUS4.5 Sample", 16) == 0 &&
           le32(head + 16) == 1;
}

std::vector<Block> index_blocks(const Blob &blob) {
    std::vector<Block> blocks;
    const uint64_t size = blob.size();
    uint64_t offset = BLOCK_START;
    while (offset < size && blocks.size() < BLOCK_COUNT) {
        auto header = blob.head(HEADER_SIZE, offset);
        if (header.size() != HEADER_SIZE || std::memcmp(header.data(), "KLP1", 4) != 0)
            throw ParseError("Nexus NXS: missing KLP1 block header");
        Block block{offset, le32(header.data() + 4), le32(header.data() + 8),
                    le32(header.data() + 12), 0};
        uint64_t packed_frames = uint64_t(block.groups) * 64;
        if (!block.groups || block.frames > MAX_FRAMES || packed_frames > block.frames ||
            block.frames - packed_frames >= 64)
            throw ParseError("Nexus NXS: invalid KLP1 frame count");
        block.tail_frames = block.frames - uint32_t(packed_frames);
        uint64_t bytes = HEADER_SIZE + uint64_t(block.groups) + block.packed_size + 2 * block.tail_frames;
        if (bytes > size - offset)
            throw ParseError("Nexus NXS: truncated KLP1 block");
        uint64_t expected = 0;
        for (uint8_t control : blob.bytes(offset + HEADER_SIZE, block.groups)) {
            if ((control >> 4) > 3) throw ParseError("Nexus NXS: unknown KLP1 bias code");
            expected += 8 * ((control & 15) + 1);
        }
        if (expected != block.packed_size)
            throw ParseError("Nexus NXS: KLP1 packed size does not match its control table");
        blocks.push_back(block);
        offset += bytes;
    }
    if (offset != size || blocks.size() != BLOCK_COUNT)
        throw ParseError("Nexus NXS: only the measured 16-pair KLP1 layout is supported");
    for (size_t i = 0; i < blocks.size(); i += 2)
        if (blocks[i].frames != blocks[i + 1].frames)
            throw ParseError("Nexus NXS: adjacent channel blocks have different lengths");
    return blocks;
}

int16_t signed_word(uint16_t value) {
    return int16_t(value < 32768 ? int(value) : int(value) - 65536);
}

void decode_channel(const Blob &blob, const Block &block, Pcm &pcm, size_t channel) {
    auto header = blob.bytes(block.offset, HEADER_SIZE);
    auto data = blob.bytes(block.offset + HEADER_SIZE,
                           size_t(block.groups) + block.packed_size + 2 * block.tail_frames);
    Reader packed(data);
    packed.seek(block.groups);
    uint16_t previous[8], delta[8]{};
    for (size_t lane = 0; lane < 8; ++lane)
        previous[lane] = le16(header.data() + 16 + lane * 2);
    size_t lane_length = size_t(block.groups) * 8;
    for (size_t group = 0; group < block.groups; ++group) {
        unsigned control = data[group], width = (control & 15) + 1;
        unsigned bias = (control >> 4) << (width > 3 ? width - 3 : 0);
        unsigned mask = (1u << width) - 1;
        unsigned bits = 0, count = 0;
        for (size_t row = 0; row < 8; ++row) {
            for (size_t lane = 0; lane < 8; ++lane) {
                while (count < width) {
                    bits |= unsigned(packed.u8()) << count;
                    count += 8;
                }
                delta[lane] = uint16_t(delta[lane] + (bits & mask) - bias);
                previous[lane] = uint16_t(previous[lane] + delta[lane]);
                pcm.data[2 * (lane * lane_length + group * 8 + row) + channel] =
                    signed_word(previous[lane]);
                bits >>= width;
                count -= width;
            }
        }
    }
    size_t tail_start = lane_length * 8;
    for (size_t i = 0; i < block.tail_frames; ++i)
        pcm.data[2 * (tail_start + i) + channel] = signed_word(packed.u16le());
}

std::vector<PresetInfo> list(VolumePtr, const std::string &path) {
    return {{path_stem(path), 0}};
}

Instrument load(VolumePtr vol, const std::string &path, int index) {
    if (index != 0) throw ParseError("Nexus NXS: invalid preset index");
    auto blob = vol->open(path);
    auto head = blob->head(20);
    if (!probe({}, head.data(), head.size(), blob->size()))
        throw ParseError("Nexus NXS: unsupported header");
    auto blocks = index_blocks(*blob);

    Instrument inst;
    inst.name = path_stem(path);
    inst.format = "Nexus KLP1";
    inst.warnings.push_back("NXS: inferred 44.1 kHz stereo map, MIDI 24-84; original loop points unavailable");
    for (size_t i = 0; i < blocks.size(); i += 2) {
        const Block left = blocks[i], right = blocks[i + 1];
        Zone zone;
        zone.root = 24 + int(i) * 2;
        zone.key_lo = i == 0 ? zone.root : zone.root - 1;
        zone.key_hi = i + 2 == blocks.size() ? zone.root : zone.root + 2;
        zone.name = inst.name + " " + std::to_string(zone.root);
        auto sample = std::make_shared<SampleRef>();
        sample->key = "nxs-klp:" + path + ":" + std::to_string(left.offset);
        sample->name = zone.name;
        sample->rate = RATE;
        sample->channels = 2;
        sample->frames = left.frames;
        sample->decode = [blob, left, right]() -> PcmPtr {
            auto pcm = std::make_shared<Pcm>();
            pcm->rate = RATE;
            pcm->channels = 2;
            pcm->data.resize(size_t(left.frames) * 2);
            decode_channel(*blob, left, *pcm, 0);
            decode_channel(*blob, right, *pcm, 1);
            return pcm;
        };
        zone.sample = sample;
        inst.zones.push_back(std::move(zone));
    }
    finish_instrument(inst);
    return inst;
}

} // namespace

void register_nxs_klp() {
    register_reader({"Nexus KLP1", "nxs", probe, list, load});
}

} // namespace omni

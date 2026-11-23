// PLC2 version 1, mode 1. Payload offsets start immediately after the wrapper.
#pragma once
#include "../core/audio.hpp"
#include <cstring>

namespace omni {
inline PcmPtr read_arta(const Blob &file, AudioInfo *info, bool header_only) {
    const auto header = file.head(16);
    if (header.size() != 16 || std::memcmp(header.data(), "PLC2", 4) ||
        header[4] != 1 || header[5] != 1)
        throw ParseError("Unsupported ARTA wrapper version or mode");
    const uint64_t length = file.size() - 16;
    if (length < 42 || length > (128u << 20)) throw ParseError("Invalid ARTA payload length");
    auto bytes = file.bytes(16, header_only ? 42 : size_t(length));
    uint32_t state = uint32_t(length);
    for (size_t offset = 0; offset < bytes.size(); ++offset) {
        const uint32_t feedback = (state ^ (state >> 2) ^ (state >> 3) ^ (state >> 5)) & 1;
        state = (state >> 1) | (feedback << 31);
        bytes[offset] ^= uint8_t(state ^ offset);
    }
    if (std::memcmp(bytes.data(), "fLaC", 4) || (bytes[4] & 127) != 0 ||
        bytes[5] || bytes[6] || bytes[7] != 34)
        throw ParseError("ARTA has no FLAC STREAMINFO");
    uint64_t packed = 0;
    for (size_t i = 18; i < 26; ++i) packed = (packed << 8) | bytes[i];
    AudioInfo meta;
    meta.rate = int(packed >> 44);
    meta.channels = int((packed >> 41) & 7) + 1;
    meta.bits = int((packed >> 36) & 31) + 1;
    meta.frames = packed & 0xfffffffffULL;
    if (!meta.rate || !meta.frames || meta.frames * meta.channels > (128u << 20) / sizeof(int16_t))
        throw ParseError("Invalid or oversized ARTA audio");
    if (info) *info = meta;
    if (header_only) return nullptr;
    auto pcm = decode_audio(*make_mem_blob(std::move(bytes)));
    if (pcm->frames() != meta.frames) throw ParseError("Incomplete ARTA FLAC stream");
    return pcm;
}
} // namespace omni

#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "format.hpp"

namespace omni {

// SysEx messages (without F0 / F7) from a raw .syx or a Standard MIDI File. 2.4.33 (review of T7): an SMF is recognised by
// its MThd header and every F0 there is followed by a VLQ length (was: "not 0x41 / 0x42 = a length", which misread an SMF
// SysEx 65 or 66 bytes long as raw Roland / Korg data)
inline std::vector<std::vector<uint8_t>> mex_sysex_messages(const std::vector<uint8_t> &d) {
    std::vector<std::vector<uint8_t>> out;
    const bool smf = d.size() >= 4 && !std::memcmp(d.data(), "MThd", 4);
    for (size_t i = 0; i < d.size();) {
        if (d[i] != 0xF0) { i++; continue; }
        size_t p = i + 1;
        if (smf) {
            for (int n = 0; n < 4 && p < d.size(); n++) {
                uint8_t v = d[p++];
                if ((v & 0x80) == 0) break;
            }
        }
        size_t e = p;
        while (e < d.size() && d[e] != 0xF7) e++;
        if (e < d.size() && p < e) out.emplace_back(d.begin() + long(p), d.begin() + long(e));
        i = e < d.size() ? e + 1 : i + 1;
    }
    return out;
}

inline uint32_t mex_roland_address(const uint8_t *p) {
    return uint32_t(p[0]) * 0x4000 + uint32_t(p[1]) * 0x80 + p[2];
}

inline std::vector<uint8_t> mex_roland_dt1(BlobPtr blob, uint8_t model) {
    std::vector<uint8_t> d = blob->all(size_t(8) << 20);
    std::vector<uint8_t> out, current;
    uint32_t next = 0;
    bool started = false;
    for (const auto &m : mex_sysex_messages(d)) {
        if (m.size() < 10 || m[0] != 0x41 || m[2] != model || m[3] != 0x12) continue;
        const uint32_t address = mex_roland_address(&m[4]);
        const size_t payload = m.size() - 8;
        if (!started || address != next) {
            if (current.size() > out.size()) out = current;
            current.clear();
            started = true;
            next = address;
        }
        current.insert(current.end(), m.begin() + 7, m.end() - 1);
        next += uint32_t(payload);
    }
    return current.size() > out.size() ? current : out;
}

inline std::vector<uint8_t> mex_iff_body(BlobPtr blob) {
    std::vector<uint8_t> d = blob->all(size_t(8) << 20);
    if (d.size() < 16 || std::memcmp(d.data(), "FORM", 4) || std::memcmp(d.data() + 8, "BANK", 4)) return {};
    const uint64_t end = std::min<uint64_t>(d.size(), uint64_t(be32(&d[4])) + 8);
    for (size_t p = 12; p + 8 <= end;) {
        const uint32_t n = be32(&d[p + 4]);
        if (uint64_t(p) + 8 + n > end) return {};   // 2.4.33: 64-bit (a 32-bit size_t wrapped past a huge n on the Live II)
        if (!std::memcmp(&d[p], "BODY", 4)) return std::vector<uint8_t>(d.begin() + long(p + 8), d.begin() + long(p + 8 + n));
        p += 8 + ((n + 1) & ~uint32_t(1));
    }
    return {};
}

}  // namespace omni

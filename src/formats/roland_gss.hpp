// Omni Sampler: the Roland GSS PCM chip family's wave ROMs (SC-55 / SC-155 / SC-55mkII, JV-880, SR-JV80 boards), shared
// by roland_sc55.cpp and roland_jv880.cpp. Facts from libEmuSC (LGPL-2.1+) and measured on the ROMs (see those readers):
//   - Per 1 MB the first 32 bytes are plain; byte i (i >= 32) belongs at the address whose bit b is bit ADDR[b] of i,
//     with its bits in the order DATA.
//   - A frame is one signed byte shifted left by a 4-bit exponent from its MB's first 32 KB (byte (address & 0xFFFFF)
//     >> 5, the high nibble when address bit 4 is set), summed from the sample's start; x 31/64 to 16 bits (the
//     loudest sums, about 67,000, just fit).
//   - A loop runs from its start to the last frame inclusive; back-and-forth loops come back mirrored about their start.
//   - Envelope times: a 128-step table in ms (the SC-55's CPU ROMs).
#pragma once
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include "../core/model.hpp"

namespace omni {
namespace gss {

constexpr int RATE = 32000;
constexpr uint64_t MB = 0x100000;

// Appends the descrambled ROM to out (whole MBs). wide: the 16-bit-wide wave ROMs of the JV-1080 (other address
// lines, the same data lines, no plain header; the order from hackyourlife's srscramble, checked on the JV-1080's:
// "INT100A 1994-05-22" at 0x20, exponents 4 - 9)
inline void descramble(const std::vector<uint8_t> &raw, std::vector<uint8_t> &out, bool wide = false) {
    static const int ADDR8[20] = {2, 0, 3, 4, 1, 9, 13, 10, 18, 17, 6, 15, 11, 16, 8, 5, 12, 7, 14, 19};
    static const int ADDR16[20] = {0, 4, 2, 3, 1, 13, 7, 12, 5, 10, 16, 9, 6, 8, 14, 17, 11, 15, 18, 19};
    const int *const ADDR = wide ? ADDR16 : ADDR8;
    static const int DATA[8] = {2, 0, 4, 5, 7, 6, 3, 1};
    uint8_t dmap[256];
    for (int v = 0; v < 256; v++) {
        int o = 0;
        for (int b = 0; b < 8; b++) o |= ((v >> DATA[b]) & 1) << b;
        dmap[v] = uint8_t(o);
    }
    // each address bit comes from one source bit: the map is the OR of the low and the high 10 bits' maps
    uint32_t lo[1024] = {}, hi[1024] = {};
    for (int b = 0; b < 20; b++)
        for (uint32_t v = 0; v < 1024; v++) {
            if (ADDR[b] < 10 && ((v >> ADDR[b]) & 1)) lo[v] |= 1u << b;
            if (ADDR[b] >= 10 && ((v >> (ADDR[b] - 10)) & 1)) hi[v] |= 1u << b;
        }
    const size_t base = out.size();
    out.resize(base + raw.size());
    for (size_t m = 0; m + MB <= raw.size(); m += MB) {
        const uint32_t plain = wide ? 0 : 32;
        std::memcpy(&out[base + m], &raw[m], plain);
        for (uint32_t i = plain; i < MB; i++) out[base + m + (lo[i & 1023] | hi[i >> 10])] = dmap[raw[m + i]];
    }
}

// Frames at..at + last (inclusive) of a descrambled wave image; a back-and-forth loop of `loop` frames before the last
// one is unrolled (loop + 1 frames more). rate: the XV's samples have their own; out = sum x num / 64 (the SC-88's
// loudest sums, 87,040, need 24)
inline PcmPtr decode(const std::vector<uint8_t> &w, uint64_t at, int last, int pingpong_loop, int rate = RATE,
                     int num = 31) {
    auto p = std::make_shared<Pcm>();
    p->rate = rate;
    p->channels = 1;
    std::vector<int32_t> v;
    v.reserve(size_t(last) + 1);
    int32_t acc = 0;
    for (int i = 0; i <= last; i++) {
        const uint64_t a = at + uint64_t(i);
        if (a >= w.size()) break;
        const uint8_t ex = w[size_t(((a & 0xFFFFF) >> 5) | (a & ~uint64_t(0xFFFFF)))];
        const int sh = (a & 0x10) ? ex >> 4 : ex & 15;
        acc += int32_t(int8_t(w[size_t(a)])) * (1 << sh);
        v.push_back(acc);
    }
    const int n = int(v.size()) - 1;
    if (pingpong_loop > 0 && n == last && pingpong_loop <= last) {   // the way back, mirrored about the loop's start
        const int L = last - pingpong_loop;
        const int32_t sl = L > 0 ? v[size_t(L - 1)] : 0;
        v.push_back(sl + (v[size_t(last)] - v[size_t(last - 1)]));
        for (int m = 1; m < pingpong_loop; m++) v.push_back(2 * sl - v[size_t(last - m - 1)]);
        v.push_back(sl);
    }
    p->data.resize(v.size());
    for (size_t i = 0; i < v.size(); i++) p->data[i] = int16_t(std::clamp(int32_t(int64_t(v[i]) * num / 64), -32768, 32767));
    return p;
}

inline double env_seconds(int t) {
    static const uint16_t ENV_TIME[128] = {
        0, 4, 8, 15, 31, 40, 49, 58, 68, 78, 88, 99, 110, 122, 134, 146, 159, 173, 187, 202, 217, 233, 249, 266, 284, 302,
        321, 341, 362, 383, 406, 429, 453, 478, 504, 531, 559, 588, 618, 650, 682, 716, 751, 788, 826, 866, 907, 949, 994,
        1040, 1088, 1137, 1189, 1243, 1298, 1356, 1416, 1479, 1544, 1611, 1681, 1754, 1830, 1909, 1990, 2075, 2163, 2255,
        2350, 2449, 2552, 2658, 2769, 2885, 3004, 3129, 3258, 3392, 3531, 3676, 3827, 3983, 4146, 4315, 4490, 4672, 4862,
        5058, 5263, 5475, 5695, 5925, 6163, 6410, 6667, 6934, 7211, 7500, 7799, 8110, 8433, 8769, 9118, 9480, 9857, 10248,
        10655, 11077, 11516, 11972, 12445, 12937, 13448, 13980, 14531, 15105, 15700, 16319, 16962, 17630, 18324, 19045,
        19794, 20572, 21380, 22220, 23093, 24000};
    return ENV_TIME[t & 0x7F] / 1000.0;
}

}  // namespace gss
}  // namespace omni

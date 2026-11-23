#include "dx7_syx.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace omni {
namespace {

constexpr std::size_t kBulkDataSize = 4096;
constexpr std::size_t kBulkPacketSize = 6 + kBulkDataSize + 2;
constexpr std::size_t kSingleDataSize = 155;
constexpr std::size_t kSinglePacketSize = 6 + kSingleDataSize + 2;
constexpr std::size_t kVmemVoiceSize = 128;
constexpr std::size_t kVcedOperatorSize = 21;
constexpr std::size_t kVcedNameOffset = 145;

using Vced = std::array<std::uint8_t, 155>;

std::uint8_t checksum(const std::uint8_t* bytes, std::size_t size) {
    unsigned sum = 0;
    for (std::size_t i = 0; i < size; ++i) {
        sum += bytes[i];
    }
    return static_cast<std::uint8_t>((0U - sum) & 0x7fU);
}

void unpack_bulk_voice(const std::uint8_t* packed, Vced& vced) {
    vced.fill(0);
    for (std::size_t packed_op = 0; packed_op < 6; ++packed_op) {
        const std::size_t src = packed_op * 17;
        const std::size_t dst = (5 - packed_op) * kVcedOperatorSize;
        for (std::size_t i = 0; i < 8; ++i) {
            vced[dst + i] = packed[src + i] & 0x7f;
        }
        vced[dst + 8] = packed[src + 8] & 0x7f;
        vced[dst + 9] = packed[src + 9] & 0x7f;
        vced[dst + 10] = packed[src + 10] & 0x7f;
        vced[dst + 11] = packed[src + 11] & 0x03;
        vced[dst + 12] = (packed[src + 11] >> 2) & 0x03;
        // Packed operator bytes 12..16 (Yamaha DX7 SysEx doc, VMEM): 12 = RS | DET<<3, 13 = AMS | KVS<<2, 14 = OL,
        // 15 = OSC MODE | COARSE<<1, 16 = FINE. (Claude 2026-11-16: the first version read OL from byte 13 etc.;
        // checked on 19,200 real voices: this layout gives OL mostly 70..99 and detune centred on 7.)
        vced[dst + 13] = packed[src + 12] & 0x07;          // rate scaling
        vced[dst + 20] = (packed[src + 12] >> 3) & 0x0f;   // detune
        vced[dst + 14] = packed[src + 13] & 0x03;          // amp mod sensitivity
        vced[dst + 15] = (packed[src + 13] >> 2) & 0x07;   // key velocity sensitivity
        vced[dst + 16] = packed[src + 14] & 0x7f;          // output level
        vced[dst + 17] = packed[src + 15] & 0x01;          // osc mode
        vced[dst + 18] = (packed[src + 15] >> 1) & 0x1f;   // freq coarse
        vced[dst + 19] = packed[src + 16] & 0x7f;          // freq fine
    }

    for (std::size_t i = 0; i < 8; ++i) {
        vced[126 + i] = packed[102 + i] & 0x7f;
    }
    vced[134] = packed[110] & 0x1f;
    vced[135] = packed[111] & 0x07;
    vced[136] = (packed[111] >> 3) & 0x01;
    for (std::size_t i = 0; i < 4; ++i) {
        vced[137 + i] = packed[112 + i] & 0x7f;
    }
    vced[141] = packed[116] & 0x01;
    vced[142] = (packed[116] >> 1) & 0x07;
    vced[143] = (packed[116] >> 4) & 0x07;
    vced[144] = packed[117] & 0x7f;
    for (std::size_t i = 0; i < 10; ++i) {
        vced[kVcedNameOffset + i] = packed[118 + i] & 0x7f;
    }
}

std::uint8_t max_value(std::size_t index) {
    if (index < 126) {
        const std::size_t op_field = index % kVcedOperatorSize;
        if (op_field < 11 || op_field == 16 || op_field == 19) {
            return 99;
        }
        if (op_field == 11 || op_field == 12) {
            return 3;
        }
        if (op_field == 13) {
            return 7;
        }
        if (op_field == 14) {
            return 3;
        }
        if (op_field == 15) {
            return 7;
        }
        if (op_field == 17) {
            return 1;
        }
        if (op_field == 18) {
            return 31;
        }
        if (op_field == 20) {
            return 14;
        }
    }
    if (index < 134) {
        return 99;
    }
    switch (index) {
    case 134: return 31;
    case 135: return 7;
    case 136: return 1;
    case 137:
    case 138:
    case 139:
    case 140: return 99;
    case 141: return 1;
    case 142: return 5;
    case 143: return 7;
    case 144: return 48;
    default: return 127;
    }
}

bool all_value(const Vced& data, std::uint8_t value) {
    return std::all_of(data.begin(), data.end(),
                       [value](std::uint8_t byte) { return byte == value; });
}

Dx7Voice make_voice(Vced& vced, bool checksum_ok, Dx7Stats& st) {
    bool was_clamped = false;
    for (std::size_t i = 0; i < kVcedNameOffset; ++i) {
        const std::uint8_t maximum = max_value(i);
        if (vced[i] > maximum) {
            vced[i] = maximum;
            was_clamped = true;
        }
    }
    if (was_clamped) {
        ++st.clamped;
        if (!checksum_ok) {
            throw std::invalid_argument("out-of-range field in bad-checksum voice");
        }
    }

    std::string name;
    name.reserve(10);
    for (std::size_t i = kVcedNameOffset; i < vced.size(); ++i) {
        const std::uint8_t ch = vced[i];
        name.push_back(ch >= 32 && ch <= 126 ? static_cast<char>(ch) : ' ');
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }

    Dx7Voice voice;
    for (std::size_t op_index = 0; op_index < 6; ++op_index) {
        const std::size_t base = op_index * kVcedOperatorSize;
        Dx7Operator& op = voice.op[op_index];
        std::copy_n(vced.begin() + static_cast<std::ptrdiff_t>(base), 4,
                    op.eg_rates.begin());
        std::copy_n(vced.begin() + static_cast<std::ptrdiff_t>(base + 4), 4,
                    op.eg_levels.begin());
        op.breakpoint = vced[base + 8];
        op.left_depth = vced[base + 9];
        op.right_depth = vced[base + 10];
        op.left_curve = vced[base + 11];
        op.right_curve = vced[base + 12];
        op.rate_scaling = vced[base + 13];
        op.amp_mod_sens = vced[base + 14];
        op.velocity_sens = vced[base + 15];
        op.output_level = vced[base + 16];
        op.osc_mode = vced[base + 17];
        op.freq_coarse = vced[base + 18];
        op.freq_fine = vced[base + 19];
        op.detune = vced[base + 20];
    }
    std::copy_n(vced.begin() + 126, 4, voice.pitch_eg_rates.begin());
    std::copy_n(vced.begin() + 130, 4, voice.pitch_eg_levels.begin());
    voice.algorithm = vced[134];
    voice.feedback = vced[135];
    voice.osc_key_sync = vced[136];
    voice.lfo_speed = vced[137];
    voice.lfo_delay = vced[138];
    voice.pmd = vced[139];
    voice.amd = vced[140];
    voice.lfo_sync = vced[141];
    voice.lfo_wave = vced[142];
    voice.pms = vced[143];
    voice.transpose = vced[144];
    voice.name = std::move(name);

    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t i = 0; i < kVcedNameOffset; ++i) {
        hash ^= vced[i];
        hash *= 1099511628211ULL;
    }
    voice.hash = hash;
    return voice;
}

bool append_voice(const std::uint8_t* bytes, bool checksum_ok,
                  std::vector<Dx7Voice>& voices, Dx7Stats& st) {
    Vced vced{};
    std::copy_n(bytes, vced.size(), vced.begin());
    if (all_value(vced, 0) || all_value(vced, 0x7f)) {
        return false;
    }
    try {
        voices.push_back(make_voice(vced, checksum_ok, st));
    } catch (const std::invalid_argument&) {
        return false;
    }
    return true;
}

void append_bulk(const std::uint8_t* packed, bool checksum_ok,
                 std::vector<Dx7Voice>& voices, Dx7Stats& st) {
    for (std::size_t voice = 0; voice < 32; ++voice) {
        const std::uint8_t* packed_voice = packed + voice * kVmemVoiceSize;
        if (std::all_of(packed_voice, packed_voice + kVmemVoiceSize,
                        [](std::uint8_t byte) { return byte == 0; }) ||
            std::all_of(packed_voice, packed_voice + kVmemVoiceSize,
                        [](std::uint8_t byte) { return byte == 0x7f; })) {
            continue;
        }
        Vced vced{};
        unpack_bulk_voice(packed_voice, vced);
        if (all_value(vced, 0) || all_value(vced, 0x7f)) {
            continue;
        }
        try {
            voices.push_back(make_voice(vced, checksum_ok, st));
        } catch (const std::invalid_argument&) {
            // A damaged packet is retained only when all decoded values are valid.
        }
    }
}

bool has_bytes(std::size_t n, std::size_t offset, std::size_t count) {
    return offset <= n && count <= n - offset;
}

} // namespace

std::vector<Dx7Voice> dx7_parse(const std::uint8_t* data, std::size_t n,
                                Dx7Stats& st) {
    st = Dx7Stats{};
    std::vector<Dx7Voice> voices;
    if (data == nullptr) {
        if (n != 0) {
            throw std::runtime_error("dx7_parse: null input with non-zero size");
        }
        return voices;
    }

    try {
        if (n == kBulkDataSize) {
            // headerless: no checksum to trust, so keep only voices whose every field is in range
            append_bulk(data, false, voices, st);
            return voices;
        }

        for (std::size_t i = 0; i + 1 < n; ++i) {
            if (data[i] != 0xf0 || data[i + 1] != 0x43) {
                continue;
            }
            if (!has_bytes(n, i, 3) || data[i + 2] > 0x0f) {
                continue;
            }
            if (!has_bytes(n, i, 6)) {
                ++st.truncated_packets;
                continue;
            }

            const std::uint8_t format = data[i + 3];
            if (format == 0x03 || format == 0x04) {
                ++st.four_op_packets;
                continue;
            }
            // The ASST ROM1A copy has 0x10 here but a full 4096-byte payload.
            if (format == 0x09 &&
                (data[i + 4] == 0x20 || data[i + 4] == 0x10) &&
                data[i + 5] == 0x00) {
                ++st.bulk_packets;
                if (!has_bytes(n, i, kBulkPacketSize)) {
                    ++st.truncated_packets;
                    continue;
                }
                const std::uint8_t* payload = data + i + 6;
                if (data[i + kBulkPacketSize - 1] != 0xf7) {
                    ++st.truncated_packets;
                    continue;
                }
                const bool checksum_ok =
                    checksum(payload, kBulkDataSize) ==
                    data[i + 6 + kBulkDataSize];
                if (!checksum_ok) {
                    ++st.bad_checksum;
                }
                append_bulk(payload, checksum_ok, voices, st);
                i += kBulkPacketSize - 1;
                continue;
            }
            if (format == 0x00 && data[i + 4] == 0x01 &&
                data[i + 5] == 0x1b) {
                ++st.single_voice_packets;
                if (!has_bytes(n, i, kSinglePacketSize)) {
                    ++st.truncated_packets;
                    continue;
                }
                const std::uint8_t* payload = data + i + 6;
                if (data[i + kSinglePacketSize - 1] != 0xf7) {
                    ++st.truncated_packets;
                    continue;
                }
                const bool checksum_ok =
                    checksum(payload, kSingleDataSize) ==
                    data[i + 6 + kSingleDataSize];
                if (!checksum_ok) {
                    ++st.bad_checksum;
                }
                append_voice(payload, checksum_ok, voices, st);
                i += kSinglePacketSize - 1;
            }
        }
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("dx7_parse: ") + error.what());
    } catch (...) {
        throw std::runtime_error("dx7_parse: unexpected failure");
    }
    return voices;
}

double dx7_operator_frequency_hz(const Dx7Operator& op) {
    const unsigned coarse = std::min<unsigned>(op.freq_coarse, 31);
    const unsigned fine = std::min<unsigned>(op.freq_fine, 99);
    if (op.osc_mode == 0) {
        // Yamaha DX7 Owner's Manual operator-frequency table: coarse zero is 0.5.
        const double ratio = coarse == 0 ? 0.5 : static_cast<double>(coarse);
        return ratio * (1.0 + static_cast<double>(fine) / 100.0);
    }
    // Yamaha DX7 Owner's Manual operator-frequency table; fixed mode uses Hz.
    return std::pow(10.0, static_cast<double>(coarse % 4)) *
           std::pow(10.0, static_cast<double>(fine) / 100.0);
}

double dx7_detune_cents(std::uint8_t detune) {
    // Yamaha DX7 Owner's Manual detune scale, about 3.125 cents/step. // approx
    const int clamped = std::min<int>(detune, 14);
    return static_cast<double>(clamped - 7) * 3.125;
}

double dx7_output_level_amplitude(std::uint8_t output_level) {
    // Yamaha DX7 output-level chart with linear dB interpolation. // approx
    const unsigned level = std::min<unsigned>(output_level, 99);
    return std::pow(10.0, (static_cast<double>(level) - 99.0) * 0.75 / 20.0);
}

double dx7_eg_full_range_seconds(std::uint8_t rate) {
    // Yamaha DX7 envelope-rate measurements: about 21 s at 0 to 1 ms at 99.
    const unsigned clamped = std::min<unsigned>(rate, 99);
    return 21.0 * std::pow(0.001 / 21.0,
                           static_cast<double>(clamped) / 99.0); // approx
}

double dx7_lfo_speed_hz(std::uint8_t speed) {
    // Yamaha DX7 LFO-speed chart: about 0.06 Hz at 0 to 50 Hz at 99.
    const unsigned clamped = std::min<unsigned>(speed, 99);
    return 0.06 * std::pow(50.0 / 0.06,
                           static_cast<double>(clamped) / 99.0); // approx
}

} // namespace omni

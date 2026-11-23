#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omni {

struct Dx7Operator {
    std::array<std::uint8_t, 4> eg_rates{};
    std::array<std::uint8_t, 4> eg_levels{};
    std::uint8_t breakpoint = 0;
    std::uint8_t left_depth = 0;
    std::uint8_t right_depth = 0;
    std::uint8_t left_curve = 0;
    std::uint8_t right_curve = 0;
    std::uint8_t rate_scaling = 0;
    std::uint8_t amp_mod_sens = 0;
    std::uint8_t velocity_sens = 0;
    std::uint8_t output_level = 0;
    std::uint8_t osc_mode = 0;
    std::uint8_t freq_coarse = 0;
    std::uint8_t freq_fine = 0;
    std::uint8_t detune = 0;
};

struct Dx7Voice {
    // op[0] is OP1 and op[5] is OP6; bulk VMEM stores OP6 first.
    std::array<Dx7Operator, 6> op{};
    std::array<std::uint8_t, 4> pitch_eg_rates{};
    std::array<std::uint8_t, 4> pitch_eg_levels{};
    std::uint8_t algorithm = 0;
    std::uint8_t feedback = 0;
    std::uint8_t osc_key_sync = 0;
    std::uint8_t lfo_speed = 0;
    std::uint8_t lfo_delay = 0;
    std::uint8_t pmd = 0;
    std::uint8_t amd = 0;
    std::uint8_t lfo_sync = 0;
    std::uint8_t lfo_wave = 0;
    std::uint8_t pms = 0;
    std::uint8_t transpose = 0;
    std::string name;
    std::uint64_t hash = 0;
};

struct Dx7Stats {
    std::size_t bulk_packets = 0;
    std::size_t single_voice_packets = 0;
    std::size_t four_op_packets = 0;
    std::size_t truncated_packets = 0;
    std::size_t bad_checksum = 0;
    std::size_t clamped = 0;
};

std::vector<Dx7Voice> dx7_parse(const std::uint8_t* data, std::size_t n,
                                Dx7Stats& st);

double dx7_operator_frequency_hz(const Dx7Operator& op);
double dx7_detune_cents(std::uint8_t detune);
double dx7_output_level_amplitude(std::uint8_t output_level);
double dx7_eg_full_range_seconds(std::uint8_t rate);
double dx7_lfo_speed_hz(std::uint8_t speed);

} // namespace omni

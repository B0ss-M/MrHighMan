#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Decode a caller-owned span read at its absolute byte offset in TritonExtremePcm.bin.
// The transform operates on bytes; sample layout, addressing and loops belong to the bank map.
namespace omni { namespace triton_extreme {
inline std::uint32_t advance(std::uint32_t state) {
    return (state & 0x40000000u) ? ((state ^ 0x1064u) << 1) | 1u : state << 1;
}
using Transform = std::array<std::uint32_t, 32>;
inline std::uint32_t apply_transform(const Transform& columns, std::uint32_t state) {
    std::uint32_t result = 0;
    for (unsigned bit = 0; state; ++bit, state >>= 1)
        if (state & 1u) result ^= columns[bit];
    return result;
}
inline std::uint32_t state_at(std::uint64_t offset) {
    static const auto powers = [] {
        std::array<Transform, 64> table{};
        for (unsigned bit = 0; bit < 32; ++bit)
            table[0][bit] = advance(std::uint32_t{1} << bit);
        for (unsigned power = 1; power < table.size(); ++power)
            for (unsigned bit = 0; bit < 32; ++bit)
                table[power][bit] = apply_transform(table[power-1], table[power-1][bit]);
        return table;
    }();
    std::uint32_t state = 1;
    for (unsigned bit = 0; offset; ++bit, offset >>= 1)
        if (offset & 1u) state = apply_transform(powers[bit], state);
    return state;
}
inline void decode_span(std::uint64_t file_offset, std::uint8_t* bytes, std::size_t count) {
    auto state = state_at(file_offset);
    for (std::size_t i = 0; i < count; ++i) {
        state = advance(state);
        bytes[i] ^= static_cast<std::uint8_t>(state >> 8);
    }
}
}} // namespace omni::triton_extreme

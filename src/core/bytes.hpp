// Omni Sampler: bounds-checked binary reading. Every format reader goes through these, so a truncated or
// corrupt file throws ParseError instead of reading past its buffer.
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace omni {

struct ParseError : std::runtime_error {
    explicit ParseError(const std::string &what) : std::runtime_error(what) {}
};

inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t le24(const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16); }
inline uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint64_t le64(const uint8_t *p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }
inline uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t be24(const uint8_t *p) { return uint32_t(p[0] << 16 | p[1] << 8 | p[2]); }
inline uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }
inline uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

// A read cursor over a byte range it does not own.
class Reader {
public:
    Reader() = default;
    Reader(const uint8_t *data, size_t size) : data_(data), size_(size) {}
    explicit Reader(const std::vector<uint8_t> &v) : data_(v.data()), size_(v.size()) {}

    size_t size() const { return size_; }
    size_t pos() const { return pos_; }
    size_t left() const { return pos_ <= size_ ? size_ - pos_ : 0; }
    bool eof() const { return pos_ >= size_; }
    const uint8_t *data() const { return data_; }
    const uint8_t *here() const { return data_ + pos_; }

    void seek(size_t p) {
        if (p > size_) throw ParseError("seek past end (" + std::to_string(p) + " > " + std::to_string(size_) + ")");
        pos_ = p;
    }
    void skip(size_t n) { need(n); pos_ += n; }
    void need(size_t n) const {
        if (n > left()) throw ParseError("truncated data (need " + std::to_string(n) + " at " + std::to_string(pos_) + ")");
    }
    const uint8_t *take(size_t n) { need(n); const uint8_t *p = data_ + pos_; pos_ += n; return p; }
    Reader sub(size_t n) { const uint8_t *p = take(n); return Reader(p, n); }
    Reader sub_at(size_t off, size_t n) const {
        if (off > size_ || n > size_ - off) throw ParseError("range outside data");
        return Reader(data_ + off, n);
    }

    uint8_t u8() { return *take(1); }
    int8_t s8() { return int8_t(u8()); }
    uint16_t u16le() { return le16(take(2)); }
    uint16_t u16be() { return be16(take(2)); }
    int16_t s16le() { return int16_t(u16le()); }
    int16_t s16be() { return int16_t(u16be()); }
    uint32_t u24le() { return le24(take(3)); }
    uint32_t u24be() { return be24(take(3)); }
    uint32_t u32le() { return le32(take(4)); }
    uint32_t u32be() { return be32(take(4)); }
    int32_t s32le() { return int32_t(u32le()); }
    int32_t s32be() { return int32_t(u32be()); }
    uint64_t u64le() { return le64(take(8)); }
    uint64_t u64be() { return be64(take(8)); }
    float f32le() { uint32_t v = u32le(); float f; std::memcpy(&f, &v, 4); return f; }
    float f32be() { uint32_t v = u32be(); float f; std::memcpy(&f, &v, 4); return f; }
    double f64le() { uint64_t v = u64le(); double d; std::memcpy(&d, &v, 8); return d; }
    double f64be() { uint64_t v = u64be(); double d; std::memcpy(&d, &v, 8); return d; }

    // n bytes as text, cut at the first NUL, trailing spaces removed
    std::string str(size_t n) {
        const uint8_t *p = take(n);
        size_t len = 0;
        while (len < n && p[len]) len++;
        while (len && p[len - 1] == ' ') len--;
        return std::string(reinterpret_cast<const char *>(p), len);
    }
    std::string cstr() {   // NUL-terminated
        size_t start = pos_;
        while (pos_ < size_ && data_[pos_]) pos_++;
        std::string s(reinterpret_cast<const char *>(data_ + start), pos_ - start);
        if (pos_ < size_) pos_++;
        return s;
    }
    // UTF-16LE text of n code units, cut at NUL, converted to UTF-8
    std::string utf16le(size_t units);
    bool starts(const char *magic) const {
        size_t n = std::strlen(magic);
        return left() >= n && std::memcmp(here(), magic, n) == 0;
    }

private:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0, pos_ = 0;
};

std::string utf16_to_utf8(const uint16_t *units, size_t count);
inline std::string Reader::utf16le(size_t units) {
    std::vector<uint16_t> u(units);
    for (size_t i = 0; i < units; i++) u[i] = u16le();
    size_t n = 0;
    while (n < units && u[n]) n++;
    return utf16_to_utf8(u.data(), n);
}

std::string lower(std::string s);
std::string trim(const std::string &s);
bool ends_with_ci(const std::string &s, const std::string &suffix);
bool equals_ci(const std::string &a, const std::string &b);

}  // namespace omni

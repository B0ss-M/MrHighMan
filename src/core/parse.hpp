// Omni Sampler: text and compression helpers shared by the readers: inflate (zlib / gzip / raw deflate, via the
// vendored tinf), a small XML DOM and a small JSON DOM. Both parsers are lenient and throw ParseError on garbage.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "bytes.hpp"

namespace omni {

// expected = the decompressed size when the container knows it (0 = grow until it fits)
std::vector<uint8_t> inflate_zlib(const uint8_t *src, size_t n, size_t expected = 0);
std::vector<uint8_t> inflate_raw(const uint8_t *src, size_t n, size_t expected = 0);
std::vector<uint8_t> inflate_gzip(const uint8_t *src, size_t n);
// FastLZ (levels 1 and 2), as NI uses it in Kontakt 4.2+ and NI containers
std::vector<uint8_t> fastlz_decompress(const uint8_t *src, size_t n, size_t out_size);

struct XmlNode {
    std::string name, text;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<XmlNode>> children;

    const XmlNode *child(const std::string &n) const;                  // first child by name (case-sensitive)
    std::vector<const XmlNode *> all(const std::string &n) const;      // children by name
    std::string attr(const std::string &n, const std::string &def = "") const;
    bool has_attr(const std::string &n) const;
    double attr_num(const std::string &n, double def) const;
    std::string child_text(const std::string &n, const std::string &def = "") const;
    double child_num(const std::string &n, double def) const;
    const XmlNode *find(const std::string &n) const;                   // depth-first search
};
std::unique_ptr<XmlNode> parse_xml(const std::string &text);           // returns the root element

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json &operator[](const std::string &k) const;               // Null when missing
    const Json &operator[](size_t i) const;
    bool has(const std::string &k) const;
    double as_num(double def = 0) const;
    int as_int(int def = 0) const { return int(as_num(def)); }
    bool as_bool(bool def = false) const;
    std::string as_str(const std::string &def = "") const;
};
Json parse_json(const std::string &text);

}  // namespace omni

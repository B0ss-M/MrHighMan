// Omni Sampler: inflate, XML and JSON (see parse.hpp).
#include "parse.hpp"
#include <cstdlib>
#include <cstring>

extern "C" {
#include "../third_party/tinf/tinf.h"
}

namespace omni {

// ---------------------------------------------------------------------------------------------------------------
// inflate

namespace {
template <typename F>
std::vector<uint8_t> grow_until_fits(size_t expected, size_t n, F fn) {
    size_t cap = expected ? expected : std::max<size_t>(n * 4, 4096);
    for (int tries = 0; tries < 12; tries++) {
        std::vector<uint8_t> out(cap);
        unsigned int len = unsigned(cap);
        int r = fn(out.data(), &len);
        if (r == TINF_OK) { out.resize(len); return out; }
        if (r != TINF_BUF_ERROR) throw ParseError("corrupt compressed data");
        if (cap > (size_t(1) << 30)) break;
        cap *= 4;
    }
    throw ParseError("compressed data too large");
}
}  // namespace

std::vector<uint8_t> inflate_zlib(const uint8_t *src, size_t n, size_t expected) {
    tinf_init();
    return grow_until_fits(expected, n, [&](uint8_t *d, unsigned int *len) { return tinf_zlib_uncompress(d, len, src, unsigned(n)); });
}

std::vector<uint8_t> inflate_raw(const uint8_t *src, size_t n, size_t expected) {
    tinf_init();
    return grow_until_fits(expected, n, [&](uint8_t *d, unsigned int *len) { return tinf_uncompress(d, len, src, unsigned(n)); });
}

std::vector<uint8_t> inflate_gzip(const uint8_t *src, size_t n) {
    if (n < 18 || src[0] != 0x1F || src[1] != 0x8B) throw ParseError("not gzip data");
    size_t size = le32(src + n - 4);
    tinf_init();
    return grow_until_fits(size ? size : 0, n, [&](uint8_t *d, unsigned int *len) { return tinf_gzip_uncompress(d, len, src, unsigned(n)); });
}

std::vector<uint8_t> fastlz_decompress(const uint8_t *in, size_t n, size_t out_size) {
    std::vector<uint8_t> out(out_size);
    if (!n) return {};
    int level = (in[0] >> 5) + 1;
    if (level != 1 && level != 2) throw ParseError("FastLZ: unknown level");
    size_t ip = 0, op = 0;
    uint32_t ctrl = in[ip++] & 31;
    for (;;) {
        if (ctrl >= 32) {
            uint32_t len = (ctrl >> 5) - 1, ofs = (ctrl & 31) << 8;
            size_t ref = op - ofs;   // may wrap: checked below
            if (len == 7 - 1) {
                if (level == 1) { if (ip >= n) break; len += in[ip++]; }
                else { uint32_t code; do { if (ip >= n) throw ParseError("FastLZ: truncated"); code = in[ip++]; len += code; } while (code == 255); }
            }
            if (ip >= n) throw ParseError("FastLZ: truncated");
            uint32_t code = in[ip++];
            ref -= code;
            if (level == 2 && code == 255 && ofs == (31u << 8)) {
                if (ip + 1 >= n) throw ParseError("FastLZ: truncated");
                ofs = uint32_t(in[ip] << 8 | in[ip + 1]);
                ip += 2;
                ref = op - ofs - 8191;
            }
            if (op + len + 3 > out_size) throw ParseError("FastLZ: output overrun");
            if (ref < 1 || ref - 1 >= op) throw ParseError("FastLZ: bad reference");
            ref--;
            for (uint32_t i = 0; i < len + 3; i++) out[op++] = out[ref++];
        } else {
            ctrl++;
            if (op + ctrl > out_size || ip + ctrl > n) throw ParseError("FastLZ: literal overrun");
            std::memcpy(&out[op], &in[ip], ctrl);
            op += ctrl;
            ip += ctrl;
        }
        if (ip >= n) break;
        ctrl = in[ip++];
    }
    out.resize(op);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// XML

const XmlNode *XmlNode::child(const std::string &n) const {
    for (auto &c : children) if (c->name == n) return c.get();
    return nullptr;
}
std::vector<const XmlNode *> XmlNode::all(const std::string &n) const {
    std::vector<const XmlNode *> v;
    for (auto &c : children) if (c->name == n) v.push_back(c.get());
    return v;
}
std::string XmlNode::attr(const std::string &n, const std::string &def) const {
    for (auto &a : attrs) if (a.first == n) return a.second;
    return def;
}
bool XmlNode::has_attr(const std::string &n) const {
    for (auto &a : attrs) if (a.first == n) return true;
    return false;
}
double XmlNode::attr_num(const std::string &n, double def) const {
    for (auto &a : attrs) if (a.first == n) return a.second.empty() ? def : std::atof(a.second.c_str());
    return def;
}
std::string XmlNode::child_text(const std::string &n, const std::string &def) const {
    const XmlNode *c = child(n);
    return c ? c->text : def;
}
double XmlNode::child_num(const std::string &n, double def) const {
    const XmlNode *c = child(n);
    if (!c) return def;
    std::string t = trim(c->text);
    if (t.empty()) return def;
    if (t == "True" || t == "true") return 1;
    if (t == "False" || t == "false") return 0;
    return std::atof(t.c_str());
}
const XmlNode *XmlNode::find(const std::string &n) const {
    if (name == n) return this;
    for (auto &c : children) if (const XmlNode *f = c->find(n)) return f;
    return nullptr;
}

namespace {

void append_utf8(std::string &out, uint32_t c) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += char(0xE0 | c >> 12); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    else { out += char(0xF0 | c >> 18); out += char(0x80 | (c >> 12 & 0x3F)); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
}

std::string unescape(const std::string &s) {
    if (s.find('&') == std::string::npos) return s;
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out += s[i]; continue; }
        std::string e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (!e.empty() && e[0] == '#') append_utf8(out, uint32_t(e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? std::strtoul(e.c_str() + 2, nullptr, 16) : std::strtoul(e.c_str() + 1, nullptr, 10)));
        else { out += s.substr(i, semi - i + 1); }
        i = semi;
    }
    return out;
}

struct XmlParser {
    const std::string &s;
    size_t p = 0;
    explicit XmlParser(const std::string &t) : s(t) {}
    bool starts(const char *x) const { return s.compare(p, std::strlen(x), x) == 0; }
    void skip_ws() { while (p < s.size() && isspace((unsigned char)s[p])) p++; }
    std::string name() {
        size_t a = p;
        while (p < s.size() && !isspace((unsigned char)s[p]) && s[p] != '>' && s[p] != '/' && s[p] != '=') p++;
        return s.substr(a, p - a);
    }
    void skip_misc() {
        for (;;) {
            skip_ws();
            if (starts("<?")) { size_t e = s.find("?>", p); p = e == std::string::npos ? s.size() : e + 2; }
            else if (starts("<!--")) { size_t e = s.find("-->", p); p = e == std::string::npos ? s.size() : e + 3; }
            else if (starts("<!")) { size_t e = s.find('>', p); p = e == std::string::npos ? s.size() : e + 1; }
            else break;
        }
    }
    std::unique_ptr<XmlNode> element(int depth) {
        if (depth > 200) throw ParseError("XML nested too deep");
        if (p >= s.size() || s[p] != '<') throw ParseError("XML: expected element");
        p++;
        auto n = std::unique_ptr<XmlNode>(new XmlNode());
        n->name = name();
        for (;;) {
            skip_ws();
            if (p >= s.size()) throw ParseError("XML: unterminated tag");
            if (s[p] == '/') { p += 2; return n; }
            if (s[p] == '>') { p++; break; }
            std::string an = name();
            skip_ws();
            std::string av;
            if (p < s.size() && s[p] == '=') {
                p++;
                skip_ws();
                char q = p < s.size() ? s[p] : '"';
                if (q == '"' || q == '\'') {
                    size_t e = s.find(q, p + 1);
                    if (e == std::string::npos) throw ParseError("XML: unterminated attribute");
                    av = unescape(s.substr(p + 1, e - p - 1));
                    p = e + 1;
                } else av = name();
            }
            n->attrs.push_back({an, av});
        }
        // content
        for (;;) {
            if (p >= s.size()) throw ParseError("XML: unterminated element " + n->name);
            if (starts("</")) {
                size_t e = s.find('>', p);
                p = e == std::string::npos ? s.size() : e + 1;
                return n;
            }
            if (starts("<!--")) { size_t e = s.find("-->", p); p = e == std::string::npos ? s.size() : e + 3; continue; }
            if (starts("<![CDATA[")) {
                size_t e = s.find("]]>", p);
                n->text += s.substr(p + 9, (e == std::string::npos ? s.size() : e) - p - 9);
                p = e == std::string::npos ? s.size() : e + 3;
                continue;
            }
            if (starts("<?")) { size_t e = s.find("?>", p); p = e == std::string::npos ? s.size() : e + 2; continue; }
            if (s[p] == '<') { n->children.push_back(element(depth + 1)); continue; }
            size_t e = s.find('<', p);
            if (e == std::string::npos) e = s.size();
            n->text += unescape(s.substr(p, e - p));
            p = e;
        }
    }
};

}  // namespace

std::unique_ptr<XmlNode> parse_xml(const std::string &text) {
    XmlParser x(text);
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) x.p = 3;
    x.skip_misc();
    return x.element(0);
}

// ---------------------------------------------------------------------------------------------------------------
// JSON

namespace {
const Json &null_json() { static Json j; return j; }

struct JsonParser {
    const std::string &s;
    size_t p = 0;
    explicit JsonParser(const std::string &t) : s(t) {}
    void ws() { while (p < s.size() && isspace((unsigned char)s[p])) p++; }
    std::string string() {
        if (s[p] != '"') throw ParseError("JSON: expected string");
        p++;
        std::string out;
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c != '\\') { out += c; continue; }
            if (p >= s.size()) break;
            char e = s[p++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                uint32_t c1 = uint32_t(std::strtoul(s.substr(p, 4).c_str(), nullptr, 16));
                p += 4;
                if (c1 >= 0xD800 && c1 < 0xDC00 && s.compare(p, 2, "\\u") == 0) {
                    uint32_t c2 = uint32_t(std::strtoul(s.substr(p + 2, 4).c_str(), nullptr, 16));
                    p += 6;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                }
                append_utf8(out, c1);
                break;
            }
            default: out += e; break;
            }
        }
        p++;
        return out;
    }
    Json value(int depth) {
        if (depth > 500) throw ParseError("JSON nested too deep");
        ws();
        if (p >= s.size()) throw ParseError("JSON: unexpected end");
        Json j;
        char c = s[p];
        if (c == '{') {
            j.type = Json::Object;
            p++;
            ws();
            if (p < s.size() && s[p] == '}') { p++; return j; }
            for (;;) {
                ws();
                std::string k = string();
                ws();
                if (p >= s.size() || s[p] != ':') throw ParseError("JSON: expected ':'");
                p++;
                j.obj.push_back({k, value(depth + 1)});
                ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == '}') { p++; return j; }
                throw ParseError("JSON: expected ',' or '}'");
            }
        }
        if (c == '[') {
            j.type = Json::Array;
            p++;
            ws();
            if (p < s.size() && s[p] == ']') { p++; return j; }
            for (;;) {
                j.arr.push_back(value(depth + 1));
                ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == ']') { p++; return j; }
                throw ParseError("JSON: expected ',' or ']'");
            }
        }
        if (c == '"') { j.type = Json::String; j.str = string(); return j; }
        if (s.compare(p, 4, "true") == 0) { p += 4; j.type = Json::Bool; j.b = true; return j; }
        if (s.compare(p, 5, "false") == 0) { p += 5; j.type = Json::Bool; return j; }
        if (s.compare(p, 4, "null") == 0) { p += 4; return j; }
        char *end = nullptr;
        j.num = std::strtod(s.c_str() + p, &end);
        if (end == s.c_str() + p) throw ParseError("JSON: bad value");
        p = size_t(end - s.c_str());
        j.type = Json::Number;
        return j;
    }
};
}  // namespace

const Json &Json::operator[](const std::string &k) const {
    for (auto &kv : obj) if (kv.first == k) return kv.second;
    return null_json();
}
const Json &Json::operator[](size_t i) const { return i < arr.size() ? arr[i] : null_json(); }
bool Json::has(const std::string &k) const {
    for (auto &kv : obj) if (kv.first == k) return true;
    return false;
}
double Json::as_num(double def) const {
    if (type == Number) return num;
    if (type == Bool) return b ? 1 : 0;
    if (type == String && !str.empty()) return std::atof(str.c_str());
    return def;
}
bool Json::as_bool(bool def) const {
    if (type == Bool) return b;
    if (type == Number) return num != 0;
    if (type == String) return str == "true" || str == "True" || str == "1";
    return def;
}
std::string Json::as_str(const std::string &def) const {
    if (type == String) return str;
    if (type == Number) { char b2[32]; std::snprintf(b2, sizeof b2, "%g", num); return b2; }
    return def;
}

Json parse_json(const std::string &text) {
    JsonParser j(text);
    return j.value(0);
}

}  // namespace omni

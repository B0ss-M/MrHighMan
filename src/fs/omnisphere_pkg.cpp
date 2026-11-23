// Omni Sampler 2.4.9: Spectrasonics library packages (.omnisphere: what Omnisphere's "Install .omnisphere file" takes,
// e.g. third-party libraries like "Tom Wolfe's Glacier") browse as a folder: Patches/... (.prt_omn, .mlt_omn) and
// Soundsources/<name>/ (<name>.zmap and the soundsource unpacked: <name>/<name>.wav, SampledInstrument.xml,
// Pitch 0-127/HitBundle.xml, Layer.xml). The format is the same container as a user soundsource's .db: an XML
// <FileSystem> table of <DIR name=...> and <FILE name= offset= size=/> (offsets from the first byte after
// </FileSystem> and its line end), then the data. Each file is a slice of the package: nothing is copied or extracted.
// The Omnisphere reader (formats/omnisphere.cpp) plays the patches whose soundsources are in the package.
#include <cstdlib>
#include <cstring>
#include "fs.hpp"
#include "../core/parse.hpp"

namespace omni {
namespace {

constexpr size_t MAX_INDEX = size_t(8) << 20;   // the table (Cima, 1101 files: 85 KB)
constexpr size_t MAX_ENTRIES = 200000;
constexpr int MAX_DEPTH = 32;

bool probe_pkg(const std::string &lower_name, const uint8_t *h, size_t n, uint64_t size) {
    const size_t L = lower_name.size();
    return L > 11 && lower_name.compare(L - 11, 11, ".omnisphere") == 0 && n >= 12 && size > 12 && !std::memcmp(h, "<FileSystem>", 12);
}

bool number(const std::string &s, uint64_t &v) {
    if (s.empty() || s.size() > 19) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    v = std::strtoull(s.c_str(), nullptr, 10);
    return true;
}

void walk(const XmlNode *n, const std::string &dir, int depth, uint64_t data, uint64_t total, const BlobPtr &img, TreeVolume &vol,
          size_t &count) {
    if (depth > MAX_DEPTH) throw ParseError("Omnisphere library: folders nested too deep");
    for (const auto &c : n->children) {
        if (c->name != "DIR" && c->name != "FILE") continue;
        if (++count > MAX_ENTRIES) throw ParseError("Omnisphere library: too many files");
        const std::string name = c->attr("name");
        // (no path tricks: a name is one path part)
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) continue;
        const std::string p = dir.empty() ? name : dir + "/" + name;
        if (c->name == "DIR") {
            // 2.4.11: no add_dir: a folder appears through the files under it (add_file creates the parents), so the
            // ~24 empty category folders Omnisphere packages carry ("Rhythmic Aggressive" ...) are not listed
            walk(c.get(), p, depth + 1, data, total, img, vol, count);
            continue;
        }
        uint64_t off = 0, len = 0;
        if (!number(c->attr("offset"), off) || !number(c->attr("size"), len)) throw ParseError("Omnisphere library: damaged file table (" + name + ")");
        // inside the package: data + off + len <= total, checked without overflow
        if (data > total || off > total - data || len > total - data - off)
            throw ParseError("Omnisphere library: \"" + name + "\" lies outside the file (a cut-off download?)");
        if (len == 0) continue;   // (library.dat: an empty marker)
        vol.add_file(p, make_slice(img, data + off, len));
    }
}

VolumePtr mount_pkg(BlobPtr img) {
    const std::vector<uint8_t> h = img->head(MAX_INDEX);
    const std::string s(h.begin(), h.end());
    const size_t end = s.find("</FileSystem>");
    if (end == std::string::npos) throw ParseError("Omnisphere library: no file table");
    size_t data = end + 13;
    if (data < s.size() && s[data] == '\r') data++;
    if (data < s.size() && s[data] == '\n') data++;
    auto root = parse_xml(s.substr(0, end + 13));
    if (!root || root->name != "FileSystem") throw ParseError("Omnisphere library: no file table");
    auto vol = std::make_shared<TreeVolume>("Omnisphere library");
    size_t count = 0;
    walk(root.get(), "", 0, data, img->size(), img, *vol, count);
    if (!vol->file_count()) throw ParseError("Omnisphere library: empty");
    return vol;
}

}  // namespace

void register_omnisphere_pkg() {
    register_image_type({"Omnisphere library", probe_pkg, mount_pkg});
}

}  // namespace omni

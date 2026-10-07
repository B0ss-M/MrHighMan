// Omni Sampler: the NI container ("hsin"): a tree of items, each with a chain of typed data chunks, where the
// next chunk of a chain sits inside the previous one. Translated from ConvertWithMoss's format/ni/nicontainer (LGPL-3.0).
#include "ni_common.hpp"
#include "../core/audio.hpp"
#include "../core/parse.hpp"

namespace omni {

std::string utf16_len(Reader &r) {
    uint32_t n = r.u32le();
    if (n > r.left() / 2) throw ParseError("bad string length");
    return r.utf16le(n);
}

void NiSampleFinder::index(const std::string &dir, int depth, int &budget) {
    if (depth < 0 || budget <= 0) return;
    std::vector<DirEntry> es;
    try { es = vol_->list(dir); } catch (const std::exception &) { return; }
    for (auto &e : es) {
        if (--budget <= 0) return;
        std::string full = path_join(dir, e.name);
        if (e.dir) index(full, depth - 1, budget);
        else if (is_audio_ext(path_ext(e.name))) by_name_.emplace(lower(e.name), full);
    }
}

// "<stem>.wav" referenced, "<stem>_C4.wav" on disk (exports and converters add the root note): the one file whose
// stem is the referenced stem plus a separator and a suffix; "" when none or several match.
static std::string suffixed_match(const std::vector<std::string> &names, const std::string &want_stem) {
    std::string w = lower(want_stem), hit;
    int n = 0;
    for (auto &nm : names) {
        std::string st = lower(path_stem(nm));
        if (st.size() > w.size() + 1 && st.compare(0, w.size(), w) == 0 && (st[w.size()] == '_' || st[w.size()] == ' ' || st[w.size()] == '-')) {
            hit = nm;
            n++;
        }
    }
    return n == 1 ? hit : "";
}

std::string NiSampleFinder::find(const std::string &ref_in) {
    std::string ref = ref_in;
    for (char &c : ref) if (c == '\\') c = '/';
    std::string name = path_name(ref), found;
    if (name.empty()) return "";
    bool absolute = ref[0] == '/' || (ref.size() > 1 && ref[1] == ':');
    std::string up2 = path_dir(path_dir(dir_));
    if (!absolute && find_ci(*vol_, path_resolve(dir_, ref), found)) return found;
    if (find_ci(*vol_, path_join(dir_, name), found)) return found;
    if (!up2.empty() && find_ci(*vol_, path_join(up2, name), found)) return found;
    if (!indexed_) {
        indexed_ = true;
        int budget = 20000;
        index(up2.empty() ? dir_ : up2, 5, budget);
    }
    auto it = by_name_.find(lower(name));
    if (it != by_name_.end()) return it->second;
    // renamed with a note suffix: look in the referenced folder, the preset's folder, then the index
    for (const std::string &d : {absolute ? std::string() : path_dir(path_resolve(dir_, ref)), dir_}) {
        if (d.empty()) continue;
        std::vector<std::string> names;
        try { for (auto &e : vol_->list(d)) if (!e.dir && is_audio_ext(path_ext(e.name))) names.push_back(e.name); } catch (const std::exception &) {}
        std::string m = suffixed_match(names, path_stem(name));
        if (!m.empty()) return path_join(d, m);
    }
    std::vector<std::string> keys;
    for (auto &kv : by_name_) keys.push_back(kv.first);
    std::string m = suffixed_match(keys, path_stem(name));
    return m.empty() ? "" : by_name_[m];
}

namespace {

void ni_item(Reader &r, NiContainer &f, int depth);

void ni_chunk_chain(Reader &r, NiContainer &f, int depth) {
    uint64_t size = r.u64le();
    if (size < 8 || size - 8 > r.left()) throw ParseError("bad NI container chunk");
    Reader b = r.sub(size_t(size - 8));
    b.take(4);   // domain
    uint32_t type = b.u32le();
    if (b.u32le() != 1) throw ParseError("unknown NI container chunk version");
    if (type != 1) ni_chunk_chain(b, f, depth + 1);   // the next chunk sits inside this one, before its data
    // this chunk's data
    try {
        if (type == 101) {   // authoring application
            b.u32le(); b.u8();
            uint32_t app = b.u32le();
            if (f.app < 0) f.app = int(app);
        } else if (type == 109) {   // preset chunk item
            b.u32le(); b.u32le();
            uint32_t items = b.u32le();
            if (items != 1) return;
            uint32_t sz = b.u32le();
            b.u32le();
            const uint8_t *p = b.take(sz);
            if (f.preset.empty()) f.preset.assign(p, p + sz);
        } else if (type == 106) {   // authorization: product IDs mean an encrypted library
            b.u32le();
            if (b.u32le() == 1) {
                b.u32le();
                uint32_t n = b.u32le();
                for (uint32_t i = 0; i < n; i++) if (!trim(utf16_len(b)).empty()) f.encrypted = true;
            }
        } else if (type == 115) {   // sub tree, possibly FastLZ compressed
            b.u32le();
            bool compressed = b.u8() > 0;
            if (compressed) {
                uint32_t usize = b.u32le(), csize = b.u32le();
                const uint8_t *p = b.take(csize);
                std::vector<uint8_t> un;
                try { un = fastlz_decompress(p, csize, usize); } catch (const ParseError &) { f.encrypted = true; return; }
                Reader sr(un);
                ni_item(sr, f, depth + 1);
            } else ni_item(b, f, depth + 1);
        }
    } catch (const ParseError &) {}
}

void ni_item(Reader &r, NiContainer &f, int depth) {
    if (depth > 32) throw ParseError("NI container nested too deep");
    uint64_t size = r.u64le();
    if (size < 8 || size - 8 > r.left()) throw ParseError("bad NI container item");
    Reader b = r.sub(size_t(size - 8));
    if (b.u32le() != 1 || !b.starts("hsin")) throw ParseError("not an NI container");
    b.skip(4 + 8 + 16);
    ni_chunk_chain(b, f, depth);
    b.u32le();
    uint32_t n = b.u32le();
    for (uint32_t i = 0; i < n && !b.eof(); i++) {
        b.u32le(); b.skip(4); b.u32le();
        ni_item(b, f, depth + 1);
    }
}

}  // namespace

NiContainer read_ni_container(const std::vector<uint8_t> &d) {
    NiContainer f;
    Reader r(d);
    ni_item(r, f, 0);
    return f;
}

}  // namespace omni

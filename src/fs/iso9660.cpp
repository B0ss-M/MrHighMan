// Omni Sampler: ISO 9660 CD/DVD images (.iso, and raw 2352-byte sector .bin/.img rips), with Joliet (Unicode) and
// Rock Ridge (POSIX) long names. Sample CDs in WAV/AIFF/SF2/... and data discs with sampler images on them.
#include <cstring>
#include <set>
#include "../core/bytes.hpp"
#include "fs.hpp"

namespace omni {
namespace {

constexpr uint64_t SECTOR = 2048;
const uint8_t SYNC[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};

// 2048-byte user data out of raw 2352-byte sectors
class RawCdBlob : public Blob {
public:
    RawCdBlob(BlobPtr raw, uint64_t data_offset) : raw_(std::move(raw)), off_(data_offset) {}
    uint64_t size() const override { return raw_->size() / 2352 * SECTOR; }
    void read(uint64_t offset, void *dst, size_t n) const override {
        if (offset + n > size()) throw ParseError("read past the end of the CD image");
        auto *out = static_cast<uint8_t *>(dst);
        while (n) {
            uint64_t sec = offset / SECTOR, in = offset % SECTOR;
            size_t k = size_t(std::min<uint64_t>(n, SECTOR - in));
            raw_->read(sec * 2352 + off_ + in, out, k);
            out += k; offset += k; n -= k;
        }
    }

private:
    BlobPtr raw_;
    uint64_t off_;
};

// where the 2048-byte sectors start in a raw image: 16 (mode 1) or 24 (mode 2 form 1); 0 = not raw
uint64_t raw_data_offset(const uint8_t *h, size_t n) {
    const uint64_t at = 16 * 2352;
    if (n < at + 24 + 6 || std::memcmp(h + at, SYNC, 12)) return 0;
    int mode = h[at + 15];
    uint64_t off = mode == 2 ? 24 : 16;
    return std::memcmp(h + at + off + 1, "CD001", 5) ? 0 : off;
}

bool probe_iso(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    if (n >= 16 * SECTOR + 6 && !std::memcmp(h + 16 * SECTOR + 1, "CD001", 5)) return true;
    return raw_data_offset(h, n) != 0;
}

struct Rec {
    uint32_t lba = 0, len = 0;
    bool dir = false, more = false;   // more: a multi-extent file continues in the next record
    std::string name;
};

class IsoReader {
public:
    IsoReader(BlobPtr img, std::shared_ptr<TreeVolume> vol) : img_(std::move(img)), vol_(std::move(vol)) {}

    void mount() {
        uint32_t root_lba = 0, root_len = 0;
        bool have = false;
        for (uint32_t s = 16; s < 64; s++) {
            std::vector<uint8_t> d = img_->bytes(uint64_t(s) * SECTOR, SECTOR);
            if (std::memcmp(&d[1], "CD001", 5)) break;
            int type = d[0];
            if (type == 255) break;
            if (type == 1 && !have) { root_lba = le32(&d[156 + 2]); root_len = le32(&d[156 + 10]); have = true; }
            if (type == 2) {   // Joliet: a supplementary descriptor with a UCS-2 escape sequence
                if (d[88] == '%' && d[89] == '/' && (d[90] == '@' || d[90] == 'C' || d[90] == 'E')) {
                    root_lba = le32(&d[156 + 2]);
                    root_len = le32(&d[156 + 10]);
                    joliet_ = have = true;
                    break;
                }
            }
        }
        if (!have) throw ParseError("no ISO 9660 volume descriptor");
        walk(root_lba, root_len, "", 0);
    }

private:
    BlobPtr img_;
    std::shared_ptr<TreeVolume> vol_;
    bool joliet_ = false;
    std::set<uint32_t> seen_;
    size_t entries_ = 0;

    std::string rec_name(const uint8_t *r, size_t len) {
        int nl = r[32];
        const uint8_t *np = r + 33;
        std::string name;
        if (joliet_) {
            std::vector<uint16_t> u;
            for (int i = 0; i + 1 < nl; i += 2) u.push_back(uint16_t(np[i] << 8 | np[i + 1]));
            name = utf16_to_utf8(u.data(), u.size());
        } else {
            name.assign(reinterpret_cast<const char *>(np), size_t(nl));
            // Rock Ridge NM entries in the system use area
            size_t su = 33 + size_t(nl) + (nl % 2 == 0 ? 1 : 0);
            std::string rr;
            while (su + 4 <= len) {
                const uint8_t *e = r + su;
                int el = e[2];
                if (el < 4 || su + size_t(el) > len) break;
                if (e[0] == 'N' && e[1] == 'M' && el >= 5 && !(e[4] & 6)) rr.append(reinterpret_cast<const char *>(e + 5), size_t(el - 5));
                su += size_t(el);
            }
            if (!rr.empty()) return rr;
        }
        size_t semi = name.find(';');
        if (semi != std::string::npos) name.resize(semi);
        if (!name.empty() && name.back() == '.') name.pop_back();
        return name;
    }

    void walk(uint32_t lba, uint32_t len, const std::string &dir, int depth) {
        if (depth > 32 || !seen_.insert(lba).second) return;
        if (len > (64u << 20)) throw ParseError("ISO directory too large");
        if (!dir.empty()) vol_->add_dir(dir);
        std::vector<uint8_t> d = img_->bytes(uint64_t(lba) * SECTOR, len);
        std::vector<Rec> recs;
        size_t pos = 0;
        while (pos < d.size()) {
            size_t rl = d[pos];
            if (rl == 0) { pos = (pos / SECTOR + 1) * SECTOR; continue; }   // records never cross a sector
            if (rl < 34 || pos + rl > d.size()) break;
            const uint8_t *r = &d[pos];
            int nl = r[32];
            if (33 + size_t(nl) > rl) break;
            if (!(nl == 1 && (r[33] == 0 || r[33] == 1))) {   // not . or ..
                Rec rec;
                rec.lba = le32(r + 2) + r[1];   // + extended attribute record length
                rec.len = le32(r + 10);
                rec.dir = r[25] & 2;
                rec.more = r[25] & 0x80;
                rec.name = rec_name(r, rl);
                recs.push_back(rec);
            }
            pos += rl;
        }
        for (size_t i = 0; i < recs.size(); i++) {
            if (++entries_ > 200000) throw ParseError("too many files in the ISO image");
            const Rec &rc = recs[i];
            std::string name = rc.name.empty() ? "unnamed" : rc.name;
            for (char &c : name) if (c == '/' || c == '\\') c = '_';
            std::string path = dir.empty() ? name : dir + "/" + name;
            if (rc.dir) { walk(rc.lba, rc.len, path, depth + 1); continue; }
            std::vector<std::pair<uint64_t, uint64_t>> ext{{uint64_t(rc.lba) * SECTOR, rc.len}};
            uint64_t total = rc.len;
            while (recs[i].more && i + 1 < recs.size()) {   // multi-extent (files over 4 GB)
                i++;
                ext.push_back({uint64_t(recs[i].lba) * SECTOR, recs[i].len});
                total += recs[i].len;
            }
            BlobPtr img = img_;
            vol_->add_lazy(path, total, [img, ext] { return ext.size() == 1 ? make_slice(img, ext[0].first, ext[0].second) : make_extents(img, ext); });
        }
    }
};

VolumePtr mount_iso(BlobPtr image) {
    BlobPtr img = image;
    std::vector<uint8_t> h = image->head(16 * 2352 + 64);
    if (uint64_t off = raw_data_offset(h.data(), h.size())) img = std::make_shared<RawCdBlob>(image, off);
    auto vol = std::make_shared<TreeVolume>("ISO 9660");
    IsoReader(img, vol).mount();
    return vol;
}

}  // namespace

void register_iso9660() {
    register_image_type({"ISO 9660", probe_iso, mount_iso});
}

}  // namespace omni

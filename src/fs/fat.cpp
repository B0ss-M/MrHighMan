// Omni Sampler: FAT12/16/32 disk images: floppies (MPC2000/2000XL/3000, S5000/S6000, Z4/Z8, EMU ESI/E4 DOS disks),
// ZIP/Jaz dumps and CF/SD card images, unpartitioned or with an MBR (one folder per partition when there are several).
// VFAT long file names are used when present. File cluster chains are followed only when a file is opened.
#include <cstring>
#include <set>
#include "../core/bytes.hpp"
#include "fs.hpp"

namespace omni {
namespace {

struct Bpb {
    uint32_t bps = 0, spc = 0, reserved = 0, nfats = 0, root_entries = 0, total = 0, fat_size = 0, root_cluster = 0;
    int bits = 0;
    uint64_t fat_off = 0, root_off = 0, data_off = 0, clusters = 0, cluster_bytes = 0;
};

bool pow2(uint32_t v) { return v && !(v & (v - 1)); }

// A plausible FAT boot sector at p (512 bytes), filled into b. strict: also require a jump or the 55 AA signature.
bool parse_bpb(const uint8_t *p, Bpb &b) {
    b.bps = le16(p + 11);
    b.spc = p[13];
    b.reserved = le16(p + 14);
    b.nfats = p[16];
    b.root_entries = le16(p + 17);
    b.total = le16(p + 19) ? le16(p + 19) : le32(p + 32);
    b.fat_size = le16(p + 22) ? le16(p + 22) : le32(p + 36);
    int media = p[21];
    if (!(b.bps == 512 || b.bps == 1024 || b.bps == 2048 || b.bps == 4096) || !pow2(b.spc) || b.spc > 128) return false;
    if (b.reserved < 1 || b.nfats < 1 || b.nfats > 2 || b.fat_size == 0 || b.total == 0) return false;
    if (!(media == 0xF0 || media >= 0xF8)) return false;
    bool jump = p[0] == 0xEB || p[0] == 0xE9, sig = p[510] == 0x55 && p[511] == 0xAA;
    if (!jump && !sig) return false;
    uint64_t root_sectors = (uint64_t(b.root_entries) * 32 + b.bps - 1) / b.bps;
    uint64_t first_data = b.reserved + uint64_t(b.nfats) * b.fat_size + root_sectors;
    if (first_data >= b.total) return false;
    b.clusters = (b.total - first_data) / b.spc;
    b.bits = b.clusters < 4085 ? 12 : b.clusters < 65525 ? 16 : 32;
    if (b.bits == 32) {
        if (b.root_entries != 0 || le16(p + 22) != 0) return false;
        b.root_cluster = le32(p + 44);
    } else if (b.root_entries == 0) return false;
    b.fat_off = uint64_t(b.reserved) * b.bps;
    b.root_off = b.fat_off + uint64_t(b.nfats) * b.fat_size * b.bps;
    b.data_off = first_data * b.bps;
    b.cluster_bytes = uint64_t(b.spc) * b.bps;
    return true;
}

// MBR partitions that hold FAT file systems (by type, then checked by their boot sector)
std::vector<uint64_t> mbr_partitions(const uint8_t *p, uint64_t image_size = 0) {
    std::vector<uint64_t> out;
    if (p[510] != 0x55 || p[511] != 0xAA) return out;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = p + 446 + 16 * i;
        if (e[0] != 0 && e[0] != 0x80) return {};
        int type = e[4];
        uint32_t lba = le32(e + 8), count = le32(e + 12);
        if (!lba || !count) continue;
        if (image_size && (uint64_t(lba) + count) * 512 > image_size + (1u << 20)) return {};   // must fit the image
        if (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C || type == 0x0E) out.push_back(uint64_t(lba) * 512);
    }
    return out;
}

bool probe_fat(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    if (n < 512) return false;
    Bpb b;
    if (parse_bpb(h, b)) return true;
    // partitions: check the boot sector when it is within the head, else trust the table (mount verifies)
    for (uint64_t off : mbr_partitions(h, size))
        if (off + 512 > n || parse_bpb(h + off, b)) return true;
    return false;
}

// FAT entries, read on demand through a small page cache (FATs of big cards are many megabytes)
class FatTable {
public:
    FatTable(BlobPtr img, const Bpb &b, uint64_t base) : img_(std::move(img)), b_(b), base_(base) {}
    uint32_t next(uint32_t c) {
        std::lock_guard<std::mutex> lock(m_);
        uint64_t off = b_.bits == 12 ? c + c / 2 : uint64_t(c) * (b_.bits / 8);
        if (b_.bits == 12) {
            uint32_t v = byte(off) | uint32_t(byte(off + 1)) << 8;
            v = c & 1 ? v >> 4 : v & 0xFFF;
            return v >= 0xFF7 ? 0xFFFFFFFF : v;
        }
        if (b_.bits == 16) {
            uint32_t v = byte(off) | uint32_t(byte(off + 1)) << 8;
            return v >= 0xFFF7 ? 0xFFFFFFFF : v;
        }
        uint32_t v = (byte(off) | uint32_t(byte(off + 1)) << 8 | uint32_t(byte(off + 2)) << 16 | uint32_t(byte(off + 3)) << 24) & 0x0FFFFFFF;
        return v >= 0x0FFFFFF7 ? 0xFFFFFFFF : v;
    }
    // the byte extents of the chain starting at cluster c (contiguous clusters merged), at most max_bytes long
    std::vector<std::pair<uint64_t, uint64_t>> chain(uint32_t c, uint64_t max_bytes) {
        std::vector<std::pair<uint64_t, uint64_t>> ext;
        uint64_t total = 0;
        uint64_t guard = b_.clusters + 2;
        while (c >= 2 && c < b_.clusters + 2 && total < max_bytes && guard--) {
            uint64_t off = base_ + b_.data_off + uint64_t(c - 2) * b_.cluster_bytes;
            uint64_t len = std::min(b_.cluster_bytes, max_bytes - total);
            if (!ext.empty() && ext.back().first + ext.back().second == off) ext.back().second += len;
            else ext.push_back({off, len});
            total += len;
            c = next(c);
        }
        return ext;
    }
    const Bpb &bpb() const { return b_; }

private:
    BlobPtr img_;
    Bpb b_;
    uint64_t base_;
    std::mutex m_;
    std::map<uint64_t, std::vector<uint8_t>> pages_;
    uint8_t byte(uint64_t off) {
        if (off >= uint64_t(b_.fat_size) * b_.bps) throw ParseError("FAT entry out of range");
        uint64_t page = off / 65536;
        auto it = pages_.find(page);
        if (it == pages_.end()) {
            if (pages_.size() > 256) pages_.clear();
            it = pages_.emplace(page, img_->head(65536, base_ + b_.fat_off + page * 65536)).first;
        }
        size_t k = size_t(off % 65536);
        return k < it->second.size() ? it->second[k] : 0;
    }
};

std::string short_name(const uint8_t *e) {
    auto part = [](const uint8_t *p, size_t n, bool lower_case) {
        std::string s(reinterpret_cast<const char *>(p), n);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (lower_case) for (char &c : s) c = char(std::tolower((unsigned char)c));
        return s;
    };
    std::string base = part(e, 8, e[12] & 0x08), ext = part(e + 8, 3, e[12] & 0x10);
    if (!base.empty() && uint8_t(base[0]) == 0x05) base[0] = char(0xE5);
    std::string out;
    for (char c : base + (ext.empty() ? "" : "." + ext)) {   // code page 437 high characters: keep ASCII only
        if (uint8_t(c) < 0x20 || uint8_t(c) >= 0x7F) out += '_';
        else out += c;
    }
    return out;
}

uint8_t lfn_checksum(const uint8_t *e) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = uint8_t(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

class FatReader {
public:
    FatReader(BlobPtr img, std::shared_ptr<TreeVolume> vol, std::shared_ptr<FatTable> fat, uint64_t base, std::string prefix)
        : img_(std::move(img)), vol_(std::move(vol)), fat_(std::move(fat)), base_(base), prefix_(std::move(prefix)) {}

    void mount() {
        const Bpb &b = fat_->bpb();
        if (!prefix_.empty()) vol_->add_dir(prefix_);
        if (b.bits == 32) walk_chain(b.root_cluster, prefix_, 0);
        else walk(img_->bytes(base_ + b.root_off, size_t(b.root_entries) * 32), prefix_, 0);
    }

private:
    BlobPtr img_;
    std::shared_ptr<TreeVolume> vol_;
    std::shared_ptr<FatTable> fat_;
    uint64_t base_;
    std::string prefix_;
    std::set<uint32_t> seen_;
    size_t entries_ = 0;

    void walk_chain(uint32_t cluster, const std::string &dir, int depth) {
        if (depth > 32 || !seen_.insert(cluster).second) return;
        std::vector<uint8_t> d;
        for (auto &e : fat_->chain(cluster, 32u << 20)) {
            std::vector<uint8_t> part = img_->bytes(e.first, size_t(e.second));
            d.insert(d.end(), part.begin(), part.end());
        }
        walk(d, dir, depth);
    }

    void walk(const std::vector<uint8_t> &d, const std::string &dir, int depth) {
        std::vector<uint16_t> lfn;
        int lfn_sum = -1;
        for (size_t pos = 0; pos + 32 <= d.size(); pos += 32) {
            const uint8_t *e = &d[pos];
            if (e[0] == 0) break;
            if (e[0] == 0xE5) { lfn.clear(); continue; }
            uint8_t attr = e[11];
            if (attr == 0x0F) {   // a long name piece; pieces come last-first
                int seq = e[0] & 0x1F;
                if (e[0] & 0x40) { lfn.assign(size_t(seq) * 13, 0xFFFF); lfn_sum = e[13]; }
                if (seq < 1 || size_t(seq) * 13 > lfn.size() || e[13] != lfn_sum) { lfn.clear(); continue; }
                static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int k = 0; k < 13; k++) lfn[size_t(seq - 1) * 13 + size_t(k)] = le16(e + at[k]);
                continue;
            }
            std::string name;
            if (!lfn.empty() && lfn_checksum(e) == lfn_sum) {
                size_t n = 0;
                while (n < lfn.size() && lfn[n] != 0 && lfn[n] != 0xFFFF) n++;
                name = utf16_to_utf8(lfn.data(), n);
            }
            lfn.clear();
            if (attr & 0x08) continue;   // volume label
            if (name.empty()) name = short_name(e);
            if (name.empty() || name == "." || name == "..") continue;
            for (char &c : name) if (c == '/' || c == '\\') c = '_';
            if (++entries_ > 200000) throw ParseError("too many files in the FAT image");
            uint32_t cluster = le16(e + 26) | (fat_->bpb().bits == 32 ? uint32_t(le16(e + 20)) << 16 : 0);
            std::string path = dir.empty() ? name : dir + "/" + name;
            if (attr & 0x10) {
                vol_->add_dir(path);
                if (cluster >= 2) walk_chain(cluster, path, depth + 1);
                continue;
            }
            uint32_t size = le32(e + 28);
            auto fat = fat_;
            BlobPtr img = img_;
            vol_->add_lazy(path, size, [fat, img, cluster, size] {
                if (size == 0) return make_mem_blob({});
                auto ext = fat->chain(cluster, size);
                return make_extents(img, ext);
            });
        }
    }
};

VolumePtr mount_fat(BlobPtr image) {
    std::vector<uint8_t> h = image->head(512);
    if (h.size() < 512) throw ParseError("not a FAT image");
    std::vector<uint64_t> parts;
    Bpb b;
    if (parse_bpb(h.data(), b)) parts.push_back(0);
    else
        for (uint64_t off : mbr_partitions(h.data())) {
            std::vector<uint8_t> bs = image->head(512, off);
            if (bs.size() == 512 && parse_bpb(bs.data(), b)) parts.push_back(off);
        }
    if (parts.empty()) throw ParseError("no FAT file system in this image");
    auto vol = std::make_shared<TreeVolume>("FAT");
    int index = 0;
    for (uint64_t off : parts) {
        index++;
        std::vector<uint8_t> bs = image->head(512, off);
        parse_bpb(bs.data(), b);
        auto fat = std::make_shared<FatTable>(image, b, off);
        std::string prefix = parts.size() > 1 ? "Partition " + std::to_string(index) : "";
        try { FatReader(image, vol, fat, off, prefix).mount(); }
        catch (const ParseError &) { if (parts.size() == 1) throw; }
    }
    return vol;
}

}  // namespace

void register_fat() {
    register_image_type({"FAT", probe_fat, mount_fat});
}

}  // namespace omni

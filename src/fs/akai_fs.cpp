// Omni Sampler: Akai sampler disk images.
//  - S1000/S3000/S3200/CD3000 hard disks and CD-ROMs: partitions (A, B, ...) of 8 KB blocks, each with a volume
//    directory at 0xCA (100 x 16 bytes), a FAT at 0x70A and per-volume file directories (S1000 125 files, S3000
//    509 files spanning two blocks). Layout as read by ConvertWithMoss's AkaiDiskImage/AkaiPartition (LGPL-3.0);
//    unlike it, files follow their FAT chain (fragmented hard disks), falling back to contiguous blocks when the
//    chain is broken.
//  - S900/S950 floppies (ASCII names, 1 KB blocks, as CWM's AkaiS900DiskImage) and S1000/S3000 floppies (Akai
//    character set names, same directory entry as the hard disk's file entries).
// Files are presented with extensions by type: S1000 program .p / sample .s, S3000 .p3 / .s3, S900 .p9 / .s9.
#include <algorithm>
#include <cstring>
#include "fs.hpp"

namespace omni {

std::string akai_to_ascii(const uint8_t *p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        int b = p[i];
        char c;
        if (b <= 9) c = char('0' + b);
        else if (b == 10) c = ' ';
        else if (b >= 11 && b <= 36) c = char('A' + b - 11);
        else if (b == 37) c = '#';
        else if (b == 38) c = '+';
        else if (b == 39) c = '-';
        else if (b == 40) c = '.';
        else c = ' ';
        s += c;
    }
    return trim(s);
}

namespace {

constexpr uint64_t HD_BLOCK = 0x2000;
constexpr int VOL_DIR_OFF = 0xCA, VOL_DIR_ENTRIES = 100, FAT_OFF = 0x70A;

bool akai_name_ok(const uint8_t *p, size_t n) {
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (p[i] > 40) return false;
        if (p[i] != 10) any = true;
    }
    return any;
}

std::string file_ext(int type, bool s3000) {
    int t = type >= 128 ? type - 128 : type;
    bool s3 = s3000 || type >= 128;
    if (t == 'p') return s3 ? ".p3" : ".p";
    if (t == 's') return s3 ? ".s3" : ".s";
    return "";
}

struct Partition {
    uint64_t offset;
    std::vector<uint16_t> fat;
};

// The blocks of a file: its FAT chain when intact, else contiguous blocks from its start.
std::vector<std::pair<uint64_t, uint64_t>> file_extents(const Partition &part, uint64_t img_size, int start, uint64_t size) {
    std::vector<std::pair<uint64_t, uint64_t>> ext;
    uint64_t blocks = (size + HD_BLOCK - 1) / HD_BLOCK;
    uint64_t left = size;
    int b = start;
    bool chain_ok = true;
    for (uint64_t i = 0; i < blocks; i++) {
        if (b <= 0 || size_t(b) >= part.fat.size()) { chain_ok = false; break; }
        uint64_t off = part.offset + uint64_t(b) * HD_BLOCK, take = std::min<uint64_t>(left, HD_BLOCK);
        if (off + take > img_size) { chain_ok = false; break; }
        if (!ext.empty() && ext.back().first + ext.back().second == off) ext.back().second += take;
        else ext.push_back({off, take});
        left -= take;
        if (i + 1 < blocks) {
            uint16_t next = part.fat[size_t(b)];
            if (next == 0 || next >= 0x4000) { chain_ok = false; break; }
            b = next;
        }
    }
    if (chain_ok && left == 0) return ext;
    return {{part.offset + uint64_t(start) * HD_BLOCK, size}};
}

// ---------------------------------------------------------------------------------------------------------------
// hard disk / CD-ROM

bool volumes_at(const uint8_t *h, size_t n);

std::vector<Partition> scan_partitions(const Blob &img) {
    std::vector<Partition> parts;
    uint64_t size = img.size(), off = 0;
    // some CD rips (.nrg) carry a 0x4B000 byte lead-in before partition A
    std::vector<uint8_t> h0 = img.head(FAT_OFF);
    if (!volumes_at(h0.data(), h0.size())) {
        std::vector<uint8_t> h1 = img.head(FAT_OFF, 0x4B000);
        if (volumes_at(h1.data(), h1.size())) off = 0x4B000;
    }
    for (int i = 0; i < 26 && off + FAT_OFF < size; i++) {
        std::vector<uint8_t> h = img.bytes(off, std::min<uint64_t>(size - off, FAT_OFF + 2 * 30720));
        uint16_t blocks = le16(h.data());
        // a partition needs at least one valid volume entry
        bool has_volume = false;
        for (int v = 0; v < VOL_DIR_ENTRIES; v++) {
            const uint8_t *e = h.data() + VOL_DIR_OFF + v * 16;
            int type = le16(e + 12);
            if ((type == 1 || type == 3 || type == 7) && akai_name_ok(e, 12)) { has_volume = true; break; }
        }
        if (has_volume) {
            Partition p;
            p.offset = off;
            size_t nfat = std::min<size_t>(blocks ? blocks : 30720, (h.size() - FAT_OFF) / 2);
            for (size_t k = 0; k < nfat; k++) p.fat.push_back(le16(h.data() + FAT_OFF + 2 * k));
            parts.push_back(p);
        }
        if (blocks == 0 || blocks == 0x8000 || blocks == 0x0FFF || blocks == 0xFFFF || blocks >= 30720) break;
        off += uint64_t(blocks) * HD_BLOCK;
    }
    return parts;
}

VolumePtr mount_akai_hd(BlobPtr img) {
    std::vector<Partition> parts = scan_partitions(*img);
    if (parts.empty()) throw ParseError("no Akai partitions");
    auto vol = std::make_shared<TreeVolume>("Akai S1000/S3000 disk");
    uint64_t img_size = img->size();
    for (size_t pi = 0; pi < parts.size(); pi++) {
        const Partition &part = parts[pi];
        std::string pname = std::string(1, char('A' + pi));
        std::vector<uint8_t> vd = img->bytes(part.offset + VOL_DIR_OFF, VOL_DIR_ENTRIES * 16);
        for (int v = 0; v < VOL_DIR_ENTRIES; v++) {
            const uint8_t *e = vd.data() + v * 16;
            int type = le16(e + 12), start = le16(e + 14);
            if (type != 1 && type != 3 && type != 7) continue;
            bool s3000 = type != 1;
            std::string vname = akai_to_ascii(e, 12);
            if (vname.empty()) vname = "VOLUME " + std::to_string(v + 1);
            std::string vpath = pname + "/" + vname;
            int max_files = s3000 ? 509 : 126;
            // the directory: entries 0..340 in the volume's first block, the rest in the block after it (FAT)
            uint64_t dir1 = part.offset + uint64_t(start) * HD_BLOCK;
            if (dir1 + HD_BLOCK > img_size) continue;
            std::vector<uint8_t> d1 = img->bytes(dir1, size_t(HD_BLOCK)), d2;
            if (max_files > 341 && size_t(start) < part.fat.size()) {
                // the second directory cluster: by the FAT chain, or the next cluster marked 0x8000
                uint16_t next = part.fat[size_t(start)];
                if ((next == 0 || next >= 0x4000) && size_t(start) + 1 < part.fat.size() && part.fat[size_t(start) + 1] == 0x8000) next = uint16_t(start + 1);
                uint64_t dir2 = part.offset + uint64_t(next) * HD_BLOCK;
                if (next && next < 0x4000 && dir2 + HD_BLOCK <= img_size) d2 = img->bytes(dir2, size_t(HD_BLOCK));
            }
            vol->add_dir(vpath);
            for (int f = 0; f < max_files; f++) {
                const uint8_t *fe;
                if (f < 341) fe = d1.data() + f * 24;
                else if (!d2.empty()) fe = d2.data() + (f - 341) * 24;
                else break;
                int ftype = fe[16];
                uint64_t fsize = le24(fe + 17);
                int fstart = le16(fe + 20);
                std::string ext = file_ext(ftype, s3000);
                if (ext.empty() || !fsize || !akai_name_ok(fe, 12)) continue;
                std::string fname = akai_to_ascii(fe, 12) + ext;
                auto extents = file_extents(part, img_size, fstart, fsize);
                vol->add_lazy(vpath + "/" + fname, fsize, [img, extents] { return make_extents(img, extents); });
            }
        }
    }
    if (!vol->file_count()) throw ParseError("no programs or samples on this Akai disk");
    return vol;
}

bool volumes_at(const uint8_t *h, size_t n) {
    if (n < FAT_OFF) return false;
    int good = 0;
    for (int v = 0; v < VOL_DIR_ENTRIES; v++) {
        const uint8_t *e = h + VOL_DIR_OFF + v * 16;
        int type = le16(e + 12);
        if (type == 0) continue;
        if ((type == 1 || type == 3 || type == 7) && akai_name_ok(e, 12)) good++;
        else if (type != 0xFFFF && type > 255) return false;
    }
    return good > 0;
}

bool probe_akai_hd(const std::string &name, const uint8_t *h, size_t n, uint64_t size) {
    if (n < FAT_OFF || size < 0x10000) return false;
    if (n > 0x8006 && !std::memcmp(h + 0x8001, "CD001", 5)) return false;   // ISO 9660
    if (volumes_at(h, n)) return true;
    return n >= 0x4B000 + FAT_OFF && volumes_at(h + 0x4B000, n - 0x4B000);   // CD rip with a lead-in
}

// ---------------------------------------------------------------------------------------------------------------
// floppies (1 KB blocks, directory at 0)

bool s900_name_ok(const uint8_t *p, size_t n) {
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < 0x20 || p[i] > 0x7E) return false;
        if (p[i] != ' ') any = true;
    }
    return any;
}

enum class Floppy { None, S900, S1000 };

Floppy floppy_kind(const uint8_t *h, size_t n, uint64_t size) {
    if (size != 819200 && size != 1638400 && size != 1474560 && size != 737280) return Floppy::None;
    if (n < 64 * 24) return Floppy::None;
    int s900 = 0, s1000 = 0, bad = 0;
    for (int i = 0; i < 64; i++) {
        const uint8_t *e = h + i * 24;
        bool empty = true;
        for (int k = 0; k < 24; k++) if (e[k]) { empty = false; break; }
        if (empty) continue;
        if ((e[16] == 'S' || e[16] == 'P') && s900_name_ok(e, 10) && le24(e + 17)) s900++;
        else if (akai_name_ok(e, 12) && (e[16] == 's' || e[16] == 'p' || e[16] == 's' + 128 || e[16] == 'p' + 128)) s1000++;
        else bad++;
    }
    if (s900 > 0 && s900 >= s1000 && s900 > bad) return Floppy::S900;
    if (s1000 > 0 && s1000 > bad) return Floppy::S1000;
    return Floppy::None;
}

VolumePtr mount_akai_floppy(BlobPtr img) {
    std::vector<uint8_t> h = img->head(512 * 24);
    Floppy kind = floppy_kind(h.data(), h.size(), img->size());
    if (kind == Floppy::None) throw ParseError("not an Akai floppy image");
    auto vol = std::make_shared<TreeVolume>(kind == Floppy::S900 ? "Akai S900/S950 floppy" : "Akai S1000/S3000 floppy");
    int entries = kind == Floppy::S900 ? 64 : (h.size() >= 512 * 24 ? 512 : 64);
    uint64_t size = img->size();
    for (int i = 0; i < entries && size_t(i + 1) * 24 <= h.size(); i++) {
        const uint8_t *e = h.data() + i * 24;
        uint64_t len = le24(e + 17);
        uint64_t start = uint64_t(le16(e + 20)) * 1024;
        if (!len || start + len > size) continue;
        std::string name;
        if (kind == Floppy::S900) {
            if (!s900_name_ok(e, 10) || (e[16] != 'S' && e[16] != 'P')) continue;
            name = trim(std::string(reinterpret_cast<const char *>(e), 10)) + (e[16] == 'P' ? ".p9" : ".s9");
            int compression = le16(e + 22);
            if (compression && e[16] == 'S') name = trim(std::string(reinterpret_cast<const char *>(e), 10)) + ".s9c";
        } else {
            std::string ext = file_ext(e[16], false);
            if (ext.empty() || !akai_name_ok(e, 12)) continue;
            name = akai_to_ascii(e, 12) + ext;
        }
        vol->add_file(name, make_slice(img, start, len));
    }
    if (!vol->file_count()) throw ParseError("empty Akai floppy");
    return vol;
}

bool probe_akai_floppy(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    return floppy_kind(h, n, size) != Floppy::None;
}

}  // namespace

void register_akai_images() {
    register_image_type({"Akai floppy", probe_akai_floppy, mount_akai_floppy});
    register_image_type({"Akai S1000/S3000 disk", probe_akai_hd, mount_akai_hd});
}

}  // namespace omni

// Omni Sampler: the E-mu disk filesystem of the EIII, EIIIX, ESI and EOS (E4, E-Synth, E6400, e5000...) samplers'
// CD-ROMs and hard disks: a superblock ("EMU3"), a cluster chain list, a root directory of folders and
// dir-content blocks with the file entries, in 512 byte blocks. Read as ConvertWithMoss's Emu3DiskImage
// (LGPL-3.0, layout from the mpc2emu project and the emu3fs kernel module). Each folder becomes a directory;
// banks are named .e4b (EOS) or .e3b (EIII family) by the form type of their entry, verified on first read.
#include <cstring>
#include <set>
#include "fs.hpp"

namespace omni {
namespace {

constexpr uint64_t BLK = 512;

std::string emu_name(const uint8_t *p) {
    std::string s;
    for (int i = 0; i < 16 && p[i]; i++) s += (p[i] >= 0x20 && p[i] < 0x7F) ? char(p[i]) : '?';
    s = trim(s);
    for (char &c : s) if (c == '/') c = '-';
    return s;
}

VolumePtr mount_emu3(BlobPtr img) {
    std::vector<uint8_t> sb = img->bytes(0, BLK);
    if (std::memcmp(sb.data(), "EMU3", 4)) throw ParseError("not an E-mu disk");
    uint32_t root_start = le32(&sb[8]), root_blocks = le32(&sb[12]), fat_start = le32(&sb[0x18]), fat_blocks = le32(&sb[0x1C]);
    uint32_t data_start = le32(&sb[0x20]);
    int extra = sb[0x28];
    uint64_t total = img->size() / BLK;
    if (!root_start || !root_blocks || root_blocks > 64 || !fat_start || !fat_blocks || fat_blocks > 64 || !data_start || extra < 1 ||
        extra > 12 || root_start + root_blocks > total || fat_start + fat_blocks > total)
        throw ParseError("malformed E-mu disk superblock");
    uint64_t cluster_bytes = uint64_t(1) << (15 + extra);
    std::vector<uint8_t> fatd = img->bytes(uint64_t(fat_start) * BLK, size_t(fat_blocks) * BLK);
    std::vector<uint16_t> fat(fatd.size() / 2);
    for (size_t i = 0; i < fat.size(); i++) fat[i] = le16(&fatd[2 * i]);
    std::vector<uint8_t> root = img->bytes(uint64_t(root_start) * BLK, size_t(root_blocks) * BLK);
    auto vol = std::make_shared<TreeVolume>("E-mu disk");
    std::set<uint32_t> seen_blocks;
    for (size_t off = 0; off + 32 <= root.size(); off += 32) {
        int ftype = root[off + 17];
        if (ftype != 0x40 && ftype != 0x80) continue;
        std::string folder = emu_name(&root[off]);
        if (folder.empty()) folder = "Folder " + std::to_string(off / 32 + 1);
        vol->add_dir(folder);
        for (int i = 0; i < 7; i++) {
            uint32_t block = le16(&root[off + 18 + size_t(i) * 2]);
            if (!block || block == 0xFFFF || block >= total || !seen_blocks.insert(block).second) continue;
            std::vector<uint8_t> ents = img->bytes(uint64_t(block) * BLK, BLK);
            for (size_t e = 0; e + 32 <= ents.size(); e += 32) {
                const uint8_t *en = &ents[e];
                uint32_t start = le16(en + 18), nclusters = le16(en + 20), last_blocks = le16(en + 22), last_bytes = le16(en + 24);
                int type = en[26];
                if (type == 0 || type == 0x80 || start < 1 || nclusters < 1 || (last_blocks < 1 && last_bytes == 0)) continue;
                uint64_t last = std::min<uint64_t>((last_blocks - 1ull) * BLK + (last_bytes ? last_bytes : BLK), cluster_bytes);
                uint64_t size = (nclusters - 1ull) * cluster_bytes + last;
                std::vector<std::pair<uint64_t, uint64_t>> ext;
                uint32_t c = start;
                uint64_t left = size;
                std::set<uint32_t> visited;
                bool ok = true;
                for (uint32_t k = 0; k < nclusters; k++) {
                    if (c < 1 || c >= fat.size() || !visited.insert(c).second) { ok = false; break; }
                    uint64_t at = (uint64_t(data_start) + (c - 1ull) * (cluster_bytes / BLK)) * BLK, take = std::min(left, cluster_bytes);
                    if (at + take > img->size()) { ok = false; break; }
                    if (!ext.empty() && ext.back().first + ext.back().second == at) ext.back().second += take;
                    else ext.push_back({at, take});
                    left -= take;
                    if (k + 1 < nclusters) c = fat[c] == 0x7FFF ? 0 : fat[c];
                }
                if (!ok || ext.empty()) continue;
                bool eos = !std::memcmp(en + 28, "E4B0", 4);
                std::string name = emu_name(en);
                if (name.empty()) name = "Bank " + std::to_string(e / 32 + 1);
                vol->add_lazy(folder + "/" + name + (eos ? ".e4b" : ".e3b"), size, [img, ext] { return make_extents(img, ext); });
            }
        }
    }
    if (!vol->file_count()) throw ParseError("no banks on this E-mu disk");
    return vol;
}

bool probe_emu3(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 512 && !std::memcmp(h, "EMU3", 4); }

}  // namespace

void register_emu_images() {
    register_image_type({"E-mu disk", probe_emu3, mount_emu3});
}

}  // namespace omni

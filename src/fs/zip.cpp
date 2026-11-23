// Omni Sampler 2.4.10: ZIP archives mount as folders; deflate entries inflate only when a loader opens them.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../core/bytes.hpp"
#include "../third_party/tinf/tinf.h"
#include "fs.hpp"

namespace omni {
namespace {

constexpr uint32_t EOCD = 0x06054B50, EOCD64 = 0x06064B50, LOC64 = 0x07064B50;
constexpr uint32_t CENTRAL = 0x02014B50, LOCAL = 0x04034B50;
constexpr uint64_t MAX_ENTRIES = 100000, MAX_DIRECTORY = 128u << 20;
// 2.4.11 (audit H5): an inflated entry is held whole, next to its compressed copy and then the decoded PCM: on the 32-bit
// Live II (2 GB, one address space with the MPC app) 512 MB of each was an out-of-memory kill. 192 MB there, 512 MB on 64-bit.
constexpr uint64_t MAX_FILE = uint64_t(2) << 30, MAX_DEFLATE = sizeof(void *) == 4 ? uint64_t(192) << 20 : uint64_t(512) << 20;
constexpr uint64_t MAX_COMPRESSED = MAX_DEFLATE;
constexpr uint64_t MAX_PATH_BYTES = uint64_t(8) << 20;
constexpr uint32_t CP437[128] = {
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0
};

struct End {
    uint64_t entries = 0, directory_size = 0, directory_offset = 0, record_offset = 0;
};

bool range_ok(uint64_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

std::vector<uint8_t> read_range(const Blob &blob, uint64_t offset, size_t length) {
    if (!range_ok(blob.size(), offset, length)) throw ParseError("ZIP: record outside archive");
    return blob.bytes(offset, length);
}

End find_end(const Blob &image) {
    const uint64_t size = image.size();
    if (size < 22) throw ParseError("ZIP: no end record");
    const uint64_t tail_size = std::min<uint64_t>(size, 65557);
    const uint64_t tail_offset = size - tail_size;
    const std::vector<uint8_t> tail = read_range(image, tail_offset, size_t(tail_size));
    size_t pos = tail.size() - 22;
    size_t eocd = tail.size();
    for (;;) {
        if (le32(tail.data() + pos) == EOCD) {
            const uint16_t comment = le16(tail.data() + pos + 20);
            if (pos + 22 + size_t(comment) == tail.size()) { eocd = pos; break; }
        }
        if (pos == 0) break;
        --pos;
    }
    if (eocd == tail.size()) throw ParseError("ZIP: no end record");
    const uint64_t eocd_offset = tail_offset + eocd;
    const uint8_t *p = tail.data() + eocd;
    const uint16_t disk = le16(p + 4), directory_disk = le16(p + 6);
    const uint16_t disk_count = le16(p + 8), count = le16(p + 10);
    const uint32_t directory_size = le32(p + 12), directory_offset = le32(p + 16);
    if (disk || directory_disk || (disk_count != count && disk_count != 0xFFFF))
        throw ParseError("ZIP: multi-disk archives are not supported");

    End out;
    out.entries = count;
    out.directory_size = directory_size;
    out.directory_offset = directory_offset;
    out.record_offset = eocd_offset;
    const bool sentinels = count == 0xFFFF || disk_count == 0xFFFF || directory_size == 0xFFFFFFFF ||
                           directory_offset == 0xFFFFFFFF;
    if (eocd_offset >= 20) {
        const std::vector<uint8_t> locator = read_range(image, eocd_offset - 20, 20);
        if (le32(locator.data()) == LOC64) {
            if (le32(locator.data() + 4) != 0 || le32(locator.data() + 16) != 1)
                throw ParseError("ZIP: multi-disk ZIP64 archives are not supported");
            const uint64_t zip64_offset = le64(locator.data() + 8);
            const std::vector<uint8_t> head = read_range(image, zip64_offset, 56);
            if (le32(head.data()) != EOCD64 || le64(head.data() + 4) < 44)
                throw ParseError("ZIP: invalid ZIP64 end record");
            const uint64_t record_size = le64(head.data() + 4);
            if (record_size > std::numeric_limits<uint64_t>::max() - 12 ||
                !range_ok(size, zip64_offset, 12 + record_size) || zip64_offset + 12 + record_size > eocd_offset - 20)
                throw ParseError("ZIP: ZIP64 end record outside archive");
            if (le32(head.data() + 16) != 0 || le32(head.data() + 20) != 0 ||
                le64(head.data() + 24) != le64(head.data() + 32))
                throw ParseError("ZIP: multi-disk ZIP64 archives are not supported");
            out.entries = le64(head.data() + 32);
            out.directory_size = le64(head.data() + 40);
            out.directory_offset = le64(head.data() + 48);
            out.record_offset = zip64_offset;
        } else if (sentinels) {
            throw ParseError("ZIP: missing ZIP64 end record");
        }
    } else if (sentinels) {
        throw ParseError("ZIP: missing ZIP64 locator");
    }
    if (out.entries > MAX_ENTRIES) throw ParseError("ZIP: too many entries");
    if (out.directory_size > MAX_DIRECTORY || !range_ok(size, out.directory_offset, out.directory_size) ||
        out.directory_offset + out.directory_size > out.record_offset)
        throw ParseError("ZIP: central directory outside archive or too large");
    return out;
}

void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) out.push_back(char(cp));
    else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

std::string decode_name(const uint8_t *p, size_t n, bool utf8) {
    if (utf8) return std::string(reinterpret_cast<const char *>(p), n);
    std::string out;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = p[i];
        if (c < 0x80) out.push_back(char(c));
        else append_utf8(out, CP437[c - 0x80]);
    }
    return out;
}

bool safe_path(std::string &path, bool is_dir) {
    for (char &c : path) if (c == '\\') c = '/';
    if (path.empty() || path[0] == '/' || path.find('\0') != std::string::npos) return false;
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') return false;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(start, end - start);
        if (part == "..") return false;
        if (!part.empty() && part != ".") {
            if (part[0] == '.') return false;
            parts.push_back(part);
            if (parts.size() > 32) return false;
        }
        start = end + 1;
    }
    if (parts.empty() || parts[0] == "__MACOSX") return false;
    path.clear();
    for (const std::string &part : parts) {
        if (!path.empty()) path += '/';
        path += part;
    }
    if (path.size() > 4096) return false;
    if (is_dir) path += '/';
    return true;
}

struct Entry {
    std::string path;
    uint16_t flags = 0, method = 0;
    uint32_t crc = 0;
    uint64_t compressed = 0, uncompressed = 0, local_offset = 0;
    bool directory = false;
};

void parse_zip64_extra(const uint8_t *p, size_t n, Entry &entry, uint32_t comp32, uint32_t uncomp32,
                       uint32_t offset32, uint16_t disk16) {
    size_t pos = 0;
    bool found = false;
    while (pos + 4 <= n) {
        const uint16_t id = le16(p + pos), len = le16(p + pos + 2);
        pos += 4;
        if (len > n - pos) throw ParseError("ZIP: malformed extra field");
        if (id == 1) {
            size_t q = pos, end = pos + len;
            if (uncomp32 == 0xFFFFFFFF) {
                if (end - q < 8) throw ParseError("ZIP: missing ZIP64 uncompressed size");
                entry.uncompressed = le64(p + q); q += 8;
            }
            if (comp32 == 0xFFFFFFFF) {
                if (end - q < 8) throw ParseError("ZIP: missing ZIP64 compressed size");
                entry.compressed = le64(p + q); q += 8;
            }
            if (offset32 == 0xFFFFFFFF) {
                if (end - q < 8) throw ParseError("ZIP: missing ZIP64 local offset");
                entry.local_offset = le64(p + q); q += 8;
            }
            if (disk16 == 0xFFFF) {
                if (end - q < 4) throw ParseError("ZIP: missing ZIP64 disk number");
                if (le32(p + q) != 0) throw ParseError("ZIP: multi-disk entry not supported");
            }
            found = true;
            break;
        }
        pos += len;
    }
    if ((uncomp32 == 0xFFFFFFFF || comp32 == 0xFFFFFFFF || offset32 == 0xFFFFFFFF || disk16 == 0xFFFF) && !found)
        throw ParseError("ZIP: missing ZIP64 extra field");
}

uint64_t local_data_offset(const Blob &image, const Entry &entry) {
    const std::vector<uint8_t> head = read_range(image, entry.local_offset, 30);
    if (le32(head.data()) != LOCAL) throw ParseError("ZIP: invalid local file header: " + entry.path);
    const uint64_t variable = uint64_t(30) + le16(head.data() + 26) + le16(head.data() + 28);
    if (!range_ok(image.size(), entry.local_offset, variable)) throw ParseError("ZIP: truncated local header: " + entry.path);
    const uint64_t data = entry.local_offset + variable;
    if (!range_ok(image.size(), data, entry.compressed)) throw ParseError("ZIP: truncated entry data: " + entry.path);
    return data;
}

VolumePtr mount_zip(BlobPtr image) {
    const End end = find_end(*image);
    const std::vector<uint8_t> directory = read_range(*image, end.directory_offset, size_t(end.directory_size));
    auto volume = std::make_shared<TreeVolume>("ZIP");
    size_t pos = 0;
    uint64_t path_bytes = 0;
    for (uint64_t i = 0; i < end.entries; i++) {
        if (pos > directory.size() || directory.size() - pos < 46 || le32(directory.data() + pos) != CENTRAL)
            throw ParseError("ZIP: invalid central directory entry");
        const uint8_t *p = directory.data() + pos;
        Entry entry;
        entry.flags = le16(p + 8);
        entry.method = le16(p + 10);
        entry.crc = le32(p + 16);
        const uint32_t comp32 = le32(p + 20), uncomp32 = le32(p + 24);
        const uint16_t name_len = le16(p + 28), extra_len = le16(p + 30), comment_len = le16(p + 32);
        const uint16_t disk16 = le16(p + 34);
        const uint32_t offset32 = le32(p + 42);
        const uint64_t variable = uint64_t(46) + name_len + extra_len + comment_len;
        if (!range_ok(directory.size(), pos, variable)) throw ParseError("ZIP: truncated central directory entry");
        const uint8_t *name = p + 46;
        const uint8_t *extra = name + name_len;
        entry.compressed = comp32;
        entry.uncompressed = uncomp32;
        entry.local_offset = offset32;
        parse_zip64_extra(extra, extra_len, entry, comp32, uncomp32, offset32, disk16);
        if (disk16 != 0 && disk16 != 0xFFFF) throw ParseError("ZIP: multi-disk entry not supported");
        entry.directory = name_len && (name[name_len - 1] == '/' || name[name_len - 1] == '\\');
        entry.path = decode_name(name, name_len, (entry.flags & 0x0800) != 0);
        if (safe_path(entry.path, entry.directory)) {
            if (entry.path.size() > MAX_PATH_BYTES - path_bytes) throw ParseError("ZIP: total path data exceeds limit");
            path_bytes += entry.path.size();
            if (entry.uncompressed > MAX_FILE) throw ParseError("ZIP: entry exceeds 2 GB: " + entry.path);
            if (entry.method == 8) {
                if (entry.uncompressed > MAX_DEFLATE || entry.compressed > MAX_COMPRESSED)
                    throw ParseError("ZIP: deflate entry exceeds size limit: " + entry.path);
                if (entry.uncompressed && (!entry.compressed || entry.uncompressed > entry.compressed * 1000))
                    throw ParseError("ZIP: deflate ratio exceeds 1000: " + entry.path);
            }
            if (entry.directory) {
                volume->add_dir(entry.path.substr(0, entry.path.size() - 1));
            } else if ((entry.flags & 0x0041) || (entry.method != 0 && entry.method != 8)) {
                const std::string reason = (entry.flags & 0x0041) ? "encrypted" : "uses method " + std::to_string(entry.method);
                const std::string name_for_error = entry.path;
                volume->add_lazy(entry.path, entry.uncompressed, [reason, name_for_error]() -> BlobPtr {
                    throw ParseError("ZIP: " + name_for_error + " is " + reason + ": not supported");
                });
            } else if (entry.method == 0) {
                if (entry.compressed != entry.uncompressed) throw ParseError("ZIP: invalid stored entry size: " + entry.path);
                const uint64_t data = local_data_offset(*image, entry);
                volume->add_file(entry.path, make_slice(image, data, entry.uncompressed));
            } else {
                const BlobPtr archive = image;
                const std::string name_for_error = entry.path;
                // the inflated bytes live while a loader holds them, then go (not cached for the mount's life)
                struct Live { std::mutex m; std::weak_ptr<const Blob> blob; };
                auto live = std::make_shared<Live>();
                volume->add_lazy(entry.path, entry.uncompressed, [archive, entry, name_for_error, live]() -> BlobPtr {
                    std::lock_guard<std::mutex> lock(live->m);
                    if (BlobPtr b = live->blob.lock()) return b;
                    const uint64_t data = local_data_offset(*archive, entry);
                    std::vector<uint8_t> compressed = read_range(*archive, data, size_t(entry.compressed));
                    std::vector<uint8_t> output(size_t(std::max<uint64_t>(1, entry.uncompressed)));
                    unsigned int output_size = unsigned(output.size());
                    const int rc = tinf_uncompress(output.data(), &output_size, compressed.data(), unsigned(compressed.size()));
                    std::vector<uint8_t>().swap(compressed);   // 2.4.11: freed now, not after the CRC and the blob copy
                    if (rc != TINF_OK || output_size != entry.uncompressed)
                        throw ParseError("ZIP: deflate failed or size mismatch: " + name_for_error);
                    output.resize(output_size);
                    if (tinf_crc32(output.data(), output_size) != entry.crc)
                        throw ParseError("ZIP: CRC mismatch: " + name_for_error);
                    BlobPtr b = make_mem_blob(std::move(output));
                    live->blob = b;
                    return b;
                }, false);
            }
        }
        pos += size_t(variable);
    }
    return volume;
}

bool probe_zip(const std::string &name, const uint8_t *head, size_t n, uint64_t) {
    return (ends_with_ci(name, ".zip") || ends_with_ci(name, ".vitalbank") || ends_with_ci(name, ".dslibrary")) && n >= 4 &&   // 2.4.11: Vital banks are ZIPs
           (le32(head) == 0x04034B50 || (n >= 4 && le32(head) == 0x06054B50));
}

}  // namespace

// 2.4.10: register ZIP as an image type so normal format readers browse its files unchanged.
void register_zip() {
    register_image_type({"ZIP", probe_zip, mount_zip});
}

}  // namespace omni

// Omni Sampler: byte sources and a virtual file system in which disk images (ISO 9660, Akai, E-mu, MPC2000,
// floppy images...) open like folders. A path is one string: "/sdcard/Akai/Strings.iso/A/STRINGS/VIOLIN.P" walks
// the host file system to Strings.iso, then the image's own tree. Readers only ever see Volume + inner path, so
// one Akai program reader serves a CD-ROM, a hard disk image and files exported to a folder alike.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "bytes.hpp"

namespace omni {

// Random-access bytes. read() throws ParseError on a short read. Implementations are thread-safe.
struct Blob {
    virtual ~Blob() = default;
    virtual uint64_t size() const = 0;
    virtual void read(uint64_t offset, void *dst, size_t n) const = 0;

    std::vector<uint8_t> bytes(uint64_t offset, size_t n) const {
        std::vector<uint8_t> v(n);
        if (n) read(offset, v.data(), n);
        return v;
    }
    // At most n bytes from offset (fewer at the end), never throws for a short tail.
    std::vector<uint8_t> head(size_t n, uint64_t offset = 0) const {
        uint64_t s = size();
        if (offset >= s) return {};
        return bytes(offset, size_t(std::min<uint64_t>(n, s - offset)));
    }
    std::vector<uint8_t> all(size_t limit = size_t(1) << 31) const;
};
using BlobPtr = std::shared_ptr<const Blob>;

BlobPtr open_host_file(const std::string &path);                     // throws ParseError
BlobPtr make_mem_blob(std::vector<uint8_t> data);
BlobPtr make_slice(BlobPtr parent, uint64_t offset, uint64_t size);  // clipped to the parent
// A file stored in pieces (FAT chains, extent lists): (offset, length) pairs in parent order.
BlobPtr make_extents(BlobPtr parent, std::vector<std::pair<uint64_t, uint64_t>> extents);

struct DirEntry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
};

// A file system: the host's, or one inside an image. Inner paths use '/', no leading slash, "" = root.
struct Volume {
    virtual ~Volume() = default;
    virtual std::string kind() const = 0;                        // "ISO 9660", "Akai S1000/S3000", ...
    virtual std::vector<DirEntry> list(const std::string &dir) = 0;
    virtual BlobPtr open(const std::string &path) = 0;           // throws ParseError when missing
    virtual bool is_dir(const std::string &path);
    virtual bool exists(const std::string &path);
};
using VolumePtr = std::shared_ptr<Volume>;

// Image types a file can mount as. probe() looks only at the first bytes and the size.
struct ImageType {
    const char *name;
    bool (*probe)(const std::string &lower_name, const uint8_t *head, size_t n, uint64_t size);
    VolumePtr (*mount)(BlobPtr image);                           // throws ParseError
};
void register_image_type(const ImageType &type);
const std::vector<ImageType> &image_types();
const ImageType *probe_image(const std::string &name, const Blob &blob);
size_t image_probe_bytes();                                      // how many head bytes probe() wants

// A resolved path: the volume it lives in and the path inside it.
struct Location {
    VolumePtr volume;
    std::string inner;       // path inside volume
    std::string full;        // the whole path as given
};

// Resolves full paths through mounted images, caching mounts. One per engine instance.
class Vfs {
public:
    Vfs();
    Location resolve(const std::string &full);                   // throws ParseError
    std::vector<DirEntry> list(const std::string &full);         // a folder or a mounted image
    bool is_container(const std::string &full);                  // folder or mountable image
    BlobPtr open(const std::string &full) { Location l = resolve(full); return l.volume->open(l.inner); }
    void forget();                                               // drop cached image mounts

private:
    VolumePtr host_;
    std::mutex mutex_;
    std::map<std::string, VolumePtr> mounts_;                    // full path of image -> its volume
    VolumePtr mount_image(const std::string &full, VolumePtr parent, const std::string &inner);
};

// Path helpers ('/'-separated)
std::string path_join(const std::string &dir, const std::string &name);
std::string path_dir(const std::string &path);
std::string path_name(const std::string &path);
std::string path_stem(const std::string &path);
std::string path_ext(const std::string &path);                    // lower case, without the dot
std::vector<std::string> path_split(const std::string &path);
// Resolve a relative reference (SFZ sample=, Kontakt file list) against a base folder; handles ../ and '\'.
std::string path_resolve(const std::string &base_dir, const std::string &ref);
// Case-insensitive lookup of a name in a folder of a volume (sample files on FAT/ISO media often change case).
bool find_ci(Volume &vol, const std::string &path, std::string &found);

}  // namespace omni

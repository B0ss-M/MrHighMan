// Omni Sampler: disk image file systems. Each mounts an image Blob as a Volume (see core/vfs.hpp).
#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include "../core/vfs.hpp"

namespace omni {

// Akai sampler character set (S1000/S3000 names) to ASCII
std::string akai_to_ascii(const uint8_t *p, size_t n);

// A simple in-memory directory tree for image volumes: files are blobs, folders hold children.
class TreeVolume : public Volume {
public:
    explicit TreeVolume(std::string kind) : kind_(std::move(kind)) {}
    std::string kind() const override { return kind_; }
    std::vector<DirEntry> list(const std::string &dir) override;
    BlobPtr open(const std::string &path) override;
    bool is_dir(const std::string &path) override;
    bool exists(const std::string &path) override;
    // building
    void add_dir(const std::string &path);
    void add_file(const std::string &path, BlobPtr blob);   // creates parent folders; renames duplicates
    void add_lazy(const std::string &path, uint64_t size, std::function<BlobPtr()> make);
    size_t file_count() const { return files_.size(); }

private:
    struct File { BlobPtr blob; uint64_t size = 0; std::function<BlobPtr()> make; };
    std::string kind_;
    std::map<std::string, std::vector<std::string>> dirs_{{"", {}}};   // dir -> child names (in order)
    std::map<std::string, File> files_;
    std::mutex mutex_;
};

void register_akai_images();
void register_roland_images();
void register_iso9660();
void register_fat();
void register_emu_images();
void register_floppy_containers();   // HFE, IMD -> sector images handed to the other types

}  // namespace omni

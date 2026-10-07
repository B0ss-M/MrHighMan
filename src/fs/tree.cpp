// Omni Sampler: TreeVolume, the directory tree image volumes are built into (see fs.hpp).
#include "fs.hpp"

namespace omni {

std::vector<DirEntry> TreeVolume::list(const std::string &dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DirEntry> out;
    auto it = dirs_.find(dir);
    if (it == dirs_.end()) return out;
    for (auto &name : it->second) {
        std::string full = path_join(dir, name);
        DirEntry e;
        e.name = name;
        auto f = files_.find(full);
        if (f != files_.end()) e.size = f->second.size;
        else e.dir = true;
        out.push_back(e);
    }
    return out;
}

BlobPtr TreeVolume::open(const std::string &path) {
    std::function<BlobPtr()> make;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto f = files_.find(path);
        if (f == files_.end()) throw ParseError("not found in image: " + path);
        if (f->second.blob) return f->second.blob;
        make = f->second.make;
    }
    BlobPtr b = make();
    std::lock_guard<std::mutex> lock(mutex_);
    files_[path].blob = b;
    return b;
}

bool TreeVolume::is_dir(const std::string &path) {
    std::lock_guard<std::mutex> lock(mutex_);
    return dirs_.count(path) > 0;
}

bool TreeVolume::exists(const std::string &path) {
    std::lock_guard<std::mutex> lock(mutex_);
    return dirs_.count(path) > 0 || files_.count(path) > 0;
}

void TreeVolume::add_dir(const std::string &path) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string cur;
    for (auto &part : path_split(path)) {
        std::string next = path_join(cur, part);
        if (!dirs_.count(next)) {
            dirs_[next] = {};
            dirs_[cur].push_back(part);
        }
        cur = next;
    }
}

void TreeVolume::add_file(const std::string &path, BlobPtr blob) {
    uint64_t size = blob ? blob->size() : 0;
    add_lazy(path, size, [blob] { return blob; });
}

void TreeVolume::add_lazy(const std::string &path, uint64_t size, std::function<BlobPtr()> make) {
    std::string dir = path_dir(path), name = path_name(path);
    if (dir == "/") dir.clear();
    add_dir(dir);
    std::lock_guard<std::mutex> lock(mutex_);
    std::string full = path_join(dir, name);
    // duplicate names (Akai volumes allow them): "NAME (2).P"
    for (int n = 2; files_.count(full) || dirs_.count(full); n++) {
        std::string stem = path_stem(name), ext = name.substr(stem.size());
        full = path_join(dir, stem + " (" + std::to_string(n) + ")" + ext);
    }
    File f;
    f.size = size;
    f.make = std::move(make);
    files_[full] = f;
    dirs_[dir].push_back(path_name(full));
}

}  // namespace omni

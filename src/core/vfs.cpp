// Omni Sampler: blobs, the host volume and image mounting (see vfs.hpp).
#include "vfs.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omni {

// ---------------------------------------------------------------------------------------------------------------
// text helpers (declared in bytes.hpp)

std::string utf16_to_utf8(const uint16_t *u, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00);
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | c >> 12); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | c >> 18); out += char(0x80 | (c >> 12 & 0x3F)); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::string lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

bool equals_ci(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool ends_with_ci(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && equals_ci(s.substr(s.size() - suffix.size()), suffix);
}

// ---------------------------------------------------------------------------------------------------------------
// blobs

std::vector<uint8_t> Blob::all(size_t limit) const {
    uint64_t s = size();
    if (s > limit) throw ParseError("file too large (" + std::to_string(s >> 20) + " MB)");
    return bytes(0, size_t(s));
}

namespace {

struct FileBlob : Blob {
    int fd = -1;
    uint64_t length = 0;
    explicit FileBlob(const std::string &path) {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw ParseError("cannot open " + path + ": " + std::strerror(errno));
        struct stat st;
        if (fstat(fd, &st) != 0) { ::close(fd); throw ParseError("cannot stat " + path); }
        length = uint64_t(st.st_size);
    }
    ~FileBlob() override { if (fd >= 0) ::close(fd); }
    uint64_t size() const override { return length; }
    void read(uint64_t offset, void *dst, size_t n) const override {
        if (offset > length || n > length - offset) throw ParseError("read past end of file");
        uint8_t *p = static_cast<uint8_t *>(dst);
        while (n) {
            ssize_t r = ::pread(fd, p, n, off_t(offset));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) throw ParseError("read error");
            p += r; offset += uint64_t(r); n -= size_t(r);
        }
    }
};

struct MemBlob : Blob {
    std::vector<uint8_t> data;
    explicit MemBlob(std::vector<uint8_t> d) : data(std::move(d)) {}
    uint64_t size() const override { return data.size(); }
    void read(uint64_t offset, void *dst, size_t n) const override {
        if (offset > data.size() || n > data.size() - offset) throw ParseError("read past end of data");
        if (n) std::memcpy(dst, data.data() + offset, n);
    }
};

struct SliceBlob : Blob {
    BlobPtr parent;
    uint64_t base, length;
    SliceBlob(BlobPtr p, uint64_t b, uint64_t l) : parent(std::move(p)), base(b), length(l) {}
    uint64_t size() const override { return length; }
    void read(uint64_t offset, void *dst, size_t n) const override {
        if (offset > length || n > length - offset) throw ParseError("read past end of file");
        parent->read(base + offset, dst, n);
    }
};

struct ExtentBlob : Blob {
    BlobPtr parent;
    std::vector<std::pair<uint64_t, uint64_t>> ext;   // (parent offset, length)
    std::vector<uint64_t> starts;                     // logical start of each extent
    uint64_t length = 0;
    ExtentBlob(BlobPtr p, std::vector<std::pair<uint64_t, uint64_t>> e) : parent(std::move(p)), ext(std::move(e)) {
        for (auto &x : ext) { starts.push_back(length); length += x.second; }
    }
    uint64_t size() const override { return length; }
    void read(uint64_t offset, void *dst, size_t n) const override {
        if (offset > length || n > length - offset) throw ParseError("read past end of file");
        uint8_t *p = static_cast<uint8_t *>(dst);
        size_t i = size_t(std::upper_bound(starts.begin(), starts.end(), offset) - starts.begin()) - 1;
        while (n) {
            uint64_t in = offset - starts[i];
            size_t take = size_t(std::min<uint64_t>(n, ext[i].second - in));
            parent->read(ext[i].first + in, p, take);
            p += take; offset += take; n -= take; i++;
        }
    }
};

}  // namespace

BlobPtr open_host_file(const std::string &path) { return std::make_shared<FileBlob>(path); }
BlobPtr make_mem_blob(std::vector<uint8_t> data) { return std::make_shared<MemBlob>(std::move(data)); }
BlobPtr make_slice(BlobPtr parent, uint64_t offset, uint64_t size) {
    uint64_t ps = parent->size();
    if (offset > ps) offset = ps;
    if (size > ps - offset) size = ps - offset;
    return std::make_shared<SliceBlob>(std::move(parent), offset, size);
}
BlobPtr make_extents(BlobPtr parent, std::vector<std::pair<uint64_t, uint64_t>> extents) {
    uint64_t ps = parent->size();
    for (auto &e : extents) {   // clip to the image: a damaged chain must not read outside it
        if (e.first > ps) e.first = ps;
        if (e.second > ps - e.first) e.second = ps - e.first;
    }
    if (extents.size() == 1) return make_slice(std::move(parent), extents[0].first, extents[0].second);
    return std::make_shared<ExtentBlob>(std::move(parent), std::move(extents));
}

// ---------------------------------------------------------------------------------------------------------------
// volumes

bool Volume::is_dir(const std::string &path) {
    if (path.empty()) return true;
    std::string dir = path_dir(path), name = path_name(path);
    for (auto &e : list(dir)) if (e.name == name) return e.dir;
    return false;
}

bool Volume::exists(const std::string &path) {
    if (path.empty()) return true;
    std::string dir = path_dir(path), name = path_name(path);
    for (auto &e : list(dir)) if (e.name == name) return true;
    return false;
}

namespace {

// The host file system. Inner paths are absolute host paths without their leading '/'.
struct HostVolume : Volume {
    std::string kind() const override { return "Folder"; }
    static std::string real(const std::string &inner) { return "/" + inner; }
    std::vector<DirEntry> list(const std::string &dir) override {
        std::vector<DirEntry> out;
        DIR *d = opendir(real(dir).c_str());
        if (!d) return out;
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;   // also hides macOS ._ files and .DS_Store
            DirEntry de;
            de.name = e->d_name;
            struct stat st;
            if (stat(real(path_join(dir, de.name)).c_str(), &st) != 0) continue;
            de.dir = S_ISDIR(st.st_mode);
            de.size = uint64_t(st.st_size);
            out.push_back(de);
        }
        closedir(d);
        return out;
    }
    BlobPtr open(const std::string &path) override { return open_host_file(real(path)); }
    bool is_dir(const std::string &path) override {
        struct stat st;
        return stat(real(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    bool exists(const std::string &path) override {
        struct stat st;
        return stat(real(path).c_str(), &st) == 0;
    }
};

std::vector<ImageType> &types_storage() {
    static std::vector<ImageType> types;
    return types;
}

}  // namespace

void register_image_type(const ImageType &type) { types_storage().push_back(type); }
const std::vector<ImageType> &image_types() { return types_storage(); }
size_t image_probe_bytes() { return 0x4C000; }   // up to an Akai CD rip's lead-in

const ImageType *probe_image(const std::string &name, const Blob &blob) {
    std::vector<uint8_t> head = blob.head(image_probe_bytes());
    std::string ln = lower(name);
    for (auto &t : image_types())
        if (t.probe(ln, head.data(), head.size(), blob.size())) return &t;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Vfs

Vfs::Vfs() : host_(std::make_shared<HostVolume>()) {}

void Vfs::forget() {
    std::lock_guard<std::mutex> lock(mutex_);
    mounts_.clear();
}

VolumePtr Vfs::mount_image(const std::string &full, VolumePtr parent, const std::string &inner) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mounts_.find(full);
        if (it != mounts_.end()) return it->second;
    }
    if (parent->is_dir(inner)) return nullptr;
    BlobPtr blob = parent->open(inner);
    const ImageType *t = probe_image(path_name(inner), *blob);
    if (!t) return nullptr;
    VolumePtr v = t->mount(blob);
    std::lock_guard<std::mutex> lock(mutex_);
    if (mounts_.size() > 16) mounts_.clear();   // images hold their open file; keep the cache small
    mounts_[full] = v;
    return v;
}

Location Vfs::resolve(const std::string &full) {
    Location loc;
    loc.full = full;
    loc.volume = host_;
    std::vector<std::string> parts = path_split(full);
    std::string inner, prefix;
    for (size_t i = 0; i < parts.size(); i++) {
        inner = path_join(inner, parts[i]);
        prefix += "/" + parts[i];
        if (i + 1 == parts.size()) break;
        if (loc.volume->is_dir(inner)) continue;
        if (!loc.volume->exists(inner)) throw ParseError("not found: " + prefix);
        VolumePtr v = mount_image(prefix, loc.volume, inner);
        if (!v) throw ParseError("not a folder or disk image: " + prefix);
        loc.volume = v;
        inner.clear();
    }
    loc.inner = inner;
    return loc;
}

std::vector<DirEntry> Vfs::list(const std::string &full) {
    Location l = resolve(full);
    if (l.volume->is_dir(l.inner)) {
        std::vector<DirEntry> v = l.volume->list(l.inner);
        std::sort(v.begin(), v.end(), [](const DirEntry &a, const DirEntry &b) {
            if (a.dir != b.dir) return a.dir;
            return lower(a.name) < lower(b.name);
        });
        return v;
    }
    VolumePtr img = mount_image(full, l.volume, l.inner);
    if (!img) throw ParseError("not a folder or disk image: " + full);
    std::vector<DirEntry> v = img->list("");
    std::sort(v.begin(), v.end(), [](const DirEntry &a, const DirEntry &b) {
        if (a.dir != b.dir) return a.dir;
        return lower(a.name) < lower(b.name);
    });
    return v;
}

bool Vfs::is_container(const std::string &full) {
    try {
        Location l = resolve(full);
        if (l.volume->is_dir(l.inner)) return true;
        return mount_image(full, l.volume, l.inner) != nullptr;
    } catch (const std::exception &) {
        return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// paths

std::vector<std::string> path_split(const std::string &path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string path_join(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string path_dir(const std::string &path) {
    size_t p = path.find_last_of('/');
    if (p == std::string::npos) return "";
    if (p == 0) return "/";
    return path.substr(0, p);
}

std::string path_name(const std::string &path) {
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string path_stem(const std::string &path) {
    std::string n = path_name(path);
    size_t d = n.find_last_of('.');
    return d == std::string::npos || d == 0 ? n : n.substr(0, d);
}

std::string path_ext(const std::string &path) {
    std::string n = path_name(path);
    size_t d = n.find_last_of('.');
    return d == std::string::npos ? "" : lower(n.substr(d + 1));
}

std::string path_resolve(const std::string &base_dir, const std::string &ref) {
    std::string r = ref;
    std::replace(r.begin(), r.end(), '\\', '/');
    bool absolute = !r.empty() && r[0] == '/';
    std::vector<std::string> parts = absolute ? std::vector<std::string>() : path_split(base_dir);
    bool lead = absolute || (!base_dir.empty() && base_dir[0] == '/');
    for (auto &p : path_split(r)) {
        if (p == ".") continue;
        if (p == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.push_back(p);
    }
    std::string out = lead ? "/" : "";
    for (size_t i = 0; i < parts.size(); i++) out += (i ? "/" : "") + parts[i];
    return out;
}

bool find_ci(Volume &vol, const std::string &path, std::string &found) {
    if (vol.exists(path)) { found = path; return true; }
    // walk component by component, matching each case-insensitively
    std::vector<std::string> parts = path_split(path);
    std::string cur = !path.empty() && path[0] == '/' ? "/" : "";
    for (auto &p : parts) {
        std::string next;
        bool ok = false;
        for (auto &e : vol.list(cur == "/" ? std::string() : cur)) {
            if (equals_ci(e.name, p)) { next = path_join(cur == "/" ? std::string() : cur, e.name); ok = true; break; }
        }
        if (!ok) return false;
        cur = next;
    }
    found = cur;
    return true;
}

}  // namespace omni

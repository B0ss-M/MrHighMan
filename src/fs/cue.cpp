// Omni Sampler: CUE sheets (.cue + its .bin files), the usual way CD rips are kept. Tapping the .cue opens the disc:
//   - one data track and nothing else: the data track's file system (ISO 9660, Akai, E-mu, Roland...) as the root,
//     the same as tapping its .bin
//   - audio tracks (sample CDs in CD audio): "Track NN.wav", 44.1 kHz 16-bit stereo, beside "Data" (or "Data N" for
//     more than one) for the data tracks
// FILE names are looked up next to the .cue, ignoring case; a single-file sheet whose FILE is missing (renamed rips:
// "SNY-ZGCEPG02.BIN" for "Zero G ... .bin") falls back to the .bin / .img of the .cue's own name.
// MOTOROLA (big-endian audio) files are byte-swapped; WAVE / MP3 FILEs are not read.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include "../core/parse.hpp"
#include "fs.hpp"

namespace omni {
namespace {

struct CueTrack {
    int number = 0;
    bool audio = false;
    int sector = 2352;                 // bytes per sector in the file
    int64_t index0 = -1, index1 = -1;  // in sectors from the start of the file
    int file = 0;
};

struct CueFile {
    std::string name;
    bool motorola = false;
};

int64_t msf(const std::string &s) {   // mm:ss:ff, 75 frames a second
    int m = 0, sec = 0, f = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d", &m, &sec, &f) != 3) throw ParseError("bad CUE time: " + s);
    return (int64_t(m) * 60 + sec) * 75 + f;
}

// the next word, or a "quoted name"
std::string word(std::istringstream &in) {
    in >> std::ws;
    if (in.peek() == '"') {
        in.get();
        std::string s;
        std::getline(in, s, '"');
        return s;
    }
    std::string s;
    in >> s;
    return s;
}

void parse_cue(const std::string &text, std::vector<CueFile> &files, std::vector<CueTrack> &tracks) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream in(line);
        std::string cmd = lower(word(in));
        if (cmd == "file") {
            CueFile f;
            f.name = word(in);
            std::string type = lower(word(in));
            if (type == "wave" || type == "mp3" || type == "aiff") throw ParseError("CUE sheets of " + type + " files are not read: open the files themselves");
            f.motorola = type == "motorola";
            files.push_back(f);
        } else if (cmd == "track") {
            if (files.empty()) throw ParseError("CUE TRACK before any FILE");
            CueTrack t;
            t.number = std::atoi(word(in).c_str());
            std::string mode = lower(word(in));
            t.audio = mode == "audio";
            size_t slash = mode.find('/');
            if (slash != std::string::npos) t.sector = std::atoi(mode.c_str() + slash + 1);
            else if (mode == "cdg") t.sector = 2448;
            if (t.sector != 2048 && t.sector != 2336 && t.sector != 2352 && t.sector != 2448) throw ParseError("CUE track mode not read: " + mode);
            t.file = int(files.size()) - 1;
            tracks.push_back(t);
        } else if (cmd == "index" && !tracks.empty()) {
            int n = std::atoi(word(in).c_str());
            int64_t at = msf(word(in));
            if (n == 0) tracks.back().index0 = at;
            if (n == 1) tracks.back().index1 = at;
        }
    }
    if (tracks.empty()) throw ParseError("CUE sheet without tracks");
}

bool probe_cue(const std::string &lower_name, const uint8_t *h, size_t n, uint64_t size) {
    if (lower_name.size() < 4 || lower_name.compare(lower_name.size() - 4, 4, ".cue") || size > (1 << 20)) return false;
    std::string t = lower(std::string(reinterpret_cast<const char *>(h), std::min<size_t>(n, 4096)));
    return t.find("file") != std::string::npos && t.find("track") != std::string::npos;
}

// A WAV header in front of a track's samples (16-bit stereo 44.1 kHz, swapped when the file is big-endian)
class TrackWav : public Blob {
public:
    TrackWav(BlobPtr pcm, bool swap) : pcm_(std::move(pcm)), swap_(swap) {
        uint32_t data = uint32_t(std::min<uint64_t>(pcm_->size(), 0xFFFFFFF0u - 44) & ~uint64_t(3));
        auto le = [&](uint32_t v, int n) { for (int i = 0; i < n; i++) head_.push_back(uint8_t(v >> (8 * i))); };
        auto tag = [&](const char *s) { head_.insert(head_.end(), s, s + 4); };
        tag("RIFF"); le(36 + data, 4); tag("WAVE");
        tag("fmt "); le(16, 4); le(1, 2); le(2, 2); le(44100, 4); le(44100 * 4, 4); le(4, 2); le(16, 2);
        tag("data"); le(data, 4);
        size_ = 44 + data;
    }
    uint64_t size() const override { return size_; }
    void read(uint64_t offset, void *dst, size_t n) const override {
        if (offset + n > size_) throw ParseError("read past the end of a CD track");
        uint8_t *out = static_cast<uint8_t *>(dst);
        while (n && offset < 44) { *out++ = head_[size_t(offset++)]; n--; }
        if (!n) return;
        uint64_t at = offset - 44;
        if (!swap_) { pcm_->read(at, out, n); return; }
        // byte-swap 16-bit words: read whole words around the range
        uint64_t a = at & ~uint64_t(1), b = std::min<uint64_t>((at + n + 1) & ~uint64_t(1), pcm_->size());
        std::vector<uint8_t> tmp(size_t(b - a));
        pcm_->read(a, tmp.data(), tmp.size());
        for (size_t i = 0; i + 1 < tmp.size(); i += 2) std::swap(tmp[i], tmp[i + 1]);
        std::memcpy(out, tmp.data() + (at - a), n);
    }

private:
    BlobPtr pcm_;
    bool swap_;
    std::vector<uint8_t> head_;
    uint64_t size_ = 0;
};

// The disc: data tracks as folders (their own volumes), audio tracks as WAV files
class CueVolume : public Volume {
public:
    std::string kind() const override { return "CD image (CUE)"; }
    std::vector<DirEntry> list(const std::string &dir) override {
        if (dir.empty()) return root_;
        std::string rest;
        VolumePtr v = sub(dir, rest);
        if (!v) return {};
        return v->list(rest);
    }
    BlobPtr open(const std::string &path) override {
        auto it = wavs_.find(path);
        if (it != wavs_.end()) return it->second;
        std::string rest;
        VolumePtr v = sub(path, rest);
        if (!v) throw ParseError("not found on the CD: " + path);
        return v->open(rest);
    }
    bool is_dir(const std::string &path) override {
        if (path.empty() || data_.count(path)) return true;
        std::string rest;
        VolumePtr v = sub(path, rest);
        return v && v->is_dir(rest);
    }
    bool exists(const std::string &path) override {
        if (is_dir(path) || wavs_.count(path)) return true;
        std::string rest;
        VolumePtr v = sub(path, rest);
        return v && v->exists(rest);
    }

    std::vector<DirEntry> root_;
    std::map<std::string, VolumePtr> data_;   // "Data" -> the data track's file system
    std::map<std::string, BlobPtr> wavs_;     // "Track 02.wav" -> its audio

private:
    VolumePtr sub(const std::string &path, std::string &rest) {
        size_t slash = path.find('/');
        auto it = data_.find(path.substr(0, slash));
        if (it == data_.end()) return nullptr;
        rest = slash == std::string::npos ? std::string() : path.substr(slash + 1);
        return it->second;
    }
};

VolumePtr mount_cue_blob(BlobPtr) { throw ParseError("a CUE sheet opens from its folder (it needs its .bin)"); }

VolumePtr mount_cue(VolumePtr parent, const std::string &inner) {
    std::vector<uint8_t> d = parent->open(inner)->all(size_t(1) << 20);
    std::vector<CueFile> files;
    std::vector<CueTrack> tracks;
    parse_cue(std::string(d.begin(), d.end()), files, tracks);
    std::string dir = path_dir(inner), stem = path_stem(inner);
    // each FILE's blob
    std::vector<BlobPtr> blobs;
    std::vector<std::string> names;
    for (auto &f : files) {
        std::string found;
        std::string want = path_join(dir, path_name(path_resolve("", f.name)));
        if (!find_ci(*parent, want, found) && files.size() == 1)
            for (const char *ext : {".bin", ".img", ".raw"})
                if (find_ci(*parent, path_join(dir, stem + ext), found)) break;
        if (found.empty() || parent->is_dir(found)) throw ParseError("the CUE sheet's file is missing: " + f.name);
        blobs.push_back(parent->open(found));
        names.push_back(path_name(found));
    }
    // each track runs to the next one in its file (its pregap, INDEX 00, belongs to the track before), or the file's end
    struct Span { const CueTrack *t; uint64_t start, len; };
    std::vector<Span> spans;
    for (size_t i = 0; i < tracks.size(); i++) {
        const CueTrack &t = tracks[i];
        if (t.index1 < 0) continue;
        uint64_t fsize = blobs[size_t(t.file)]->size();
        uint64_t start = uint64_t(t.index1) * uint64_t(t.sector), end = fsize;
        if (i + 1 < tracks.size() && tracks[i + 1].file == t.file) {
            const CueTrack &n = tracks[i + 1];
            int64_t next = n.index0 >= 0 ? n.index0 : n.index1;
            if (next >= 0) end = uint64_t(next) * uint64_t(n.sector);
        }
        if (start >= std::min(end, fsize)) continue;
        spans.push_back({&t, start, std::min(end, fsize) - start});
    }
    int n_data = 0, n_audio = 0;
    for (auto &s : spans) (s.t->audio ? n_audio : n_data)++;
    auto mount_data = [&](const Span &s) -> VolumePtr {
        BlobPtr b = make_slice(blobs[size_t(s.t->file)], s.start, s.len);
        if (s.t->sector == 2336) throw ParseError("MODE2/2336 data tracks are not read");
        const ImageType *t = probe_image(names[size_t(s.t->file)], *b);
        if (!t || !t->mount) return nullptr;
        return t->mount(b);
    };
    // the common case: a data CD, opened as its file system
    if (n_data == 1 && n_audio == 0) {
        VolumePtr v = mount_data(spans[0]);
        if (!v) throw ParseError("the CD's data track is not a disk this sampler reads");
        return v;
    }
    auto vol = std::make_shared<CueVolume>();
    int k = 0;
    for (auto &s : spans) {
        char name[32];
        if (s.t->audio) {
            if (s.t->sector != 2352) continue;
            std::snprintf(name, sizeof name, "Track %02d.wav", s.t->number);
            BlobPtr w = std::make_shared<TrackWav>(make_slice(blobs[size_t(s.t->file)], s.start, s.len), files[size_t(s.t->file)].motorola);
            vol->wavs_[name] = w;
            vol->root_.push_back(DirEntry{name, false, w->size()});
        } else {
            VolumePtr v;
            try { v = mount_data(s); } catch (const std::exception &) {}
            if (!v) continue;
            if (n_data > 1) std::snprintf(name, sizeof name, "Data %d", ++k);
            else std::snprintf(name, sizeof name, "Data");
            vol->data_[name] = v;
            vol->root_.push_back(DirEntry{name, true, 0});
        }
    }
    if (vol->root_.empty()) throw ParseError("nothing on this CD that the sampler reads");
    return vol;
}

}  // namespace

void register_cue() {
    register_image_type({"CD image (CUE)", probe_cue, mount_cue_blob, mount_cue, nullptr});
}

}  // namespace omni

// Omni Sampler: floppy disk containers. An HFE (HxC floppy emulator) or IMD (ImageDisk) file opens as a folder with
// one raw sector image inside, which the other image types and readers then take: Akai S900/S1000 floppies, Emax
// banks, MPC2000/MPC60 FAT disks, and E-mu Emulator I/II disks (.ei / .eii). HFE track decoding (IBM MFM, IBM FM,
// E-mu FM) is translated from ConvertWithMoss's file/hfe package (LGPL-3.0); the E-mu FM format was documented by
// ///Esynthesist. IMD follows ConvertWithMoss's ImdFile.
#include <algorithm>
#include <cstring>
#include <map>
#include "fs.hpp"

namespace omni {
namespace {

struct Sector { int cyl, head, num, size; std::vector<uint8_t> data; bool crc_ok; };

int crc_ccitt(const uint8_t *d, size_t n, int crc = 0xFFFF) {
    for (size_t k = 0; k < n; k++) {
        crc ^= d[k] << 8;
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc & 0xFFFF;
}
int crc_lsb(const uint8_t *d, size_t n) {
    int crc = 0;
    for (size_t k = 0; k < n; k++) {
        crc ^= d[k];
        for (int i = 0; i < 8; i++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc & 0xFFFF;
}

enum Mode { MSB, LSB, SWAP_MSB, SWAP_LSB };

struct Bits {
    const std::vector<uint8_t> &d;
    Mode mode;
    size_t pos = 0;
    Bits(const std::vector<uint8_t> &data, Mode m) : d(data), mode(m) {}
    size_t total() const { return d.size() * 8; }
    bool remaining() const { return pos + 32 < total(); }
    int bit() {
        if (pos >= total()) return 0;
        size_t byte = pos / 8;
        int b;
        switch (mode) {
        case LSB: b = int(pos % 8); break;
        case SWAP_MSB: byte = (byte & ~size_t(1)) | (1 - (byte & 1)); b = 7 - int(pos % 8); break;
        case SWAP_LSB: byte = (byte & ~size_t(1)) | (1 - (byte & 1)); b = int(pos % 8); break;
        default: b = 7 - int(pos % 8); break;
        }
        pos++;
        return byte < d.size() ? (d[byte] >> b) & 1 : 0;
    }
    int peek16() { size_t s = pos; int w = 0; for (int i = 0; i < 16; i++) w = w << 1 | bit(); pos = s; return w; }
    int mfm_byte() { int r = 0; for (int i = 0; i < 8; i++) { bit(); r = r << 1 | bit(); } return r & 0xFF; }
    // FM: cells possibly doubled in HFE
    int lbit(int cw, int phase) { if (cw == 1) return bit(); int a = bit(), b = bit(); return phase == 0 ? a : b; }
    int lpeek16(int cw, int phase) { size_t s = pos; int w = 0; for (int i = 0; i < 16; i++) w = w << 1 | lbit(cw, phase); pos = s; return w; }
    int lbyte(int cw, int phase) { int r = 0; for (int i = 0; i < 8; i++) { lbit(cw, phase); r = r << 1 | lbit(cw, phase); } return r & 0xFF; }
};

// --- IBM MFM ---------------------------------------------------------------------------------------------------
bool mfm_sync(Bits &b) {
    int count = 0, searched = 0;
    while (b.remaining() && searched < 100000) {
        if (b.peek16() == 0x4489) { b.pos += 16; if (++count >= 3) return true; }
        else { count = 0; b.pos++; searched++; }
    }
    return false;
}

std::vector<Sector> mfm_decode(const std::vector<uint8_t> &track) {
    for (Mode m : {LSB, MSB, SWAP_MSB, SWAP_LSB}) {
        std::vector<Sector> out;
        Bits b(track, m);
        while (b.pos + 100 < b.total()) {
            if (!mfm_sync(b)) break;
            if (b.mfm_byte() != 0xFE) continue;
            if (!b.remaining()) break;
            uint8_t h[8] = {0xA1, 0xA1, 0xA1, 0xFE, 0, 0, 0, 0};
            for (int i = 4; i < 8; i++) h[i] = uint8_t(b.mfm_byte());
            int c1 = b.mfm_byte(), c2 = b.mfm_byte();
            if (crc_ccitt(h, 8) != (c1 << 8 | c2) || h[7] > 6) continue;
            Sector s{h[4], h[5], h[6], 128 << h[7], {}, true};
            // data mark
            size_t start = b.pos;
            bool found = false;
            while (b.remaining() && b.pos - start < 50000) {
                if (!mfm_sync(b)) break;
                size_t save = b.pos;
                int mark = b.mfm_byte();
                if (mark == 0xFB || mark == 0xF8) {
                    s.data.resize(size_t(s.size));
                    for (int i = 0; i < s.size; i++) s.data[size_t(i)] = uint8_t(b.mfm_byte());
                    std::vector<uint8_t> all = {0xA1, 0xA1, 0xA1, uint8_t(mark)};
                    all.insert(all.end(), s.data.begin(), s.data.end());
                    int d1 = b.mfm_byte(), d2 = b.mfm_byte();
                    s.crc_ok = crc_ccitt(all.data(), all.size()) == (d1 << 8 | d2);
                    found = true;
                    break;
                }
                b.pos = save;
            }
            if (found) out.push_back(std::move(s));
        }
        if (!out.empty()) return out;
    }
    return {};
}

// --- IBM FM ----------------------------------------------------------------------------------------------------
std::vector<Sector> fm_decode(const std::vector<uint8_t> &track, int physical_head) {
    auto attempt = [&](Mode m, int cw, int phase) {
        std::vector<Sector> out;
        Bits b(track, m);
        while (b.pos + 200 < b.total()) {
            int searched = 0;
            bool sync = false;
            while (b.remaining() && searched < 400000) {
                if (b.lpeek16(cw, phase) == 0xF57E) { b.pos += size_t(16 * cw); sync = true; break; }
                b.pos++;
                searched++;
            }
            if (!sync) break;
            uint8_t h[5] = {0xFE, 0, 0, 0, 0};
            for (int i = 1; i < 5; i++) h[i] = uint8_t(b.lbyte(cw, phase));
            int c1 = b.lbyte(cw, phase), c2 = b.lbyte(cw, phase);
            if (crc_ccitt(h, 5) != (c1 << 8 | c2) || h[4] > 6) continue;
            Sector s{h[1], physical_head, h[3], 128 << h[4], {}, true};
            size_t start = b.pos;
            int mark = -1;
            while (b.remaining() && b.pos - start < size_t(20000 * cw)) {
                int w = b.lpeek16(cw, phase);
                if (w == 0xF56F) { mark = 0xFB; break; }
                if (w == 0xF56A) { mark = 0xF8; break; }
                b.pos++;
            }
            if (mark < 0) continue;
            b.pos += size_t(16 * cw);
            s.data.resize(size_t(s.size));
            for (int i = 0; i < s.size; i++) s.data[size_t(i)] = uint8_t(b.lbyte(cw, phase));
            std::vector<uint8_t> all = {uint8_t(mark)};
            all.insert(all.end(), s.data.begin(), s.data.end());
            int d1 = b.lbyte(cw, phase), d2 = b.lbyte(cw, phase);
            s.crc_ok = crc_ccitt(all.data(), all.size()) == (d1 << 8 | d2);
            out.push_back(std::move(s));
        }
        return out;
    };
    for (Mode m : {LSB, MSB, SWAP_MSB, SWAP_LSB})
        for (int phase : {0, 1}) { auto v = attempt(m, 2, phase); if (!v.empty()) return v; }
    for (Mode m : {LSB, MSB, SWAP_MSB, SWAP_LSB}) { auto v = attempt(m, 1, 0); if (!v.empty()) return v; }
    return {};
}

// --- E-mu FM (Emulator I / II): one 3584-byte sector per track ---------------------------------------------------
constexpr int EMU_SECTOR = 3584, SLOTS_CELL = 4, SLOTS_BYTE = 32, MARK_SLOTS = 64;

uint64_t emu_mark_pattern() {
    uint64_t p = 0;
    for (int mb : {0xFA, 0x96})
        for (int bit = 0; bit < 8; bit++)
            for (int slot = 0; slot < SLOTS_CELL; slot++) {
                bool pulse = slot == 1 || (slot == 3 && ((mb >> bit) & 1));
                p = p << 1 | (pulse ? 1 : 0);
            }
    return p;
}

std::vector<Sector> emu_fm_decode(const std::vector<uint8_t> &track, int cyl, int head) {
    std::vector<uint8_t> pulses(track.size() * 8);
    for (size_t i = 0; i < track.size(); i++)
        for (int bit = 0; bit < 8; bit++) pulses[i * 8 + size_t(bit)] = (track[i] >> bit) & 1;
    static const uint64_t PATTERN = emu_mark_pattern();
    auto find = [&](size_t from) -> long {
        uint64_t w = 0;
        for (size_t s = from; s < pulses.size(); s++) {
            w = w << 1 | pulses[s];
            if (w == PATTERN && s + 1 >= from + MARK_SLOTS) return long(s + 1 - MARK_SLOTS);
        }
        return -1;
    };
    auto read = [&](size_t from, int count) {
        std::vector<uint8_t> out(static_cast<size_t>(count));
        size_t slot = from + 3;
        for (int i = 0; i < count; i++) {
            int v = 0;
            for (int bit = 0; bit < 8; bit++) { if (slot < pulses.size() && pulses[slot]) v |= 1 << bit; slot += SLOTS_CELL; }
            out[size_t(i)] = uint8_t(v);
        }
        return out;
    };
    long hm = find(0);
    if (hm < 0) return {};
    size_t hs = size_t(hm) + MARK_SLOTS;
    if (hs + 3 * SLOTS_BYTE > pulses.size()) return {};
    std::vector<uint8_t> h = read(hs, 3);
    bool ok = crc_lsb(&h[0], 1) == (h[2] << 8 | h[1]);
    long dm = find(hs + 3 * SLOTS_BYTE);
    if (dm < 0) return {};
    size_t ds = size_t(dm) + MARK_SLOTS;
    if (ds + size_t(EMU_SECTOR) * SLOTS_BYTE > pulses.size()) return {};
    std::vector<uint8_t> data = read(ds, EMU_SECTOR);
    size_t cs = ds + size_t(EMU_SECTOR) * SLOTS_BYTE;
    if (cs + 2 * SLOTS_BYTE <= pulses.size()) {
        std::vector<uint8_t> c = read(cs, 2);
        if (crc_lsb(data.data(), data.size()) != (c[1] << 8 | c[0])) ok = false;
    } else ok = false;
    return {Sector{cyl, head, 0, EMU_SECTOR, data, ok}};
}

// --- HFE container ---------------------------------------------------------------------------------------------
struct Decoded { std::vector<uint8_t> image; std::string name; };

Decoded decode_hfe(const Blob &blob) {
    std::vector<uint8_t> h = blob.bytes(0, 512);
    if (std::memcmp(h.data(), "HXCPICFE", 8) && std::memcmp(h.data(), "HXCHFEV3", 8)) throw ParseError("not an HFE file");
    if (!std::memcmp(h.data(), "HXCHFEV3", 8)) throw ParseError("HFE v3 files are not supported");
    int tracks = h[9], sides = h[10], encoding = h[11];
    uint64_t lut = uint64_t(le16(&h[18])) * 512;
    if (!tracks || !sides || sides > 2) throw ParseError("bad HFE geometry");
    std::vector<uint8_t> table = blob.bytes(lut, size_t(tracks) * 4);
    std::vector<Sector> sectors;
    for (int t = 0; t < tracks; t++) {
        uint64_t off = uint64_t(le16(&table[size_t(t) * 4])) * 512;
        size_t len = le16(&table[size_t(t) * 4 + 2]);
        if (!len || off + len > blob.size()) continue;
        std::vector<uint8_t> raw = blob.bytes(off, len);
        for (int side = 0; side < sides; side++) {
            std::vector<uint8_t> sd;
            for (size_t blk = 0; blk * 512 < raw.size(); blk++) {
                size_t from = blk * 512 + size_t(side) * 256;
                for (size_t i = 0; i < 256 && from + i < raw.size() && (blk * 512 + 512 <= raw.size() || from + i < raw.size()); i++) sd.push_back(raw[from + i]);
            }
            std::vector<Sector> got;
            if (encoding == 3) got = emu_fm_decode(sd, t, side);
            else if (encoding == 2) got = fm_decode(sd, side);
            else if (encoding == 0 || encoding == 1) got = mfm_decode(sd);
            else throw ParseError("unsupported HFE track encoding " + std::to_string(encoding));
            for (auto &s : got) { if (encoding != 3) s.head = side; sectors.push_back(std::move(s)); }
        }
    }
    if (sectors.empty()) throw ParseError("no readable sectors on this floppy image");
    Decoded out;
    if (encoding == 3) {
        // E-mu: track number = cylinder * sides + side, one sector each
        out.image.assign(size_t(tracks) * sides * EMU_SECTOR, 0);
        for (auto &s : sectors) {
            size_t lba = size_t(s.cyl) * sides + size_t(s.head);
            if ((lba + 1) * EMU_SECTOR <= out.image.size()) std::copy(s.data.begin(), s.data.end(), out.image.begin() + long(lba * EMU_SECTOR));
        }
        out.name = sides == 1 ? "disk.ei" : "disk.eii";
        return out;
    }
    std::map<int, int> sizes;
    int spt = 0;
    for (auto &s : sectors) { sizes[s.size]++; spt = std::max(spt, s.num); }
    int bps = std::max_element(sizes.begin(), sizes.end(), [](auto &a, auto &b) { return a.second < b.second; })->first;
    out.image.assign(size_t(tracks) * sides * spt * bps, 0xF6);
    for (auto &s : sectors) {
        if (s.size != bps || s.num < 1 || s.cyl >= tracks) continue;
        size_t lba = (size_t(s.cyl) * sides + size_t(s.head)) * spt + size_t(s.num - 1);
        if ((lba + 1) * size_t(bps) <= out.image.size()) std::copy(s.data.begin(), s.data.end(), out.image.begin() + long(lba * bps));
    }
    out.name = "disk.img";
    return out;
}

VolumePtr mount_hfe(BlobPtr blob) {
    Decoded d = decode_hfe(*blob);
    auto vol = std::make_shared<TreeVolume>("Floppy (HFE)");
    vol->add_file(d.name, make_mem_blob(std::move(d.image)));
    return vol;
}

bool probe_hfe(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    return n >= 8 && (!std::memcmp(h, "HXCPICFE", 8) || !std::memcmp(h, "HXCHFEV3", 8));
}

// --- IMD (ImageDisk) --------------------------------------------------------------------------------------------
VolumePtr mount_imd(BlobPtr blob) {
    std::vector<uint8_t> d = blob->all(64 << 20);
    Reader r(d);
    while (!r.eof() && r.u8() != 0x1A) {}
    std::map<long, std::vector<uint8_t>> tracks;
    int max_cyl = -1, max_head = -1, common_spt = -1, common_size = -1;
    while (!r.eof()) {
        r.u8();   // mode
        int cyl = r.u8(), hb = r.u8(), ns = r.u8(), sc = r.u8();
        if (sc > 6) throw ParseError("unsupported IMD sector size");
        int size = 128 << sc, head = hb & 0x3F;
        std::vector<int> map(static_cast<size_t>(ns));
        for (auto &m : map) m = r.u8();
        if (hb & 0x80) r.skip(size_t(ns));
        if (hb & 0x40) r.skip(size_t(ns));
        std::vector<std::vector<uint8_t>> secs(size_t(ns), std::vector<uint8_t>(size_t(size), 0));
        for (int i = 0; i < ns; i++) {
            int type = r.u8();
            if (type == 0) continue;
            if (type & 1) { const uint8_t *p = r.take(size_t(size)); std::copy(p, p + size, secs[size_t(i)].begin()); }
            else if (type <= 8) std::fill(secs[size_t(i)].begin(), secs[size_t(i)].end(), r.u8());
            else throw ParseError("bad IMD sector type");
        }
        std::vector<int> order(static_cast<size_t>(ns));
        for (int i = 0; i < ns; i++) order[size_t(i)] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return map[size_t(a)] < map[size_t(b)]; });
        std::vector<uint8_t> tb;
        for (int i : order) tb.insert(tb.end(), secs[size_t(i)].begin(), secs[size_t(i)].end());
        tracks[long(cyl) << 8 | head] = tb;
        max_cyl = std::max(max_cyl, cyl);
        max_head = std::max(max_head, head);
        if (common_spt < 0) { common_spt = ns; common_size = size; }
    }
    std::vector<uint8_t> image;
    for (int c = 0; c <= max_cyl; c++)
        for (int hd = 0; hd <= max_head; hd++) {
            auto it = tracks.find(long(c) << 8 | hd);
            if (it != tracks.end()) image.insert(image.end(), it->second.begin(), it->second.end());
            else image.insert(image.end(), size_t(common_spt * common_size), 0);
        }
    auto vol = std::make_shared<TreeVolume>("Floppy (IMD)");
    vol->add_file("disk.img", make_mem_blob(std::move(image)));
    return vol;
}

bool probe_imd(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 4 && !std::memcmp(h, "IMD ", 4); }

}  // namespace

void register_floppy_containers() {
    register_image_type({"HFE floppy", probe_hfe, mount_hfe});
    register_image_type({"IMD floppy", probe_imd, mount_imd});
}

}  // namespace omni

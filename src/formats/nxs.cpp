// Omni Sampler: reFX Nexus multisamples (.nxs). Measured 2026-11-06 against the multisample loader of Nexus 5.3.10
// (arm64) and on the 6,797 .nxs files of a Nexus 4 soundbank plus BellCrazy, 70s StringsT and Flute: 5,653 read with
// every table fitting the file; the other 1,144 are the newer NEXUS4.5/5.2 sample formats. The measured KLP1 layout has a separate reader.
// Facts only:
//   - Header forms, all little-endian:
//       A "plain":    zone records from offset 0, no count; records until one is not a zone; the audio fills the end
//                     of the file (BellCrazy: 17 records, zeros, audio from 0x24C).
//       B "counted":  u32 zone count 1..128, records, audio (70s StringsT).
//       D "hashed":   u32 zone count XOR H(0x3141f32d, wide), scrambled records, audio (305 files).
//       C "NEXUS2.5": "NEXUS2.5 Sample\0", u32 variation count 1..127, then per variation: u32 zone count (XOR
//                     H(0x3141f32d, wide) when over 0x80, as stored in every file seen), scrambled records, then that
//                     variation's audio. Variations are round robins (276 files have more than one).
//       "NEXUS4.5 Sample": the native KLP1 reader handles the measured 16-pair layout.
//       "NEXUS5.2 Sample": unsupported.
//     Some C files hold a whole second copy of themselves after the first (18 seen); like Nexus, reading stops after
//     the last variation.
//   - Zone record, 7 x u32 (measured): [0] sample rate  [1] frames  [2] loop start (frame)
//       [3] bytes: root, key low, key high, flags (bit 1 = stereo: a second block follows; bit 0 = looped; other bits
//           seen (0x80), meaning unknown, ignored)  [4] size of block 1 (left / mono)  [5] size of block 2  [6] unknown.
//     A record with rate or frames 0 ends the table (the loader's rule). Key low may exceed key high (Nexus does not
//     check; finish_instrument orders them). Root 255 is seen on 53 of 96,446 zones (a first zone from key 0): its
//     meaning is not known; the zone's key high is used. The loop runs from [2] to the last frame, inclusive: the
//     frames before [2] equal the last frames within 1 LSB in every looped zone checked (52), a seamless wrap.
//   - Scramble (from the loader): name = file name without ".nxs", a..z upper-cased; H(seed) adds c | c<<7 | c<<15
//     (| c<<25 "wide") per character. Records: a = (H(0x18755432) & 0x3fffe) | 1, c = (H(0x63139162) & 0x3ffffe) | 1,
//     x = H(0x31415927, wide), restarted for each table; per u32: x = a*x + c, word ^= x. The key is the file name:
//     a renamed C or D file does not decode (load says so). No secret key is involved.
//   - Audio block: byte 0 = flags (bit 0 = a 24-bit form; not seen in any readable file, so not read), then a bit
//     stream read LSB first. Each residual is an adaptive prefix code (decode_residual, transcribed from the loader;
//     its 256-entry table is generated in code_table() and was checked equal to Nexus's own). Samples: d += r; s += d
//     (16-bit wrap). The loader gives the decoder the block size + 1 bytes (the next file byte, 0 past the end).
// Not read: envelopes, filters, effects and velocity (they live in Nexus presets, not in .nxs); word [6].
// Also here: Nexus 2.3-2.7 presets (.fxp), their sample layers built from these readers (see "presets" below).
#include <algorithm>
#include <cstring>
#include <map>
#include "../core/bytes.hpp"
#include "format.hpp"

namespace omni {
namespace {

constexpr size_t REC = 28;
constexpr uint32_t MAX_COUNT = 128;        // zones per table (the loader's limit)
constexpr uint32_t MAX_ZONES = 4096;       // all variations together
constexpr uint32_t MAX_FRAMES = 0xd00000;  // the loader's own limit
const char MAGIC25[16] = {'N', 'E', 'X', 'U', 'S', '2', '.', '5', ' ', 'S', 'a', 'm', 'p', 'l', 'e', 0};
const char MAGIC45[16] = {'N', 'E', 'X', 'U', 'S', '4', '.', '5', ' ', 'S', 'a', 'm', 'p', 'l', 'e', 0};
const char MAGIC52[16] = {'N', 'E', 'X', 'U', 'S', '5', '.', '2', ' ', 'S', 'a', 'm', 'p', 'l', 'e', 0};

struct Rec {
    uint32_t rate = 0, frames = 0, loop = 0;
    int root = 60, lo = 0, hi = 127, flags = 0;
    uint32_t size1 = 0, size2 = 0;
    uint64_t off = 0;          // first block in the file
    int variation = 0;
    bool stereo() const { return flags & 2; }
    bool looped() const { return (flags & 1) && loop < frames; }
    uint64_t bytes() const { return uint64_t(size1) + (stereo() ? size2 : 0); }
};

Rec to_rec(const uint32_t *w) {
    Rec r;
    r.rate = w[0];
    r.frames = w[1];
    r.loop = w[2];
    r.root = int(w[3] & 0xff);
    r.lo = int(w[3] >> 8 & 0xff);
    r.hi = int(w[3] >> 16 & 0xff);
    r.flags = int(w[3] >> 24);
    r.size1 = w[4];
    r.size2 = w[5];
    return r;
}

bool end_of_table(const uint32_t *w) { return w[0] == 0 || w[1] == 0; }

// What the loader's own check accepts, sizes bounded so a corrupt record cannot ask for gigabytes.
bool valid(const Rec &r) {
    if (r.rate < 8000 || r.rate > 192000 || r.frames < 4 || r.frames > MAX_FRAMES) return false;
    uint64_t cap = uint64_t(r.frames) * 8 + 64;
    return r.size1 != 0 && r.size1 <= cap && (!r.stereo() || (r.size2 != 0 && r.size2 <= cap));
}

void words(const uint8_t *p, uint32_t *w, size_t n = 7) {
    for (size_t i = 0; i < n; i++) w[i] = le32(p + 4 * i);
}

// ---- name-derived scramble ----
uint32_t name_hash(const std::string &s, uint32_t seed, bool wide) {
    uint32_t h = seed;
    for (unsigned char b : s) {
        int32_t c = int8_t(b);                       // the loader works on signed chars
        if (b >= 'a' && b <= 'z') c -= 0x20;
        uint32_t u = uint32_t(c);
        h += u | u << 7 | u << 15 | (wide ? u << 25 : 0);
    }
    return h;
}

struct Key {
    uint32_t count, x, a, c;
};

Key make_key(const std::string &path) {
    std::string s = path_stem(path_name(path));
    Key k;
    k.count = name_hash(s, 0x3141f32d, true);
    k.x = name_hash(s, 0x31415927, true);
    k.a = (name_hash(s, 0x18755432, false) & 0x3fffe) | 1;
    k.c = (name_hash(s, 0x63139162, false) & 0x3ffffe) | 1;
    return k;
}

const char *RENAMED = "Nexus .nxs: header does not decode (the file must keep its original name)";

// One table of `count` records at `at`; descrambled when `key` is given. Records up to the first end-of-table one.
std::vector<Rec> read_table(const Blob &f, uint64_t at, uint32_t count, const Key *key) {
    if (at + uint64_t(count) * REC > f.size()) throw ParseError("Nexus .nxs: header cut short");
    std::vector<uint8_t> t = f.bytes(at, size_t(count) * REC);
    std::vector<Rec> out;
    uint32_t x = key ? key->x : 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t w[7];
        words(&t[size_t(i) * REC], w);
        if (key)
            for (uint32_t &v : w) {
                x = x * key->a + key->c;
                v ^= x;
            }
        if (end_of_table(w)) break;
        Rec r = to_rec(w);
        if (!valid(r)) throw ParseError(key ? RENAMED : "Nexus .nxs: bad zone record " + std::to_string(i));
        out.push_back(r);
    }
    return out;
}

struct Header {
    char form = 0;                 // 'A', 'B', 'C' or 'D'
    int variations = 1;
    std::vector<Rec> zones;        // all variations, each zone with its block offset
};

// Lays out the audio of `zones` from `at`, returns the offset after it; throws when it does not fit.
uint64_t place(std::vector<Rec> &zones, uint64_t at, uint64_t size) {
    for (Rec &r : zones) {
        r.off = at;
        at += r.bytes();
        if (at > size) throw ParseError("Nexus .nxs: audio sizes do not fit the file");
    }
    return at;
}

Header read_header(const Blob &f, const std::string &path) {
    const uint64_t size = f.size();
    std::vector<uint8_t> h = f.head(4 + REC);   // magic + variations, or a count and the first record
    Header out;
    if (h.size() >= 16 && (!std::memcmp(h.data(), MAGIC45, 16) || !std::memcmp(h.data(), MAGIC52, 16)))
        throw ParseError("Nexus .nxs: Nexus 4.5/5 sample format (not supported)");
    Key key = make_key(path);
    if (h.size() >= 24 && !std::memcmp(h.data(), MAGIC25, 16)) {
        out.form = 'C';
        uint32_t nvar = le32(&h[16]);
        if (nvar < 1 || nvar > 127) throw ParseError("Nexus .nxs: unknown variation count " + std::to_string(nvar));
        out.variations = int(nvar);
        uint64_t at = 20;
        for (uint32_t v = 0; v < nvar; v++) {
            std::vector<uint8_t> c = f.head(4, at);
            if (c.size() < 4) throw ParseError("Nexus .nxs: header cut short");
            uint32_t raw = le32(c.data());
            uint32_t count = raw > 0x80 ? raw ^ key.count : raw;
            if (count < 1 || count > MAX_COUNT) throw ParseError(RENAMED);
            std::vector<Rec> t = read_table(f, at + 4, count, &key);
            at = place(t, at + 4 + uint64_t(count) * REC, size);
            for (Rec &r : t) r.variation = int(v);
            out.zones.insert(out.zones.end(), t.begin(), t.end());
            if (out.zones.size() > MAX_ZONES) throw ParseError("Nexus .nxs: too many zones");
        }
    } else if (h.size() >= 4 + REC && le32(&h[0]) >= 1 && le32(&h[0]) <= MAX_COUNT) {
        out.form = 'B';
        uint32_t count = le32(&h[0]);
        out.zones = read_table(f, 4, count, nullptr);
        place(out.zones, 4 + uint64_t(count) * REC, size);
    } else if (h.size() >= 4 + REC && (le32(&h[0]) ^ key.count) >= 1 && (le32(&h[0]) ^ key.count) <= MAX_COUNT) {
        out.form = 'D';
        uint32_t count = le32(&h[0]) ^ key.count;
        out.zones = read_table(f, 4, count, &key);
        place(out.zones, 4 + uint64_t(count) * REC, size);
    } else {
        out.form = 'A';
        std::vector<uint8_t> t = f.head(size_t(MAX_ZONES) * REC);
        uint64_t at = 0;
        while (at + REC <= t.size() && out.zones.size() < MAX_ZONES) {
            uint32_t w[7];
            words(&t[size_t(at)], w);
            if (end_of_table(w)) break;
            Rec r = to_rec(w);
            if (!valid(r)) break;
            out.zones.push_back(r);
            at += REC;
        }
        if (out.zones.empty()) throw ParseError("Nexus .nxs: not a readable multisample");
        uint64_t total = 0;
        for (const Rec &r : out.zones) total += r.bytes();
        if (total > size || size - total < at) throw ParseError("Nexus .nxs: audio sizes do not fit the file");
        place(out.zones, size - total, size);
    }
    if (out.zones.empty()) throw ParseError("Nexus .nxs: no zones");
    return out;
}

// ---- audio ----
struct Code {
    int8_t cls, bits;
};

// The decoder's first-byte table: "00" 0, "10" +1, "01" -1 (bits as read, LSB first), else n >= 2 ones then a zero
// (n + 1 bits): -(2q+2), -(2q+3), +(q+2) for (n-2) = 3q, 3q+1, 3q+2. 0xff starts an escape (handled apart).
const Code *code_table() {
    static const std::vector<Code> t = [] {
        std::vector<Code> v(256);
        for (int e = 0; e < 256; e++) {
            if (e == 255) { v[size_t(e)] = {0, 8}; continue; }
            switch (e & 3) {
            case 0: v[size_t(e)] = {0, 2}; continue;
            case 1: v[size_t(e)] = {1, 2}; continue;
            case 2: v[size_t(e)] = {-1, 2}; continue;
            }
            int n = 0;
            while (e >> n & 1) n++;
            int q = (n - 2) / 3, r = (n - 2) % 3;
            int cls = r == 0 ? -(2 * q + 2) : r == 1 ? -(2 * q + 3) : q + 2;
            v[size_t(e)] = {int8_t(cls), int8_t(n + 1)};
        }
        return v;
    }();
    return t.data();
}

inline int32_t asr(uint32_t v, int s) { return int32_t(v) >> s; }   // arithmetic shift, as the loader's

struct Bits {
    const uint8_t *b;
    uint32_t n;                    // bytes readable (block size + 1)
    uint32_t idx = 1;              // byte 0 is the block's flags
    uint32_t bits = 0, cnt = 0;
    uint32_t k = 0;                // adaptive class offset (signed, 8.8)
    bool err = false;
};

// One residual (FUN_00a127d8 of Nexus 5.3.10). Every read is bounded by n; on running out err is set.
int32_t decode_residual(Bits &s, const Code *tab) {
    uint32_t cnt = s.cnt, v, u6, u7;
    if (cnt < 8) {
        if (s.idx >= s.n) { s.err = true; return 0; }
        s.bits |= uint32_t(s.b[s.idx++]) << (cnt & 31);
        cnt += 8;
        s.cnt = cnt;
    }
    v = s.bits;
    if ((v & 0xff) == 0xff) {      // escape: a long run of ones, counted, then mapped
        int32_t i8 = -1, i9 = -1;
        do {
            if (s.idx >= s.n) { s.err = true; return 0; }
            v = (uint32_t(s.b[s.idx++]) << ((cnt - 8) & 31)) | (v >> 8);
            i8 += 8;
        } while ((v & 0xff) == 0xff);
        for (;;) {
            bool zero = false;
            while (cnt != 0) {
                i9 = i8;
                cnt--;
                uint32_t bit = v & 1;
                v >>= 1;
                i8 = i9 + 1;
                if (!bit) { zero = true; break; }
            }
            if (zero) break;
            i9 = i8;
            if (s.idx >= s.n) { s.err = true; s.bits = v; s.cnt = 0; return 0; }
            uint8_t byte = s.b[s.idx++];
            cnt = 7;
            v = byte >> 1;
            i8 = i9 + 1;
            if (!(byte & 1)) break;
        }
        s.bits = v;
        s.cnt = cnt;
        uint32_t u5 = uint32_t(i9) + 3;
        u7 = u5 / 3;
        if (u5 != u7 * 3) u7 = (uint32_t(i9) + 2) / 3 - (uint32_t(i9) + 2);
    } else {
        const Code &c = tab[v & 0xff];
        u7 = uint32_t(int32_t(c.cls));
        s.bits = v >> (c.bits & 31);
        cnt -= uint32_t(c.bits);
        s.cnt = cnt;
    }
    u7 += uint32_t(asr(s.k, 8));
    uint32_t u5 = u7 + 1;
    s.k = s.k - uint32_t(asr(s.k + 0x10, 5)) + u5 * 8;
    if (u7 == 0xffffffffu) return 0;
    u6 = s.bits;
    cnt = s.cnt;
    while (cnt < u5) {
        if (s.idx >= s.n) { s.err = true; return 0; }
        u6 |= uint32_t(s.b[s.idx++]) << (cnt & 31);
        cnt += 8;
    }
    s.bits = u6 >> (u5 & 31);
    s.cnt = cnt - u5;
    uint32_t m = 1u << (u7 & 31);
    uint32_t r = (u6 & (m - 1)) | m;
    if (u6 & m) r = 0u - (u6 & ~(0xffffffffu << (u5 & 31)));
    return int32_t(r);
}

// One block to `frames` samples written every `step` int16s from out.
void decode_block(const std::vector<uint8_t> &blk, uint32_t frames, int16_t *out, int step) {
    if (blk.size() < 2) throw ParseError("Nexus .nxs: empty audio block");
    if (blk[0] & 1) throw ParseError("Nexus .nxs: 24-bit audio block (not supported)");
    const Code *tab = code_table();
    Bits s;
    s.b = blk.data();
    s.n = uint32_t(blk.size());
    int16_t d = 0, v = 0;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t r = decode_residual(s, tab);
        if (s.err) throw ParseError("Nexus .nxs: audio block ends early");
        d = int16_t(uint16_t(d) + uint16_t(r));
        v = int16_t(uint16_t(v) + uint16_t(d));
        out[size_t(i) * size_t(step)] = v;
    }
}

// The block at off of `size` bytes plus the next byte of the file (0 past the end), as the loader reads it.
std::vector<uint8_t> read_block(const Blob &f, uint64_t off, uint32_t size) {
    std::vector<uint8_t> b = f.bytes(off, size);
    std::vector<uint8_t> next = f.head(1, off + size);
    b.push_back(next.empty() ? 0 : next[0]);
    return b;
}

PcmPtr decode_zone(VolumePtr vol, const std::string &path, Rec r) {
    BlobPtr f = vol->open(path);
    auto pcm = std::make_shared<Pcm>();
    pcm->rate = int(r.rate);
    pcm->channels = r.stereo() ? 2 : 1;
    pcm->data.assign(size_t(r.frames) * size_t(pcm->channels), 0);
    decode_block(read_block(*f, r.off, r.size1), r.frames, pcm->data.data(), pcm->channels);
    if (r.stereo()) decode_block(read_block(*f, r.off + r.size1, r.size2), r.frames, pcm->data.data() + 1, 2);
    return pcm;
}

// ".nxs" is Nexus's alone, and a hashed count (form D) cannot be told from the first bytes without the file name, so
// the extension decides; read_header tells the forms apart and says why a file cannot be read.
bool probe_nxs(const std::string &, const uint8_t *head, size_t n, uint64_t) {
    return n >= 4 && (n < 16 || std::memcmp(head, MAGIC45, 16) != 0);
}

std::vector<PresetInfo> list_nxs(VolumePtr vol, const std::string &path) {
    read_header(*vol->open(path), path);   // headers only: throws when the file is not readable
    return {PresetInfo{path_stem(path), 0}};
}

// The zones of one .nxs added to inst in `group`, keys limited to lo..hi (zones outside are left out). Returns the
// number of variations; counts roots that were not given.
int add_nxs_zones(VolumePtr vol, const std::string &path, Instrument &inst, int group, int lo, int hi, int &unknown_root) {
    auto blob = vol->open(path);
    auto header = blob->head(16);
    if (header.size() == 16 && std::memcmp(header.data(), MAGIC45, 16) == 0) {
        const FormatReader *reader = find_reader(*vol, path);
        if (!reader) throw ParseError("Nexus .nxs: unsupported 4.5 sample layout");
        Instrument source = reader->load(vol, path, 0);
        for (auto &zone : source.zones) {
            if (zone.key_hi < lo || zone.key_lo > hi) continue;
            zone.key_lo = std::max(zone.key_lo, lo);
            zone.key_hi = std::min(zone.key_hi, hi);
            zone.group = group;
            inst.zones.push_back(std::move(zone));
        }
        for (const auto &warning : source.warnings)
            if (std::find(inst.warnings.begin(), inst.warnings.end(), warning) == inst.warnings.end())
                inst.warnings.push_back(warning);
        return 1;
    }
    Header h = read_header(*blob, path);
    const std::string stem = path_stem(path);
    for (const Rec &r : h.zones) {
        int zlo = std::min(r.lo, r.hi), zhi = std::max(r.lo, r.hi);
        if (zhi < lo || zlo > hi) continue;
        auto ref = std::make_shared<SampleRef>();
        ref->name = stem + " " + std::to_string(r.root) + (h.variations > 1 ? " v" + std::to_string(r.variation + 1) : "");
        ref->key = "nxs:" + path + "@" + std::to_string(r.off);
        ref->rate = int(r.rate);
        ref->channels = r.stereo() ? 2 : 1;
        ref->frames = r.frames;
        ref->decode = [vol, path, r]() { return decode_zone(vol, path, r); };
        Zone z;
        z.name = ref->name;
        z.sample = ref;
        z.group = group;
        z.key_lo = std::max(zlo, lo);
        z.key_hi = std::min(zhi, hi);
        if (r.root > 127) {
            z.root = std::max(r.lo, r.hi);
            unknown_root++;
        } else {
            z.root = r.root;
        }
        if (h.variations > 1) {
            z.play_logic = PlayLogic::RoundRobin;
            z.seq_position = r.variation + 1;
            z.seq_length = h.variations;
        }
        if (r.looped()) {
            Loop l;
            l.start = r.loop;
            l.end = int64_t(r.frames) - 1;
            z.loops.push_back(l);
        }
        inst.zones.push_back(std::move(z));
    }
    return h.variations;
}

Instrument load_nxs(VolumePtr vol, const std::string &path, int) {
    Instrument inst;
    inst.name = path_stem(path);
    inst.format = "reFX Nexus multisample";
    int unknown_root = 0;
    int variations = add_nxs_zones(vol, path, inst, 0, 0, 127, unknown_root);
    if (variations > 1) inst.warnings.push_back(std::to_string(variations) + " variations play as round robins");
    if (unknown_root) inst.warnings.push_back(std::to_string(unknown_root) + " zone(s) without a root: key high used");
    inst.warnings.push_back("Nexus .nxs: samples, key ranges and loops only (envelopes, filter and effects are in Nexus presets)");
    finish_instrument(inst);
    return inst;
}

// ---- presets (.fxp) ----
// Measured 2026-11-06 on the 1,166 NEXUS2.4 / 2.5 / 2.7 presets of a Nexus 4 soundbank (all descramble to a chunk list
// that ends exactly at the end of the data):
//   - VST preset: "CcnK", u32 BE size, "FPCh", ..., fxID "NEXU" at 16, u32 BE chunk size at 0x38, chunk at 0x3C.
//   - Chunk: 16-byte magic ("NEXUS2.4 Preset\0" ...), bytes 0x10..0x2F = preset name XOR 0x5A, then from 0x30 a list of
//     sub-chunks, scrambled (FUN_00483860 of Nexus 5.3.10): u32 tag ^ x; 8 name bytes ^ (x's 4 bytes twice) ^
//     AA AA AA AA FF FF FF FF; u32 data length (plain); data padded to 16-byte blocks, per block x = c + x*a, words ^=
//     x, x^0xAAAAAAAA, x^0xFFFFFFFF, x^0xAAAAAAAA. A raw tag of 0 ends the list. a = (H(name, 0x25877321) & 0x3fffe) |
//     1, c = (H(name, 0x69532613) & 0x3ffffe) | 1, x starts at c + H(name, 0x97361948) * a (H the wide hash above);
//     an empty name uses a = 0x37321, c = 0x132613, H = 0x97361948. The key is the preset's own name: no secret key.
//   - Sub-chunk names are the tag + 8 bytes: OSC0..OSC7 (bytes 3..34: the oscillator's multisample, "Rom/ZigSaw LD1" =
//     <library>/Samples/Rom/ZigSaw LD1.nxs; "OSC/..." = a built-in synth oscillator), LAY0..LAY7_PROP (byte 0: layer
//     on), LAYn_SRCS (byte i = 1: the layer plays OSCi), LAYn_KEYZ (key low, key high), and ~440 more (filter, amp,
//     LFOs, arp, trance gate, effects) not read yet: their scales are not measured.
// NEXUS2 Preset (2.0-2.2) is Syncrosoft MCFACT copy protection and NEXUS3..5 Preset are encrypted with a key built into
// Nexus: neither is read.
const char *FXP_READABLE[] = {"NEXUS2.3 Preset", "NEXUS2.4 Preset", "NEXUS2.5 Preset", "NEXUS2.7 Preset"};

uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool probe_fxp(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    return n >= 0x4c && !std::memcmp(h, "CcnK", 4) && !std::memcmp(h + 8, "FPCh", 4) && !std::memcmp(h + 16, "NEXU", 4) &&
           !std::memcmp(h + 0x3c, "NEXUS", 5);
}

using Chunks = std::map<std::string, std::vector<uint8_t>>;

// The descrambled sub-chunks of a readable preset; ParseError says why others are not read.
Chunks read_fxp(const Blob &f) {
    std::vector<uint8_t> h = f.head(0x4c);
    if (!probe_fxp("", h.data(), h.size(), f.size())) throw ParseError("not a Nexus preset");
    uint32_t size = be32(&h[0x38]);
    if (size < 0x30 || size > (4u << 20) || uint64_t(size) + 0x3c > f.size()) throw ParseError("Nexus preset: bad chunk size");
    std::vector<uint8_t> b = f.bytes(0x3c, size);
    std::string magic(reinterpret_cast<const char *>(b.data()), strnlen(reinterpret_cast<const char *>(b.data()), 16));
    if (std::find(std::begin(FXP_READABLE), std::end(FXP_READABLE), magic) == std::end(FXP_READABLE)) {
        if (magic == "NEXUS2 Preset") throw ParseError("Nexus 2.0-2.2 preset: copy-protected (MCFACT), not read");
        throw ParseError("Nexus preset \"" + magic + "\": unsupported state; Nexus 2.3–2.7 presets are supported");
    }
    for (size_t i = 0x10; i < 0x30; i++) b[i] ^= 0x5a;
    std::string name(reinterpret_cast<const char *>(&b[0x10]), strnlen(reinterpret_cast<const char *>(&b[0x10]), 0x20));
    uint32_t a = 0x37321, c = 0x132613, x = 0;
    if (!name.empty()) {
        a = (name_hash(name, 0x25877321, true) & 0x3fffe) | 1;
        c = (name_hash(name, 0x69532613, true) & 0x3ffffe) | 1;
        x = c + name_hash(name, 0x97361948, true) * a;
    } else {
        x = c + 0x97361948u * a;
    }
    static const uint8_t MASK[8] = {0xaa, 0xaa, 0xaa, 0xaa, 0xff, 0xff, 0xff, 0xff};
    Chunks out;
    size_t p = 0x30;
    while (p + 16 <= b.size()) {
        uint32_t raw = le32(&b[p]);
        if (raw == 0) break;
        uint8_t tagname[13] = {0};
        uint32_t tag = raw ^ x;
        for (int i = 0; i < 4; i++) tagname[i] = uint8_t(tag >> (8 * i));
        for (int i = 0; i < 8; i++) tagname[4 + i] = uint8_t(b[p + 4 + size_t(i)] ^ uint8_t(x >> (8 * (i & 3))) ^ MASK[i]);
        uint32_t len = le32(&b[p + 12]);
        uint64_t blocks = (uint64_t(len) + 15) >> 4;
        size_t q = p + 16;
        if (len > b.size() || q + blocks * 16 > b.size()) throw ParseError("Nexus preset: sub-chunk past the end");
        for (uint64_t k = 0; k < blocks; k++, q += 16) {
            x = c + x * a;
            uint32_t w0 = le32(&b[q]) ^ x, w1 = le32(&b[q + 4]) ^ x ^ 0xaaaaaaaau, w2 = le32(&b[q + 8]) ^ x ^ 0xffffffffu,
                     w3 = le32(&b[q + 12]) ^ x ^ 0xaaaaaaaau;
            uint32_t w[4] = {w0, w1, w2, w3};
            for (int i = 0; i < 16; i++) b[q + size_t(i)] = uint8_t(w[i / 4] >> (8 * (i & 3)));
        }
        std::string key(reinterpret_cast<const char *>(tagname), strnlen(reinterpret_cast<const char *>(tagname), 12));
        out[key].assign(b.begin() + long(p + 16), b.begin() + long(p + 16 + len));
        if (out.size() > 4096) throw ParseError("Nexus preset: too many sub-chunks");
        p = q;
    }
    if (!out.count("LAY0_PROP")) throw ParseError("Nexus preset: no layers (does not decode)");
    return out;
}

bool is_nexus_samples(Volume &vol, const std::string &dir) {
    if (!equals_ci(path_name(dir), "Samples")) return false;
    try {
        for (const DirEntry &e : vol.list(dir))
            if (e.dir) return true;
    } catch (const std::exception &) {}
    return false;
}

std::vector<PresetInfo> list_fxp(VolumePtr vol, const std::string &path) {
    read_fxp(*vol->open(path));
    return {PresetInfo{path_stem(path), 0}};
}

Instrument load_fxp(VolumePtr vol, const std::string &path, int) {
    Chunks ch = read_fxp(*vol->open(path));
    static std::string cached_samples;
    std::string samples = find_beside(*vol, path, is_nexus_samples, cached_samples);
    Instrument inst;
    inst.name = path_stem(path);
    inst.format = "reFX Nexus preset";
    inst.library = path_name(path_dir(path));
    inst.groups.clear();                       // one group per sample layer (finish_instrument restores one if none)
    std::vector<std::string> problems;
    int unknown_root = 0, layers = 0;
    for (int n = 0; n < 8; n++) {
        const std::string L = "LAY" + std::to_string(n) + "_";
        auto prop = ch.find(L + "PROP"), srcs = ch.find(L + "SRCS"), keyz = ch.find(L + "KEYZ");
        if (prop == ch.end() || prop->second.empty() || prop->second[0] != 1 || srcs == ch.end()) continue;
        int lo = 0, hi = 127;
        if (keyz != ch.end() && keyz->second.size() >= 2) {
            lo = std::min<int>(keyz->second[0], 127);
            hi = std::min<int>(keyz->second[1], 127);
            if (lo > hi) std::swap(lo, hi);
        }
        for (size_t i = 0; i < srcs->second.size() && i < 8; i++) {
            if (!srcs->second[i]) continue;
            auto osc = ch.find("OSC" + std::to_string(i));
            if (osc == ch.end() || osc->second.size() < 35) continue;
            const char *s = reinterpret_cast<const char *>(&osc->second[3]);
            std::string ref(s, strnlen(s, 32));
            if (ref.empty() || ref.back() == '/') continue;
            if (ref.compare(0, 4, "OSC/") == 0) {
                problems.push_back("layer " + std::to_string(n + 1) + ": built-in oscillator " + ref.substr(4) + " (not read)");
                continue;
            }
            auto parts = path_split(ref);
            if (ref.front() == '/' || ref.find_first_of("\\:") != std::string::npos ||
                std::find(parts.begin(), parts.end(), "..") != parts.end()) {
                problems.push_back("layer " + std::to_string(n + 1) + ": invalid sample reference");
                continue;
            }
            std::string found;
            if (samples.empty() || !find_ci(*vol, path_join(samples, ref + ".nxs"), found)) {
                problems.push_back("layer " + std::to_string(n + 1) + ": " + ref + ".nxs not found");
                continue;
            }
            int group = int(inst.groups.size());
            Group g;
            g.name = "Layer " + std::to_string(n + 1) + ": " + path_stem(found);
            inst.groups.push_back(g);
            size_t before = inst.zones.size();
            try {
                add_nxs_zones(vol, found, inst, group, lo, hi, unknown_root);
                if (inst.zones.size() > before) layers++;
                else inst.groups.pop_back();
            } catch (const ParseError &e) {
                inst.zones.resize(before);
                inst.groups.pop_back();
                problems.push_back("layer " + std::to_string(n + 1) + ": " + e.what());
            }
        }
    }
    if (inst.zones.empty())
        throw ParseError(problems.empty() ? "Nexus preset: no sample layers"
                                          : (samples.empty() ? "Nexus preset: the library's Samples folder was not found; "
                                                             : "Nexus preset: ") + problems[0]);
    for (const std::string &p : problems) inst.warnings.push_back(p);
    if (unknown_root) inst.warnings.push_back(std::to_string(unknown_root) + " zone(s) without a root: key high used");
    inst.warnings.push_back("Nexus preset: " + std::to_string(layers) + " sample layer(s) with their key zones; levels, "
                            "envelopes, filter, effects, arp and trance gate are not read");
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_nxs() {
    register_reader({"reFX Nexus multisample", "nxs", probe_nxs, list_nxs, load_nxs});
    register_reader({"reFX Nexus preset", "fxp", probe_fxp, list_fxp, load_fxp});
}

}  // namespace omni

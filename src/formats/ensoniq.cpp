// Omni Sampler: Ensoniq EPS / EPS-16+ / ASR-10 / ASR-X-era disks and instruments.
//
// Disks (raw .img floppies and hard-disk dumps, Giebler .GKH, compressed .EDE / .EDA, and HFE floppies through the
// floppy container) mount as folders: the disk's directories, each instrument as "<NAME>.efe" (an EFE header in front of
// the instrument's blocks, so the one reader below takes them and exported .efe files alike). The parsing follows the
// MR HighMan ASR10 Sampler's native reader (ports/asr10-sampler/src/native_asr.cpp), which is checked against the
// factory Essential Sound Disks; the sound mapping (envelopes, filters, layers, patch selects, key-up layers, copied
// wave samples) follows its engine, so an instrument plays the way it does there.
//
// An instrument has up to 8 layers; each layer maps the 88 keys from A0 (note 21) to wave samples and has a velocity
// window. The four patch selects (neither / left / right / both buttons on the ASR) each switch a set of layers on; the
// instrument lists those that differ as presets. Layers in the key-up set play on note-off (release triggers).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include "../fs/fs.hpp"
#include "format.hpp"

namespace omni {
namespace {

constexpr size_t BLK = 512;
constexpr size_t EFE_HEADER = 512;
constexpr size_t WAVE_HEADER = 0x120 - 4;   // a wave sample block's header (after the 4-byte block size)
constexpr int FIRST_KEY = 21;               // A0: wave_refs[0]

// --- the instrument file -------------------------------------------------------------------------------------------

struct EnsEnv {
    int time[6] = {};       // attack, decay 1-3 (time[1..3]), (unused), release (time[5]); 0..99
    int level[5] = {};      // 0..127
    int release_level = 0, vel_sens = 0, key_scale = 0, mode = 0;
};

struct EnsWave {
    int index = 0;
    std::string name;
    int copy_index = 0;
    int root = 60, pitch_amount = 0, bend = 2, fine = 0;
    int filter_mode = 2, cutoff1 = 150, cutoff2 = 150, key1 = 0, key2 = 0, fenv1 = 0, fenv2 = 0;
    int volume = 127, pan = 0, key_lo = 0, key_hi = 127, rate = 44100, loop_mode = 0;
    int start = 0, end = 0, loop_start = 0, loop_end = 0;
    EnsEnv penv, fenv, aenv;
    std::shared_ptr<std::vector<int16_t>> pcm;   // shared by copies
};

struct EnsLayer {
    int index = 0, slot = 0;
    std::string name;
    int vel_lo = 0, vel_hi = 127;
    int refs[88] = {};
};

struct EnsInst {
    std::string name;
    int key_lo = 0, key_hi = 127, transpose = 0;
    int patch[4] = {1, 1, 1, 1};
    int key_down = 1, key_up = 0;
    std::vector<EnsLayer> layers;
    std::vector<EnsWave> waves;
};

// The instrument's records keep one byte per 16-bit word (the EPS's 8-bit bus): the value is the word's first byte.
struct WordReader {
    const std::vector<uint8_t> &d;
    size_t pos;
    void need(size_t n) const { if (pos + n > d.size()) throw ParseError("truncated Ensoniq instrument"); }
    int s() { need(2); int v = int8_t(d[pos]); pos += 2; return v; }
    int u() { need(2); int v = d[pos]; pos += 2; return v; }
    int byte() { need(1); return d[pos++]; }
    void skip(size_t n) { need(n); pos += n; }
    std::string name(size_t chars) {
        need(chars * 2);
        std::string v;
        for (size_t i = 0; i < chars; i++) {
            uint8_t c = d[pos + i * 2];
            if (!c) break;
            v += c >= 32 && c < 127 ? char(c) : ' ';
        }
        pos += chars * 2;
        return trim(v);
    }
    // sample addresses: four words, one byte each, the frame number in the top 23 bits
    uint32_t addr() { need(8); uint32_t v = uint32_t(d[pos]) << 24 | uint32_t(d[pos + 2]) << 16 | uint32_t(d[pos + 4]) << 8 | d[pos + 6]; pos += 8; return v; }
};

uint32_t eps_long(const std::vector<uint8_t> &d, size_t off) {   // a block size: two words, 12 bits each
    if (off + 4 > d.size()) throw ParseError("truncated Ensoniq block size");
    return uint32_t(be16(&d[off + 2]) >> 4) << 12 | uint32_t(be16(&d[off]) >> 4);
}

uint32_t table_offset(const std::vector<uint8_t> &d, size_t off) {
    if (off + 4 > d.size()) throw ParseError("truncated Ensoniq offset table");
    return uint32_t(d[off + 3] >> 4) << 20 | uint32_t(d[off]) << 12 | uint32_t(d[off + 2]) << 4 | (d[off + 3] & 0x0F);
}

EnsEnv read_env(WordReader &r) {
    EnsEnv e;
    r.skip(2);   // envelope type
    for (int i = 0; i < 5; i++) {
        r.skip(2);   // soft (low velocity) level
        e.level[i] = r.u();
        e.time[i] = r.u();
    }
    r.skip(2);   // velocity switch
    e.release_level = r.s();
    e.time[5] = r.u();
    e.vel_sens = r.u();
    e.key_scale = r.u();
    e.mode = r.u();
    return e;
}

EnsWave read_wave(int index, const std::vector<uint8_t> &b) {
    if (b.size() < WAVE_HEADER) throw ParseError("Ensoniq wave sample smaller than its header");
    EnsWave w;
    w.index = index;
    WordReader r{b, 6};
    w.name = r.name(12);
    w.copy_index = r.u();
    r.u();   // copy layer
    w.penv = read_env(r);
    w.fenv = read_env(r);
    w.aenv = read_env(r);
    w.root = r.byte();
    r.byte();   // volume modulation crossfade curve
    w.pitch_amount = r.s();
    r.skip(4);  // LFO and random to pitch
    w.bend = r.u();
    r.skip(2);  // modulation source
    w.fine = r.s();
    r.skip(2);  // modulation amount
    w.filter_mode = r.u();
    w.cutoff1 = r.u();
    w.cutoff2 = r.u();
    w.key1 = r.s();
    w.key2 = r.s();
    w.fenv1 = r.s();
    w.fenv2 = r.s();
    r.skip(8);  // filter modulation sources and amounts
    w.volume = r.byte();
    r.skip(1 + 1 + 1 + 8 + 1);   // output bus, amp / pan modulation sources, crossfade curve, EPS pan
    w.pan = int8_t(r.byte());
    r.skip(1 + 1 + 14);          // amp / pan modulation amounts, LFO
    w.loop_mode = r.u();
    w.start = int(r.addr() >> 9);
    w.end = int(r.addr() >> 9);
    w.loop_start = int(r.addr() >> 9);
    uint32_t le = r.addr();
    w.loop_end = int((le >> 9) + ((le >> 5) & 0x0F));
    int ri = r.u();
    w.rate = ri == 0 ? 48000 : ri == 1 ? 44100 : int(625000.0 / ri + 0.5);
    w.key_lo = r.u();
    w.key_hi = r.u();
    r.skip(10);
    if (r.pos != WAVE_HEADER) throw ParseError("Ensoniq wave sample header layout mismatch");
    size_t n = (b.size() - WAVE_HEADER) / 2;
    if (n) {
        w.pcm = std::make_shared<std::vector<int16_t>>(n);
        for (size_t i = 0; i < n; i++) (*w.pcm)[i] = int16_t(be16(&b[WAVE_HEADER + i * 2]));
    }
    return w;
}

EnsLayer read_layer(const std::vector<uint8_t> &b) {
    if (b.size() < 220) throw ParseError("Ensoniq layer block too small");
    EnsLayer l;
    WordReader r{b, 2};
    l.index = r.s();
    r.skip(2);
    l.name = r.name(12);
    r.skip(5);   // glide, delay, restrike, legato
    r.byte();    // stereo link
    l.vel_lo = r.u();
    l.vel_hi = r.u();
    r.skip(4);   // pitch table, delay
    for (int &ref : l.refs) ref = r.u();
    return l;
}

EnsInst read_instrument(const std::vector<uint8_t> &d, const std::string &fallback) {
    if (d.size() < 644) throw ParseError("Ensoniq instrument shorter than its header");
    EnsInst in;
    WordReader r{d, 10};
    in.name = r.name(12);
    r.skip(10);
    for (int &p : in.patch) p = r.s() & 0xFF;
    in.key_down = r.s() & 0xFF;
    in.key_up = r.s() & 0xFF;
    r.skip(4);
    int hw = be16(&d[r.pos]);
    r.skip(2);
    if (hw != 0x0000 && hw != 0xFFFF && hw != 0xFFFE) throw ParseError("not an EPS / EPS-16+ / ASR-10 instrument");
    in.key_lo = r.s();
    in.key_hi = r.s();
    in.transpose = r.s();
    r.skip(32);   // pitch table
    uint32_t layer_off[8], wave_off[128];
    for (auto &o : layer_off) { o = table_offset(d, r.pos); r.pos += 4; }
    for (auto &o : wave_off) { o = table_offset(d, r.pos); r.pos += 4; }
    // a layer keeps its table slot: the patch selects and key-down / key-up sets are bit masks by slot, and a slot
    // may be empty before a used one (ASR FLUTE: one layer, in a later slot)
    for (int i = 0; i < 8; i++) {
        uint32_t o = layer_off[i];
        if (!o) continue;
        uint32_t size = eps_long(d, o);
        if (!size) continue;
        if (size < 4 || o + size > d.size()) throw ParseError("Ensoniq layer block out of range");
        in.layers.push_back(read_layer(std::vector<uint8_t>(d.begin() + o + 4, d.begin() + o + size)));
        in.layers.back().slot = i;
    }
    for (int i = 0; i < 128; i++) {
        uint32_t o = wave_off[i];
        if (!o) continue;
        uint32_t size = eps_long(d, o);
        if (!size) break;
        if (size < 4 || o + size > d.size()) throw ParseError("Ensoniq wave sample block out of range");
        in.waves.push_back(read_wave(i, std::vector<uint8_t>(d.begin() + o + 4, d.begin() + o + size)));
    }
    // copied wave samples play another one's audio with their own settings (e.g. a brighter velocity layer)
    for (auto &w : in.waves) {
        for (int hop = 0, src = w.copy_index; !w.pcm && src > 0 && hop < 8; hop++) {
            const EnsWave *from = nullptr;
            for (auto &c : in.waves) if (c.index == src && &c != &w) from = &c;
            if (!from) break;
            if (from->pcm) w.pcm = from->pcm;
            else src = from->copy_index;
        }
    }
    if (in.name.empty()) in.name = fallback;
    return in;
}

// --- the sound: the ASR10 Sampler engine's mapping --------------------------------------------------------------------

double env_seconds(int v) {   // envelope time 0..99
    if (v <= 0) return 0;
    double x = std::min(v, 99) / 99.0;
    return 0.01 + 49.0 * x * x * x;
}

double cutoff_hz(int units) {   // filter 0..150 (150 = open), about 10 octaves from 16 Hz
    return std::max(16.0, std::min(20000.0, 16.0 * std::pow(2.0, units * (10.0 / 150.0))));
}

// The ASR envelope (level 0 -> 1 over the attack, -> 2 -> (3) -> 4 over the decays, sustain at 4, release) as an ADSR
// relative to its peak; peak is the level the zone's gain is scaled by.
Envelope to_envelope(const EnsEnv &e, double &peak) {
    Envelope o;
    o.set = true;
    double l0 = e.level[0] / 127.0, l1 = e.level[1] / 127.0, l2 = e.level[2] / 127.0, l4 = e.level[4] / 127.0;
    peak = std::max({l0, l1, l2, l4, 1e-3});
    o.start_level = l0 / peak;
    o.attack = env_seconds(e.time[0]);
    if (l1 >= l2) {   // the usual: attack to the top, then fall
        o.hold = 0;
        o.decay = env_seconds(e.time[1]) + env_seconds(e.time[2] + e.time[3]);
    } else {          // a swell: rising on through the first decay
        o.attack += env_seconds(e.time[1]);
        o.decay = env_seconds(e.time[2] + e.time[3]);
    }
    o.sustain = std::max(0.0, std::min(1.0, l4 / peak));
    o.release = std::max(0.004, env_seconds(e.time[5]));
    o.time_vel_tracking = 0;
    return o;
}

Instrument build(const EnsInst &in, int mask, const std::string &file_key, const std::string &format) {
    Instrument inst;
    inst.name = in.name;
    inst.format = format;
    std::map<const std::vector<int16_t> *, SampleRefPtr> refs;
    std::map<int, const EnsWave *> waves;
    for (auto &w : in.waves) waves[w.index] = &w;
    int skipped = 0;
    for (const EnsLayer &L : in.layers) {
        int bit = 1 << L.slot;
        if (!(mask & bit)) continue;
        for (int pass = 0; pass < 2; pass++) {
            bool up = pass == 1;
            if (!((up ? in.key_up : in.key_down) & bit)) continue;
            for (int k = 0; k < 88;) {
                int ref = L.refs[k], k1 = k;
                while (k1 + 1 < 88 && L.refs[k1 + 1] == ref) k1++;
                int lo = k + FIRST_KEY, hi = k1 + FIRST_KEY;
                k = k1 + 1;
                if (!ref) continue;
                auto wi = waves.find(ref);
                if (wi == waves.end() || !wi->second->pcm) { skipped++; continue; }
                const EnsWave &w = *wi->second;
                lo = std::max({lo, w.key_lo, in.key_lo});
                hi = std::min({hi, w.key_hi, in.key_hi});
                if (lo > hi) continue;
                Zone z;
                z.name = w.name.empty() ? L.name : w.name;
                z.key_lo = lo;
                z.key_hi = hi;
                z.vel_lo = L.vel_lo;
                z.vel_hi = up ? 127 : L.vel_hi;
                z.root = w.root;
                z.tune = in.transpose + w.fine / 100.0;
                z.bend_up = w.bend * 100;
                z.bend_down = -w.bend * 100;
                z.pan = clampd(w.pan / 99.0, -1, 1);
                double peak = 1;
                z.amp_env = to_envelope(w.aenv, peak);
                z.gain_db = 20.0 * std::log10(std::max(1, w.volume) / 127.0 * peak);
                z.trigger = up ? Trigger::Release : Trigger::Attack;
                if (up) z.amp_vel_depth = 0;   // the ASR plays key-up layers at a fixed velocity
                // filter: mode 0 LP2/HP2, 1 LP3/HP1, 2 LP2/LP2, 3 LP3/LP1. The low-pass carries the sound; the
                // high-pass of modes 0/1 is left out (one filter per zone here)
                int mode = w.filter_mode & 3;
                double fc = mode >= 2 ? std::min(w.cutoff1, w.cutoff2) : w.cutoff1;
                if (fc < 150 || w.fenv1 != 0) {
                    z.filter.type = FilterType::LowPass;
                    z.filter.poles = mode == 0 ? 2 : mode == 1 ? 3 : mode == 2 ? 4 : 4;
                    z.filter.cutoff = cutoff_hz(int(fc));
                    z.filter.key_tracking = w.key1 / 127.0;
                    if (w.fenv1) {
                        double fpeak = 1;
                        z.filter.env = to_envelope(w.fenv, fpeak);
                        z.filter.env_depth = clampd(w.fenv1 / 127.0 * fpeak, -1, 1);
                    }
                }
                // playback: loop mode 0 forward, 1 backward (no loop), 2 loop forward, 3 loop back and forth, 4 loop
                // while held then on to the end
                int frames = int(w.pcm->size());
                z.start = std::max(0, std::min(w.start, frames - 1));
                z.stop = w.end > z.start ? std::min(w.end, frames) : -1;
                if (w.loop_mode == 1) z.reverse = true;
                if (w.loop_mode >= 2 && w.loop_mode <= 4 && w.loop_end > w.loop_start + 1 && w.loop_end <= frames) {
                    Loop l;
                    l.type = w.loop_mode == 3 ? LoopType::Alternating : LoopType::Forward;
                    l.start = w.loop_start;
                    l.end = w.loop_end - 1;
                    l.until_release = w.loop_mode == 4;
                    z.loops.push_back(l);
                }
                const std::vector<int16_t> *key = w.pcm.get();
                SampleRefPtr &sr = refs[key];
                if (!sr) {
                    sr = std::make_shared<SampleRef>();
                    sr->key = file_key + ":" + std::to_string(w.index);
                    sr->name = w.name;
                    sr->rate = w.rate;
                    sr->channels = 1;
                    sr->frames = frames;
                    auto data = w.pcm;
                    int rate = w.rate;
                    sr->decode = [data, rate] {
                        auto p = std::make_shared<Pcm>();
                        p->rate = rate;
                        p->channels = 1;
                        p->data = *data;
                        return PcmPtr(p);
                    };
                }
                z.sample = sr;
                inst.zones.push_back(z);
            }
        }
    }
    if (skipped) inst.warnings.push_back(std::to_string(skipped) + " key ranges point at missing wave samples");
    finish_instrument(inst);
    return inst;
}

// --- EFE files ---------------------------------------------------------------------------------------------------

bool efe_header(const uint8_t *h, size_t n) {
    return n >= 64 && h[0] == 0x0D && h[1] == 0x0A && h[47] == 0x0D && h[48] == 0x0A && h[49] == 0x1A;
}

bool probe_efe(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    return efe_header(h, n) && size > EFE_HEADER + 644 && !std::memcmp(h + 30, "Instr", 5);
}

EnsInst read_efe(VolumePtr vol, const std::string &path) {
    BlobPtr b = vol->open(path);
    std::vector<uint8_t> h = b->head(EFE_HEADER);
    if (!efe_header(h.data(), h.size())) throw ParseError("not an Ensoniq EFE file");
    size_t body = size_t(be16(&h[0x34])) * BLK;
    if (body < 644 || EFE_HEADER + body > b->size()) body = size_t(b->size() - EFE_HEADER);
    std::string fallback = trim(std::string(reinterpret_cast<const char *>(&h[18]), 12));
    return read_instrument(b->bytes(EFE_HEADER, body), fallback.empty() ? path_stem(path) : fallback);
}

const char *const PATCH_NAMES[4] = {"", " (Patch L)", " (Patch R)", " (Patch L+R)"};

// patch 0 always; the others when they switch on a different set of layers
std::vector<PresetInfo> list_efe(VolumePtr vol, const std::string &path) {
    EnsInst in = read_efe(vol, path);
    std::vector<PresetInfo> out{PresetInfo{in.name, 0}};
    std::set<int> seen{in.patch[0]};
    for (int p = 1; p < 4; p++)
        if (in.patch[p] && seen.insert(in.patch[p]).second) out.push_back(PresetInfo{in.name + PATCH_NAMES[p], p});
    return out;
}

Instrument load_efe(VolumePtr vol, const std::string &path, int index) {
    EnsInst in = read_efe(vol, path);
    std::string key = "ens:" + std::to_string(reinterpret_cast<uintptr_t>(vol.get())) + ":" + path;
    Instrument inst = build(in, in.patch[index & 3], key, "Ensoniq EPS/ASR");
    if (inst.zones.empty()) {
        // the patch's layers are not there (a damaged disk dump: AD-002.img's ASR FLUTE): play the layers that are
        inst = build(in, 0xFF, key, "Ensoniq EPS/ASR");
        if (!inst.zones.empty()) inst.warnings.insert(inst.warnings.begin(), "the patch's layers are missing (damaged disk?): playing all layers");
    }
    if (inst.zones.empty()) throw ParseError("this Ensoniq instrument has no playable wave samples");
    if ((index & 3) != 0) inst.name += PATCH_NAMES[index & 3];
    return inst;
}

// --- disks -------------------------------------------------------------------------------------------------------

// A compressed disk (.EDE: EPS floppies, .EDA: ASR / EPS-16+ disks): the image's blocks with the blocks of a filler
// pattern left out, listed in a bitmap. Read back on the fly: a skipped block reads as the pattern.
class SkipBlob : public Blob {
public:
    SkipBlob(BlobPtr packed, uint64_t payload, std::vector<int64_t> map) : packed_(std::move(packed)), payload_(payload), map_(std::move(map)) {}
    uint64_t size() const override { return uint64_t(map_.size()) * BLK; }
    void read(uint64_t off, void *dst, size_t n) const override {
        if (off + n > size()) throw ParseError("read past the end of an Ensoniq image");
        uint8_t *o = static_cast<uint8_t *>(dst);
        while (n) {
            uint64_t blk = off / BLK, in = off % BLK;
            size_t take = size_t(std::min<uint64_t>(n, BLK - in));
            int64_t src = map_[size_t(blk)];
            if (src < 0) {
                for (size_t i = 0; i < take; i++) o[i] = ((in + i) & 1) ? 0xB6 : 0x6D;
            } else {
                packed_->read(payload_ + uint64_t(src) * BLK + in, o, take);
            }
            o += take; off += take; n -= take;
        }
    }

private:
    BlobPtr packed_;
    uint64_t payload_;
    std::vector<int64_t> map_;   // logical block -> packed block, -1 = skipped
};

bool skip_bit(const std::vector<uint8_t> &t, size_t i) { return i / 8 < t.size() && (t[i / 8] >> (7 - i % 8) & 1); }

BlobPtr unpack_skip(BlobPtr file, size_t table_at, size_t table_len) {
    std::vector<uint8_t> skip = file->bytes(table_at, table_len);
    uint64_t payload = EFE_HEADER, packed_blocks = (file->size() - 1024) / BLK;
    // the logical size: the disk's own block count (in its device ID block) when present, else as many as the bitmap needs
    size_t logical = 0;
    if (!skip_bit(skip, 1)) {
        size_t id_block = skip_bit(skip, 0) ? 0 : 1;
        if (id_block < packed_blocks) {
            std::vector<uint8_t> id = file->bytes(payload + id_block * BLK, BLK);
            if (id[38] == 'I' && id[39] == 'D' && be32(&id[10]) == 512) logical = be32(&id[14]);
        }
    }
    if (logical == 0 || logical > (1u << 21)) {
        logical = size_t(packed_blocks);
        for (;;) {
            size_t next = size_t(packed_blocks), lim = std::min(logical, skip.size() * 8);
            for (size_t b = 0; b < lim; b++) if (skip_bit(skip, b)) next++;
            if (next == logical) break;
            logical = next;
        }
    }
    std::vector<int64_t> map(logical);
    int64_t p = 0;
    for (size_t b = 0; b < logical; b++) map[b] = skip_bit(skip, b) ? -1 : (uint64_t(p) < packed_blocks ? p++ : -1);
    return std::make_shared<SkipBlob>(file, payload, std::move(map));
}

// The image a container holds (GKH, EDE, EDA) or the image itself.
BlobPtr disk_of(BlobPtr f) {
    std::vector<uint8_t> h = f->head(512);
    if (h.size() >= 8 && !std::memcmp(h.data(), "TDDF", 4)) {
        if (h[4] != 'I' || h[5] != 1) throw ParseError("unsupported GKH header");
        size_t tags = le16(&h[6]);
        std::vector<uint8_t> t = f->bytes(8, tags * 10);
        for (size_t i = 0; i < tags; i++)
            if (t[i * 10] == 0x0B) return make_slice(f, le32(&t[i * 10 + 6]), le32(&t[i * 10 + 2]));
        throw ParseError("GKH file holds no Ensoniq disk");
    }
    if (h.size() >= 512 && h[0] == 0x0D && h[1] == 0x0A && h[78] == 0x0D && h[79] == 0x0A) {
        if (h[157] == 0x0D && h[158] == 0x0A && h[159] == 0x1A) return unpack_skip(f, 160, 200);   // EDE
        if (h[93] == 0x0D && h[94] == 0x0A && h[95] == 0x1A) return unpack_skip(f, 96, 400);      // EDA
    }
    return f;
}

bool raw_disk(const uint8_t *h, size_t n) {
    return n >= 1056 && h[512 + 38] == 'I' && h[512 + 39] == 'D' && h[512] == 0 && (h[513] == 0 || h[513] == 0x80) &&
           (h[514] == 1 || h[514] == 2) && be32(h + 522) == 512 && h[1024 + 28] == 'O' && h[1024 + 29] == 'S';
}

bool probe_disk(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    if (raw_disk(h, n)) return true;
    if (n >= 8 && !std::memcmp(h, "TDDF", 4) && h[4] == 'I') return true;
    if (n >= 512 && h[0] == 0x0D && h[1] == 0x0A && h[78] == 0x0D && h[79] == 0x0A &&
        ((h[157] == 0x0D && h[158] == 0x0A && h[159] == 0x1A && h[509] == 0 && be16(h + 510) == 3) ||
         (h[93] == 0x0D && h[94] == 0x0A && h[95] == 0x1A && h[509] == 0 && be16(h + 510) == 203)))
        return true;
    return false;
}

struct DiskFs {
    BlobPtr img;
    uint64_t blocks;
    std::map<uint32_t, uint32_t> fat_cache;
    uint32_t next(uint32_t b) {   // the FAT: 3 bytes per block, 170 per FAT block, from block 5
        auto it = fat_cache.find(b);
        if (it != fat_cache.end()) return it->second;
        uint64_t at = (5 + b / 170) * BLK + (b % 170) * 3;
        std::vector<uint8_t> e = img->bytes(at, 3);
        return fat_cache[b] = be24(e.data());
    }
    std::vector<std::pair<uint64_t, uint64_t>> extents(uint32_t first, uint32_t nblocks, uint32_t contiguous) {
        if (!nblocks || contiguous > nblocks || !contiguous || first + contiguous > blocks) throw ParseError("bad Ensoniq directory entry");
        std::vector<std::pair<uint64_t, uint64_t>> ext{{uint64_t(first) * BLK, uint64_t(contiguous) * BLK}};
        uint32_t cur = first + contiguous - 1;
        for (uint32_t left = nblocks - contiguous, hops = 0; left; left--) {
            uint32_t n = next(cur);
            if (n <= 2 || n >= blocks || ++hops > nblocks) throw ParseError("bad Ensoniq FAT chain");
            if (ext.back().first + ext.back().second == uint64_t(n) * BLK) ext.back().second += BLK;
            else ext.push_back({uint64_t(n) * BLK, BLK});
            cur = n;
        }
        return ext;
    }
};

std::string ens_name(const uint8_t *p) {
    std::string s;
    for (int i = 0; i < 12 && p[i]; i++) s += p[i] >= 32 && p[i] < 127 && p[i] != '/' ? char(p[i]) : ' ';
    return trim(s);
}

// An instrument as an EFE file: a header naming it, then its blocks.
class EfeBlob : public Blob {
public:
    EfeBlob(std::vector<uint8_t> header, BlobPtr body) : h_(std::move(header)), body_(std::move(body)) {}
    uint64_t size() const override { return h_.size() + body_->size(); }
    void read(uint64_t off, void *dst, size_t n) const override {
        uint8_t *o = static_cast<uint8_t *>(dst);
        if (off < h_.size()) {
            size_t take = size_t(std::min<uint64_t>(n, h_.size() - off));
            std::memcpy(o, &h_[size_t(off)], take);
            o += take; off += take; n -= take;
        }
        if (n) body_->read(off - h_.size(), o, n);
    }

private:
    std::vector<uint8_t> h_;
    BlobPtr body_;
};

std::vector<uint8_t> efe_header_for(const std::string &name, uint32_t nblocks) {
    std::vector<uint8_t> h(EFE_HEADER, 0);
    std::string line = "Eps File:       " + (name + std::string(12, ' ')).substr(0, 12) + "Instrument     ";
    h[0] = 0x0D; h[1] = 0x0A;
    std::memcpy(&h[2], line.data(), std::min<size_t>(45, line.size()));
    h[47] = 0x0D; h[48] = 0x0A; h[49] = 0x1A;
    h[0x33] = 3;   // file type: instrument
    h[0x34] = uint8_t(nblocks >> 8); h[0x35] = uint8_t(nblocks);
    return h;
}

void walk(DiskFs &fs, std::shared_ptr<TreeVolume> vol, const std::string &dir, uint32_t first, uint32_t nblocks,
          uint32_t contiguous, int depth) {
    if (depth > 16) return;
    std::vector<uint8_t> d;
    {
        BlobPtr b = make_extents(fs.img, fs.extents(first, nblocks, contiguous));
        d = b->all();
    }
    if (!dir.empty()) vol->add_dir(dir);
    for (size_t off = 0; off + 26 <= d.size(); off += 26) {
        int type = d[off + 1];
        if (!type) continue;
        std::string name = ens_name(&d[off + 2]);
        uint32_t nb = be16(&d[off + 14]), cont = be16(&d[off + 16]), start = be32(&d[off + 18]);
        try {
            if (type == 2) {   // a directory
                walk(fs, vol, path_join(dir, name.empty() ? "Folder" : name), start, nb, cont, depth + 1);
            } else if (type == 3) {   // an instrument
                auto ext = fs.extents(start, nb, cont);
                BlobPtr img = fs.img;
                std::string n = name.empty() ? "Instrument" : name;
                std::vector<uint8_t> hdr = efe_header_for(n, nb);
                vol->add_lazy(path_join(dir, n + ".efe"), EFE_HEADER + uint64_t(nb) * BLK,
                              [img, ext, hdr] { return BlobPtr(std::make_shared<EfeBlob>(hdr, make_extents(img, ext))); });
            }
        } catch (const ParseError &) {
            // a damaged entry: leave it out, keep the rest of the disk
        }
    }
}

VolumePtr mount_disk(BlobPtr file) {
    BlobPtr img = disk_of(file);
    std::vector<uint8_t> h = img->head(1100);
    if (!raw_disk(h.data(), h.size())) throw ParseError("not an Ensoniq disk");
    std::string label;
    for (int i = 0; i < 8; i++) { uint8_t c = h[512 + 30 + i]; if (c >= 32 && c < 127) label += char(c); }
    DiskFs fs{img, img->size() / BLK, {}};
    auto vol = std::make_shared<TreeVolume>("Ensoniq disk" + (trim(label).empty() ? std::string() : " " + trim(label)));
    walk(fs, vol, "", 3, 2, 2, 0);   // the root directory: blocks 3-4
    if (!vol->file_count()) throw ParseError("no EPS / ASR instruments on this Ensoniq disk");
    return vol;
}

}  // namespace

void register_ensoniq() {
    register_reader({"Ensoniq EPS/ASR instrument", "efe", probe_efe, list_efe, load_efe});
}

void register_ensoniq_images() {
    register_image_type({"Ensoniq disk", probe_disk, mount_disk});
}

}  // namespace omni

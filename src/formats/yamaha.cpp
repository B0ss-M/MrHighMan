// Omni Sampler: Yamaha Motif / MOXF / Montage / MODX libraries (YSFC) and Motif ES voice + sample files (.W7A + .W8A).
//
// YSFC ("YAMAHA-YSFC", Motif XS .X0*, Motif XF .X3*, MOXF .X6*, Montage .X7*, Montage M .Y2*, MODX .X8*): read as
// ConvertWithMoss's YamahaYsfcDetector / YsfcFile and its file package (LGPL-3.0, Jürgen Moßgraber), but from file
// offsets instead of reading the whole file (libraries are hundreds of MB). A header with a chunk catalogue; each
// "E..." chunk lists entries (name, size and offset of the item in the matching "D..." chunk). Waveforms: EWFM/DWFM
// (key-banks: key / velocity range, root, tune, level, pan, loop, rate) and EWIM/DWIM (the audio, 16-bit little
// endian, a stereo key-bank is a left and a right item). Every waveform is a preset. Montage and MODX libraries also
// list their performances (EPFM/DPFM), as ConvertWithMoss does: each part (its AWM elements) is a group; an element
// plays its waveform within its key / velocity limits with its level, pan, tune, amplitude envelope, filter and filter
// envelope; XA cycle / random become round robin / random, XA key-off a release trigger. Where ConvertWithMoss
// simplifies, this follows the hardware: the key-bank and element levels add up (it keeps the element's), pans add
// (it averages, which narrows stereo pairs), and the part's key / velocity limits, note shift and detune apply.
// Elements that use the keyboard's preset (ROM) waveforms are skipped and reported. The pitch envelope is not read.
//
// Motif ES (not read by ConvertWithMoss; worked out from the Sonic Reality Sampler library): the .W7A ("A0745 ALL",
// "YSFC" at 0x30) lists the waveforms (EWFM, 32-byte entries, a 16-character name first); the .W8A next to it
// ("A0745 SAMPLE") holds them: at 0x200 one (offset, record count) pair per waveform, 16 bytes each. A record is an
// 80-byte header then 16-bit big-endian audio, padded to 16 bytes: size in bytes (+0), root key (+0x11), fine tune
// (+0x13, signed), key range (+0x31, +0x33), velocity range (+0x34, +0x35), loop start and loop length (+0x38,
// +0x40: eight hex digits, one per byte; start + length is the sample's end), pan (+0x4A: 1 left, 64 centre, 127
// right), first of a stereo pair (+0x4B). 44.1 kHz (checked by the pitch of the samples against their root keys).
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include "../core/audio.hpp"
#include "format.hpp"

namespace omni {
namespace {

// --- YSFC container ----------------------------------------------------------------------------------------------

struct Entry { std::string name; uint32_t size = 0, offset = 0; };

struct Ysfc {
    BlobPtr b;
    int ver = 0;                                  // 101 Motif XS, 102 Motif XF, 103 MOXF, 4xx Montage, 41x Montage M, 5xx MODX
    std::map<std::string, uint64_t> chunk;        // chunk ID -> file offset
    bool v1() const { return ver < 400; }         // Motif / MOXF
    bool motif() const { return ver <= 102; }
    bool performances() const { return (ver >= 400 && ver < 410) || (ver >= 500 && ver < 510); }

    explicit Ysfc(BlobPtr blob) : b(std::move(blob)) {
        std::vector<uint8_t> h = b->bytes(0, 64);
        if (std::memcmp(h.data(), "YAMAHA-YSFC", 11)) throw ParseError("not a Yamaha YSFC file");
        const char *v = reinterpret_cast<const char *>(&h[16]);
        ver = std::isdigit((unsigned char)v[0]) && std::isdigit((unsigned char)v[2]) && std::isdigit((unsigned char)v[4])
                  ? (v[0] - '0') * 100 + (v[2] - '0') * 10 + (v[4] - '0') : 100;
        uint32_t cat = be32(&h[32]);
        if (cat > (1u << 16)) throw ParseError("bad YSFC catalogue");
        std::vector<uint8_t> c = b->bytes(64, cat);
        for (size_t i = 0; i + 8 <= c.size(); i += 8) chunk[std::string(reinterpret_cast<const char *>(&c[i]), 4)] = be32(&c[i + 4]);
    }

    std::vector<Entry> entries(const char *id) const {
        std::vector<Entry> out;
        auto it = chunk.find(id);
        if (it == chunk.end()) return out;
        uint64_t o = it->second;
        std::vector<uint8_t> h = b->bytes(o, 12);
        uint32_t len = be32(&h[4]), n = be32(&h[8]);
        std::vector<uint8_t> d = b->bytes(o + 12, std::min<uint64_t>(len, b->size() - o - 12));
        size_t p = 0;
        for (uint32_t i = 0; i < n && p + 8 <= d.size(); i++) {
            uint32_t L = be32(&d[p + 4]);
            if (std::memcmp(&d[p], "Entr", 4) || p + 8 + L > d.size()) break;
            const uint8_t *e = &d[p + 8];
            size_t q = 0;
            Entry en;
            if (ver <= 102) q += 4;
            en.size = be32(e + q); q += 4;
            if (ver <= 102) q += 4;
            en.offset = be32(e + q); q += 4;
            q += 4;   // content number
            if (ver <= 102) q += ver <= 101 ? 1 : 2;
            else if (ver >= 400) {
                q += 6;
                if ((ver > 402 && ver < 410) || ver >= 500) q += 4;
                if (ver >= 410 && ver < 500) q += 9;
            }
            while (q < L && e[q]) en.name += char(e[q++]);
            out.push_back(en);
            p += 8 + L;
        }
        return out;
    }

    // an item's content: the "Data" block at the entry's offset in the data chunk
    uint64_t item(const char *id, const Entry &e, uint32_t &size) const {
        auto it = chunk.find(id);
        if (it == chunk.end()) throw ParseError(std::string("YSFC without ") + id);
        uint64_t o = it->second + e.offset;
        std::vector<uint8_t> h = b->bytes(o, 8);
        if (std::memcmp(h.data(), "Data", 4)) throw ParseError("bad YSFC data item");
        size = be32(&h[4]);
        return o + 8;
    }
};

// "256:EP1 Body VelSoft" -> "EP1 Body VelSoft"
std::string entry_name(const std::string &s) {
    size_t c = s.find(':');
    std::string n = c != std::string::npos && c > 0 && std::isdigit((unsigned char)s[0]) ? s.substr(c + 1) : s;
    return trim(n);
}

double norm_range(int v, int neg, int pos) { return clampd(v < 0 ? -double(v) / neg : double(v) / pos, -1, 1); }

struct Keybank {
    int key_lo = 0, key_hi = 127, vel_lo = 1, vel_hi = 127, level = 255, pan = 64, fixed = 0, root = 60, coarse = 64,
        fine = 64, channels = 1, wave_format = 0, loop_mode = 0, encrypted = 0, rate = 44100;
    int64_t play_start = 0, loop_start = 0, loop_end = 0, length = 0;
};

uint32_t u32(const uint8_t *p, bool big) { return big ? be32(p) : le32(p); }

std::vector<Keybank> read_keybanks(const Ysfc &y, const std::vector<uint8_t> &d) {
    std::vector<Keybank> out;
    const bool v1 = y.v1(), motif = y.motif(), big = v1 && !motif;
    const size_t rec = motif ? 68 : v1 ? 76 : 64;
    for (size_t o = 4; o + rec <= d.size(); o += rec) {
        const uint8_t *p = &d[o];
        Keybank k;
        k.key_lo = p[0]; k.key_hi = p[1]; k.vel_lo = p[2]; k.vel_hi = p[3];
        k.level = v1 ? std::min(255, 2 * p[4]) : p[4];
        k.pan = p[5] & 0x7F; k.fixed = p[7]; k.root = p[8]; k.coarse = p[9]; k.fine = p[10]; k.channels = p[11];
        k.wave_format = p[14]; k.loop_mode = p[15]; k.encrypted = p[16];
        int rest = p[18];
        size_t q = motif ? 32 : 36;
        k.rate = int(u32(p + q, big)); k.play_start = u32(p + q + 4, big); k.loop_start = u32(p + q + 8, big);
        k.loop_end = u32(p + q + 12, big);
        q += 16;
        if (!v1) { k.loop_start = 16 * k.loop_start + rest; q += 8; }
        k.length = u32(p + q, big);
        out.push_back(k);
    }
    return out;
}

// the audio items of a waveform: file offset and size of each
std::vector<std::pair<uint64_t, uint32_t>> read_wave_items(const Ysfc &y, uint64_t at, uint32_t size) {
    std::vector<std::pair<uint64_t, uint32_t>> out;
    uint32_t n = be32(y.b->bytes(at, 4).data());
    uint64_t o = at + 4, end = at + size;
    for (uint32_t i = 0; i < n && o + 4 <= end; i++) {
        uint32_t s = be32(y.b->bytes(o, 4).data());
        out.push_back({o + 4, s});
        o += 4 + uint64_t(s);
    }
    return out;
}

// a left and a right item as one stereo sample (16-bit little endian each)
SampleRefPtr stereo_ref(BlobPtr b, uint64_t l, uint64_t r, int64_t frames, int rate, const std::string &name, bool big) {
    auto ref = std::make_shared<SampleRef>();
    ref->key = "ys2:" + std::to_string(reinterpret_cast<uintptr_t>(b.get())) + ":" + std::to_string(l) + ":" + std::to_string(r);
    ref->name = name; ref->rate = rate; ref->channels = 2; ref->frames = frames;
    ref->decode = [b, l, r, frames, rate, big] {
        std::vector<uint8_t> L = b->bytes(l, size_t(frames) * 2), R = b->bytes(r, size_t(frames) * 2);
        auto p = std::make_shared<Pcm>();
        p->rate = rate; p->channels = 2; p->data.resize(size_t(frames) * 2);
        for (int64_t i = 0; i < frames; i++) {
            const uint8_t *a = &L[size_t(i) * 2], *c = &R[size_t(i) * 2];
            p->data[size_t(i) * 2] = int16_t(big ? be16(a) : le16(a));
            p->data[size_t(i) * 2 + 1] = int16_t(big ? be16(c) : le16(c));
        }
        return PcmPtr(p);
    };
    return ref;
}

Zone keybank_zone(const Keybank &k, const std::string &name) {
    Zone z;
    z.name = name + " " + std::to_string(k.root);
    z.root = k.root; z.key_lo = k.key_lo; z.key_hi = k.key_hi;
    z.vel_lo = k.vel_lo; z.vel_hi = k.vel_hi;
    if (k.fixed == 1) z.key_tracking = 0;
    z.tune = (k.coarse - 64) + std::lround((k.fine - 64) * 1.5625) / 100.0;
    z.gain_db = k.level == 0 ? -96 : -95.25 + (k.level - 1) * 0.375;
    z.pan = norm_range(k.pan - 64, 63, 63);
    z.start = k.play_start;
    if (k.length > 0) z.stop = k.length;
    if (k.loop_mode != 1 && k.loop_end > k.loop_start) {
        Loop l;
        l.start = k.loop_start; l.end = k.loop_end;
        if (k.loop_mode == 2) l.type = LoopType::Backward;
        z.loops.push_back(l);
    }
    return z;
}

// one waveform's zones (ConvertWithMoss createSampleZones)
std::vector<Zone> waveform_zones(const Ysfc &y, size_t wi, const std::vector<Entry> &ewfm, const std::vector<Entry> &ewim, std::string &name) {
    if (wi >= ewfm.size() || wi >= ewim.size()) throw ParseError("waveform out of range");
    name = entry_name(ewfm[wi].name);
    uint32_t ks = 0, ws = 0;
    uint64_t ka = y.item("DWFM", ewfm[wi], ks), wa = y.item("DWIM", ewim[wi], ws);
    std::vector<Keybank> kb = read_keybanks(y, y.b->bytes(ka, ks));
    auto items = read_wave_items(y, wa, ws);
    std::vector<Zone> zones;
    size_t ki = 0, wix = 0;
    while (ki < kb.size() && wix < items.size()) {
        const Keybank &k = kb[ki];
        if (k.wave_format != 0 && k.wave_format != 5) throw ParseError(k.wave_format == 4 ? "compressed (WXC) waveforms are not supported" : "unsupported waveform format");
        if (k.encrypted && !y.v1()) throw ParseError("encrypted waveforms (a protected library)");
        Zone z = keybank_zone(k, name);
        int64_t frames = items[wix].second / 2;
        if (k.channels != 2) {
            z.sample = raw_sample_ref(y.b, items[wix].first, frames, 1, Enc::S16LE, k.rate, z.name);
            zones.push_back(z);
            ki++; wix++;
            continue;
        }
        if (wix + 1 >= items.size()) throw ParseError("a stereo waveform without its right channel");
        int64_t rframes = items[wix + 1].second / 2;
        if (rframes == frames) {
            z.sample = stereo_ref(y.b, items[wix].first, items[wix + 1].first, frames, k.rate, z.name, false);
            zones.push_back(z);
        } else {   // left and right of different lengths: two mono zones, panned
            Zone r = z;
            z.sample = raw_sample_ref(y.b, items[wix].first, frames, 1, Enc::S16LE, k.rate, z.name);
            r.sample = raw_sample_ref(y.b, items[wix + 1].first, rframes, 1, Enc::S16LE, k.rate, z.name + " R");
            z.pan = -1; r.pan = 1;
            zones.push_back(z); zones.push_back(r);
        }
        ki += y.v1() ? 1 : 2;
        wix += 2;
    }
    return zones;
}

// --- Montage / MODX performances -----------------------------------------------------------------------------------

// envelope times 0..127 in seconds (ConvertWithMoss YamahaYsfcPartElement)
const float ENV_TIMES[128] = {0.2f, 0.21f, 0.22f, 0.23f, 0.24f, 0.25f, 0.26f, 0.27f, 0.28f, 0.29f, 0.3f, 0.304f, 0.308f, 0.312f, 0.316f, 0.32f,
    0.324f, 0.328f, 0.332f, 0.336f, 0.34f, 0.343f, 0.346f, 0.349f, 0.352f, 0.355f, 0.358f, 0.361f, 0.364f, 0.367f, 0.37f, 0.373f, 0.376f, 0.379f,
    0.382f, 0.385f, 0.388f, 0.391f, 0.394f, 0.397f, 0.4f, 0.404f, 0.408f, 0.412f, 0.416f, 0.42f, 0.424f, 0.428f, 0.432f, 0.436f, 0.44f, 0.446f,
    0.452f, 0.458f, 0.464f, 0.47f, 0.476f, 0.482f, 0.488f, 0.494f, 0.5f, 0.54f, 0.58f, 0.62f, 0.66f, 0.7f, 0.74f, 0.78f, 0.82f, 0.86f, 0.9f, 1.02f,
    1.14f, 1.26f, 1.38f, 1.5f, 1.62f, 1.74f, 1.86f, 1.98f, 2.1f, 2.29f, 2.48f, 2.67f, 2.86f, 3.05f, 3.24f, 3.43f, 3.62f, 3.81f, 4.0f, 4.8f, 5.6f,
    6.4f, 7.2f, 8.0f, 8.8f, 9.6f, 10.4f, 11.2f, 12.0f, 13.3f, 14.6f, 15.9f, 17.2f, 18.5f, 19.8f, 21.1f, 22.4f, 23.7f, 25.0f, 28.9f, 32.8f, 36.7f,
    40.6f, 44.5f, 48.4f, 52.3f, 56.2f, 60.1f, 64.0f, 67.714f, 71.429f, 75.143f, 78.857f, 82.571f, 86.286f, 90.0f};
double env_t(int v) { return ENV_TIMES[clampi(v, 0, 127)]; }

// pitch key follow, 0..127 -> -200..200 % (ConvertWithMoss PITCH_KEY)
const int16_t PITCH_KEY[128] = {-200, -199, -198, -197, -196, -195, -194, -193, -192, -191, -190, -185, -180, -175, -170, -165, -160, -155,
    -150, -145, -140, -135, -130, -125, -120, -115, -110, -105, -104, -103, -102, -101, -100, -99, -98, -97, -96, -95, -90, -85, -80, -75, -70,
    -65, -60, -55, -50, -45, -40, -35, -30, -25, -20, -15, -10, -9, -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25,
    30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 110, 115, 120, 125, 130, 135, 140, 145,
    150, 155, 160, 165, 170, 175, 180, 185, 190, 192, 193, 194, 195, 196, 197, 198, 199, 200};

// filter type -> (type, poles); 0 LPF24D 1 LPF24A 2 LPF18 3 LPF18s 4 LPF12+HPF12 5 LPF6+HPF12 6 HPF24D 7 HPF12 8 BPF12D 10 BPFw
// 11 BPF6 12 BEF12 13 BEF6 14 DualLPF 15 DualHPF 16 DualBPF 17 DualBEF 19 LPF12+BPF6 (ConvertWithMoss's map)
bool filter_of(int t, FilterType &type, int &poles) {
    static const int8_t T[20] = {1, 1, 1, 1, 1, 1, 2, 2, 3, -1, 3, 3, 4, 4, 1, 2, 3, 4, -1, 1};
    static const int8_t P[20] = {4, 4, 3, 3, 2, 1, 4, 2, 2, 0, 1, 1, 2, 1, 4, 4, 4, 4, 0, 2};
    if (t < 0 || t >= 20 || T[t] < 0) return false;
    type = T[t] == 1 ? FilterType::LowPass : T[t] == 2 ? FilterType::HighPass : T[t] == 3 ? FilterType::BandPass : FilterType::BandReject;
    poles = P[t];
    return true;
}

struct Reader {
    std::vector<uint8_t> d;
    size_t p = 0;
    uint32_t u32() { if (p + 4 > d.size()) throw ParseError("truncated YSFC performance"); uint32_t v = be32(&d[p]); p += 4; return v; }
    std::vector<uint8_t> block() {
        uint32_t n = u32();
        if (p + n > d.size()) throw ParseError("truncated YSFC performance block");
        std::vector<uint8_t> v(d.begin() + long(p), d.begin() + long(p + n));
        p += n;
        return v;
    }
};

struct Part { std::string name; int sw = 1, kbd = 1, vel_lo = 1, vel_hi = 127, note_lo = 0, note_hi = 127, pb_up = 66, pb_down = 62, detune = 128, shift = 64; std::vector<std::vector<uint8_t>> elements; bool awm = false; };

std::string fixed_name(const std::vector<uint8_t> &b, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && i < b.size() && b[i]; i++) s += b[i] >= 32 && b[i] < 127 ? char(b[i]) : ' ';
    return trim(s);
}

std::vector<Part> read_performance(const std::vector<uint8_t> &data, std::string &name) {
    Reader r{data};
    name = fixed_name(r.block(), 21);
    for (int i = 0; i < 4; i++) r.block();   // reverb, variation, master EQ, master effect
    uint32_t np = r.u32();
    if (np > 64) throw ParseError("bad YSFC performance");
    std::vector<Part> parts(np);
    for (auto &p : parts) {
        std::vector<uint8_t> b = r.block();
        if (b.size() < 41) continue;
        p.name = fixed_name(b, 21);
        p.sw = b[24]; p.kbd = b[25]; p.vel_lo = b[26]; p.vel_hi = b[27]; p.note_lo = b[28]; p.note_hi = b[29];
        p.pb_up = b[30]; p.pb_down = b[31]; p.detune = b[36]; p.shift = b[40];
    }
    r.block(); r.block();   // A/D and digital input parts
    for (auto &p : parts) {
        uint32_t type = r.u32();
        if (type == 0 || type == 1) {
            uint32_t ne = r.u32();
            if (ne > 128) throw ParseError("bad YSFC part");
            p.awm = true;
            for (uint32_t e = 0; e < ne; e++) p.elements.push_back(r.block());
        } else if (type == 2) {   // FM-X: no samples
            uint32_t ops = r.u32();
            r.block();
            for (uint32_t o = 0; o < ops; o++) r.block();
        } else {
            throw ParseError("unknown YSFC part type");
        }
    }
    return parts;
}

// an element's settings on a zone of its waveform (ConvertWithMoss fillParameterValues, see the notes at the top)
void apply_element(Zone &z, const uint8_t *e, const Part &p) {
    if (e[10] == 3) z.play_logic = PlayLogic::RoundRobin;
    else if (e[10] == 4) z.play_logic = PlayLogic::Random;
    else if (e[10] == 2) z.trigger = Trigger::Release;
    else if (e[10] == 1) z.trigger = Trigger::Legato;
    z.exclusive_group = clampi(e[5], 0, 127);
    if (e[3] == 0) z.one_shot = true;
    z.tune += (e[67] - 64) + (e[68] - 64) / 100.0 + (p.shift - 64) + 1200 * std::log2((440 + (p.detune - 128) * 0.1) / 440) / 100;
    z.key_tracking = PITCH_KEY[e[71] & 127] / 100.0;
    z.pan = clampd(z.pan + norm_range(e[6] - 64, 63, 63), -1, 1);
    z.gain_db += e[40] == 0 ? -96 : -95.25 + 2 * e[40] * 0.375;
    z.amp_vel_depth = clampd(norm_range(e[41] - 64, 64, 63), 0, 1);
    z.amp_key_tracking = norm_range(e[66] - 64, 64, 63);
    Envelope &a = z.amp_env;
    a.set = true;
    a.attack = env_t(e[44]) / 6.0;
    a.decay = env_t(e[45]) + env_t(e[46]);
    a.release = env_t(e[48]);
    a.sustain = e[52] / 127.0;
    a.time_vel_tracking = norm_range(e[54] - 64, 64, 63);
    a.time_key_tracking = norm_range(e[55] - 64, 64, 63);
    z.bend_up = (p.pb_up - 64) * 100;
    z.bend_down = (p.pb_down - 64) * 100;
    FilterType ft;
    int poles;
    if (!filter_of(e[91], ft, poles)) return;
    z.filter.type = ft;
    z.filter.poles = poles;
    z.filter.cutoff = std::pow(2.0, e[92] / 255.0 * std::log2(MAX_CUTOFF_HZ));
    z.filter.resonance = e[94] / 127.0;
    z.filter.vel_depth = norm_range(e[112] - 64, 64, 63);
    z.filter.env_depth = norm_range(e[109] - 64, 64, 63);
    Envelope &f = z.filter.env;
    f.set = true;
    f.delay = env_t(e[99]);
    f.attack = env_t(e[100]) / 6.0;
    f.decay = env_t(e[101]) + env_t(e[102]);
    f.release = env_t(e[103]);
    f.start_level = norm_range(e[104] - 128, 128, 127);
    f.sustain = clampd(norm_range(e[107] - 128, 128, 127), 0, 1);
    f.end_level = norm_range(e[108] - 128, 128, 127);
    f.time_vel_tracking = norm_range(e[111] - 64, 64, 63);
    f.time_key_tracking = norm_range(e[114] - 64, 64, 63);
    int kf = int(std::lround((e[124] - 64) * 200.0 / 64));   // ConvertWithMoss's table, -200..200 %
    z.filter.key_tracking = clampd(kf / 100.0, -1, 1);
}

bool clip(Zone &z, int klo, int khi, int vlo, int vhi) {
    if (z.key_hi < klo || z.key_lo > khi || z.vel_hi < vlo || z.vel_lo > vhi) return false;
    z.key_lo = std::max(z.key_lo, klo); z.key_hi = std::min(z.key_hi, khi);
    z.vel_lo = std::max(z.vel_lo, vlo); z.vel_hi = std::min(z.vel_hi, vhi);
    return true;
}

Instrument load_performance(const Ysfc &y, size_t pi, const std::vector<Entry> &ewfm, const std::vector<Entry> &ewim) {
    std::vector<Entry> epfm = y.entries("EPFM");
    if (pi >= epfm.size()) throw ParseError("performance out of range");
    uint32_t size = 0;
    uint64_t at = y.item("DPFM", epfm[pi], size);
    Instrument inst;
    std::vector<Part> parts = read_performance(y.b->bytes(at, size), inst.name);
    if (inst.name.empty()) inst.name = entry_name(epfm[pi].name);
    inst.format = "Yamaha Montage / MODX performance";
    std::map<size_t, std::vector<Zone>> cache;
    int rom = 0, missing = 0;
    // the parts the keyboard plays (part switch and keyboard control on); a performance with none: its first part
    std::vector<size_t> use;
    for (size_t i = 0; i < parts.size(); i++) if (parts[i].awm && parts[i].sw && parts[i].kbd) use.push_back(i);
    if (use.empty()) for (size_t i = 0; i < parts.size(); i++) if (parts[i].awm) { use.push_back(i); break; }
    inst.groups.clear();
    for (size_t pi2 : use) {
        const Part &p = parts[pi2];
        int group = int(inst.groups.size());
        inst.groups.push_back(Group{p.name, Trigger::Attack});
        // XA cycle / random elements: positions in their cycle, shares of the random range
        int cycle_n = 0, random_n = 0;
        for (auto &e : p.elements) if (e.size() >= 146 && e[0] == 1) { cycle_n += e[10] == 3; random_n += e[10] == 4; }
        int cycle_i = 0, random_i = 0;
        for (auto &eb : p.elements) {
            if (eb.size() < 146 || eb[0] != 1) continue;
            if (eb[1] == 0) { rom++; continue; }   // a preset (ROM) waveform
            size_t wn = le16(&eb[eb.size() - 2]);
            if (wn < 1 || wn > ewfm.size()) { missing++; continue; }
            auto it = cache.find(wn - 1);
            if (it == cache.end()) {
                std::string nm;
                it = cache.emplace(wn - 1, waveform_zones(y, wn - 1, ewfm, ewim, nm)).first;
            }
            const uint8_t *e = eb.data();
            int pos = e[10] == 3 ? ++cycle_i : e[10] == 4 ? ++random_i : 0;
            for (const Zone &wz : it->second) {
                Zone z = wz;
                if (!clip(z, e[11], e[12], std::max(1, int(e[13])), e[14]) || !clip(z, p.note_lo, p.note_hi, std::max(1, p.vel_lo), p.vel_hi)) continue;
                apply_element(z, e, p);
                if (z.play_logic == PlayLogic::RoundRobin) { z.seq_length = std::max(1, cycle_n); z.seq_position = pos; }
                if (z.play_logic == PlayLogic::Random) { z.rand_lo = double(pos - 1) / random_n; z.rand_hi = double(pos) / random_n; }
                z.group = group;
                inst.zones.push_back(z);
            }
        }
    }
    if (inst.groups.empty()) inst.groups.push_back(Group{});
    if (rom) inst.warnings.push_back(std::to_string(rom) + " elements use the keyboard's preset waveforms (not in the library)");
    if (missing) inst.warnings.push_back(std::to_string(missing) + " elements point at missing waveforms");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError(rom ? "this performance only uses the keyboard's preset waveforms" : "this performance plays no waveforms of the library");
    return inst;
}

// --- YSFC reader ---------------------------------------------------------------------------------------------------

bool probe_ysfc(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 16 && !std::memcmp(h, "YAMAHA-YSFC", 11); }

// presets: the performances (Montage / MODX), then the waveforms
std::vector<PresetInfo> list_ysfc(VolumePtr vol, const std::string &path) {
    Ysfc y(vol->open(path));
    std::vector<PresetInfo> out;
    int i = 0;
    // a performance is listed when it plays at least one waveform of the library (an expansion library's performances
    // often use only the keyboard's preset waveforms: nothing of them can play here)
    if (y.performances()) {
        size_t nw = y.entries("EWFM").size();
        for (auto &e : y.entries("EPFM")) {
            bool plays = false;
            try {
                uint32_t size = 0;
                uint64_t at = y.item("DPFM", e, size);
                std::string nm;
                for (auto &p : read_performance(y.b->bytes(at, size), nm))
                    for (auto &el : p.elements)
                        if (el.size() >= 146 && el[0] == 1 && el[1] != 0) {
                            size_t wn = le16(&el[el.size() - 2]);
                            plays = plays || (wn >= 1 && wn <= nw);
                        }
            } catch (const ParseError &) {}
            if (plays) out.push_back(PresetInfo{entry_name(e.name), i});
            i++;
        }
    }
    bool perf = !out.empty();
    int w = 0;
    for (auto &e : y.entries("EWFM")) out.push_back(PresetInfo{entry_name(e.name) + (perf ? " (waveform)" : ""), 100000 + w++});
    return out;
}

Instrument load_ysfc(VolumePtr vol, const std::string &path, int index) {
    Ysfc y(vol->open(path));
    std::vector<Entry> ewfm = y.entries("EWFM"), ewim = y.entries("EWIM");
    if (ewfm.empty()) throw ParseError("this Yamaha file holds no waveforms (samples)");
    if (index < 100000) return load_performance(y, size_t(index), ewfm, ewim);
    Instrument inst;
    inst.zones = waveform_zones(y, size_t(index - 100000), ewfm, ewim, inst.name);
    inst.format = y.ver < 102 ? "Yamaha Motif XS" : y.ver == 102 ? "Yamaha Motif XF" : y.ver == 103 ? "Yamaha MOXF"
                : y.ver < 410 ? "Yamaha Montage" : y.ver < 500 ? "Yamaha Montage M" : "Yamaha MODX";
    inst.format += " waveform";
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("this waveform has no samples");
    return inst;
}

// --- Motif ES (.W7A + .W8A) ----------------------------------------------------------------------------------------

bool probe_es(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    return n >= 0x80 && !std::memcmp(h + 0x30, "YSFC", 4) && !std::memcmp(h + 0x10, "Ver ", 4) && h[0] == 'A';
}

uint32_t hex_digits(const uint8_t *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v << 4 | (p[i] & 15);
    return v;
}

struct EsFiles { BlobPtr w7, w8; std::vector<std::string> names; std::vector<std::pair<uint32_t, uint32_t>> table; };

EsFiles open_es(VolumePtr vol, const std::string &path) {
    EsFiles f;
    std::string stem = path_join(path_dir(path), path_stem(path)), found;
    BlobPtr b = vol->open(path);
    std::vector<uint8_t> h = b->head(0x40);
    bool is_sample = h.size() >= 12 && !std::memcmp(&h[6], "SAMPLE", 6);
    if (is_sample) {
        f.w8 = b;
        for (const char *e : {".w7a", ".w7v", ".w7w"}) if (find_ci(*vol, stem + e, found)) { f.w7 = vol->open(found); break; }
    } else {
        f.w7 = b;
        if (!find_ci(*vol, stem + ".w8a", found) && !find_ci(*vol, stem + ".w8w", found))
            throw ParseError("the samples file (" + path_stem(path) + ".W8A) is not next to it");
        f.w8 = vol->open(found);
    }
    std::vector<uint8_t> t = f.w8->bytes(0x200, 0x200);
    for (size_t i = 0; i + 8 <= t.size(); i += 16) {
        uint32_t off = be32(&t[i]), cnt = be32(&t[i + 4]);
        if (off == 0xFFFFFFFF || !off) break;
        f.table.push_back({off, cnt});
    }
    if (f.w7) {   // names: the W7A's EWFM entries (32 bytes each, after a 32-byte header)
        std::vector<uint8_t> cat = f.w7->bytes(0x80, 0x180);
        for (size_t i = 0; i + 8 <= cat.size(); i += 8) {
            if (std::memcmp(&cat[i], "EWFM", 4)) continue;
            uint64_t o = be32(&cat[i + 4]);
            uint32_t len = be32(f.w7->bytes(o + 4, 4).data());
            std::vector<uint8_t> d = f.w7->bytes(o + 32, std::max<uint32_t>(len, 32) - 32);
            for (size_t k = 0; k + 32 <= d.size(); k += 32) f.names.push_back(fixed_name(std::vector<uint8_t>(d.begin() + long(k), d.begin() + long(k + 16)), 16));
            break;
        }
    }
    for (size_t i = f.names.size(); i < f.table.size(); i++) f.names.push_back("Waveform " + std::to_string(i + 1));
    for (auto &n : f.names) { if (n.size() > 4 && std::isdigit((unsigned char)n[0]) && n[3] == ' ') n = trim(n.substr(4)); }
    if (f.table.empty()) throw ParseError("no waveforms in the Motif ES samples file");
    return f;
}

std::vector<PresetInfo> list_es(VolumePtr vol, const std::string &path) {
    EsFiles f = open_es(vol, path);
    std::vector<PresetInfo> out;
    for (size_t i = 0; i < f.table.size(); i++) out.push_back(PresetInfo{f.names[i], int(i)});
    return out;
}

Instrument load_es(VolumePtr vol, const std::string &path, int index) {
    EsFiles f = open_es(vol, path);
    if (index < 0 || size_t(index) >= f.table.size()) throw ParseError("waveform out of range");
    Instrument inst;
    inst.name = f.names[size_t(index)];
    inst.format = "Yamaha Motif ES waveform";
    uint64_t o = f.table[size_t(index)].first;
    uint32_t n = f.table[size_t(index)].second;
    struct Rec { uint64_t audio; int64_t frames; std::vector<uint8_t> h; };
    std::vector<Rec> recs;
    for (uint32_t i = 0; i < n && o + 0x50 <= f.w8->size(); i++) {
        std::vector<uint8_t> h = f.w8->bytes(o, 0x50);
        uint32_t size = be32(&h[0]);
        if (o + 0x50 + size > f.w8->size()) throw ParseError("truncated Motif ES sample");
        recs.push_back({o + 0x50, int64_t(size / 2), h});
        o += (uint64_t(size) + 0x50 + 15) / 16 * 16;
    }
    for (size_t i = 0; i < recs.size(); i++) {
        const Rec &r = recs[i];
        const uint8_t *h = r.h.data();
        Zone z;
        z.name = inst.name + " " + std::to_string(h[0x11]);
        z.root = h[0x11];
        z.tune = int8_t(h[0x13]) / 100.0;
        z.key_lo = h[0x31]; z.key_hi = h[0x33];
        z.vel_lo = h[0x34]; z.vel_hi = h[0x35];
        uint32_t ls = hex_digits(h + 0x38, 8), ll = hex_digits(h + 0x40, 8);
        if (ll > 1 && ls + ll <= r.frames) {
            Loop l;
            l.start = ls; l.end = int64_t(ls) + ll - 1;
            z.loops.push_back(l);
        }
        // a stereo pair: this record marked first, the next one the same keys and length
        bool pair = h[0x4B] == 1 && i + 1 < recs.size() && recs[i + 1].frames == r.frames && recs[i + 1].h[0x31] == h[0x31] && recs[i + 1].h[0x33] == h[0x33];
        if (pair) {
            z.sample = stereo_ref(f.w8, r.audio, recs[i + 1].audio, r.frames, 44100, z.name, true);
            i++;
        } else {
            z.sample = raw_sample_ref(f.w8, r.audio, r.frames, 1, Enc::S16BE, 44100, z.name);
            z.pan = norm_range(int(h[0x4A]) - 64, 63, 63);
        }
        inst.zones.push_back(z);
    }
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("this waveform has no samples");
    return inst;
}

}  // namespace

void register_yamaha() {
    register_reader({"Yamaha Motif / MOXF / Montage / MODX library", "x0a x0w x0v x3a x3w x3v x6a x6w x6v x7u x7l x7a x8u x8l x8a y2l y2u",
                     probe_ysfc, list_ysfc, load_ysfc});
    register_reader({"Yamaha Motif ES library", "w7a w7v w7w w8a w8w", probe_es, list_es, load_es});
}

}  // namespace omni

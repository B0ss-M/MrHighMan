// Omni Sampler: tracker instruments, and the instruments inside tracker modules (the songs are not played): FastTracker 2
// .xi / .xm, Impulse Tracker .iti / .its / .it, Scream Tracker 3 .s3m, ProTracker .mod (2.1.1). An instrument is one preset (in a module without
// instruments, a sample is): its note-to-sample map becomes key zones, its volume envelope an ADSR fit, its fade-out
// the release. Layouts from OpenMPT's soundlib (XMTools.h, ITTools.h, S3MTools.h, ITCompression.cpp, Sndmix.cpp;
// BSD-3-Clause; facts only, own code):
//   - Notes: XM note 0 is C-0 and C-4 (48) is MIDI 60; IT keyboard note 60 (C-5) is MIDI 60; S3M samples play at C-5.
//   - Pitch: an XM sample plays 8363 Hz at C-4, moved by its relative note + finetune / 128 semitones; IT and S3M
//     samples play their C5Speed at MIDI 60.
//   - XM sample data is delta coded, 8 or 16 bit, stereo = all left then all right; ModPlug's ADPCM (reserved 0xAD,
//     also IT cvt 0xFF) is a 16-entry delta table then 4-bit codes, low nibble first. IT: signed or unsigned, big-endian
//     and delta by cvt, or IT 2.14 / 2.15 compressed: per channel, blocks of a u16 size and LSB-first bits decoding
//     at most 0x8000 bytes each, with 1-6 / 7-8(16) / 9(17)-bit codes, 2.15 integrating twice. S3M: unsigned
//     (format 2) or signed (format 1).
//   - Envelopes advance one node tick per tick, 2.5 / BPM seconds; fade-out lowers 65536 by 2 x the instrument's fade
//     a tick (XM: the file's value; IT: x 32, old IT instruments x 64). XM without a volume envelope cuts at key-off.
//   - Auto-vibrato: position + rate a tick, 256 to the cycle; depth in 1/64 semitone; sweep ramps it in.
//   - ProTracker .mod (2.1.1; layout from OpenMPT's Load_mod.cpp, facts only): a 20-byte title, 31 sample headers of
//     30 bytes (name 22, length in words BE, finetune low nibble -8..7 in 1/8 semitone, volume 0..64, loop start and
//     loop length in words BE, length 1 = no loop), song length, restart, a 128-byte order list, the 4-byte tag at
//     1080 (M.K. M!K! M&K! N.T. FLT4 4CHN = 4 channels; FLT8 CD81 OKTA OCTA = 8; xCHN; xxCH / xxCN; TDZx), patterns of
//     64 rows x channels x 4 bytes (as many as the highest order + 1), then the samples, 8-bit signed. A sample plays
//     8363 Hz at C-2 (period 428) = MIDI 60, as trackers import it. Very old modules give the loop start in bytes.
//     15-sample Soundtracker modules (no tag) are not read.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include "format.hpp"

namespace omni {
namespace {

enum class Codec { Pcm, Delta, Adpcm, It214, It215, None };

struct TSample {
    std::string name;
    uint64_t offset = 0;
    int64_t frames = 0;
    int bits = 8, channels = 1;
    bool is_signed = true, big_endian = false;
    Codec codec = Codec::Pcm;
    double rate = 8363;                          // Hz at MIDI 60
    bool loop = false, sustain = false;          // sustain: loops only while the key is held
    LoopType loop_type = LoopType::Forward;
    int64_t loop_start = 0, loop_end = 0;        // inclusive
    double gain_db = 0;
    bool has_pan = false;
    double pan = 0;
    LfoWave vib_wave = LfoWave::Sine;
    int vib_rate = 0, vib_depth = 0, vib_sweep = 0;
    bool it_sweep = false;                       // IT: depth grows by sweep / 256 a tick; XM: full after sweep ticks
};

struct TInstr {
    std::string name;
    int sample[128], note[128];                  // per MIDI key: index into Module::samples (-1 = none), note played
    std::vector<std::pair<int, int>> env;        // tick, level 0..64
    bool env_on = false, cut = false;            // cut: no envelope, key-off silences the note (XM, S3M)
    int env_sus = -1;                            // the node the envelope holds at while the key is down
    double fade_ticks = 0;                       // 0 = no fade-out
    double gain_db = 0;
    bool has_pan = false;
    double pan = 0;
    double cutoff_hz = 0, resonance = 0;         // 0 = no filter
    TInstr() { std::fill(std::begin(sample), std::end(sample), -1); std::fill(std::begin(note), std::end(note), 0); }
};

struct Module {
    const char *format = "";
    std::vector<TSample> samples;
    std::vector<TInstr> instrs;
    std::vector<int> numbers;                    // each instrument's number in the file (for the preset names)
    double tick = 0.02;                          // seconds
};

double level_db(double v) { return v > 0 ? 20.0 * std::log10(v) : 0.0; }   // a default volume of 0 plays at full
std::string text(const uint8_t *p, size_t n) {
    size_t len = 0;
    while (len < n && p[len]) len++;
    std::string s(reinterpret_cast<const char *>(p), len);
    for (char &c : s) if (static_cast<unsigned char>(c) < 32) c = ' ';
    return trim(s);
}
std::vector<uint8_t> at(const BlobPtr &b, uint64_t off, size_t n) {   // zero-filled past the end
    std::vector<uint8_t> v = b->head(n, off);
    v.resize(n, 0);
    return v;
}
void key_range(TInstr &ins, int sample) { for (int k = 0; k < 128; k++) { ins.sample[k] = sample; ins.note[k] = k; } }

// ---- sample data

struct Bits {
    const std::vector<uint8_t> &d;
    size_t pos = 0;
    uint32_t buf = 0;
    int n = 0;
    struct End {};
    uint32_t get(int w) {
        while (n < w) {
            if (pos >= d.size()) throw End();
            buf |= uint32_t(d[pos++]) << n;
            n += 8;
        }
        uint32_t v = buf & ((1u << w) - 1);
        buf >>= w;
        n -= w;
        return v;
    }
};

void it_decompress(const BlobPtr &blob, const TSample &s, Pcm &p) {
    const bool b16 = s.bits == 16, it215 = s.codec == Codec::It215;
    const int def = b16 ? 17 : 9, fetch = b16 ? 4 : 3, lower = b16 ? -8 : -4, upper = b16 ? 7 : 3;
    const uint32_t mask = b16 ? 0xFFFF : 0xFF;
    const int64_t per_block = 0x8000 / (b16 ? 2 : 1);
    uint64_t pos = s.offset, end = blob->size();
    for (int c = 0; c < s.channels; c++) {
        int64_t written = 0;
        while (written < s.frames && pos + 2 <= end) {
            std::vector<uint8_t> len = blob->bytes(pos, 2);
            size_t size = le16(len.data());
            pos += 2;
            if (!size) continue;
            std::vector<uint8_t> block = blob->head(size, pos);
            pos += size;
            Bits bits{block};
            uint32_t mem1 = 0, mem2 = 0;
            int64_t left = std::min(s.frames - written, per_block);
            int width = def;
            auto write = [&](int v, int top) {
                if (v & top) v -= top << 1;
                mem1 = (mem1 + uint32_t(v)) & mask;
                mem2 = (mem2 + mem1) & mask;
                uint32_t u = it215 ? mem2 : mem1;
                int16_t out = b16 ? int16_t(uint16_t(u)) : int16_t(int8_t(uint8_t(u)) * 256);
                p.data[size_t(written * s.channels + c)] = out;
                written++;
                left--;
            };
            auto change = [&](int w) { w++; if (w >= width) w++; width = w; };
            try {
                while (left > 0 && width <= def) {
                    int v = int(bits.get(width));
                    int top = 1 << (width - 1);
                    if (width <= 6) {
                        if (v == top) change(int(bits.get(fetch)));
                        else write(v, top);
                    } else if (width < def) {
                        if (v >= top + lower && v <= top + upper) change(v - (top + lower));
                        else write(v, top);
                    } else {
                        if (v & top) width = (v & ~top) + 1;
                        else write(v & ~top, 0);
                    }
                }
            } catch (const Bits::End &) {}   // a short block: the rest stays silent
        }
    }
}

PcmPtr decode(const BlobPtr &blob, const TSample &s) {
    auto p = std::make_shared<Pcm>();
    p->rate = int(std::lround(s.rate));
    p->channels = s.channels;
    p->data.assign(size_t(s.frames * s.channels), 0);
    if (s.codec == Codec::It214 || s.codec == Codec::It215) { it_decompress(blob, s, *p); return p; }
    if (s.codec == Codec::Adpcm) {
        std::vector<uint8_t> raw = at(blob, s.offset, size_t(16 + (s.frames + 1) / 2));
        int8_t acc = 0;
        for (int64_t i = 0; i < s.frames; i++) {
            uint8_t b = raw[size_t(16 + i / 2)];
            acc = int8_t(acc + int8_t(raw[(i & 1) ? b >> 4 : b & 15]));
            p->data[size_t(i)] = int16_t(acc * 256);
        }
        return p;
    }
    const size_t bps = size_t(s.bits / 8);
    std::vector<uint8_t> raw = at(blob, s.offset, size_t(s.frames) * size_t(s.channels) * bps);
    const bool delta = s.codec == Codec::Delta;
    for (int c = 0; c < s.channels; c++) {
        int acc = 0;
        const uint8_t *src = raw.data() + size_t(c) * size_t(s.frames) * bps;
        for (int64_t i = 0; i < s.frames; i++) {
            int v;
            if (bps == 1) {
                uint8_t u = src[i];
                if (!s.is_signed) u ^= 0x80;
                v = int8_t(u);
                if (delta) v = acc = int8_t(acc + v);
                v *= 256;
            } else {
                const uint8_t *q = src + size_t(i) * 2;
                uint16_t u = s.big_endian ? be16(q) : le16(q);
                if (!s.is_signed) u ^= 0x8000;
                v = int16_t(u);
                if (delta) v = acc = int16_t(acc + v);
            }
            p->data[size_t(i * s.channels + c)] = int16_t(v);
        }
    }
    return p;
}

// ---- FastTracker 2

// The 230-byte instrument block XM and XI share: note map, envelopes, auto-vibrato, fade-out
void xm_instrument(const uint8_t *b, int first, int count, TInstr &ins, std::vector<TSample> &samples) {
    int n = std::min<int>(b[192], 12);
    for (int i = 0; i < n; i++) {
        int tick = le16(b + 96 + i * 4), value = le16(b + 98 + i * 4);
        if (i > 0 && tick < ins.env.back().first && !(tick & 0xFF00)) {   // some editors saved only the low byte
            tick |= ins.env.back().first & 0xFF00;
            if (tick < ins.env.back().first) tick += 0x100;
        }
        ins.env.push_back({tick, std::min(value, 64)});
    }
    int flags = b[200];
    ins.env_on = (flags & 1) && n > 0;
    ins.env_sus = (flags & 2) && b[194] < n ? b[194] : (flags & 4) && b[196] < n ? b[196] : -1;
    ins.cut = !ins.env_on;
    int fade = le16(b + 206);
    ins.fade_ticks = fade ? 32768.0 / fade : 0;
    for (int k = 0; k < 128; k++) {
        int i = clampi(k - 12, 0, 95);   // FT2's notes are C-0 .. B-7 (MIDI 12-107): the outer keys take the edge samples
        int s = b[i] < count ? b[i] : 0;
        ins.sample[k] = first + s;
        ins.note[k] = k;
    }
    static const LfoWave waves[4] = {LfoWave::Sine, LfoWave::Square, LfoWave::SawDown, LfoWave::SawUp};
    for (int s = first; s < first + count; s++) {
        samples[size_t(s)].vib_wave = waves[b[202] & 3];
        samples[size_t(s)].vib_sweep = b[203];
        samples[size_t(s)].vib_depth = b[204];
        samples[size_t(s)].vib_rate = b[205];
    }
}

// count 40-byte sample headers at hdr, their data from data on; returns the end of the data
uint64_t xm_samples(const BlobPtr &blob, uint64_t hdr, int count, uint64_t data, std::vector<TSample> &out) {
    std::vector<uint8_t> h = at(blob, hdr, size_t(count) * 40);
    for (int i = 0; i < count; i++) {
        const uint8_t *q = h.data() + i * 40;
        TSample s;
        uint32_t length = le32(q), lstart = le32(q + 4), llen = le32(q + 8);
        int flags = q[14];
        s.name = text(q + 18, 22);
        s.bits = flags & 0x10 ? 16 : 8;
        s.channels = flags & 0x20 ? 2 : 1;
        int frame = s.bits / 8 * s.channels;
        s.codec = q[17] == 0xAD && !(flags & 0x30) ? Codec::Adpcm : Codec::Delta;
        s.offset = data;
        s.frames = length / uint32_t(frame);
        data += s.codec == Codec::Adpcm ? 16 + (uint64_t(length) + 1) / 2 : length;
        if (s.codec == Codec::Adpcm) s.frames = length;
        if (s.frames > 0 && blob->head(4, s.offset) == std::vector<uint8_t>{'O', 'g', 'g', 'S'}) s.codec = Codec::None;   // OggMod
        s.rate = 8363.0 * std::pow(2.0, (int8_t(q[16]) + int8_t(q[13]) / 128.0) / 12.0);
        s.gain_db = level_db(std::min<int>(q[12], 64) / 64.0);
        s.has_pan = true;
        s.pan = (q[15] - 128) / 128.0;
        int64_t ls = lstart / uint32_t(frame), le = (int64_t(lstart) + llen) / frame;
        if ((flags & 3) && le > ls && le <= s.frames) {
            s.loop = true;
            s.loop_type = flags & 2 ? LoopType::Alternating : LoopType::Forward;
            s.loop_start = ls;
            s.loop_end = le - 1;
        }
        out.push_back(s);
    }
    return data;
}

Module parse_xi(const BlobPtr &blob) {
    std::vector<uint8_t> h = at(blob, 0, 298);
    if (std::memcmp(h.data(), "Extended Instrument: ", 21)) throw ParseError("not a FastTracker 2 instrument");
    Module m;
    m.format = "FastTracker 2 instrument";
    int count = std::min<int>(le16(h.data() + 296), 128);
    TInstr ins;
    ins.name = text(h.data() + 21, 22);
    xm_samples(blob, 298, count, 298 + uint64_t(count) * 40, m.samples);
    xm_instrument(h.data() + 66, 0, count, ins, m.samples);
    if (count) { m.instrs.push_back(ins); m.numbers.push_back(1); }
    return m;
}

Module parse_xm(const BlobPtr &blob) {
    std::vector<uint8_t> h = at(blob, 0, 80);
    if (std::memcmp(h.data(), "Extended Module: ", 17)) throw ParseError("not a FastTracker 2 module");
    int version = le16(h.data() + 58);
    if (version < 0x104) throw ParseError("XM version " + std::to_string(version >> 8) + ".0" + std::to_string(version & 0xFF) + " is not supported (1.04 only)");
    Module m;
    m.format = "FastTracker 2 module";
    int patterns = le16(h.data() + 70), instruments = le16(h.data() + 72), bpm = le16(h.data() + 78);
    if (bpm >= 32) m.tick = 2.5 / bpm;
    uint64_t pos = 60 + uint64_t(le32(h.data() + 60)), end = blob->size();
    for (int p = 0; p < patterns && pos + 9 <= end; p++) {
        std::vector<uint8_t> ph = blob->bytes(pos, 9);
        uint32_t len = le32(ph.data());
        if (len < 8) break;
        pos += len + le16(ph.data() + 7);
    }
    for (int i = 1; i <= instruments && pos + 4 <= end; i++) {
        uint32_t len = le32(blob->bytes(pos, 4).data());
        if (!len) len = 263;
        std::vector<uint8_t> ih = at(blob, pos, std::max<size_t>(len, 263));
        if (len < 263) std::fill(ih.begin() + len, ih.end(), 0);
        int count = le16(ih.data() + 27);
        pos += len;
        if (!count) continue;
        if (count > 128) throw ParseError("damaged XM: instrument " + std::to_string(i) + " lists " + std::to_string(count) + " samples");
        TInstr ins;
        ins.name = text(ih.data() + 4, 22);
        int first = int(m.samples.size());
        pos = xm_samples(blob, pos, count, pos + uint64_t(count) * 40, m.samples);
        xm_instrument(ih.data() + 33, first, count, ins, m.samples);
        m.instrs.push_back(ins);
        m.numbers.push_back(i);
    }
    return m;
}

// ---- Impulse Tracker

TSample it_sample(const BlobPtr &blob, uint64_t off, bool stereo_ok) {
    std::vector<uint8_t> h = at(blob, off, 80);
    const uint8_t *q = h.data();
    TSample s;
    s.name = text(q + 20, 26);
    if (s.name.empty()) s.name = text(q + 4, 13);
    int flags = q[18], cvt = q[46];
    s.frames = (flags & 1) ? int64_t(le32(q + 48)) : 0;
    s.bits = flags & 2 ? 16 : 8;
    s.channels = (flags & 4) && stereo_ok ? 2 : 1;
    s.offset = le32(q + 72);
    if (flags & 8) s.codec = cvt & 4 ? Codec::It215 : Codec::It214;
    else if (s.bits == 8 && cvt == 0xFF) { s.codec = Codec::Adpcm; s.channels = 1; }
    else if (cvt == 0x40 || cvt == 0x80) s.codec = Codec::None;   // OpenMPT's OPL instruments and external samples
    else {
        s.is_signed = cvt & 1;
        s.big_endian = cvt & 2;
        s.codec = cvt & 4 ? Codec::Delta : Codec::Pcm;
    }
    uint32_t c5 = le32(q + 60);
    s.rate = c5 ? std::max<uint32_t>(c5, 256) : 8363;
    s.gain_db = level_db(std::min<int>(q[17], 64) / 64.0 * std::min<int>(q[19], 64) / 64.0);
    if (q[47] & 0x80) { s.has_pan = true; s.pan = clampd(((q[47] & 0x7F) - 32) / 32.0, -1, 1); }
    auto loop = [&](uint32_t a, uint32_t b, bool bidi, bool sus) {
        if (s.loop || b <= a || b > s.frames) return;
        s.loop = true; s.sustain = sus;
        s.loop_type = bidi ? LoopType::Alternating : LoopType::Forward;
        s.loop_start = a; s.loop_end = int64_t(b) - 1;
    };
    if (flags & 0x20) loop(le32(q + 64), le32(q + 68), flags & 0x80, true);   // the sustain loop first: Omni plays one
    if (flags & 0x10) loop(le32(q + 52), le32(q + 56), flags & 0x40, false);
    static const LfoWave waves[4] = {LfoWave::Sine, LfoWave::SawDown, LfoWave::Square, LfoWave::Random};
    s.vib_rate = q[76]; s.vib_depth = q[77] & 0x7F; s.vib_sweep = q[78]; s.vib_wave = waves[q[79] & 3];
    s.it_sweep = true;
    return s;
}

// An IMPI instrument at off; sample numbers 1.. map to samples[first + n - 1]
TInstr it_instrument(const BlobPtr &blob, uint64_t off, bool old, int first, int count) {
    std::vector<uint8_t> h = at(blob, off, 554);
    const uint8_t *q = h.data();
    if (std::memcmp(q, "IMPI", 4)) throw ParseError("damaged IT instrument");
    TInstr ins;
    ins.name = text(q + 32, 26);
    if (ins.name.empty()) ins.name = text(q + 4, 13);
    for (int k = 0; k < 120; k++) {
        int note = q[64 + k * 2], smp = q[65 + k * 2];
        if (smp > 0 && smp <= count && note < 120) { ins.sample[k] = first + smp - 1; ins.note[k] = note; }
    }
    if (old) {
        int flags = q[17];
        for (int i = 0; i < 25 && q[504 + i * 2] != 0xFF; i++) ins.env.push_back({q[504 + i * 2], std::min<int>(q[505 + i * 2], 64)});
        int n = int(ins.env.size());
        ins.env_on = (flags & 1) && n > 0;
        ins.env_sus = (flags & 4) && q[20] < n ? q[20] : (flags & 2) && q[19] < n ? q[19] : -1;
        int fade = le16(q + 24);
        ins.fade_ticks = fade ? 512.0 / fade : 0;
        return ins;
    }
    const uint8_t *e = q + 304;
    int n = std::min<int>(e[1], 25);
    for (int i = 0; i < n; i++) ins.env.push_back({le16(e + 7 + i * 3), clampi(int8_t(e[6 + i * 3]), 0, 64)});
    ins.env_on = (e[0] & 1) && n > 0;
    ins.env_sus = (e[0] & 4) && e[4] < n ? e[4] : (e[0] & 2) && e[3] < n ? e[3] : -1;
    int fade = le16(q + 20);
    ins.fade_ticks = fade ? 1024.0 / fade : 0;
    ins.gain_db = level_db(std::min<int>(q[24], 128) / 128.0);
    if (!(q[25] & 0x80)) { ins.has_pan = true; ins.pan = clampd((q[25] - 32) / 32.0, -1, 1); }
    int cutoff = q[58] & 0x7F, res = q[59] & 0x80 ? q[59] & 0x7F : 0;
    if ((q[58] & 0x80) && !(cutoff == 127 && res == 0)) {
        ins.cutoff_hz = 110.0 * std::pow(2.0, 0.25 + cutoff / 24.0);
        ins.resonance = res / 127.0 * 24.0 / MAX_RESONANCE_DB;   // IT's resonance reaches about 24 dB
    }
    return ins;
}

Module parse_it(const BlobPtr &blob) {
    std::vector<uint8_t> h = at(blob, 0, 192);
    if (std::memcmp(h.data(), "IMPM", 4)) throw ParseError("not an Impulse Tracker module");
    Module m;
    m.format = "Impulse Tracker module";
    int orders = le16(h.data() + 32), instruments = le16(h.data() + 34), nsamples = le16(h.data() + 36);
    int cwtv = le16(h.data() + 40), cmwt = le16(h.data() + 42), flags = le16(h.data() + 44), tempo = h[51];
    if (tempo >= 32) m.tick = 2.5 / tempo;
    std::vector<uint8_t> offs = at(blob, 192 + uint64_t(orders), size_t(instruments + nsamples) * 4);
    for (int i = 0; i < nsamples; i++)
        m.samples.push_back(it_sample(blob, le32(offs.data() + (instruments + i) * 4), cwtv >= 0x214));
    if ((flags & 4) && instruments) {
        for (int i = 0; i < instruments; i++) {
            uint32_t o = le32(offs.data() + i * 4);
            if (!o) continue;
            m.instrs.push_back(it_instrument(blob, o, cmwt < 0x200, 0, nsamples));
            m.numbers.push_back(i + 1);
        }
    } else {   // sample mode: each sample is a preset
        for (int i = 0; i < nsamples; i++) {
            TInstr ins;
            ins.name = m.samples[size_t(i)].name;
            key_range(ins, i);
            m.instrs.push_back(ins);
            m.numbers.push_back(i + 1);
        }
    }
    return m;
}

Module parse_iti(const BlobPtr &blob) {
    std::vector<uint8_t> h = at(blob, 0, 554);
    if (std::memcmp(h.data(), "IMPI", 4)) throw ParseError("not an Impulse Tracker instrument");
    int count = h[30];
    for (int k = 0; k < 120; k++) count = std::max<int>(count, h[65 + k * 2]);   // old Schism Tracker wrote nos = 0
    Module m;
    m.format = "Impulse Tracker instrument";
    for (int i = 0; i < count; i++) m.samples.push_back(it_sample(blob, 554 + uint64_t(i) * 80, true));
    m.instrs.push_back(it_instrument(blob, 0, false, 0, count));
    m.numbers.push_back(1);
    return m;
}

Module parse_its(const BlobPtr &blob) {
    if (blob->head(4) != std::vector<uint8_t>{'I', 'M', 'P', 'S'}) throw ParseError("not an Impulse Tracker sample");
    Module m;
    m.format = "Impulse Tracker sample";
    m.samples.push_back(it_sample(blob, 0, true));
    TInstr ins;
    ins.name = m.samples[0].name;
    key_range(ins, 0);
    m.instrs.push_back(ins);
    m.numbers.push_back(1);
    return m;
}

// ---- Scream Tracker 3

Module parse_s3m(const BlobPtr &blob) {
    std::vector<uint8_t> h = at(blob, 0, 96);
    if (std::memcmp(h.data() + 44, "SCRM", 4)) throw ParseError("not a Scream Tracker 3 module");
    Module m;
    m.format = "Scream Tracker 3 module";
    int orders = le16(h.data() + 32), nsamples = le16(h.data() + 34), cwtv = le16(h.data() + 40);
    bool is_signed = le16(h.data() + 42) == 1, st3 = (cwtv & 0xF000) == 0x1000;
    if (h[50] >= 32) m.tick = 2.5 / h[50];
    std::vector<uint8_t> paras = at(blob, 96 + uint64_t(orders), size_t(nsamples) * 2);
    for (int i = 0; i < nsamples; i++) {
        std::vector<uint8_t> sh = at(blob, uint64_t(le16(paras.data() + i * 2)) * 16, 80);
        const uint8_t *q = sh.data();
        TSample s;
        s.name = text(q + 48, 28);
        if (s.name.empty()) s.name = text(q + 1, 12);
        int flags = q[31];
        s.frames = q[0] == 1 ? int64_t(le32(q + 16)) : 0;   // 1 = PCM (2 = AdLib)
        s.bits = flags & 4 ? 16 : 8;
        s.channels = flags & 2 ? 2 : 1;
        s.is_signed = is_signed;
        s.offset = (uint64_t(q[14]) << 4) | (uint64_t(q[15]) << 12) | (uint64_t(q[13]) << 20);
        if (q[30] == 4 && s.bits == 8 && s.channels == 1) s.codec = Codec::Adpcm;
        uint32_t c5 = le32(q + 32);
        if (st3) c5 = std::min<uint32_t>(c5, 0xFFFF);
        s.rate = c5 ? std::max<uint32_t>(c5, 1024) : 8363;
        s.gain_db = level_db(std::min<int>(q[28], 64) / 64.0);
        uint32_t ls = le32(q + 20), lend = std::min<uint32_t>(le32(q + 24), uint32_t(s.frames));
        if ((flags & 1) && lend > ls + 1) { s.loop = true; s.loop_start = ls; s.loop_end = int64_t(lend) - 1; }
        m.samples.push_back(s);
        TInstr ins;
        ins.name = s.name;
        ins.cut = true;
        key_range(ins, i);
        m.instrs.push_back(ins);
        m.numbers.push_back(i + 1);
    }
    return m;
}

// ---- ProTracker (2.1.1)

int mod_channels(const uint8_t *t) {
    auto is = [&](const char *m) { return std::memcmp(t, m, 4) == 0; };
    auto dig = [](uint8_t c) { return c >= '0' && c <= '9'; };
    if (is("M.K.") || is("M!K!") || is("M&K!") || is("N.T.") || is("FLT4") || is("4CHN")) return 4;
    if (is("FLT8") || is("CD81") || is("OKTA") || is("OCTA")) return 8;
    if (dig(t[0]) && !std::memcmp(t + 1, "CHN", 3)) return t[0] - '0';
    if (dig(t[0]) && dig(t[1]) && (!std::memcmp(t + 2, "CH", 2) || !std::memcmp(t + 2, "CN", 2))) return (t[0] - '0') * 10 + t[1] - '0';
    if (!std::memcmp(t, "TDZ", 3) && dig(t[3])) return t[3] - '0';
    return 0;
}

Module parse_mod(const BlobPtr &blob) {
    std::vector<uint8_t> h = at(blob, 0, 1084);
    int chans = mod_channels(h.data() + 1080);
    if (chans < 1 || chans > 32) throw ParseError("not a ProTracker module (15-sample Soundtracker modules are not read)");
    Module m;
    m.format = "ProTracker module";
    int song_len = std::min<int>(h[950], 128), most = 0, most_song = 0;
    for (int i = 0; i < 128; i++) {
        int o = h[952 + i];
        if (o >= 128) continue;   // marker values some editors leave past the song
        most = std::max(most, o);
        if (i < song_len) most_song = std::max(most_song, o);
    }
    uint64_t total = 0;
    for (int i = 0; i < 31; i++) total += uint64_t(be16(&h[20 + i * 30 + 22])) * 2;
    const uint64_t pat = 64ull * uint64_t(chans) * 4;
    uint64_t data = 1084 + uint64_t(most + 1) * pat;
    // order bytes past the song may be garbage: count only the song's when the full count leaves too little sample data
    if (data + total > blob->size() && 1084 + uint64_t(most_song + 1) * pat + total <= blob->size()) data = 1084 + uint64_t(most_song + 1) * pat;
    for (int i = 0; i < 31; i++) {
        const uint8_t *q = &h[20 + i * 30];
        TSample s;
        s.name = text(q, 22);
        s.offset = data;
        uint64_t len = uint64_t(be16(q + 22)) * 2;
        data += len;
        s.frames = int64_t(len);
        s.bits = 8;
        s.is_signed = true;
        int ft = q[24] & 15;
        if (ft > 7) ft -= 16;
        s.rate = 8363.0 * std::pow(2.0, ft / 96.0);
        s.gain_db = level_db(std::min<int>(q[25], 64) / 64.0);
        int64_t ls = int64_t(be16(q + 26)) * 2, ll = int64_t(be16(q + 28)) * 2;
        if (ll > 2 && ls + ll > s.frames && ls / 2 + ll <= s.frames) ls /= 2;   // loop start in bytes (old editors)
        if (ll > 2 && ls < s.frames) {
            s.loop = true;
            s.loop_start = ls;
            s.loop_end = std::min<int64_t>(ls + ll, s.frames) - 1;
            if (s.loop_end <= s.loop_start) s.loop = false;
        }
        if (s.frames <= 2 && !s.loop) s.frames = 0;   // an empty slot (2-byte placeholders)
        m.samples.push_back(s);
        TInstr ins;
        ins.name = s.name;
        ins.cut = true;
        key_range(ins, i);
        m.instrs.push_back(ins);
        m.numbers.push_back(i + 1);
    }
    return m;
}

// ---- the instrument model

// The tracker's volume envelope as attack (to its loudest node before the sustain node), decay (to the sustain node),
// sustain and release (the nodes after the sustain node, or the fade-out, whichever ends first)
Envelope fit_envelope(const TInstr &ins, double tick, double &peak_db) {
    Envelope e;
    e.set = true;
    peak_db = 0;
    double fade = ins.fade_ticks > 0 ? ins.fade_ticks * tick : -1;
    if (!ins.env_on) {
        e.release = ins.cut ? 0.01 : fade > 0 ? fade : 0.25;
        return e;
    }
    const auto &pts = ins.env;
    int n = int(pts.size()), hold = ins.env_sus >= 0 ? ins.env_sus : n - 1, peak = 0;
    for (int i = 0; i <= hold; i++) if (pts[size_t(i)].second > pts[size_t(peak)].second) peak = i;
    double top = std::max(pts[size_t(peak)].second, 1);
    peak_db = level_db(top / 64.0);
    e.start_level = pts[0].second / top;
    e.attack = std::max(0, pts[size_t(peak)].first - pts[0].first) * tick;
    e.decay = std::max(0, pts[size_t(hold)].first - pts[size_t(peak)].first) * tick;
    e.sustain = pts[size_t(hold)].second / top;
    double rel = -1;
    if (ins.env_sus >= 0 && pts[size_t(n - 1)].second == 0) rel = std::max(0, pts[size_t(n - 1)].first - pts[size_t(hold)].first) * tick;
    if (fade > 0) rel = rel < 0 ? fade : std::min(rel, fade);
    if (rel < 0) rel = e.sustain > 0 ? 4.0 : 0.01;   // nothing ends the note: let it ring
    e.release = std::max(rel, 0.005);
    return e;
}

std::vector<PresetInfo> presets(const Module &m) {
    std::vector<PresetInfo> out;
    for (size_t i = 0; i < m.instrs.size(); i++) {
        const TInstr &ins = m.instrs[i];
        bool any = false;
        for (int k = 0; k < 128 && !any; k++) {
            int s = ins.sample[k];
            any = s >= 0 && s < int(m.samples.size()) && m.samples[size_t(s)].frames > 0 && m.samples[size_t(s)].codec != Codec::None;
        }
        if (!any) continue;
        char num[16];
        std::snprintf(num, sizeof num, "%02d ", m.numbers[i]);
        out.push_back({num + (ins.name.empty() ? (m.instrs.size() > 1 ? "Instrument" : "Sample") : ins.name), int(i)});
    }
    return out;
}

// A damaged header can claim gigabytes: no more frames than the file holds (IT compression spends at least a bit each)
void clamp_frames(Module &m, uint64_t size) {
    for (TSample &s : m.samples) {
        uint64_t avail = s.offset < size ? size - s.offset : 0, most;
        if (s.codec == Codec::It214 || s.codec == Codec::It215) most = avail * 8 / uint64_t(s.channels);
        else if (s.codec == Codec::Adpcm) most = avail > 16 ? (avail - 16) * 2 : 0;
        else most = avail / uint64_t(s.bits / 8 * s.channels);
        s.frames = std::min<int64_t>(s.frames, int64_t(most));
        if (s.loop && s.loop_end >= s.frames) s.loop = false;
    }
}

Instrument build(const BlobPtr &blob, const std::string &path, const Module &m, int index) {
    if (index < 0 || index >= int(m.instrs.size())) throw ParseError("no such instrument");
    const TInstr &ins = m.instrs[size_t(index)];
    Instrument inst;
    inst.format = m.format;
    inst.name = ins.name.empty() ? path_stem(path) : ins.name;
    double peak_db = 0;
    Envelope env = fit_envelope(ins, m.tick, peak_db);
    std::map<int, SampleRefPtr> refs;
    bool skipped = false;
    for (int k = 0; k < 128;) {
        int s = ins.sample[k], d = ins.note[k] - k, hi = k;
        if (s < 0 || s >= int(m.samples.size()) || m.samples[size_t(s)].frames <= 0 || m.samples[size_t(s)].codec == Codec::None) {
            if (s >= 0 && s < int(m.samples.size()) && m.samples[size_t(s)].codec == Codec::None) skipped = true;
            k++;
            continue;
        }
        while (hi + 1 < 128 && ins.sample[hi + 1] == s && ins.note[hi + 1] - (hi + 1) == d) hi++;
        const TSample &smp = m.samples[size_t(s)];
        SampleRefPtr &ref = refs[s];
        if (!ref) {
            ref = std::make_shared<SampleRef>();
            ref->name = smp.name.empty() ? "Sample " + std::to_string(s + 1) : smp.name;
            ref->rate = int(std::lround(smp.rate));
            ref->channels = smp.channels;
            ref->frames = smp.frames;
            ref->key = "trk:" + path + ":" + std::to_string(smp.offset) + ":" + std::to_string(s);
            ref->decode = [blob, smp]() { return decode(blob, smp); };
        }
        Zone z;
        z.name = ref->name;
        z.sample = ref;
        z.key_lo = k; z.key_hi = hi;
        z.root = 60 - d;
        z.tune = 12.0 * std::log2(smp.rate / std::max(1L, std::lround(smp.rate)));
        z.gain_db = smp.gain_db + ins.gain_db + peak_db;
        z.pan = smp.has_pan ? smp.pan : ins.has_pan ? ins.pan : 0;
        if (smp.loop) {
            Loop l;
            l.type = smp.loop_type;
            l.start = smp.loop_start; l.end = smp.loop_end;
            l.until_release = smp.sustain;
            z.loops.push_back(l);
        }
        z.amp_env = env;
        if (smp.vib_depth > 0 && smp.vib_rate > 0) {
            z.pitch_lfo.set = true;
            z.pitch_lfo.wave = smp.vib_wave;
            z.pitch_lfo.rate_hz = smp.vib_rate / 256.0 / m.tick;
            double ramp = smp.vib_sweep == 0 ? 0 : smp.it_sweep ? smp.vib_depth * 256.0 / smp.vib_sweep : smp.vib_sweep;
            z.pitch_lfo.fade_in = ramp * m.tick;
            z.pitch_lfo_depth = smp.vib_depth / 64.0 * 100.0 / MAX_ENVELOPE_DEPTH;
        }
        if (ins.cutoff_hz > 0) {
            z.filter.type = FilterType::LowPass;
            z.filter.poles = 2;
            z.filter.cutoff = std::min(ins.cutoff_hz, MAX_CUTOFF_HZ);
            z.filter.resonance = clampd(ins.resonance, 0, 1);
        }
        inst.zones.push_back(z);
        k = hi + 1;
    }
    if (skipped) inst.warnings.push_back("samples in an unsupported encoding (Ogg / external / AdLib) left out");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("this instrument has no samples");
    return inst;
}

template <Module (*Parse)(const BlobPtr &)>
Module parse_t(const BlobPtr &blob) {
    Module m = Parse(blob);
    clamp_frames(m, blob->size());
    return m;
}
template <Module (*Parse)(const BlobPtr &)>
std::vector<PresetInfo> list_t(VolumePtr vol, const std::string &path) { return presets(parse_t<Parse>(vol->open(path))); }
template <Module (*Parse)(const BlobPtr &)>
Instrument load_t(VolumePtr vol, const std::string &path, int index) {
    BlobPtr blob = vol->open(path);
    return build(blob, path, parse_t<Parse>(blob), index);
}

bool magic(const uint8_t *h, size_t n, size_t off, const char *m) {
    size_t len = std::strlen(m);
    return n >= off + len && std::memcmp(h + off, m, len) == 0;
}
bool probe_xi(const std::string &, const uint8_t *h, size_t n, uint64_t) { return magic(h, n, 0, "Extended Instrument: "); }
bool probe_xm(const std::string &, const uint8_t *h, size_t n, uint64_t) { return magic(h, n, 0, "Extended Module: "); }
bool probe_iti(const std::string &, const uint8_t *h, size_t n, uint64_t) { return magic(h, n, 0, "IMPI"); }
bool probe_its(const std::string &, const uint8_t *h, size_t n, uint64_t) { return magic(h, n, 0, "IMPS"); }
bool probe_it(const std::string &, const uint8_t *h, size_t n, uint64_t) { return magic(h, n, 0, "IMPM"); }
bool probe_s3m(const std::string &, const uint8_t *h, size_t n, uint64_t) { return magic(h, n, 44, "SCRM"); }
bool probe_mod(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 1084 && mod_channels(h + 1080) > 0; }

}  // namespace

void register_tracker() {
    // Every tracker extension for every reader: the probes check the content, so a module saved under the wrong
    // extension (an .it named .xm) still opens
    static const char *exts = "xi xm iti its it s3m mod";
    register_reader({"FastTracker 2 instrument", exts, probe_xi, list_t<parse_xi>, load_t<parse_xi>});
    register_reader({"FastTracker 2 module", exts, probe_xm, list_t<parse_xm>, load_t<parse_xm>});
    register_reader({"Impulse Tracker instrument", exts, probe_iti, list_t<parse_iti>, load_t<parse_iti>});
    register_reader({"Impulse Tracker sample", exts, probe_its, list_t<parse_its>, load_t<parse_its>});
    register_reader({"Impulse Tracker module", exts, probe_it, list_t<parse_it>, load_t<parse_it>});
    register_reader({"Scream Tracker 3 module", exts, probe_s3m, list_t<parse_s3m>, load_t<parse_s3m>});
    register_reader({"ProTracker module", exts, probe_mod, list_t<parse_mod>, load_t<parse_mod>});   // 2.1.1
}

}  // namespace omni

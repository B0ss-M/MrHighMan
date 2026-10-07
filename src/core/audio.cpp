// Omni Sampler: audio decoding (see audio.hpp). NCW decoding follows ConvertWithMoss's NcwFile (LGPL-3.0,
// Jürgen Moßgraber), with 64-bit bit accumulators and proper sign extension of uncompressed blocks.
#include "audio.hpp"
#include <cmath>
#include <cstring>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "../third_party/dr_flac.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"
#undef L
#undef C
#undef R

namespace omni {

int enc_bytes(Enc e) {
    switch (e) {
    case Enc::S8: case Enc::U8: return 1;
    case Enc::S16LE: case Enc::S16BE: return 2;
    case Enc::S24LE: case Enc::S24BE: return 3;
    case Enc::S32LE: case Enc::S32BE: case Enc::F32LE: case Enc::F32BE: return 4;
    case Enc::F64LE: case Enc::F64BE: return 8;
    }
    return 2;
}

static inline int16_t clip16f(double v) {
    double s = v * 32768.0;
    if (s >= 32767.0) return 32767;
    if (s <= -32768.0) return -32768;
    return int16_t(std::lrint(s));
}
static inline int16_t round32(int32_t v) {   // 32-bit to 16 with rounding, saturated
    int64_t r = (int64_t(v) + 0x8000) >> 16;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    return int16_t(r);
}

void convert_raw(const uint8_t *s, size_t n, Enc enc, int16_t *d) {
    switch (enc) {
    case Enc::S8: for (size_t i = 0; i < n; i++) d[i] = int16_t(int8_t(s[i]) * 256); break;
    case Enc::U8: for (size_t i = 0; i < n; i++) d[i] = int16_t((int(s[i]) - 128) * 256); break;
    case Enc::S16LE: for (size_t i = 0; i < n; i++) d[i] = int16_t(le16(s + 2 * i)); break;
    case Enc::S16BE: for (size_t i = 0; i < n; i++) d[i] = int16_t(be16(s + 2 * i)); break;
    case Enc::S24LE: for (size_t i = 0; i < n; i++) d[i] = round32(int32_t(le24(s + 3 * i) << 8)); break;
    case Enc::S24BE: for (size_t i = 0; i < n; i++) d[i] = round32(int32_t(be24(s + 3 * i) << 8)); break;
    case Enc::S32LE: for (size_t i = 0; i < n; i++) d[i] = round32(int32_t(le32(s + 4 * i))); break;
    case Enc::S32BE: for (size_t i = 0; i < n; i++) d[i] = round32(int32_t(be32(s + 4 * i))); break;
    case Enc::F32LE: case Enc::F32BE:
        for (size_t i = 0; i < n; i++) {
            uint32_t u = enc == Enc::F32LE ? le32(s + 4 * i) : be32(s + 4 * i);
            float f; std::memcpy(&f, &u, 4);
            d[i] = clip16f(f);
        }
        break;
    case Enc::F64LE: case Enc::F64BE:
        for (size_t i = 0; i < n; i++) {
            uint64_t u = enc == Enc::F64LE ? le64(s + 8 * i) : be64(s + 8 * i);
            double f; std::memcpy(&f, &u, 8);
            d[i] = clip16f(f);
        }
        break;
    }
}

PcmPtr pcm_from_raw(const uint8_t *src, int64_t frames, int channels, Enc enc, int rate) {
    auto p = std::make_shared<Pcm>();
    p->rate = rate > 0 ? rate : 44100;
    p->channels = channels > 0 ? channels : 1;
    p->data.resize(size_t(frames) * p->channels);
    convert_raw(src, p->data.size(), enc, p->data.data());
    return p;
}

PcmPtr pcm_from_planar(const uint8_t *src, int64_t frames, int channels, Enc enc, int rate) {
    if (channels <= 1) return pcm_from_raw(src, frames, 1, enc, rate);
    auto p = std::make_shared<Pcm>();
    p->rate = rate > 0 ? rate : 44100;
    p->channels = channels;
    p->data.resize(size_t(frames) * channels);
    std::vector<int16_t> tmp(static_cast<size_t>(frames));
    size_t plane = size_t(frames) * enc_bytes(enc);
    for (int c = 0; c < channels; c++) {
        convert_raw(src + plane * c, size_t(frames), enc, tmp.data());
        for (int64_t i = 0; i < frames; i++) p->data[size_t(i) * channels + c] = tmp[size_t(i)];
    }
    return p;
}

SampleRefPtr raw_sample_ref(BlobPtr blob, uint64_t offset, int64_t frames, int channels, Enc enc, int rate,
                            const std::string &name, bool planar) {
    auto r = std::make_shared<SampleRef>();
    r->name = name;
    r->rate = rate;
    r->channels = channels;
    r->frames = frames;
    r->key = "raw:" + std::to_string(reinterpret_cast<uintptr_t>(blob.get())) + ":" + std::to_string(offset);
    r->decode = [blob, offset, frames, channels, enc, rate, planar]() -> PcmPtr {
        if (frames <= 0) { auto p = std::make_shared<Pcm>(); p->rate = rate; p->channels = channels; return p; }
        uint64_t avail = blob->size() > offset ? blob->size() - offset : 0;
        int64_t fit = int64_t(avail / uint64_t(enc_bytes(enc) * channels));
        int64_t n = frames < fit ? frames : fit;   // a truncated image keeps what is there
        std::vector<uint8_t> b = blob->bytes(offset, size_t(n) * enc_bytes(enc) * channels);
        return planar ? pcm_from_planar(b.data(), n, channels, enc, rate) : pcm_from_raw(b.data(), n, channels, enc, rate);
    };
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// WAV

namespace {

double ext80(const uint8_t *p) {   // IEEE 754 80-bit extended (AIFF sample rate)
    int expo = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t mant = be64(p + 2);
    if (!expo && !mant) return 0;
    double v = std::ldexp(double(mant), expo - 16383 - 63);
    return (p[0] & 0x80) ? -v : v;
}

struct WavLayout {
    uint64_t data_off = 0, data_len = 0;
    int channels = 0, rate = 0, bits = 0, tag = 0, block_align = 0;
    bool big_endian = false;
};

WavLayout parse_wav(const Blob &blob, AudioInfo *info) {
    std::vector<uint8_t> h = blob.head(12);
    if (h.size() < 12) throw ParseError("not a WAV file");
    WavLayout w;
    bool rf64 = std::memcmp(h.data(), "RF64", 4) == 0 || std::memcmp(h.data(), "BW64", 4) == 0;
    w.big_endian = std::memcmp(h.data(), "RIFX", 4) == 0;
    if ((std::memcmp(h.data(), "RIFF", 4) && !rf64 && !w.big_endian) || std::memcmp(h.data() + 8, "WAVE", 4))
        throw ParseError("not a WAV file");
    auto rd32 = [&](const uint8_t *p) { return w.big_endian ? be32(p) : le32(p); };
    auto rd16 = [&](const uint8_t *p) { return w.big_endian ? be16(p) : le16(p); };
    uint64_t size = blob.size(), pos = 12, ds64_data = 0;
    bool have_fmt = false;
    while (pos + 8 <= size) {
        std::vector<uint8_t> ch = blob.bytes(pos, 8);
        uint64_t len = rd32(ch.data() + 4);
        uint64_t body = pos + 8;
        if (!std::memcmp(ch.data(), "ds64", 4) && len >= 24) {
            std::vector<uint8_t> d = blob.bytes(body, 24);
            ds64_data = le64(d.data() + 8);
        } else if (!std::memcmp(ch.data(), "fmt ", 4) && len >= 16) {
            std::vector<uint8_t> f = blob.bytes(body, size_t(std::min<uint64_t>(len, 40)));
            w.tag = rd16(f.data());
            w.channels = rd16(f.data() + 2);
            w.rate = int(rd32(f.data() + 4));
            w.block_align = rd16(f.data() + 12);
            w.bits = rd16(f.data() + 14);
            if (w.tag == 0xFFFE && f.size() >= 26) w.tag = rd16(f.data() + 24);   // extensible: sub format
            have_fmt = true;
        } else if (!std::memcmp(ch.data(), "data", 4)) {
            w.data_off = body;
            w.data_len = (rf64 && len == 0xFFFFFFFFu) ? ds64_data : len;
            if (w.data_len > size - body) w.data_len = size - body;
            if (!info) { if (have_fmt) break; }
        } else if (info && !std::memcmp(ch.data(), "smpl", 4) && len >= 36) {
            std::vector<uint8_t> s = blob.bytes(body, size_t(std::min<uint64_t>(len, 36 + 24 * 64)));
            info->root = int(rd32(s.data() + 12));
            info->fine = rd32(s.data() + 16) / 4294967296.0 * 100.0;
            uint32_t n = rd32(s.data() + 28);
            for (uint32_t i = 0; i < n && 36 + 24 * (i + 1) <= s.size(); i++) {
                const uint8_t *l = s.data() + 36 + 24 * i;
                Loop lp;
                uint32_t type = rd32(l + 4);
                lp.type = type == 1 ? LoopType::Alternating : type == 2 ? LoopType::Backward : LoopType::Forward;
                lp.start = rd32(l + 8);
                lp.end = rd32(l + 12);
                if (lp.end > lp.start) info->loops.push_back(lp);
            }
        } else if (info && !std::memcmp(ch.data(), "inst", 4) && len >= 7) {
            std::vector<uint8_t> s = blob.bytes(body, 7);
            if (info->root < 0) info->root = s[0];
            info->fine = int8_t(s[1]);
            info->gain_db = int8_t(s[2]);
            info->key_lo = s[3]; info->key_hi = s[4]; info->vel_lo = s[5]; info->vel_hi = s[6];
        }
        pos = body + len + (len & 1);
    }
    if (!have_fmt || !w.data_off) throw ParseError("WAV without fmt or data chunk");
    if (w.channels <= 0 || w.channels > 64) throw ParseError("WAV with " + std::to_string(w.channels) + " channels");
    if (info) {
        info->rate = w.rate;
        info->channels = w.channels;
        info->bits = w.bits;
        int ba = w.block_align ? w.block_align : w.channels * ((w.bits + 7) / 8);
        info->frames = ba ? int64_t(w.data_len / ba) : 0;
        if (info->root > 127) info->root = -1;
    }
    return w;
}

// IMA / DVI ADPCM (format 0x11), found in some older sample libraries
const int IMA_STEPS[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767};
const int IMA_INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

PcmPtr decode_ima(const std::vector<uint8_t> &d, int channels, int block_align, int rate) {
    auto p = std::make_shared<Pcm>();
    p->rate = rate; p->channels = channels;
    if (block_align <= 4 * channels) throw ParseError("bad IMA ADPCM block size");
    int per_block = (block_align - 4 * channels) * 8 / (4 * channels) + 1;
    for (size_t b = 0; b + size_t(block_align) <= d.size(); b += size_t(block_align)) {
        const uint8_t *blk = d.data() + b;
        std::vector<int> pred(channels), idx(channels);
        size_t base = p->data.size();
        p->data.resize(base + size_t(per_block) * channels);
        for (int c = 0; c < channels; c++) {
            pred[c] = int16_t(le16(blk + 4 * c));
            idx[c] = blk[4 * c + 2] > 88 ? 88 : blk[4 * c + 2];
            p->data[base + c] = int16_t(pred[c]);
        }
        const uint8_t *q = blk + 4 * channels;
        int frame = 1;
        while (frame < per_block) {
            for (int c = 0; c < channels; c++) {   // 8 samples per channel per 4-byte group
                for (int k = 0; k < 8 && frame + k < per_block; k++) {
                    int nib = (q[k / 2] >> ((k & 1) * 4)) & 15;
                    int step = IMA_STEPS[idx[c]];
                    int diff = step >> 3;
                    if (nib & 1) diff += step >> 2;
                    if (nib & 2) diff += step >> 1;
                    if (nib & 4) diff += step;
                    pred[c] += (nib & 8) ? -diff : diff;
                    if (pred[c] > 32767) pred[c] = 32767;
                    if (pred[c] < -32768) pred[c] = -32768;
                    idx[c] += IMA_INDEX[nib];
                    if (idx[c] < 0) idx[c] = 0;
                    if (idx[c] > 88) idx[c] = 88;
                    p->data[base + size_t(frame + k) * channels + c] = int16_t(pred[c]);
                }
                q += 4;
            }
            frame += 8;
        }
    }
    return p;
}

// MS ADPCM (format 0x02)
PcmPtr decode_msadpcm(const std::vector<uint8_t> &d, int channels, int block_align, int rate) {
    static const int ADAPT[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
    static const int C1[7] = {256, 512, 0, 192, 240, 460, 392}, C2[7] = {0, -256, 0, 64, 0, -208, -232};
    auto p = std::make_shared<Pcm>();
    p->rate = rate; p->channels = channels;
    if (block_align < 7 * channels) throw ParseError("bad MS ADPCM block size");
    int per_block = (block_align - 7 * channels) * 2 / channels + 2;
    for (size_t b = 0; b + size_t(block_align) <= d.size(); b += size_t(block_align)) {
        const uint8_t *q = d.data() + b;
        int pr[2] = {0, 0}, delta[2] = {0, 0}, s1[2] = {0, 0}, s2[2] = {0, 0};
        for (int c = 0; c < channels && c < 2; c++) pr[c] = q[c] > 6 ? 0 : q[c];
        q += channels;
        for (int c = 0; c < channels && c < 2; c++) { delta[c] = int16_t(le16(q)); q += 2; }
        for (int c = 0; c < channels && c < 2; c++) { s1[c] = int16_t(le16(q)); q += 2; }
        for (int c = 0; c < channels && c < 2; c++) { s2[c] = int16_t(le16(q)); q += 2; }
        size_t base = p->data.size();
        p->data.resize(base + size_t(per_block) * channels);
        for (int c = 0; c < channels && c < 2; c++) {
            p->data[base + c] = int16_t(s2[c]);
            p->data[base + channels + c] = int16_t(s1[c]);
        }
        size_t out = base + 2 * channels;
        const uint8_t *end = d.data() + b + block_align;
        int c = 0;
        for (; q < end; q++) {
            for (int half = 0; half < 2; half++) {
                int nib = half == 0 ? q[0] >> 4 : q[0] & 15;
                int sn = nib >= 8 ? nib - 16 : nib;
                int ch = channels == 2 ? c : 0;
                int pred = (s1[ch] * C1[pr[ch]] + s2[ch] * C2[pr[ch]]) / 256 + sn * delta[ch];
                if (pred > 32767) pred = 32767;
                if (pred < -32768) pred = -32768;
                s2[ch] = s1[ch]; s1[ch] = pred;
                delta[ch] = ADAPT[nib] * delta[ch] / 256;
                if (delta[ch] < 16) delta[ch] = 16;
                if (out < p->data.size()) p->data[out++] = int16_t(pred);
                if (channels == 2) c ^= 1;
            }
        }
    }
    return p;
}

PcmPtr decode_wav(const Blob &blob, AudioInfo *info) {
    AudioInfo local;
    WavLayout w = parse_wav(blob, info ? info : &local);
    std::vector<uint8_t> d = blob.bytes(w.data_off, size_t(w.data_len));
    if (w.tag == 0x11) return decode_ima(d, w.channels, w.block_align, w.rate);
    if (w.tag == 0x02) return decode_msadpcm(d, w.channels, w.block_align, w.rate);
    Enc enc;
    bool be = w.big_endian;
    if (w.tag == 3) enc = w.bits == 64 ? (be ? Enc::F64BE : Enc::F64LE) : (be ? Enc::F32BE : Enc::F32LE);
    else if (w.tag == 1) {
        switch (w.bits) {
        case 8: enc = Enc::U8; break;
        case 16: enc = be ? Enc::S16BE : Enc::S16LE; break;
        case 24: enc = be ? Enc::S24BE : Enc::S24LE; break;
        case 32: enc = be ? Enc::S32BE : Enc::S32LE; break;
        default: throw ParseError("WAV with " + std::to_string(w.bits) + "-bit samples");
        }
    } else throw ParseError("WAV compression " + std::to_string(w.tag) + " is not supported");
    int frame_bytes = enc_bytes(enc) * w.channels;
    return pcm_from_raw(d.data(), int64_t(d.size() / frame_bytes), w.channels, enc, w.rate);
}

// ---------------------------------------------------------------------------------------------------------------
// AIFF / AIFC

PcmPtr decode_aiff(const Blob &blob, AudioInfo *info, bool header_only) {
    std::vector<uint8_t> h = blob.head(12);
    if (h.size() < 12 || std::memcmp(h.data(), "FORM", 4) ||
        (std::memcmp(h.data() + 8, "AIFF", 4) && std::memcmp(h.data() + 8, "AIFC", 4)))
        throw ParseError("not an AIFF file");
    uint64_t size = blob.size(), pos = 12, ssnd = 0, ssnd_len = 0;
    int channels = 0, bits = 0, rate = 0;
    int64_t frames = 0;
    std::string comp = "NONE";
    struct Marker { int id; uint32_t pos; };
    std::vector<Marker> markers;
    int sus_mode = 0, sus_begin = 0, sus_end = 0, base_note = -1, detune = 0;
    while (pos + 8 <= size) {
        std::vector<uint8_t> ch = blob.bytes(pos, 8);
        uint64_t len = be32(ch.data() + 4), body = pos + 8;
        if (!std::memcmp(ch.data(), "COMM", 4) && len >= 18) {
            std::vector<uint8_t> c = blob.bytes(body, size_t(std::min<uint64_t>(len, 26)));
            channels = be16(c.data());
            frames = be32(c.data() + 2);
            bits = be16(c.data() + 6);
            rate = int(std::lround(ext80(c.data() + 8)));
            if (c.size() >= 22) comp = std::string(reinterpret_cast<char *>(c.data() + 18), 4);
        } else if (!std::memcmp(ch.data(), "SSND", 4) && len >= 8) {
            std::vector<uint8_t> s = blob.bytes(body, 8);
            ssnd = body + 8 + be32(s.data());
            ssnd_len = len - 8 - be32(s.data());
        } else if (!std::memcmp(ch.data(), "MARK", 4) && len >= 2) {
            std::vector<uint8_t> m = blob.bytes(body, size_t(len));
            size_t n = be16(m.data()), q = 2;
            for (size_t i = 0; i < n && q + 7 <= m.size(); i++) {
                markers.push_back({int(be16(m.data() + q)), be32(m.data() + q + 2)});
                q += 6;
                size_t sl = m[q];
                q += 1 + sl + ((sl + 1) & 1 ? 1 : 0);   // pstring padded to even total length
            }
        } else if (!std::memcmp(ch.data(), "INST", 4) && len >= 20) {
            std::vector<uint8_t> in = blob.bytes(body, 20);
            base_note = in[0];
            detune = int8_t(in[1]);
            if (info) { info->key_lo = in[2]; info->key_hi = in[3]; info->vel_lo = in[4]; info->vel_hi = in[5];
                        info->gain_db = int16_t(be16(in.data() + 6)); }
            sus_mode = be16(in.data() + 8); sus_begin = be16(in.data() + 10); sus_end = be16(in.data() + 12);
        }
        pos = body + len + (len & 1);
    }
    if (!channels || !ssnd) throw ParseError("AIFF without COMM or SSND chunk");
    if (info) {
        info->rate = rate; info->channels = channels; info->bits = bits; info->frames = frames;
        if (base_note >= 0 && base_note < 128) info->root = base_note;
        info->fine = detune;
        if (sus_mode) {
            uint32_t a = 0, b = 0; bool fa = false, fb = false;
            for (auto &m : markers) { if (m.id == sus_begin) { a = m.pos; fa = true; } if (m.id == sus_end) { b = m.pos; fb = true; } }
            if (fa && fb && b > a) {
                Loop l; l.start = a; l.end = int64_t(b) - 1;
                l.type = sus_mode == 2 ? LoopType::Alternating : LoopType::Forward;
                info->loops.push_back(l);
            }
        }
    }
    if (header_only) return nullptr;
    std::string c = lower(comp);
    Enc enc;
    if (c == "none" || c == "twos") enc = bits <= 8 ? Enc::S8 : bits <= 16 ? Enc::S16BE : bits <= 24 ? Enc::S24BE : Enc::S32BE;
    else if (c == "sowt") enc = bits <= 8 ? Enc::S8 : bits <= 16 ? Enc::S16LE : bits <= 24 ? Enc::S24LE : Enc::S32LE;
    else if (c == "fl32") enc = Enc::F32BE;
    else if (c == "fl64") enc = Enc::F64BE;
    else if (c == "raw ") enc = Enc::U8;
    else if (c == "in24") enc = Enc::S24BE;
    else if (c == "in32") enc = Enc::S32BE;
    else if (c == "42ni") enc = Enc::S24LE;
    else throw ParseError("AIFF compression '" + comp + "' is not supported");
    int fb = enc_bytes(enc) * channels;
    int64_t avail = int64_t(std::min<uint64_t>(ssnd_len, size - ssnd) / uint64_t(fb));
    if (frames > avail) frames = avail;
    std::vector<uint8_t> d = blob.bytes(ssnd, size_t(frames) * fb);
    return pcm_from_raw(d.data(), frames, channels, enc, rate);
}

// ---------------------------------------------------------------------------------------------------------------
// FLAC (dr_flac over a Blob)

struct BlobStream { const Blob *blob; uint64_t pos; };
size_t flac_read(void *ud, void *out, size_t n) {
    BlobStream *s = static_cast<BlobStream *>(ud);
    uint64_t sz = s->blob->size();
    if (s->pos >= sz) return 0;
    size_t take = size_t(std::min<uint64_t>(n, sz - s->pos));
    s->blob->read(s->pos, out, take);
    s->pos += take;
    return take;
}
drflac_bool32 flac_seek(void *ud, int offset, drflac_seek_origin origin) {
    BlobStream *s = static_cast<BlobStream *>(ud);
    int64_t base = origin == DRFLAC_SEEK_SET ? 0 : origin == DRFLAC_SEEK_END ? int64_t(s->blob->size()) : int64_t(s->pos);
    int64_t np = base + offset;
    if (np < 0 || uint64_t(np) > s->blob->size()) return DRFLAC_FALSE;
    s->pos = uint64_t(np);
    return DRFLAC_TRUE;
}
drflac_bool32 flac_tell(void *ud, drflac_int64 *pos) {
    *pos = drflac_int64(static_cast<BlobStream *>(ud)->pos);
    return DRFLAC_TRUE;
}

PcmPtr decode_flac(const Blob &blob, AudioInfo *info, bool header_only) {
    BlobStream s{&blob, 0};
    drflac *f = drflac_open(flac_read, flac_seek, flac_tell, &s, nullptr);
    if (!f) throw ParseError("cannot decode FLAC file");
    if (info) { info->rate = int(f->sampleRate); info->channels = f->channels; info->bits = f->bitsPerSample;
                info->frames = int64_t(f->totalPCMFrameCount); }
    if (header_only) { drflac_close(f); return nullptr; }
    auto p = std::make_shared<Pcm>();
    p->rate = int(f->sampleRate);
    p->channels = f->channels;
    p->data.resize(size_t(f->totalPCMFrameCount) * f->channels);
    drflac_uint64 got = drflac_read_pcm_frames_s16(f, f->totalPCMFrameCount, p->data.data());
    p->data.resize(size_t(got) * f->channels);
    drflac_close(f);
    return p;
}

PcmPtr decode_ogg(const Blob &blob, AudioInfo *info, bool header_only) {
    std::vector<uint8_t> all = blob.all();
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(all.data(), int(all.size()), &err, nullptr);
    if (!v) throw ParseError("cannot decode Ogg Vorbis file");
    stb_vorbis_info vi = stb_vorbis_get_info(v);
    int64_t total = int64_t(stb_vorbis_stream_length_in_samples(v));
    if (info) { info->rate = int(vi.sample_rate); info->channels = vi.channels; info->bits = 16; info->frames = total; }
    if (header_only) { stb_vorbis_close(v); return nullptr; }
    auto p = std::make_shared<Pcm>();
    p->rate = int(vi.sample_rate);
    p->channels = vi.channels;
    p->data.resize(size_t(total) * vi.channels);
    int64_t got = 0;
    while (got < total) {
        int n = stb_vorbis_get_samples_short_interleaved(v, vi.channels, p->data.data() + got * vi.channels,
                                                         int((total - got) * vi.channels));
        if (n <= 0) break;
        got += n;
    }
    p->data.resize(size_t(got) * vi.channels);
    stb_vorbis_close(v);
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// NCW

namespace {
const uint32_t NCW_MAGIC = 0xD69EA801, NCW_BLOCK = 0x3E9A0C16;
const int NCW_N = 512;

void unpack_signed(const uint8_t *d, size_t len, int bits, int32_t *out) {
    uint64_t acc = 0;
    int have = 0, count = 0;
    for (size_t i = 0; i < len && count < NCW_N; i++) {
        acc |= uint64_t(d[i]) << have;
        have += 8;
        while (have >= bits && count < NCW_N) {
            uint64_t v = acc & ((uint64_t(1) << bits) - 1);
            int64_t sv = int64_t(v);
            if (bits < 64 && (v >> (bits - 1)) & 1) sv -= int64_t(1) << bits;
            out[count++] = int32_t(sv);
            acc >>= bits;
            have -= bits;
        }
    }
    while (count < NCW_N) out[count++] = 0;
}
}  // namespace

PcmPtr decode_ncw(const Blob &blob, AudioInfo *info) {
    std::vector<uint8_t> h = blob.head(120);
    if (h.size() < 120 || le32(h.data()) != NCW_MAGIC) throw ParseError("not an NCW file");
    uint32_t version = le32(h.data() + 4);
    if (version != 0x130 && version != 0x131) throw ParseError("unknown NCW version");
    int channels = le16(h.data() + 8), bits = le16(h.data() + 10);
    int rate = int(le32(h.data() + 12));
    int64_t frames = le32(h.data() + 16);
    uint32_t off_addr = le32(h.data() + 20), off_data = le32(h.data() + 24);
    if (info) { info->rate = rate; info->channels = channels; info->bits = bits; info->frames = frames; }
    if (!channels || channels > 8 || off_data < off_addr) throw ParseError("bad NCW header");
    size_t nblocks = (off_data - off_addr) / 4;
    std::vector<uint8_t> offs = blob.bytes(off_addr, nblocks * 4);
    auto p = std::make_shared<Pcm>();
    p->rate = rate;
    p->channels = channels;
    p->data.assign(size_t(frames) * channels, 0);
    std::vector<int32_t> ch(size_t(channels) * NCW_N);
    for (size_t b = 0; b + 1 < nblocks; b++) {
        uint32_t a = le32(offs.data() + 4 * b), e = le32(offs.data() + 4 * (b + 1));
        if (e < a) throw ParseError("bad NCW block table");
        std::vector<uint8_t> blk = blob.bytes(uint64_t(off_data) + a, e - a);
        Reader r(blk);
        bool mid_side = false, is_float = false;
        for (int c = 0; c < channels; c++) {
            if (r.u32le() != NCW_BLOCK) throw ParseError("bad NCW block");
            int32_t base = r.s32le();
            int16_t bb = r.s16le();
            uint16_t flags = r.u16le();
            r.skip(4);
            if (flags & 1) mid_side = true;
            if (flags & 2) is_float = true;
            int32_t *out = &ch[size_t(c) * NCW_N];
            if (bb > 0) {
                const uint8_t *d = r.take(size_t(NCW_N) * bb / 8);
                std::vector<int32_t> deltas(NCW_N);
                unpack_signed(d, size_t(NCW_N) * bb / 8, bb, deltas.data());
                int64_t prev = base;
                for (int i = 0; i < NCW_N; i++) { out[i] = int32_t(prev); prev += deltas[i]; }
            } else if (bb < 0) {
                const uint8_t *d = r.take(size_t(NCW_N) * -bb / 8);
                unpack_signed(d, size_t(NCW_N) * -bb / 8, -bb, out);
            } else {
                int bps = bits / 8;
                const uint8_t *d = r.take(size_t(NCW_N) * bps);
                for (int i = 0; i < NCW_N; i++) {
                    const uint8_t *q = d + i * bps;
                    out[i] = bps == 2 ? int16_t(le16(q)) : bps == 3 ? int32_t(le24(q) << 8) >> 8 : int32_t(le32(q));
                }
            }
        }
        int64_t start = int64_t(b) * NCW_N;
        int n = int(std::min<int64_t>(NCW_N, frames - start));
        for (int i = 0; i < n; i++) {
            double l = 0, rr = 0;
            for (int c = 0; c < channels; c++) {
                int32_t v = ch[size_t(c) * NCW_N + i];
                double f;
                if (is_float) { float x; std::memcpy(&x, &v, 4); f = x; }
                else f = double(v) / double(int64_t(1) << (bits - 1));
                if (mid_side && channels == 2) { if (c == 0) l = f; else rr = f; }
                else p->data[size_t(start + i) * channels + c] = clip16f(f);
            }
            if (mid_side && channels == 2) {
                p->data[size_t(start + i) * 2] = clip16f(l + rr);
                p->data[size_t(start + i) * 2 + 1] = clip16f(l - rr);
            }
        }
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------
// dispatch

namespace {
enum class Kind { Wav, Aiff, Flac, Ogg, Ncw, Unknown };
Kind sniff(const Blob &blob) {
    std::vector<uint8_t> h = blob.head(12);
    if (h.size() >= 12 && (!std::memcmp(h.data(), "RIFF", 4) || !std::memcmp(h.data(), "RF64", 4) ||
                           !std::memcmp(h.data(), "RIFX", 4) || !std::memcmp(h.data(), "BW64", 4)) &&
        !std::memcmp(h.data() + 8, "WAVE", 4)) return Kind::Wav;
    if (h.size() >= 12 && !std::memcmp(h.data(), "FORM", 4) &&
        (!std::memcmp(h.data() + 8, "AIFF", 4) || !std::memcmp(h.data() + 8, "AIFC", 4))) return Kind::Aiff;
    if (h.size() >= 4 && !std::memcmp(h.data(), "fLaC", 4)) return Kind::Flac;
    if (h.size() >= 4 && !std::memcmp(h.data(), "OggS", 4)) return Kind::Ogg;
    if (h.size() >= 4 && le32(h.data()) == NCW_MAGIC) return Kind::Ncw;
    if (h.size() >= 10 && !std::memcmp(h.data(), "ID3", 3)) return Kind::Flac;   // FLAC with an ID3 tag in front
    return Kind::Unknown;
}
}  // namespace

PcmPtr decode_audio(const Blob &blob, AudioInfo *info) {
    switch (sniff(blob)) {
    case Kind::Wav: return decode_wav(blob, info);
    case Kind::Aiff: return decode_aiff(blob, info, false);
    case Kind::Flac: return decode_flac(blob, info, false);
    case Kind::Ogg: return decode_ogg(blob, info, false);
    case Kind::Ncw: return decode_ncw(blob, info);
    default: throw ParseError("unknown audio file format");
    }
}

AudioInfo probe_audio(const Blob &blob) {
    AudioInfo info;
    switch (sniff(blob)) {
    case Kind::Wav: parse_wav(blob, &info); break;
    case Kind::Aiff: decode_aiff(blob, &info, true); break;
    case Kind::Flac: decode_flac(blob, &info, true); break;
    case Kind::Ogg: decode_ogg(blob, &info, true); break;
    case Kind::Ncw: {
        std::vector<uint8_t> h = blob.head(20);
        if (h.size() >= 20) { info.channels = le16(h.data() + 8); info.bits = le16(h.data() + 10);
                              info.rate = int(le32(h.data() + 12)); info.frames = le32(h.data() + 16); }
        break;
    }
    default: throw ParseError("unknown audio file format");
    }
    return info;
}

bool is_audio_ext(const std::string &ext) {
    return ext == "wav" || ext == "wave" || ext == "aif" || ext == "aiff" || ext == "aifc" || ext == "flac" ||
           ext == "ogg" || ext == "ncw";
}

SampleRefPtr file_sample_ref(VolumePtr volume, const std::string &path) {
    auto r = std::make_shared<SampleRef>();
    r->name = path_stem(path);
    r->key = "file:" + std::to_string(reinterpret_cast<uintptr_t>(volume.get())) + ":" + path;
    r->decode = [volume, path]() -> PcmPtr {
        BlobPtr b = volume->open(path);
        return decode_audio(*b);
    };
    return r;
}

}  // namespace omni

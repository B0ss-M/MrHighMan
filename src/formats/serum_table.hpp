// Omni Sampler 2.4.32: Serum 2 preset payloads (formats/serum.cpp) and an oscillator's wavetable for the SYNTH
// ("serum:<osc>:<preset path>", loaded like Vital's / Arturia's preset-owned tables in plugin.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "../core/audio.hpp"
#include "../core/bytes.hpp"
#include "../engine/synth.hpp"
#include "../third_party/zstd/zstd.h"
#include "format.hpp"
#include "vital_table.hpp"   // 2.4.35: VitalLine (Serum LFO curves)

namespace omni {

// A validated definite-length CBOR item inside the decompressed payload (the user's Item, bounds checked on every read).
struct SerumItem {
    const uint8_t *p = nullptr, *end = nullptr;
    unsigned type = 0, code = 0;
    uint64_t value = 0;

    static SerumItem next(const uint8_t *&cursor, const uint8_t *limit, unsigned depth = 0) {
        if (cursor >= limit || depth > 48) throw ParseError("Serum 2: truncated or too deeply nested data");
        SerumItem i;
        const unsigned b = *cursor++;
        i.type = b >> 5;
        i.code = b & 31;
        if (i.code < 24) i.value = i.code;
        else {
            if (i.code > 27) throw ParseError("Serum 2: unsupported CBOR length");
            unsigned width = 1u << (i.code - 24);
            if (size_t(limit - cursor) < width) throw ParseError("Serum 2: short CBOR item");
            while (width--) i.value = (i.value << 8) | *cursor++;
        }
        i.p = cursor;
        if (i.type == 2 || i.type == 3) {
            if (i.value > uint64_t(limit - cursor)) throw ParseError("Serum 2: invalid CBOR size");
            cursor += size_t(i.value);
        } else if (i.type == 4 || i.type == 5) {
            if (i.value > uint64_t(limit - cursor)) throw ParseError("Serum 2: invalid CBOR count");   // >= 1 byte each
            for (uint64_t n = 0; n < i.value; ++n) {
                if (i.type == 4 && *cursor == 0xfa) {   // a float32: skip it (decoded when copied)
                    if (limit - cursor < 5) throw ParseError("Serum 2: short float");
                    cursor += 5;
                    continue;
                }
                next(cursor, limit, depth + 1);
                if (i.type == 5) next(cursor, limit, depth + 1);
            }
        } else if (i.type == 6) throw ParseError("Serum 2: unsupported CBOR tag");
        i.end = cursor;
        return i;
    }
    bool equals(const char *s) const {
        const size_t n = std::strlen(s);
        return type == 3 && value == n && !std::memcmp(p, s, n);
    }
    SerumItem field(const std::string &s) const {
        if (type == 5) {
            const uint8_t *cursor = p;
            for (uint64_t n = 0; n < value && cursor < end; ++n) {
                SerumItem key = next(cursor, end), child = next(cursor, end);
                if (key.equals(s.c_str())) return child;
            }
        }
        return {};
    }
    std::string text() const { return type == 3 ? std::string(reinterpret_cast<const char *>(p), size_t(value)) : std::string(); }
    double number(double fallback) const {
        if (!p) return fallback;
        double d = fallback;
        if (type == 0) d = double(value);
        else if (type == 1) d = -1.0 - double(value);
        else if (type == 7 && code == 26) { uint32_t bits = uint32_t(value); float f; std::memcpy(&f, &bits, 4); d = f; }
        else if (type == 7 && code == 27) std::memcpy(&d, &value, 8);
        else if (type == 7 && code == 25) d = fallback;   // half floats: not written by Serum 2
        return std::isfinite(d) ? d : fallback;
    }
    double number(const char *key, double fallback) const { return field(key).number(fallback); }
};

// the decompressed CBOR payload. Throws ParseError.
inline std::vector<uint8_t> serum_unpack(const Blob &preset) {
    constexpr size_t LIMIT = size_t(64) << 20;
    if (preset.size() > LIMIT) throw ParseError("Serum 2: preset too large");
    const std::vector<uint8_t> raw = preset.all(LIMIT);
    if (raw.size() < 25 || std::memcmp(raw.data(), "XferJson\0", 9)) throw ParseError("not a Serum 2 preset");
    const uint64_t metadata = le64(raw.data() + 9);
    if (metadata > raw.size() - 25) throw ParseError("Serum 2: invalid metadata length");
    const size_t off = 17 + size_t(metadata);
    const uint32_t size = le32(raw.data() + off), version = le32(raw.data() + off + 4);
    if (!size || size > LIMIT || version != 2) throw ParseError("Serum 2: unsupported payload (version " + std::to_string(version) + ")");
    const unsigned long long content = ZSTD_getFrameContentSize(raw.data() + off + 8, raw.size() - off - 8);
    if (content != ZSTD_CONTENTSIZE_UNKNOWN && content != size) throw ParseError("Serum 2: payload size mismatch");
    std::vector<uint8_t> decoded(size);
    const size_t got = ZSTD_decompress(decoded.data(), size, raw.data() + off + 8, raw.size() - off - 8);
    if (ZSTD_isError(got) || got != size) throw ParseError("Serum 2: invalid compressed payload");
    return decoded;
}

inline SerumItem serum_root(const std::vector<uint8_t> &decoded) {
    const uint8_t *cursor = decoded.data(), *end = cursor + decoded.size();
    SerumItem root = SerumItem::next(cursor, end);
    if (cursor != end || root.type != 5) throw ParseError("Serum 2: invalid preset map");
    return root;
}

// a referenced factory table ("Analog S2/Saw to Square.wav"): beside the preset and in each folder above it, the full
// path and its file name, also under a "Tables" folder there (Serum's own layout)
inline std::string serum_table_path(Volume &vol, const std::string &preset, std::string ref) {
    std::replace(ref.begin(), ref.end(), '\\', '/');
    while (!ref.empty() && ref.front() == '/') ref.erase(0, 1);
    if (ref.empty() || lower(path_ext(ref)) != "wav" || ref.find("..") != std::string::npos)
        throw ParseError("Serum 2: no wavetable file named");
    // 2.4.33 (GOGOi presets on the Live III): presets name tables "<Family> S2/<file>" / "<Family> S1/<file>", but Serum 2
    // installs them as Tables/S2 Tables/<Family>/<file> and Tables/<Family>/<file>: both spellings are tried
    std::string mapped;
    const size_t slash = ref.find('/');
    if (slash != std::string::npos && slash >= 3) {
        const std::string family = ref.substr(0, slash), rest = ref.substr(slash + 1);
        const std::string tag = lower(family.substr(family.size() - 3));
        if (tag == " s2") mapped = "S2 Tables/" + family.substr(0, family.size() - 3) + "/" + rest;
        else if (tag == " s1") mapped = family.substr(0, family.size() - 3) + "/" + rest;
    }
    std::string found;
    for (std::string dir = path_dir(preset);; dir = path_dir(dir)) {
        for (const std::string &rel : {ref, std::string("Tables/") + ref, path_name(ref)})
            if (find_ci(vol, path_resolve(dir, rel), found)) return found;
        if (!mapped.empty())
            for (const std::string &rel : {mapped, std::string("Tables/") + mapped})
                if (find_ci(vol, path_resolve(dir, rel), found)) return found;
        if (dir.empty() || path_dir(dir) == dir) break;
    }
    throw ParseError("Serum 2: factory wavetable not found: " + ref);
}

// oscillator `osc`'s table as float cycles (2048 per frame): embedded, else the referenced factory WAV
inline std::vector<float> serum_cycles(Volume &vol, const std::string &preset, int osc, std::string *name = nullptr) {
    if (osc < 0 || osc > 2) throw ParseError("Serum 2: invalid oscillator");
    const std::vector<uint8_t> decoded = serum_unpack(*vol.open(preset));
    const SerumItem wt = serum_root(decoded).field("Oscillator" + std::to_string(osc)).field("WTOsc" + std::to_string(osc));
    const SerumItem data = wt.field("embeddedWTData");
    std::vector<float> cycles;
    if (data.type == 4 && data.value >= uint64_t(WT_SIZE) && data.value % WT_SIZE == 0 && data.value <= uint64_t(WT_SIZE) * WT_MAX_FRAMES) {
        cycles.reserve(size_t(data.value));
        const uint8_t *cursor = data.p;
        while (cursor < data.end) cycles.push_back(float(clampd(SerumItem::next(cursor, data.end).number(0.0), -1, 1)));
        if (cycles.size() != data.value) throw ParseError("Serum 2: incomplete wavetable");
        if (name) { *name = wt.field("tableDisplayName").text(); if (name->empty()) *name = "Embedded"; }
        return cycles;
    }
    const std::string rel = wt.field("relativePathToWT").text();
    const std::string file = serum_table_path(vol, preset, rel);
    const BlobPtr blob = vol.open(file);
    const AudioInfo ai = probe_audio(*blob);
    if (ai.frames < WT_SIZE || ai.frames % WT_SIZE) throw ParseError("Serum 2: not a 2048-sample wavetable: " + rel);
    const PcmPtr pcm = decode_audio(*blob);
    const int64_t frames = std::min<int64_t>(pcm->frames(), int64_t(WT_SIZE) * WT_MAX_FRAMES);
    cycles.resize(size_t(frames - frames % WT_SIZE));
    for (size_t k = 0; k < cycles.size(); k++) cycles[k] = pcm->data[k * size_t(pcm->channels)] / 32768.f;   // the first channel
    if (name) *name = path_stem(file);
    return cycles;
}

// the SYNTH's table for a "serum:<osc>:<preset>" reference
inline WavetablePtr serum_table(Vfs &fs, const std::string &preset, int osc) {
    Location loc = fs.resolve(preset);
    std::string name;
    const std::vector<float> cycles = serum_cycles(*loc.volume, loc.inner, osc, &name);
    return build_wavetable(name, cycles, int(cycles.size() / WT_SIZE));
}

// 2.4.35: Serum LFO `lfo` (0..9)'s drawn curve as a VitalLine (the SYNTH's CUSTOM LFO shape, "serumlfo:<lfo>:<preset>").
// LFO<n>.curveData: xVals / yVals (0..1; numPoints + 1 values, the last closing the cycle), curveVals per segment (0.5 =
// straight). y = 0 is the TOP, as Vital's (an 808 preset's pitch-drop LFO in Envelope mode runs y 0 -> 1). A curve under
// 0.5 bends the segment to move early (that pitch drop: 0.01 .. 0.28) [G: the strength, power +-10 as Vital's]
inline bool serum_lfo_curve(const SerumItem &lfo, VitalLine &out) {
    const SerumItem cd = lfo.field("curveData");
    std::vector<double> x, y, c;
    auto read = [](const SerumItem &a, std::vector<double> &v) {
        if (a.type != 4 || a.value > 4096) return;
        const uint8_t *cur = a.p;
        while (cur < a.end) v.push_back(SerumItem::next(cur, a.end).number(0.0));
    };
    try { read(cd.field("xVals"), x); read(cd.field("yVals"), y); read(cd.field("curveVals"), c); }
    catch (const std::exception &) { return false; }
    if (x.size() < 2 || x.size() != y.size()) return false;
    out = VitalLine();
    for (size_t i = 0; i < x.size(); i++) {
        out.x.push_back(clampd(x[i], 0, 1));
        out.y.push_back(clampd(y[i], 0, 1));
        out.power.push_back(i < c.size() ? (clampd(c[i], 0, 1) - 0.5) * 20 : 0);
        if (i && out.x[i] < out.x[i - 1]) out.x[i] = out.x[i - 1];   // kept in order
    }
    return true;
}
inline bool serum_lfo_line(Volume &vol, const std::string &preset, int lfo, VitalLine &out) {
    if (lfo < 0 || lfo > 9) return false;
    const std::vector<uint8_t> decoded = serum_unpack(*vol.open(preset));
    if (!serum_lfo_curve(serum_root(decoded).field("LFO" + std::to_string(lfo)), out)) return false;
    out.name = "Serum LFO " + std::to_string(lfo + 1);
    return true;
}

}  // namespace omni

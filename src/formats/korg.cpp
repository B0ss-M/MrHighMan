// Omni Sampler: Korg Trinity / Triton / Karma sample media: KSF samples, KMP multisamples and KSC scripts.
//
// All are big-endian chunk files ("ID", size, body). Read from the factory and third-party disks (Trinity V3, Dance W/D,
// Mega Pianos, M1 and Orchestral Elements libraries, 4165 KSF / 561 KMP files):
//   KSF  SMP1  name[16], default bank u8, start u24, 2nd start u32, loop start u32, loop end u32 (inclusive)
//        SMD1  rate u32, attributes u8, loop tune s8 (cents), channels u8, bits u8 (8 / 16), frames u32, data
//              (signed PCM, 16-bit big-endian). Attribute bit 5: loop off. A loop of a few frames at the very end
//              is how one-shots hold a silent tail: played as no loop
//        SMF1  instead of SMD1: the file holding the audio (the same sample used under another name). A Triton
//              also writes an empty SMD1 (no frames) beside it
//        SNO1  sample number
//   KMP  MSP1  name[16], zone count u8, attributes u8
//        RLP1  per zone (18 bytes): original key (bit 7 a flag), top key, tune s8 (cents), level s8 (-99..+99, +99 =
//              double), pan u8 (64 = centre), cutoff s8, sample file name[12] ("SKIPPEDSAMPL" = none, "INTERNALnnnn" =
//              a ROM sample, not on disk). A zone starts one key above the previous zone's top key
//        NAME  long name; MNO1  multisample number; RLP2 per-zone extras (not used)
//   KSC  text: "#KORG Script Version 1.0" and the KMP / KSF files to load, one per line. A Triton saves them in a
//        folder named after the script (PRELOAD.KSC + PRELOAD/); floppy sets list files on the other disks
// The 8-bit data's attribute bits 0-3 vary per sample (0-5); their meaning (a level or a coding) is not known yet and
// they are not applied.
#include <cmath>
#include <cstring>
#include <map>
#include "../core/audio.hpp"
#include "format.hpp"

namespace omni {
namespace {

struct Chunk { std::string id; uint64_t off = 0, size = 0; };

std::vector<Chunk> chunks(const Blob &b) {
    std::vector<Chunk> out;
    uint64_t o = 0, n = b.size();
    while (o + 8 <= n && out.size() < 64) {
        std::vector<uint8_t> h = b.bytes(o, 8);
        Chunk c;
        c.id.assign(reinterpret_cast<const char *>(h.data()), 4);
        c.size = be32(&h[4]);
        c.off = o + 8;
        if (c.off + c.size > n) c.size = n - c.off;   // a truncated last chunk: take what is there
        out.push_back(c);
        o = c.off + c.size;
    }
    return out;
}

const Chunk *find(const std::vector<Chunk> &cs, const char *id) {
    for (auto &c : cs) if (!std::memcmp(c.id.data(), id, 4)) return &c;
    return nullptr;
}

std::string korg_name(const uint8_t *p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i]; i++) s += p[i] >= 32 && p[i] < 127 ? char(p[i]) : ' ';
    return trim(s);
}

// A sample file: next to the referring file, in a folder named after it (<KMP stem>/), in any folder beside it (the M1
// library's 00_PIANO.KMP keeps its samples in 00SAMPLE/, PBS's ACORDION.KMP in ACD_SAMP/), or one folder up and in the
// folders there (sets split over floppies: "disk 2/DS___058.KSF" takes its audio from "disk 1/"), and in their sample
// folders (TRINITY TFD sets: Disk1/ALL_INST.KSC loads Disk2/R1STEREO.KMP, whose samples are in Disk3/R1_SAMP2/).
// Folders are searched by listing them (found folders' names are real): find_ci on a miss re-lists the whole path from
// the root, thousands of folder reads per sample on a disk with sample folders (the M1's 93).
bool in_folder(Volume &vol, const std::string &dir, const std::string &name, std::string &found) {
    std::string p = path_join(dir, name);
    if (vol.exists(p)) { found = p; return true; }
    if (name.find_first_of("/\\") != std::string::npos) return find_ci(vol, p, found);   // "disk 1/x.KSF"
    try {
        for (auto &e : vol.list(dir))
            if (!e.dir && equals_ci(e.name, name)) { found = path_join(dir, e.name); return true; }
    } catch (const std::exception &) {}
    return false;
}

bool find_sample(Volume &vol, const std::string &dir, const std::string &stem, const std::string &name, std::string &found) {
    if (in_folder(vol, dir, name, found) || (vol.is_dir(path_join(dir, stem)) && in_folder(vol, path_join(dir, stem), name, found)))
        return true;
    auto in_subdirs = [&](const std::string &d) {
        try {
            for (auto &e : vol.list(d))
                if (e.dir && in_folder(vol, path_join(d, e.name), name, found)) return true;
        } catch (const std::exception &) {}
        return false;
    };
    if (in_subdirs(dir)) return true;
    std::string up = path_dir(dir);
    if (up.empty() || up == dir) return false;
    if (in_folder(vol, up, name, found) || in_subdirs(up)) return true;
    try {
        for (auto &e : vol.list(up))
            if (e.dir && path_join(up, e.name) != dir && in_subdirs(path_join(up, e.name))) return true;
    } catch (const std::exception &) {}
    return false;
}

struct Ksf {
    std::string name;
    int64_t start = 0, loop_start = 0, loop_end = -1, frames = 0;
    int rate = 48000, bits = 16, channels = 1, attr = 0, loop_tune = 0;
    BlobPtr data;              // the file holding the audio
    uint64_t data_off = 0;
    std::string key;
};

Ksf read_ksf(VolumePtr vol, const std::string &path, int depth = 0) {
    BlobPtr b = vol->open(path);
    auto cs = chunks(*b);
    const Chunk *smp = find(cs, "SMP1");
    if (!smp || smp->size < 32) throw ParseError("not a Korg KSF sample");
    std::vector<uint8_t> h = b->bytes(smp->off, 32);
    Ksf k;
    k.name = korg_name(h.data(), 16);
    k.start = be24(&h[17]);
    k.loop_start = be32(&h[24]);
    k.loop_end = be32(&h[28]);
    const Chunk *smd = find(cs, "SMD1"), *smf = find(cs, "SMF1");
    if (smd && smf && smd->size <= 12) smd = nullptr;   // an empty SMD1 next to the SMF1 that names the audio
    if (smd) {
        if (smd->size < 12) throw ParseError("short Korg sample data");
        std::vector<uint8_t> d = b->bytes(smd->off, 12);
        k.rate = int(be32(&d[0]));
        k.attr = d[4];
        k.loop_tune = int8_t(d[5]);
        k.channels = std::max(1, int(d[6]));
        k.bits = d[7];
        if (k.bits != 8 && k.bits != 16) throw ParseError("Korg sample of " + std::to_string(k.bits) + " bits");
        int64_t bytes = int64_t(smd->size) - 12;
        k.frames = std::min<int64_t>(be32(&d[8]), bytes / (k.bits / 8) / k.channels);
        k.data = b;
        k.data_off = smd->off + 12;
        k.key = "ksf:" + std::to_string(reinterpret_cast<uintptr_t>(vol.get())) + ":" + path;
    } else if (smf) {
        // the audio is in another file; its own SMP1 settings (name, loop) stay this file's
        std::vector<uint8_t> n = b->bytes(smf->off, size_t(std::min<uint64_t>(smf->size, 64)));
        std::string ref = korg_name(n.data(), n.size()), found;
        if (depth > 4 || !find_sample(*vol, path_dir(path), path_stem(path), ref, found)) throw ParseError("missing sample file " + ref);
        Ksf src = read_ksf(vol, found, depth + 1);
        k.rate = src.rate; k.attr = src.attr; k.loop_tune = src.loop_tune; k.channels = src.channels; k.bits = src.bits;
        k.frames = src.frames; k.data = src.data; k.data_off = src.data_off; k.key = src.key;
    } else {
        throw ParseError("Korg sample without audio");
    }
    if (k.rate < 4000 || k.rate > 192000) k.rate = 48000;
    return k;
}

// the sample as a zone's audio, start and loop
void apply_ksf(Zone &z, const Ksf &k, std::map<std::string, SampleRefPtr> &refs) {
    SampleRefPtr &r = refs[k.key];
    if (!r) r = raw_sample_ref(k.data, k.data_off, k.frames, k.channels, k.bits == 8 ? Enc::S8 : Enc::S16BE, k.rate, k.name);
    z.sample = r;
    z.start = std::max<int64_t>(0, std::min<int64_t>(k.start, k.frames - 1));
    bool loop_off = (k.attr & 0x20) != 0;
    bool tail = k.loop_end >= k.frames - 3 && k.loop_end - k.loop_start <= 8;   // a held silent tail: a one-shot
    if (!loop_off && !tail && k.loop_end > k.loop_start && k.loop_end < k.frames && k.loop_start >= z.start) {
        Loop l;
        l.start = k.loop_start;
        l.end = k.loop_end;
        l.tune = k.loop_tune / 100.0;
        z.loops.push_back(l);
    }
}

// --- KSF: one sample, over the whole keyboard -------------------------------------------------------------------

bool probe_ksf(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 8 && !std::memcmp(h, "SMP1", 4); }

std::vector<PresetInfo> list_ksf(VolumePtr vol, const std::string &path) {
    Ksf k = read_ksf(vol, path);
    return {PresetInfo{k.name.empty() ? path_stem(path) : k.name, 0}};
}

Instrument load_ksf(VolumePtr vol, const std::string &path, int) {
    Ksf k = read_ksf(vol, path);
    Instrument inst;
    inst.name = k.name.empty() ? path_stem(path) : k.name;
    inst.format = "Korg KSF";
    std::map<std::string, SampleRefPtr> refs;
    Zone z;
    z.name = inst.name;
    apply_ksf(z, k, refs);
    inst.zones.push_back(z);
    finish_instrument(inst);
    return inst;
}

// --- KMP: a multisample -------------------------------------------------------------------------------------------

bool probe_kmp(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 8 && !std::memcmp(h, "MSP1", 4); }

std::string kmp_name(const Blob &b, const std::vector<Chunk> &cs, const std::string &path) {
    if (const Chunk *nm = find(cs, "NAME")) {
        std::vector<uint8_t> d = b.bytes(nm->off, size_t(std::min<uint64_t>(nm->size, 64)));
        std::string s = korg_name(d.data(), d.size());
        if (!s.empty()) return s;
    }
    if (const Chunk *m = find(cs, "MSP1")) {
        std::vector<uint8_t> d = b.bytes(m->off, 16);
        std::string s = korg_name(d.data(), 16);
        if (!s.empty()) return s;
    }
    return path_stem(path);
}

// a multisample with no sample assigned (a Triton's blank "NewMS" slot)
bool kmp_empty(const Blob &b, const std::vector<Chunk> &cs) {
    const Chunk *rlp = find(cs, "RLP1");
    if (!rlp) return false;
    std::vector<uint8_t> r = b.bytes(rlp->off, size_t(rlp->size));
    for (size_t o = 0; o + 18 <= r.size(); o += 18) {
        std::string file = korg_name(&r[o] + 6, 12);
        if (!file.empty() && file.compare(0, 7, "SKIPPED")) return false;
    }
    return true;
}

std::vector<PresetInfo> list_kmp(VolumePtr vol, const std::string &path) {
    BlobPtr b = vol->open(path);
    return {PresetInfo{kmp_name(*b, chunks(*b), path), 0}};
}

Instrument load_kmp(VolumePtr vol, const std::string &path, int) {
    BlobPtr b = vol->open(path);
    auto cs = chunks(*b);
    const Chunk *msp = find(cs, "MSP1"), *rlp = find(cs, "RLP1");
    if (!msp || !rlp) throw ParseError("not a Korg KMP multisample");
    Instrument inst;
    inst.name = kmp_name(*b, cs, path);
    inst.format = "Korg KMP";
    std::vector<uint8_t> r = b->bytes(rlp->off, size_t(rlp->size));
    std::string dir = path_dir(path), stem = path_stem(path);
    std::map<std::string, SampleRefPtr> refs;
    int lo = 0, missing = 0, rom = 0;
    for (size_t o = 0; o + 18 <= r.size(); o += 18) {
        const uint8_t *e = &r[o];
        int root = e[0] & 0x7F, top = e[1] & 0x7F;
        std::string file = korg_name(e + 6, 12);
        int zlo = lo;
        lo = top + 1;
        if (file.empty() || !file.compare(0, 7, "SKIPPED")) continue;
        if (!file.compare(0, 8, "INTERNAL")) { rom++; continue; }
        std::string found;
        if (!find_sample(*vol, dir, stem, file, found)) { missing++; continue; }
        Ksf k;
        try { k = read_ksf(vol, found); } catch (const ParseError &) { missing++; continue; }
        Zone z;
        z.name = k.name.empty() ? path_stem(file) : k.name;
        z.key_lo = zlo;
        z.key_hi = std::max(zlo, top);
        z.root = root;
        z.tune = int8_t(e[2]) / 100.0;
        z.gain_db = 20.0 * std::log10(std::max(0.01, 1.0 + int8_t(e[3]) / 99.0));
        z.pan = clampd((int(e[4]) - 64) / 63.0, -1, 1);
        apply_ksf(z, k, refs);
        inst.zones.push_back(z);
    }
    if (missing) inst.warnings.push_back(std::to_string(missing) + " sample files missing");
    if (rom) inst.warnings.push_back(std::to_string(rom) + " zones use the instrument's ROM samples (not on disk)");
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError(rom ? "this multisample only uses ROM samples"
                                              : missing ? "no samples found for this multisample"
                                                        : "this multisample is empty (no samples assigned)");
    return inst;
}

// --- KSC: a load script --------------------------------------------------------------------------------------------

bool probe_ksc(const std::string &, const uint8_t *h, size_t n, uint64_t) { return n >= 12 && !std::memcmp(h, "#KORG Script", 12); }

std::vector<std::string> ksc_lines(VolumePtr vol, const std::string &path) {
    std::vector<uint8_t> d = vol->open(path)->head(1 << 20);
    std::vector<std::string> out;
    std::string cur;
    for (uint8_t c : d) {
        if (c == '\r' || c == '\n') { cur = trim(cur); if (!cur.empty() && cur[0] != '#') out.push_back(cur); cur.clear(); }
        else cur += char(c);
    }
    cur = trim(cur);
    if (!cur.empty() && cur[0] != '#') out.push_back(cur);
    return out;
}

// The script's multisamples, each a preset; a script of samples only (a drum disk) plays them one per key from C1.
std::vector<PresetInfo> list_ksc(VolumePtr vol, const std::string &path) {
    std::vector<PresetInfo> out;
    int i = 0, samples = 0;
    std::string dir = path_dir(path), stem = path_stem(path);
    for (auto &line : ksc_lines(vol, path)) {
        std::string ext = path_ext(line), found;
        if (ext == "kmp") {   // listed even when missing: loading it then says which file is not there
            std::string name = path_stem(line);
            bool empty = false;
            if (find_sample(*vol, dir, stem, line, found))
                try {
                    BlobPtr b = vol->open(found);
                    auto cs = chunks(*b);
                    name = kmp_name(*b, cs, found);
                    empty = kmp_empty(*b, cs);
                } catch (const ParseError &) {}
            if (!empty) out.push_back(PresetInfo{name, i});
        } else if (ext == "ksf") {
            samples++;
        }
        i++;
    }
    if (out.empty() && samples) out.push_back(PresetInfo{path_stem(path) + " (samples)", -1});
    return out;
}

Instrument load_ksc(VolumePtr vol, const std::string &path, int index) {
    std::vector<std::string> lines = ksc_lines(vol, path);
    std::string dir = path_dir(path), stem = path_stem(path), found;
    if (index >= 0) {
        if (size_t(index) >= lines.size() || !find_sample(*vol, dir, stem, lines[size_t(index)], found))
            throw ParseError("multisample not found: " + (size_t(index) < lines.size() ? lines[size_t(index)] : std::string("?")));
        Instrument inst = load_kmp(vol, found, 0);
        return inst;
    }
    Instrument inst;
    inst.name = path_stem(path);
    inst.format = "Korg KSC";
    std::map<std::string, SampleRefPtr> refs;
    int key = 36;
    for (auto &line : lines) {
        if (path_ext(line) != "ksf" || key > 127) continue;
        if (!find_sample(*vol, dir, stem, line, found)) continue;
        try {
            Ksf k = read_ksf(vol, found);
            Zone z;
            z.name = k.name.empty() ? path_stem(line) : k.name;
            z.key_lo = z.key_hi = z.root = key++;
            apply_ksf(z, k, refs);
            inst.zones.push_back(z);
        } catch (const ParseError &) {}
    }
    finish_instrument(inst);
    if (inst.zones.empty()) throw ParseError("no samples found for this script");
    return inst;
}

}  // namespace

void register_korg() {
    register_reader({"Korg KMP multisample", "kmp", probe_kmp, list_kmp, load_kmp});
    register_reader({"Korg KSF sample", "ksf", probe_ksf, list_ksf, load_ksf});
    register_reader({"Korg KSC script", "ksc", probe_ksc, list_ksc, load_ksc});
}

}  // namespace omni

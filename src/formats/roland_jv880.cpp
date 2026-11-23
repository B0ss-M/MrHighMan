// Omni Sampler: the Roland JV-880's patches, rhythm sets and waves, and the SR-JV80 expansion boards' (SR-JV80-01 -
// 19, the RD-500's board), played from their ROMs (the jv880_rom2 / jv880_waverom1 / 2 set that sd88me's JV-880 plugin
// and MAME use). Tapping the JV-880's control ROM (or its first wave ROM) opens the JV-880; tapping a board opens that
// board, whose patches also play the JV-880's waves when its ROMs are in the same folder. The JV-1080 (its waves, and
// its SysEx patch banks played from its ROMs and the boards), the XV-3080 (its waves, GM2 tones and kits) and the Roland Cloud JV-1080's
// bank files (played from the XV-3080's waves) are further down. Layouts measured on the ROMs
// 2026-10-22 (field names from giulioz's jv880_juce, MAME licence; facts only, own code):
//   - Wave ROMs and boards use the SC-55's scramble and sample coding (roland_gss.hpp): the JV-880's two 2 MB wave ROMs
//     make one 4 MB image; a board (raw header "Roland JV80 O") is one 8 MB image.
//   - Samples (18 bytes): level, start BE24, loop start BE24, end BE24 (addresses in the image), ?, ?, mode [12] (0
//     forward loop, 1 back and forth, 2 none, 4 - 6 reversed), root key [13], attack and sustain pitch BE16 [14] [16]
//     (1024 = 0, 1/1000 semitone); pitched as the SC-55's (319 of 331 of the JV-880's tonal loops within 5 cents).
//   - Waves (60 bytes, the SC-55's partials): name[12], 16 break keys (127 = the last), 16 sample numbers BE16.
//   - The JV-880's tables are in its control ROM (256 KB): waves from 0x4 (129), then samples at 0x1E40; patches
//     (362 bytes) at 0x8CE0 (Internal), 0x10CE0 (Preset A) and 0x18CE0 (Preset B), 64 each; rhythm sets (61 tones of 44
//     bytes, keys 36 - 96) at 0xE760, 0x16760 and 0x1E760.
//   - A board's header (descrambled): sample, wave, patch and rhythm set counts BE16 at 0x60, 0x62, 0x66, 0x68; their
//     tables BE32 at 0x80, 0x84, 0x8C, 0x90.
//   - A patch: name[12], velocity switch [12] bit 7, level [21], pan [22], then 4 tones of 84 bytes: on [0] bit 7,
//     wave group [0] bits 0-1 (0 the JV-880's, 1 the board's), wave [1], velocity range [3] - [4], coarse [37] and fine
//     [38] (signed semitones / cents), pitch key follow [40] high nibble (7 = 100 %), TVA level [67], pan [68] (128 =
//     random), envelope T1 L1 T2 L2 T3 L3 T4 [74 - 80].
//   - A rhythm tone: on / group [0], wave [1], mute group [2] (bits 0-4), the key it sounds [3], fine [4], level [30],
//     pan [31], envelope T1 L1 T2 L2 T3 L3 T4 [34 - 40].
//   - Envelope times are taken as the SC-55's table (assumed; the JV's own table is not read).
// Not read: filters, LFOs, the pitch envelope, FXM, velocity curves, the patch's effects.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include "../third_party/tinf/tinf.h"
#include "format.hpp"
#include "roland_gss.hpp"

namespace omni {
namespace {

using gss::MB;
using gss::RATE;
using gss::env_seconds;

constexpr uint64_t CTRL_SIZE = 0x40000, WAVE_SIZE = 2 * MB, BOARD_SIZE = 8 * MB;
constexpr uint32_t CTRL_CRC = 0x85DC7032;               // jv880_rom2.bin (1.0.0), its first 4 KB
constexpr uint32_t WAVE1_CRC = 0xAA8F5009, WAVE2_CRC = 0x3C4D1707;
constexpr uint32_t PATCH_AT[3] = {0x8CE0, 0x10CE0, 0x18CE0}, RHYTHM_AT[3] = {0xE760, 0x16760, 0x1E760};
const char *const BANK[3] = {"INT", "PR-A", "PR-B"};
constexpr size_t PATCH = 362, TONE = 84, RTONE = 44, RHYTHM = 61 * RTONE;
constexpr int KINDS = 1000, SOURCES = 10000;            // preset index: source x 10000 + kind x 1000 + number
enum Kind { PATCHES, RHYTHMS, WAVES };
const double KEYFOLLOW[16] = {-1, -0.7, -0.5, -0.3, -0.1, 0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.7, 1, 1.2, 1.5, 2};

struct Smp { uint32_t start = 0, lstart = 0, end = 0; int mode = 2, root = 60, pinit = 0, psust = 0, level = 127, rate = RATE; bool blank = false; };
struct Wave { std::string name; std::vector<uint8_t> brk; std::vector<uint16_t> smp; };   // 16 key slots (the XV's: any)
struct Src {
    std::string library, file;
    std::vector<Smp> smps;
    std::vector<Wave> waves;
    std::vector<std::vector<uint8_t>> patches, rhythms;
    std::vector<std::string> patch_names, rhythm_names;
    std::shared_ptr<const std::vector<uint8_t>> image;
};
using SrcPtr = std::shared_ptr<const Src>;

std::mutex cache_mutex;
std::map<std::string, SrcPtr> cache;

uint32_t be(const std::vector<uint8_t> &d, size_t at, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | (at + size_t(i) < d.size() ? d[at + size_t(i)] : 0);
    return v;
}
std::string name_at(const std::vector<uint8_t> &d, size_t at) {
    std::string s(reinterpret_cast<const char *>(&d[at]), 12);
    for (char &ch : s) if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126) ch = ' ';
    return trim(s);
}

void read_samples(Src &s, const std::vector<uint8_t> &d, size_t at, size_t n) {
    for (size_t i = 0; i < n && at + 18 * (i + 1) <= d.size(); i++) {
        const size_t o = at + 18 * i;
        Smp m;
        m.level = d[o];
        m.start = be(d, o + 1, 3);
        m.lstart = be(d, o + 4, 3);
        m.end = be(d, o + 7, 3);
        m.mode = d[o + 12];
        m.root = d[o + 13];
        m.pinit = int(be(d, o + 14, 2)) - 1024;
        m.psust = int(be(d, o + 16, 2)) - 1024;
        if (m.end < m.start || m.lstart < m.start || m.lstart > m.end) m.mode = 2;
        s.smps.push_back(m);
    }
}

Wave read_wave(const std::vector<uint8_t> &d, size_t at) {
    Wave w;
    w.name = name_at(d, at);
    for (size_t k = 0; k < 16; k++) { w.brk.push_back(d[at + 12 + k]); w.smp.push_back(uint16_t(be(d, at + 28 + 2 * k, 2))); }
    return w;
}

// Drum waves fill their unused keys with a short silent sample (20 frames of 0): those keys are left empty
void mark_blanks(Src &s) {
    for (Smp &m : s.smps) {
        if (m.end < m.start || m.end - m.start >= 64 || m.end >= s.image->size()) continue;
        const PcmPtr p = gss::decode(*s.image, m.start, int(m.end - m.start), 0);
        m.blank = std::all_of(p->data.begin(), p->data.end(), [](int16_t v) { return v == 0; });
    }
}

bool is_wave_record(const std::vector<uint8_t> &d, size_t at) {
    if (at + 60 > d.size()) return false;
    for (size_t i = 0; i < 12; i++) if (d[at + i] < 32 || d[at + i] > 126) return false;
    return std::find(&d[at + 12], &d[at + 28], 127) != &d[at + 28];
}

void add_patch(Src &s, const std::vector<uint8_t> &d, size_t at, const std::string &prefix) {
    if (at + PATCH > d.size()) return;
    s.patches.emplace_back(d.begin() + long(at), d.begin() + long(at + PATCH));
    s.patch_names.push_back(prefix + name_at(d, at));
}

void add_rhythm(Src &s, const std::vector<uint8_t> &d, size_t at, const std::string &name) {
    if (at + RHYTHM > d.size()) return;
    s.rhythms.emplace_back(d.begin() + long(at), d.begin() + long(at + RHYTHM));
    s.rhythm_names.push_back(name);
}

// The JV-880 itself: control ROM tables, the two wave ROMs' image
SrcPtr open_internal(Volume &vol, const std::string &dir) {
    const std::string key = dir + "\n<jv880>";
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    std::string ctrl, w1, w2;
    for (const DirEntry &e : vol.list(dir)) {
        if (e.dir || (e.size != CTRL_SIZE && e.size != WAVE_SIZE)) continue;
        try {
            std::vector<uint8_t> h = vol.open(path_join(dir, e.name))->head(4096);
            if (h.size() < 4096) continue;
            const uint32_t crc = tinf_crc32(h.data(), 4096);
            if (crc == CTRL_CRC && e.size == CTRL_SIZE) ctrl = e.name;
            if (crc == WAVE1_CRC && e.size == WAVE_SIZE) w1 = e.name;
            if (crc == WAVE2_CRC && e.size == WAVE_SIZE) w2 = e.name;
        } catch (const std::exception &) {}
    }
    if (ctrl.empty() || w1.empty() || w2.empty()) return nullptr;
    auto s = std::make_shared<Src>();
    s->library = "Roland JV-880";
    s->file = ctrl;
    const std::vector<uint8_t> c = vol.open(path_join(dir, ctrl))->all(CTRL_SIZE);
    size_t at = 4;
    while (is_wave_record(c, at)) { s->waves.push_back(read_wave(c, at)); at += 60; }
    size_t n = 0;
    for (const Wave &w : s->waves)
        for (uint16_t i : w.smp) if (i != 0xFFFF) n = std::max(n, size_t(i) + 1);
    read_samples(*s, c, at, n);
    for (int b = 0; b < 3; b++) {
        for (size_t j = 0; j < 64; j++) add_patch(*s, c, PATCH_AT[b] + PATCH * j, std::string(BANK[b]) + " " + std::to_string(j / 8 + 1) + std::to_string(j % 8 + 1) + " ");
        add_rhythm(*s, c, RHYTHM_AT[b], std::string("Rhythm Set ") + BANK[b]);
    }
    auto image = std::make_shared<std::vector<uint8_t>>();
    image->reserve(2 * WAVE_SIZE);
    gss::descramble(vol.open(path_join(dir, w1))->all(WAVE_SIZE), *image);
    gss::descramble(vol.open(path_join(dir, w2))->all(WAVE_SIZE), *image);
    s->image = image;
    mark_blanks(*s);
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache[key] = s;
    return s;
}

bool is_board(const uint8_t *h, size_t n, uint64_t size) {
    return size == BOARD_SIZE && n >= 16 && !std::memcmp(h, "Roland JV80 O", 13);
}

std::string board_name(const std::vector<uint8_t> &d) {
    const std::string id(reinterpret_cast<const char *>(&d[0x20]), 6);   // "POP100"
    if (id == "POP100") return "SR-JV80-01 Pop";
    if (id == "ORC100") return "SR-JV80-02 Orchestral";
    if (id == "PNO100") return "SR-JV80-03 Piano";
    if (id == "COL100") return "RD-500 Expansion";
    std::string s(reinterpret_cast<const char *>(&d[0x10]), 16);
    for (char &ch : s) if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126) ch = ' ';
    return trim(s);
}

SrcPtr open_board(Volume &vol, const std::string &path) {
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
    }
    auto image = std::make_shared<std::vector<uint8_t>>();
    image->reserve(BOARD_SIZE);
    gss::descramble(vol.open(path)->all(BOARD_SIZE), *image);
    const std::vector<uint8_t> &d = *image;
    auto s = std::make_shared<Src>();
    s->library = "Roland " + board_name(d);
    s->file = path_name(path);
    const size_t ns = be(d, 0x60, 2), nw = be(d, 0x62, 2), np = be(d, 0x66, 2), nd = be(d, 0x68, 2);
    const size_t S = be(d, 0x80, 4), W = be(d, 0x84, 4), P = be(d, 0x8C, 4), D = be(d, 0x90, 4);
    read_samples(*s, d, S, ns);
    for (size_t i = 0; i < nw && W + 60 * (i + 1) <= d.size(); i++) s->waves.push_back(read_wave(d, W + 60 * i));
    for (size_t i = 0; i < np; i++) add_patch(*s, d, P + PATCH * i, "");
    for (size_t i = 0; i < nd; i++) add_rhythm(*s, d, D + RHYTHM * i, "Rhythm Set " + std::to_string(i + 1));
    s->image = image;
    mark_blanks(*s);
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache[path] = s;
    return s;
}

// ---- zones ----

int pingpong_frames(const Smp &s) { return s.mode == 1 && s.end > s.lstart ? int(s.end - s.lstart) + 1 : 0; }

SampleRefPtr sample_ref(const SrcPtr &src, int id) {
    const Smp s = src->smps[size_t(id)];
    auto image = src->image;
    const int len = int(s.end - s.start);
    if (s.end >= image->size()) return nullptr;
    auto ref = std::make_shared<SampleRef>();
    ref->name = "Sample " + std::to_string(id + 1);
    ref->rate = s.rate;
    ref->channels = 1;
    ref->frames = len + 1 + pingpong_frames(s);
    ref->key = "jv880:" + src->library + ":" + std::to_string(id);
    const int loop = s.mode == 1 ? int(s.end - s.lstart) : 0;
    ref->decode = [image, s, len, loop]() -> PcmPtr { return gss::decode(*image, s.start, len, loop, s.rate); };
    return ref;
}

// The sample of a wave for a pitch (the key after key follow and coarse tune)
int pick(const Src &src, int wave, int pitch_key) {
    if (wave < 0 || wave >= int(src.waves.size())) return -1;
    const Wave &w = src.waves[size_t(wave)];
    const int bp = std::clamp(pitch_key, 0, 127);
    for (size_t j = 0; j < w.brk.size(); j++)
        if (w.brk[j] >= bp || w.brk[j] == 0x7F) return w.smp[j] < src.smps.size() && !src.smps[w.smp[j]].blank ? w.smp[j] : -1;
    return -1;
}

Zone make_zone(const SrcPtr &src, int sid, std::map<int, SampleRefPtr> &refs) {
    const Smp &s = src->smps[size_t(sid)];
    Zone z;
    SampleRefPtr &ref = refs[sid];
    if (!ref) ref = sample_ref(src, sid);
    z.sample = ref;
    z.root = s.root;
    z.gain_db = velocity_to_db(s.level);
    if (s.mode >= 4) {
        z.reverse = true;   // the reversed waves ("REV ..."): played backwards through, without a loop
    } else if (s.mode == 0 || s.mode == 1) {
        Loop lp;
        lp.start = s.lstart - s.start;
        lp.end = s.end - s.start + pingpong_frames(s);
        lp.tune = s.psust / 1000.0;
        z.loops.push_back(lp);
    }
    return z;
}

// T1 L1 T2 L2 T3 L3 (held) T4 (release)
Envelope envelope(const uint8_t *e, double &peak_db) {
    Envelope env;
    env.set = true;
    const double t[3] = {env_seconds(e[0]), env_seconds(e[2]), env_seconds(e[4])};
    const double l[3] = {(e[1] & 0x7F) / 127.0, (e[3] & 0x7F) / 127.0, (e[5] & 0x7F) / 127.0};
    const int peak = l[1] > l[0] ? 1 : 0;
    const double top = std::max(l[peak], 1 / 127.0);
    peak_db = 20 * std::log10(top);
    env.attack = t[0] + (peak ? t[1] : 0);
    env.decay = t[2] + (peak ? 0 : t[1]);
    env.sustain = std::min(1.0, l[2] / top);
    env.release = std::max(0.005, env_seconds(e[6]));
    return env;
}

const Src *wave_source(const SrcPtr &own, const SrcPtr &internal, int group, SrcPtr &out) {
    out = group == 0 ? internal : group == 1 && own != internal ? own : nullptr;
    return out.get();
}

void load_patch(const SrcPtr &own, const SrcPtr &internal, size_t p, Instrument &inst) {
    const std::vector<uint8_t> &d = own->patches[p];
    const bool vel_switch = d[12] & 0x80;
    std::map<const Src *, std::map<int, SampleRefPtr>> refs;
    for (size_t t = 0; t < 4; t++) {
        const uint8_t *q = &d[26 + TONE * t];
        if (!(q[0] & 0x80)) continue;
        SrcPtr src;
        if (!wave_source(own, internal, q[0] & 3, src)) {
            inst.warnings.push_back((q[0] & 3) == 0 ? "a tone needs the JV-880's ROMs beside the board" : "a tone needs an expansion board");
            continue;
        }
        const double kf = KEYFOLLOW[std::min(15, (q[40] >> 4) + 5)];
        const int coarse = int8_t(q[37]);
        const double fine = int8_t(q[38]) / 100.0;
        double peak_db = 0;
        const Envelope env = envelope(q + 74, peak_db);
        const double pan = (q[68] > 127 ? 0 : (q[68] - 64) / 64.0) + (d[22] - 64) / 64.0;
        auto key_pitch = [&](int k) { return int(std::floor(60 + (k - 60) * kf)) + coarse; };
        for (int k = 0; k < 128;) {
            const int sid = pick(*src, q[1], key_pitch(k));
            int hi = k;
            while (hi + 1 < 128 && pick(*src, q[1], key_pitch(hi + 1)) == sid) hi++;
            if (sid >= 0) {
                Zone z = make_zone(src, sid, refs[src.get()]);
                if (z.sample) {
                    const Smp &s = src->smps[size_t(sid)];
                    z.name = own->patch_names[p] + " " + std::to_string(t + 1);
                    z.key_lo = k;
                    z.key_hi = hi;
                    if (vel_switch) { z.vel_lo = std::min<int>(q[3], 127); z.vel_hi = std::clamp<int>(q[4], z.vel_lo, 127); }
                    z.key_tracking = kf;
                    // the engine plays (note - root) x key follow + tune; the chip 60 + (note - 60) x key follow + coarse
                    // + fine, its sample sounding root - attack pitch / 1000 at its root
                    z.tune = 60 + (k - 60) * kf + coarse + fine - (k - s.root) * kf - s.root + s.pinit / 1000.0;
                    z.gain_db += velocity_to_db(d[21]) + velocity_to_db(q[67]) + peak_db;
                    z.pan = std::clamp(pan, -1.0, 1.0);
                    z.amp_env = env;
                    inst.zones.push_back(std::move(z));
                }
            }
            k = hi + 1;
        }
    }
}

void load_rhythm(const SrcPtr &own, const SrcPtr &internal, size_t r, Instrument &inst) {
    const std::vector<uint8_t> &d = own->rhythms[r];
    std::map<const Src *, std::map<int, SampleRefPtr>> refs;
    for (size_t k = 0; k < 61; k++) {
        const uint8_t *q = &d[RTONE * k];
        if (!(q[0] & 0x80)) continue;
        SrcPtr src;
        if (!wave_source(own, internal, q[0] & 3, src)) continue;
        const int sid = pick(*src, q[1], q[3]);
        if (sid < 0) continue;
        Zone z = make_zone(src, sid, refs[src.get()]);
        if (!z.sample) continue;
        const Smp &s = src->smps[size_t(sid)];
        double peak_db = 0;
        z.amp_env = envelope(q + 34, peak_db);
        // drum hits ring out after a short tap, as their envelope decays
        z.amp_env.release = std::max(z.amp_env.release, z.amp_env.attack + z.amp_env.decay);
        z.name = src->waves[q[1]].name;
        z.key_lo = z.key_hi = z.root = int(36 + k);
        z.tune = q[3] + int8_t(q[4]) / 100.0 - s.root + s.pinit / 1000.0;
        z.gain_db += velocity_to_db(q[30]) + peak_db;
        z.pan = q[31] > 127 ? 0 : std::clamp((q[31] - 64) / 64.0, -1.0, 1.0);
        z.exclusive_group = q[2] & 0x1F;
        inst.zones.push_back(std::move(z));
    }
}

void load_wave(const SrcPtr &src, size_t w, Instrument &inst) {
    std::map<int, SampleRefPtr> refs;
    for (int k = 0; k < 128;) {
        const int sid = pick(*src, int(w), k);
        int hi = k;
        while (hi + 1 < 128 && pick(*src, int(w), hi + 1) == sid) hi++;
        if (sid >= 0) {
            Zone z = make_zone(src, sid, refs);
            if (z.sample) {
                const Smp &s = src->smps[size_t(sid)];
                z.name = src->waves[w].name;
                z.key_lo = k;
                z.key_hi = hi;
                z.tune = s.pinit / 1000.0;
                inst.zones.push_back(std::move(z));
            }
        }
        k = hi + 1;
    }
}

// ---- the JV-1080 ----
// Its ROMs (MAME's jv1080 set): jv1080_program.bin (1 MB, the CPU's, mapped at 0x0A000000) and four 2 MB wave ROMs
// (raw header "RolandJV1080O"; 16 bits wide: gss::descramble's wide order), one 8 MB image. Measured on the ROMs
// 2026-10-22:
//   - The program ROM holds two wave tables, each a header (wave count BE16, sample count BE16, the samples' address
//     BE32) and then the waves (60 bytes, as above): 0x71000 (326 waves, 957 samples in wave ROMs 0 - 2) and 0x7A800
//     (132 waves, 430 samples in wave ROM 3; addresses from its start).
//   - The waves as the JV-1080 numbers them: INT-A at 0x300, 255 table entries BE16 and then 255 bytes naming the table
//     (0 the first, 1 the second); INT-B at 0x5FE, 193 the same way. 448 waves (the manual's count).
// Its SysEx (F0 41 dev 6A 12 address data checksum F7): a patch is a common block (72 bytes, address 11 pp 00 00 for
// user patch pp, 03 00 00 00 the patch being edited) and four tones (129 bytes, 11 pp 10 / 12 / 14 / 16 00). Bytes read
// (the parameter order of the JV-1080's MIDI implementation; ranges checked on ROLAND.SYX and 8090USER.SYX):
//   - common: name[12], patch level [0x2E], pan [0x2F] (64 = centre), velocity range switch [0x40], octave [0x41] (3 =
//     0).
//   - tone: on [0x00], wave group type [0x01] (0 internal, 2 an expansion board), group [0x02] (1 INT-A, 2 INT-B; a
//     board's number + 3: SR-JV80-01 Pop is 4, matching its wave names; the other boards inferred), wave [0x03] [0x04]
//     (nibbles, from 0), wave gain [0x05] (0 - 3: -6, 0, +6, +12 dB), velocity range [0x0C] - [0x0D], key range [0x0E] -
//     [0x0F], coarse [0x3D] (48 = 0), fine [0x3E] (50 = 0, cents), pitch key follow [0x40] (the KEYFOLLOW steps), filter
//     type [0x50] (0 off, 1 low, 2 band, 3 high pass, 4 peaking), cutoff [0x51], cutoff key follow [0x52], resonance
//     [0x53], filter envelope depth [0x55] (63 = 0), times T1 - T4 [0x5B - 0x5E] and levels L1 - L4 [0x5F - 0x62],
//     level [0x65], envelope times T1 - T4 [0x6E - 0x71] and levels L1 - L3 [0x72 - 0x74], pan [0x77].
//   - Filter cutoff in Hz assumed (20 Hz - 20 kHz, exponential), the filter envelope's depth as 2 cutoff steps a step;
//     envelope times as the SC-55's table, as for the JV-880.
// Not read: rhythm sets, LFOs, the pitch envelope, FXM, tone delay, velocity curves, effects, PCM cards.

constexpr uint32_t J_PROG_CRC = 0xD2C75FA7, J_WAVE_CRC[4] = {0x91DEF571, 0x49EA664F, 0xBA689FD5, 0xDBDECC20};
constexpr uint32_t J_BASE = 0x0A000000, J_TABLE[2] = {0x71000, 0x7A800}, J_LIST[2] = {0x300, 0x5FE};
constexpr size_t J_LIST_N[2] = {255, 193}, J_COMMON = 0x48, J_TONE = 0x81;
std::string cached_jv1080;   // the last folder found with the ROMs (find_beside)

// the JV-1080's ROM files in a folder listing: program, wave 0 - 3; empty when one is missing
std::vector<std::string> jv1080_files(Volume &vol, const std::string &dir) {
    std::vector<std::string> out(5);
    for (const DirEntry &e : vol.list(dir)) {
        if (e.dir || (e.size != MB && e.size != WAVE_SIZE)) continue;
        try {
            std::vector<uint8_t> h = vol.open(path_join(dir, e.name))->head(4096);
            if (h.size() < 4096) continue;
            const uint32_t crc = tinf_crc32(h.data(), 4096);
            if (crc == J_PROG_CRC && e.size == MB) out[0] = e.name;
            for (size_t k = 0; k < 4; k++) if (crc == J_WAVE_CRC[k] && e.size == WAVE_SIZE) out[k + 1] = e.name;
        } catch (const std::exception &) {}
    }
    for (const std::string &f : out) if (f.empty()) return {};
    return out;
}

bool is_jv1080_dir(Volume &vol, const std::string &dir) {
    try { return !jv1080_files(vol, dir).empty(); } catch (const std::exception &) { return false; }
}

SrcPtr open_jv1080(Volume &vol, const std::string &dir) {
    const std::string key = dir + "\n<jv1080>";
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    const std::vector<std::string> f = jv1080_files(vol, dir);
    if (f.empty()) throw ParseError("JV-1080: its ROMs are missing (jv1080_program.bin, jv1080_wave0 - 3.bin in one folder)");
    auto s = std::make_shared<Src>();
    s->library = "Roland JV-1080";
    s->file = f[0];
    const std::vector<uint8_t> p = vol.open(path_join(dir, f[0]))->all(MB);
    auto image = std::make_shared<std::vector<uint8_t>>();
    image->reserve(4 * WAVE_SIZE);
    for (size_t k = 1; k <= 4; k++) gss::descramble(vol.open(path_join(dir, f[k]))->all(WAVE_SIZE), *image, true);
    std::vector<Wave> table[2];
    for (size_t g = 0; g < 2; g++) {
        const size_t h = J_TABLE[g], nw = be(p, h, 2), ns = be(p, h + 2, 2), at = be(p, h + 4, 4) - J_BASE;
        const size_t first = s->smps.size();
        read_samples(*s, p, at, ns);
        if (g == 1)   // the second table's samples are in wave ROM 3, the image's last 2 MB
            for (size_t i = first; i < s->smps.size(); i++) { Smp &m = s->smps[i]; m.start += 6 * MB; m.lstart += 6 * MB; m.end += 6 * MB; }
        for (size_t i = 0; i < nw && h + 8 + 60 * (i + 1) <= p.size(); i++) {
            Wave w = read_wave(p, h + 8 + 60 * i);
            for (uint16_t &n : w.smp) if (n != 0xFFFF) n = uint16_t(n + first);
            table[g].push_back(w);
        }
    }
    for (size_t g = 0; g < 2; g++)   // INT-A, INT-B in the JV-1080's order
        for (size_t i = 0; i < J_LIST_N[g]; i++) {
            const size_t n = be(p, J_LIST[g] + 2 * i, 2), t = p[J_LIST[g] + 2 * J_LIST_N[g] + i];
            if (t < 2 && n < table[t].size()) s->waves.push_back(table[t][n]);
            else s->waves.push_back(Wave{"?", {}, {}});
        }
    s->image = image;
    mark_blanks(*s);
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache[key] = s;
    return s;
}

// The expansion board numbered n (SR-JV80-n) in a folder: its 8 MB image with "Roland JV80 O" ("POP / ORC / Pno Exp"
// for 01 - 03, else "SR-JV80-nn" at byte 16)
std::string find_board(Volume &vol, const std::string &dir, int n) {
    static const char *const OLD[3] = {"POP Exp", "ORC Exp", "Pno Exp"};
    char want[16];
    std::snprintf(want, sizeof want, "SR-JV80-%02d", n);
    for (const DirEntry &e : vol.list(dir)) {
        if (e.dir || e.size != BOARD_SIZE) continue;
        try {
            std::vector<uint8_t> h = vol.open(path_join(dir, e.name))->head(32);
            if (!is_board(h.data(), h.size(), e.size)) continue;
            const std::string id(reinterpret_cast<const char *>(&h[16]), 10);
            if ((n <= 3 && !id.compare(0, 7, OLD[n - 1])) || id == want) return path_join(dir, e.name);
        } catch (const std::exception &) {}
    }
    return "";
}

// A patch as SysEx bytes; a Roland Cloud one names its tones' XV-3080 waves instead (left, right; from 1, 0 = none)
struct Patch1080 { std::string name; std::vector<uint8_t> common, tone[4]; bool xv = false; int xv_wave[4][2] = {}; };

// The patches of a JV-1080 SysEx file: user patches (11 pp ..) and the edit buffer (03 00 ..)
std::vector<Patch1080> read_syx1080(Volume &vol, const std::string &path) {
    const std::vector<uint8_t> d = vol.open(path)->all(size_t(1) << 22);
    std::map<int, Patch1080> by;
    for (size_t i = 0; i + 11 < d.size(); i++) {
        if (d[i] != 0xF0 || d[i + 1] != 0x41 || d[i + 3] != 0x6A || d[i + 4] != 0x12) continue;
        size_t end = i + 5;
        while (end < d.size() && d[end] != 0xF7) end++;
        if (end >= d.size() || end < i + 11) break;
        const uint8_t a1 = d[i + 5], a2 = d[i + 6], a3 = d[i + 7], a4 = d[i + 8];
        const int pn = a1 == 0x11 ? a2 : a1 == 0x03 && a2 == 0 ? 1000 : -1;
        if (pn >= 0) {
            Patch1080 &pt = by[pn];
            std::vector<uint8_t> *dst = a3 == 0 ? &pt.common : a3 >= 0x10 && a3 <= 0x16 && !(a3 & 1) ? &pt.tone[(a3 - 0x10) / 2] : nullptr;
            if (dst) {
                const size_t want = a3 == 0 ? J_COMMON : J_TONE;
                if (dst->size() < want) dst->resize(want);
                for (size_t k = i + 9; k + 1 < end && a4 + (k - i - 9) < want; k++) (*dst)[a4 + (k - i - 9)] = d[k];
            }
        }
        i = end;
    }
    std::vector<Patch1080> out;
    for (auto &kv : by) {
        Patch1080 &pt = kv.second;
        if (pt.common.size() < J_COMMON) continue;
        bool on = false;   // a bank's empty slots (every tone off: "zilch", or no name) are not listed
        for (const std::vector<uint8_t> &t : pt.tone) on |= t.size() >= J_TONE && t[0];
        if (!on) continue;
        pt.name = name_at(pt.common, 0);
        out.push_back(std::move(pt));
    }
    return out;
}

// filter type (0 off, 1 low, 2 band, 3 high pass, 4 peaking), cutoff, cutoff key follow step, resonance
Filter jv_filter(int type, int cutoff, int kf, int res) {
    Filter flt;
    if (type >= 1 && type <= 4 && !(type == 1 && cutoff >= 127 && res == 0)) {
        static const FilterType TYPE[5] = {FilterType::None, FilterType::LowPass, FilterType::BandPass, FilterType::HighPass, FilterType::BandPass};
        flt.type = TYPE[type];
        flt.cutoff = std::min(MAX_CUTOFF_HZ, 20.0 * std::pow(1000.0, std::min(cutoff, 127) / 127.0));
        flt.resonance = std::min(res, 127) / 127.0;
        flt.key_tracking = KEYFOLLOW[std::clamp(kf, 0, 15)];
    }
    return flt;
}

// the filter envelope: depth in cutoff steps x 2 (+63 sweeps the whole range: patches with cutoff 0 use it), times and
// levels T1 L1 T2 L2 T3 L3 T4 (levels 0 - 127), then the start and end levels
void jv_filter_env(Filter &flt, int depth, const uint8_t *eg, int l_start, int l_end) {
    if (flt.type == FilterType::None || depth == 0) return;
    double peak_db = 0;
    flt.env = envelope(eg, peak_db);
    const double top = std::pow(10.0, peak_db / 20);
    flt.env.start_level = std::min(1.0, l_start / 127.0 / top);
    flt.env.end_level = std::min(1.0, l_end / 127.0 / top);
    flt.env_depth = std::clamp(depth * 2 * (std::log2(1000.0) * 1200 / 127) * top / MAX_ENVELOPE_DEPTH, -1.0, 1.0);
}

// the JV-1080's patch: rom_dir its ROMs (the XV-3080's for a Roland Cloud patch: jv is then the XV), syx_dir the file's
void load_patch1080(Volume &vol, const SrcPtr &jv, const std::string &rom_dir, const std::string &syx_dir,
                    const Patch1080 &pt, Instrument &inst) {
    const std::vector<uint8_t> &c = pt.common;
    const bool vel_switch = c[0x40] != 0;
    const int octave = 12 * (int(c[0x41]) - 3);
    std::map<const Src *, std::map<int, SampleRefPtr>> refs;
    for (size_t t = 0; t < 4; t++) {
        if (pt.tone[t].size() < J_TONE) continue;
        const uint8_t *q = pt.tone[t].data();
        if (!q[0]) continue;
        const int wave = q[3] << 4 | (q[4] & 15);
        SrcPtr src;
        int w = -1, w_right = -1;
        if (pt.xv) {   // a stereo pair plays its left wave panned left, its right one right
            src = jv;
            w = pt.xv_wave[t][0] - 1;
            w_right = pt.xv_wave[t][1] - 1;
            if (w < 0) {
                inst.warnings.push_back("a tone plays an expansion's waves (not supported)");
                continue;
            }
        } else if (q[1] == 0 && (q[2] == 1 || q[2] == 2)) {
            src = jv;
            w = q[2] == 1 ? wave : int(J_LIST_N[0]) + wave;
        } else if (q[1] == 2 && q[2] >= 4) {
            const int n = q[2] - 3;
            std::string board = find_board(vol, rom_dir, n);
            if (board.empty() && syx_dir != rom_dir) board = find_board(vol, syx_dir, n);
            if (board.empty()) {   // the library layout: the boards sit with the JV-880, a folder beside the JV-1080's
                std::string jv880 = path_join(path_dir(rom_dir), "Roland JV-880");
                if (vol.is_dir(jv880)) board = find_board(vol, jv880, n);
            }
            if (board.empty()) {
                char b[64];
                std::snprintf(b, sizeof b, "a tone needs the SR-JV80-%02d board beside the ROMs", n);
                inst.warnings.push_back(b);
                continue;
            }
            src = open_board(vol, board);
            w = wave;
        } else {
            inst.warnings.push_back("a tone plays a PCM card's waves (not supported)");
            continue;
        }
        if (w >= int(src->waves.size())) continue;
        if (w_right >= int(src->waves.size())) w_right = -1;
        const double kf = KEYFOLLOW[std::min<int>(15, q[0x40])];
        const int coarse = int(q[0x3D]) - 48 + octave;
        const double fine = (int(q[0x3E]) - 50) / 100.0;
        const uint8_t eg[7] = {q[0x6E], q[0x72], q[0x6F], q[0x73], q[0x70], q[0x74], q[0x71]};
        double peak_db = 0;
        const Envelope env = envelope(eg, peak_db);
        const double pan = (q[0x77] - 64) / 64.0 + (c[0x2F] - 64) / 64.0;
        const double gain_db = 6.0 * (std::min<int>(q[0x05], 3) - 1);
        Filter flt = jv_filter(q[0x50], q[0x51], q[0x52], q[0x53]);
        const uint8_t feg[7] = {q[0x5B], q[0x5F], q[0x5C], q[0x60], q[0x5D], q[0x61], q[0x5E]};
        jv_filter_env(flt, int(q[0x55]) - 63, feg, 0, q[0x62]);
        const int key_lo = std::min<int>(q[0x0E], 127), key_hi = std::clamp<int>(q[0x0F], key_lo, 127);
        auto key_pitch = [&](int k) { return int(std::floor(60 + (k - 60) * kf)) + coarse; };
        for (int side = 0; side < (w_right >= 0 ? 2 : 1); side++) {
            const int sw = side ? w_right : w;
            const double side_pan = w_right < 0 ? 0 : side ? 1 : -1;
            for (int k = key_lo; k <= key_hi;) {
                const int sid = pick(*src, sw, key_pitch(k));
                int hi = k;
                while (hi + 1 <= key_hi && pick(*src, sw, key_pitch(hi + 1)) == sid) hi++;
                if (sid >= 0) {
                    Zone z = make_zone(src, sid, refs[src.get()]);
                    if (z.sample) {
                        const Smp &s = src->smps[size_t(sid)];
                        z.name = pt.name + " " + std::to_string(t + 1);
                        z.key_lo = k;
                        z.key_hi = hi;
                        if (vel_switch) { z.vel_lo = std::clamp<int>(q[0x0C], 1, 127); z.vel_hi = std::clamp<int>(q[0x0D], z.vel_lo, 127); }
                        z.key_tracking = kf;
                        z.tune = 60 + (k - 60) * kf + coarse + fine - (k - s.root) * kf - s.root + s.pinit / 1000.0;
                        z.gain_db += velocity_to_db(c[0x2E]) + velocity_to_db(q[0x65]) + peak_db + gain_db;
                        z.pan = std::clamp(pan + side_pan, -1.0, 1.0);
                        z.amp_env = env;
                        z.filter = flt;
                        inst.zones.push_back(std::move(z));
                    }
                }
                k = hi + 1;
            }
        }
    }
}

// levels relative to the loudest zone; throws when nothing plays
void finish_jv(Instrument &inst) {
    std::sort(inst.warnings.begin(), inst.warnings.end());
    inst.warnings.erase(std::unique(inst.warnings.begin(), inst.warnings.end()), inst.warnings.end());
    if (inst.zones.empty())
        throw ParseError("nothing to play in " + inst.name + (inst.warnings.empty() ? "" : " (" + inst.warnings[0] + ")"));
    double top = -1e9;
    for (const Zone &z : inst.zones) top = std::max(top, z.gain_db);
    for (Zone &z : inst.zones) z.gain_db -= top;
    finish_instrument(inst);
}

bool probe_jv1080(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    const uint32_t crc = n >= 4096 ? tinf_crc32(h, 4096) : 0;
    return (crc == J_PROG_CRC && size == MB) || (crc == J_WAVE_CRC[0] && size == WAVE_SIZE);
}

std::vector<PresetInfo> list_jv1080(VolumePtr vol, const std::string &path) {
    SrcPtr s = open_jv1080(*vol, path_dir(path));
    std::vector<PresetInfo> out;
    for (size_t i = 0; i < s->waves.size(); i++) {
        const bool a = i < J_LIST_N[0];
        char num[16];
        std::snprintf(num, sizeof num, "%s%03d ", a ? "A" : "B", int(a ? i : i - J_LIST_N[0]) + 1);
        out.push_back({num + s->waves[i].name, int(i)});
    }
    return out;
}

Instrument load_jv1080(VolumePtr vol, const std::string &path, int index) {
    SrcPtr s = open_jv1080(*vol, path_dir(path));
    if (index < 0 || size_t(index) >= s->waves.size()) throw ParseError("no such JV-1080 wave");
    Instrument inst;
    inst.format = "Roland JV-1080 ROM";
    inst.library = s->library;
    inst.name = s->waves[size_t(index)].name;
    inst.category = "Waves";
    load_wave(s, size_t(index), inst);
    finish_jv(inst);
    return inst;
}

// a JV-1080 patch message in the first bytes
bool probe_syx1080(const std::string &, const uint8_t *h, size_t n, uint64_t) {
    for (size_t i = 0; i + 8 < n; i++)
        if (h[i] == 0xF0 && h[i + 1] == 0x41 && h[i + 3] == 0x6A && h[i + 4] == 0x12 && (h[i + 5] == 0x11 || h[i + 5] == 0x03)) return true;
    return false;
}

std::vector<PresetInfo> list_syx1080(VolumePtr vol, const std::string &path) {
    std::vector<PresetInfo> out;
    const std::vector<Patch1080> ps = read_syx1080(*vol, path);
    for (size_t i = 0; i < ps.size(); i++) {
        char num[16];
        std::snprintf(num, sizeof num, "%03d ", int(i) + 1);
        out.push_back({num + ps[i].name, int(i)});
    }
    return out;
}

Instrument load_syx1080(VolumePtr vol, const std::string &path, int index) {
    const std::vector<Patch1080> ps = read_syx1080(*vol, path);
    if (index < 0 || size_t(index) >= ps.size()) throw ParseError("no such JV-1080 patch");
    const std::string dir = find_beside(*vol, path, is_jv1080_dir, cached_jv1080);
    if (dir.empty())
        throw ParseError("JV-1080: its ROMs are missing (jv1080_program.bin, jv1080_wave0 - 3.bin beside the file or in the Plugin Library)");
    SrcPtr jv = open_jv1080(*vol, dir);
    Instrument inst;
    inst.format = "Roland JV-1080 SysEx";
    inst.library = jv->library;
    inst.name = ps[size_t(index)].name;
    inst.category = "Patches";
    load_patch1080(*vol, jv, dir, path_dir(path), ps[size_t(index)], inst);
    finish_jv(inst);
    return inst;
}

// ---- the XV-3080 ----
// Its ROMs (MAME's xv3080 set): xv3080_program_1.11.bin (2 MB, the SH CPU's, its tables addressed from 0x00D00000) and
// xv3080_wave.bin (32 MB, "XV3080ROM_Ver001" at 0x20, not scrambled; the GSS sample coding). Measured on them
// 2026-10-22, the program ROM's table directory at 0x1A57E8:
//   - Waves: 1083 pointers (BE32 address, BE32 length) at 0x6D7E4, the XV-5080's wave list (also the Roland Cloud
//     JV-1080's numbering); a wave is name[12], 4 bytes 0, then slots (top key, 0, sample number BE16) up to key 127.
//   - Samples: count BE16 at 0x81CC0, 24 bytes each from 0x6F9C0: level [0], mode [1] (as the JV-880's), root key [2],
//     attack and sustain pitch BE16 [6] [8] (1024 = 0, 1/1000 semitone, as the JV's: 833 tonal loops within 0.3 cents
//     at all rates), rate / 100 BE16 [10] (32000, 44100, 30000 ...), start, loop start, end BE32 [12] [16] [20].
// No user or preset patches in either ROM; its GM2 sound set is in the program ROM, packed (no documentation or
// SysEx to compare: fields read from their values across the set, e.g. stereo pairs, drums against pads):
//   - 350 records (pointers BE32 at 0x62768): the 256 GM2 tones, then 94 drum sounds. A record: name[12], level [12],
//     tones on [0x24] (bit 7 the first); then 48 + 110 bytes per tone, up to the last one on (padded to 4 bytes).
//   - A tone: wave BE16 [2] (from 0, as the XV's list; FFFF none), pan [15], coarse [16] and fine [17] (64 = 0),
//     pitch key follow [19] (64 + 10 x the factor, as the Roland Cloud's), filter type [46] (0 - 2 taken as low pass,
//     3 - 4 band pass), cutoff [47], filter envelope depth [56], levels L0 - L4 [58 - 62] (64 = 0), times T1 - T4
//     [64 - 67], velocity range [79] - [82], level [83], envelope levels L1 - L4 [90 - 93], times T1 - T4 [94 - 97]
//     and release [98] (bit 7 a flag).
//   - 9 kits (pointers BE32 at 0x50430): per key 0 - 127 a record number BE16 (FFFF none), then 128-byte tables:
//     level, the note it sounds (60 = as recorded), mute group, pan; the kit's name[12] after its 1280 bytes.
// Not read: resonance, the filter's key follow, wave gain, LFOs, the pitch envelope.

constexpr uint32_t X_PROG_CRC = 0x37A24C3F, X_WAVE_CRC = 0x0FA081C5;
constexpr uint64_t X_PROG_SIZE = 2 * MB, X_WAVE_SIZE = 32 * MB;
constexpr uint32_t X_BASE = 0x00D00000, X_WAVES = 0x6D7E4, X_NWAVES = 1083, X_SAMPLES = 0x6F9C0, X_NSAMPLES = 0x81CC0;
constexpr uint32_t X_GM2 = 0x62768, X_KITS = 0x50430;
constexpr size_t X_NGM2 = 350, X_GM2_TONES = 256, X_NKITS = 9, X_COMMON = 48, X_TONE = 110, X_KIT = 1280;
constexpr int X_PATCHES = 10000, X_RHYTHMS = 20000;   // preset index: a wave's number, else these + the number
std::string cached_xv;
int koa_keyfollow(int v);

// the XV-3080's ROM files in a folder: program, wave; empty when one is missing
std::vector<std::string> xv_files(Volume &vol, const std::string &dir) {
    std::vector<std::string> out(2);
    for (const DirEntry &e : vol.list(dir)) {
        if (e.dir || (e.size != X_PROG_SIZE && e.size != X_WAVE_SIZE)) continue;
        try {
            std::vector<uint8_t> h = vol.open(path_join(dir, e.name))->head(4096);
            if (h.size() < 4096) continue;
            const uint32_t crc = tinf_crc32(h.data(), 4096);
            if (crc == X_PROG_CRC && e.size == X_PROG_SIZE) out[0] = e.name;
            if (crc == X_WAVE_CRC && e.size == X_WAVE_SIZE) out[1] = e.name;
        } catch (const std::exception &) {}
    }
    for (const std::string &f : out) if (f.empty()) return {};
    return out;
}

bool is_xv_dir(Volume &vol, const std::string &dir) {
    try { return !xv_files(vol, dir).empty(); } catch (const std::exception &) { return false; }
}

SrcPtr open_xv(Volume &vol, const std::string &dir) {
    const std::string key = dir + "\n<xv3080>";
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    const std::vector<std::string> f = xv_files(vol, dir);
    if (f.empty()) throw ParseError("XV-3080: its ROMs are missing (xv3080_program_1.11.bin, xv3080_wave.bin in one folder)");
    auto s = std::make_shared<Src>();
    s->library = "Roland XV-3080";
    s->file = f[0];
    const std::vector<uint8_t> p = vol.open(path_join(dir, f[0]))->all(X_PROG_SIZE);
    const size_t ns = be(p, X_NSAMPLES, 2);
    for (size_t i = 0; i < ns && X_SAMPLES + 24 * (i + 1) <= p.size(); i++) {
        const size_t o = X_SAMPLES + 24 * i;
        Smp m;
        m.level = p[o];
        m.mode = p[o + 1];
        m.root = p[o + 2];
        m.pinit = int(be(p, o + 6, 2)) - 1024;
        m.psust = int(be(p, o + 8, 2)) - 1024;
        m.rate = std::max<int>(8000, 100 * int(be(p, o + 10, 2)));
        m.start = be(p, o + 12, 4);
        m.lstart = be(p, o + 16, 4);
        m.end = be(p, o + 20, 4);
        if (m.end < m.start || m.lstart < m.start || m.lstart > m.end) m.mode = 2;
        s->smps.push_back(m);
    }
    for (size_t i = 0; i < X_NWAVES; i++) {
        const size_t at = be(p, X_WAVES + 8 * i, 4) - X_BASE, len = be(p, X_WAVES + 8 * i + 4, 4);
        Wave w;
        if (at + len <= p.size() && len >= 20) {
            w.name = name_at(p, at);
            for (size_t q = at + 16; q + 4 <= at + len; q += 4) {
                w.brk.push_back(p[q]);
                w.smp.push_back(uint16_t(be(p, q + 2, 2)));
                if (p[q] == 0x7F) break;
            }
        }
        s->waves.push_back(std::move(w));
    }
    for (size_t i = 0; i < X_NGM2; i++) {   // GM2 records: common, then the tones up to the last one on
        const size_t at = be(p, X_GM2 + 4 * i, 4) - X_BASE;
        if (at + X_COMMON > p.size()) break;
        const uint8_t on = p[at + 0x24] >> 4;
        size_t n = 0;
        for (size_t t = 0; t < 4; t++) if (on & (8 >> t)) n = t + 1;
        if (at + X_COMMON + X_TONE * n > p.size()) break;
        s->patches.emplace_back(p.begin() + long(at), p.begin() + long(at + X_COMMON + X_TONE * n));
        s->patch_names.push_back(name_at(p, at));
    }
    for (size_t i = 0; i < X_NKITS; i++) {
        const size_t at = be(p, X_KITS + 4 * i, 4) - X_BASE;
        if (at + X_KIT + 12 > p.size()) break;
        s->rhythms.emplace_back(p.begin() + long(at), p.begin() + long(at + X_KIT));
        s->rhythm_names.push_back(name_at(p, at + X_KIT));
    }
    s->image = std::make_shared<std::vector<uint8_t>>(vol.open(path_join(dir, f[1]))->all(X_WAVE_SIZE));
    mark_blanks(*s);
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache[key] = s;
    return s;
}

bool probe_xv(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    const uint32_t crc = n >= 4096 ? tinf_crc32(h, 4096) : 0;
    return (crc == X_PROG_CRC && size == X_PROG_SIZE) || (crc == X_WAVE_CRC && size == X_WAVE_SIZE);
}

// times T1 - T3 to levels L1 - L3 (held), T4 (release) to L4
Envelope gm2_envelope(const uint8_t *t, const uint8_t *l, double &top) {
    Envelope env;
    env.set = true;
    top = std::max({int(l[1]), int(l[2]), 1}) / 127.0;
    const bool rise = l[2] > l[1];
    env.attack = env_seconds(t[0]) + (rise ? env_seconds(t[1]) : 0);
    env.decay = env_seconds(t[2]) + (rise ? 0 : env_seconds(t[1]));
    env.sustain = std::min(1.0, l[3] / 127.0 / top);
    env.release = std::max(0.005, env_seconds(t[3]));
    env.end_level = std::min(1.0, l[4] / 127.0 / top);
    return env;
}

// The filter: cutoff in steps (as the JV's), its envelope's levels L0 - L4 around 64 x depth steps
Filter gm2_filter(const uint8_t *q) {
    Filter flt;
    flt.type = q[46] <= 2 ? FilterType::LowPass : FilterType::BandPass;
    flt.cutoff = std::min(MAX_CUTOFF_HZ, 20.0 * std::pow(1000.0, q[47] / 127.0));
    double e[5], lo = 0, hi = 0;
    for (size_t i = 0; i < 5; i++) { e[i] = (q[58 + i] - 64) / 63.0 * q[56]; lo = std::min(lo, e[i]); hi = std::max(hi, e[i]); }
    if (hi - lo < 1) return flt;
    const double step = std::log2(1000.0) * 1200 / 127;   // cents a cutoff step
    flt.cutoff = std::clamp(20.0 * std::pow(1000.0, (q[47] + lo) / 127.0), 20.0, MAX_CUTOFF_HZ);
    uint8_t l[5];
    for (size_t i = 0; i < 5; i++) l[i] = uint8_t(std::lround((e[i] - lo) / (hi - lo) * 127));
    double top = 1;
    flt.env = gm2_envelope(q + 64, l, top);
    flt.env.start_level = std::min(1.0, l[0] / 127.0 / top);
    flt.env_depth = std::min(1.0, (hi - lo) * top * step / MAX_ENVELOPE_DEPTH);
    return flt;
}

// A GM2 record's tones: over the keyboard (key < 0), or on one key of a kit, sounding note `note`
void gm2_zones(const SrcPtr &xv, const std::vector<uint8_t> &r, const std::string &name, int key, int note, Instrument &inst) {
    std::map<int, SampleRefPtr> refs;
    const uint8_t on = r[0x24] >> 4;
    for (size_t t = 0; t < 4 && X_COMMON + X_TONE * (t + 1) <= r.size(); t++) {
        if (!(on & (8 >> t))) continue;
        const uint8_t *q = &r[X_COMMON + X_TONE * t];
        const int w = q[2] << 8 | q[3];
        if (w >= int(xv->waves.size())) continue;   // [0] (0, else 2 - 0C or FF in 9 tones) not read: their waves fit
        const double kf = KEYFOLLOW[koa_keyfollow(q[19])];
        const int coarse = int(q[16]) - 64;
        const double fine = (int(q[17]) - 64) / 100.0;
        // T1 - T3 to L1 - L3, then T4 to L4 (held: taken as reached by T3), the release [98]
        const uint8_t l[5] = {0, q[90], q[91], q[93], 0};
        const uint8_t ts[4] = {uint8_t(q[94] & 0x7F), uint8_t(q[95] & 0x7F), uint8_t(q[96] & 0x7F), uint8_t(q[98] & 0x7F)};
        double top = 1;
        const Envelope env = gm2_envelope(ts, l, top);
        const double gain_db = velocity_to_db(r[12]) + velocity_to_db(q[83]) + 20 * std::log10(top);
        const Filter flt = gm2_filter(q);
        const int vel_lo = q[79], vel_hi = std::max<int>(q[82], vel_lo);
        auto key_pitch = [&](int k) { return int(std::floor(60 + (k - 60) * kf)) + coarse; };
        auto add = [&](int sid, int lo, int hi, int k) {
            Zone z = make_zone(xv, sid, refs);
            if (!z.sample) return;
            const Smp &s = xv->smps[size_t(sid)];
            z.name = name + " " + std::to_string(t + 1);
            z.key_lo = lo;
            z.key_hi = hi;
            if (vel_lo > 0 || vel_hi < 127) { z.vel_lo = std::max(1, vel_lo); z.vel_hi = std::max(z.vel_lo, vel_hi); }
            z.gain_db += gain_db;
            z.pan = std::clamp((q[15] - 64) / 64.0, -1.0, 1.0);
            z.amp_env = env;
            z.filter = flt;
            if (key < 0) {
                z.key_tracking = kf;
                z.tune = 60 + (k - 60) * kf + coarse + fine - (k - s.root) * kf - s.root + s.pinit / 1000.0;
            } else {
                z.root = key;
                z.tune = 60 + (note - 60) * kf + coarse + fine - s.root + s.pinit / 1000.0;
            }
            inst.zones.push_back(std::move(z));
        };
        if (key >= 0) {
            const int sid = pick(*xv, w, key_pitch(note));
            if (sid >= 0) add(sid, key, key, key);
            continue;
        }
        for (int k = 0; k < 128;) {
            const int sid = pick(*xv, w, key_pitch(k));
            int hi = k;
            while (hi + 1 < 128 && pick(*xv, w, key_pitch(hi + 1)) == sid) hi++;
            if (sid >= 0) add(sid, k, hi, k);
            k = hi + 1;
        }
    }
}

void load_gm2_kit(const SrcPtr &xv, size_t kit, Instrument &inst) {
    const std::vector<uint8_t> &d = xv->rhythms[kit];
    for (int k = 0; k < 128; k++) {
        const size_t rec = be(d, size_t(2 * k), 2);
        if (rec >= xv->patches.size()) continue;
        const size_t first = inst.zones.size();
        gm2_zones(xv, xv->patches[rec], xv->patch_names[rec], k, d[256 + 128 + size_t(k)], inst);
        for (size_t i = first; i < inst.zones.size(); i++) {
            Zone &z = inst.zones[i];
            z.name = xv->patch_names[rec];
            z.gain_db += velocity_to_db(d[256 + size_t(k)]);
            z.pan = std::clamp(z.pan + (d[256 + 3 * 128 + size_t(k)] - 64) / 64.0, -1.0, 1.0);
            z.exclusive_group = d[256 + 2 * 128 + size_t(k)] & 0x1F;
            if (z.amp_env.sustain <= 0) z.amp_env.release = std::max(z.amp_env.release, z.amp_env.attack + z.amp_env.decay);   // rings out after a tap
        }
    }
}

std::vector<PresetInfo> list_xv(VolumePtr vol, const std::string &path) {
    SrcPtr s = open_xv(*vol, path_dir(path));
    std::vector<PresetInfo> out;
    char num[32];
    for (size_t i = 0; i < std::min(X_GM2_TONES, s->patches.size()); i++) {
        std::snprintf(num, sizeof num, "GM2 %03d ", int(i) + 1);
        out.push_back({num + s->patch_names[i], X_PATCHES + int(i)});
    }
    for (size_t i = 0; i < s->rhythms.size(); i++) out.push_back({"GM2 Kit " + s->rhythm_names[i].substr(s->rhythm_names[i].rfind(' ') + 1), X_RHYTHMS + int(i)});
    for (size_t i = 0; i < s->waves.size(); i++) {
        std::snprintf(num, sizeof num, "%04d ", int(i) + 1);
        out.push_back({num + s->waves[i].name, int(i)});
    }
    return out;
}

Instrument load_xv(VolumePtr vol, const std::string &path, int index) {
    SrcPtr s = open_xv(*vol, path_dir(path));
    Instrument inst;
    inst.format = "Roland XV-3080 ROM";
    inst.library = s->library;
    if (index >= X_RHYTHMS && size_t(index - X_RHYTHMS) < s->rhythms.size()) {
        inst.name = s->rhythm_names[size_t(index - X_RHYTHMS)];
        inst.category = "Rhythm Sets";
        load_gm2_kit(s, size_t(index - X_RHYTHMS), inst);
    } else if (index >= X_PATCHES && size_t(index - X_PATCHES) < s->patches.size()) {
        inst.name = s->patch_names[size_t(index - X_PATCHES)];
        inst.category = "Patches";
        gm2_zones(s, s->patches[size_t(index - X_PATCHES)], inst.name, -1, 60, inst);
    } else if (index >= 0 && size_t(index) < s->waves.size()) {
        inst.name = s->waves[size_t(index)].name;
        inst.category = "Waves";
        load_wave(s, size_t(index), inst);
    } else {
        throw ParseError("no such XV-3080 preset");
    }
    finish_jv(inst);
    return inst;
}

// ---- Roland Cloud JV-1080 banks ----
// The Roland Cloud JV-1080's bank files (.bin, 2976665 bytes): "KoaBankFile00003PG-JV1080" (or "...PG-XV5080", its GM
// bank), then 128 records of 23255 bytes. Its waves are the XV-3080's (numbered from 1 in the XV's list: Ac Piano1 A
// is 19), so the banks play from the XV-3080's ROMs. Measured 2026-10-22 on the six banks that are conversions of
// JV-1080 SysEx banks we read (ROLAND, CONTRIB, 80TO1080, MEI_INT, NIRSULAM, RNDYBRWN: 768 patches, every byte below
// equal to the SysEx's, or as noted):
//   - common: name[12] [0x00] (again at [0x54]), level [0x62], pan [0x63], octave [0x67] (64 = 0), velocity range
//     switch [0x1E0]; per tone t a 9-byte entry at 0x1E1 + 9t: on [0], key range [1] - [2], velocity range [5] - [6].
//   - tone t at 0x205 + 154t: level [0], coarse [1] (64 = 0), fine [2] (64 = 0), pan [4], wave gain [52], pitch key
//     follow [57] and cutoff key follow [74] (64 + 10 x the factor: 74 = 100 %), filter type [72], cutoff [73],
//     resonance [77], filter envelope depth [79], T1 - T4 [85 - 88], L1 - L4 [90 - 93], envelope T1 - T4 [102 - 105],
//     L1 - L3 [106 - 108]; wave group [43] (1 the XV's list; 4 and 5 seen
//     in 4 tones, not read), waves left [44 - 47] and right [48 - 51] (4 nibbles each; right 0 = mono).
//   - A rhythm set (its name at [0x54] all "-"): 88 keys of 195 bytes from 0x5B7, key k sounding note 21 + k; an XV
//     rhythm tone (no SysEx to compare: the waves checked by their key names, 7945 of 8192 equal): name[12], mute
//     group [13], level [14], pitch [15] (the note it sounds), fine [16], pan [18], sustain [21], wave gain [47], wave
//     [39 - 42] (the first of four 29-byte wave layers from [27]; the others unused), filter type [162], cutoff [163],
//     resonance [166], filter envelope depth [168] (64 = 0), T1 - T4 [173 - 176], L0 - L4 [177 - 181], envelope
//     T1 - T4 [186 - 189], L1 - L3 [190 - 192].
// Empty slots ("------------", "Init Patch") are not listed. Not read: as for the SysEx, and the rhythm keys' pitch
// envelope; the Cloud's SRX bank files ("PG-NEX..") are another layout.

constexpr size_t K_HEAD = 25, K_REC = 23255, K_RECS = 128, K_SIZE = K_HEAD + K_REC * K_RECS;
constexpr size_t K_ZONES = 0x1E1, K_TONES = 0x205, K_TONE = 154, K_KEYS = 0x5B7, K_KEY = 195, K_NKEYS = 88;

int koa_keyfollow(int v) {   // the KEYFOLLOW step nearest 64 + 10 x the factor
    int best = 0;
    for (int i = 1; i < 16; i++) if (std::abs(64 + 10 * KEYFOLLOW[i] - v) < std::abs(64 + 10 * KEYFOLLOW[best] - v)) best = i;
    return best;
}

int koa_wave(const uint8_t *p) { return (p[0] & 15) << 12 | (p[1] & 15) << 8 | (p[2] & 15) << 4 | (p[3] & 15); }

bool koa_rhythm(const uint8_t *r) {
    return std::all_of(r + 0x54, r + 0x60, [](uint8_t b) { return b == '-'; });
}

std::vector<uint8_t> read_koa(Volume &vol, const std::string &path) {
    std::vector<uint8_t> d = vol.open(path)->all(K_SIZE);
    if (d.size() != K_SIZE) throw ParseError("not a Roland Cloud JV-1080 bank");
    return d;
}

// the listed records: (number, name)
std::vector<std::pair<int, std::string>> koa_records(const std::vector<uint8_t> &d) {
    std::vector<std::pair<int, std::string>> out;
    for (size_t i = 0; i < K_RECS; i++) {
        const std::string name = name_at(d, K_HEAD + K_REC * i);
        std::string lower = name;
        for (char &ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (name.find_first_not_of("- ") == std::string::npos || lower == "init patch") continue;
        const uint8_t *r = &d[K_HEAD + K_REC * i];
        bool on = koa_rhythm(r);   // a patch with every tone off ("zilch") is an empty slot too
        for (size_t t = 0; t < 4; t++) on |= r[K_ZONES + 9 * t] != 0;
        if (on) out.push_back({int(i), name});
    }
    return out;
}

Patch1080 koa_patch(const uint8_t *r, const std::string &name) {
    Patch1080 pt;
    pt.name = name;
    pt.xv = true;
    std::vector<uint8_t> &c = pt.common;
    c.assign(J_COMMON, 0);
    c[0x2E] = r[0x62];
    c[0x2F] = r[0x63];
    c[0x40] = r[0x1E0];
    c[0x41] = uint8_t(std::max(0, r[0x67] - 61));
    for (size_t t = 0; t < 4; t++) {
        const uint8_t *z = r + K_ZONES + 9 * t, *b = r + K_TONES + K_TONE * t;
        std::vector<uint8_t> &q = pt.tone[t];
        q.assign(J_TONE, 0);
        q[0x00] = z[0];
        q[0x0E] = z[1];
        q[0x0F] = z[2];
        q[0x0C] = z[5];
        q[0x0D] = z[6];
        q[0x05] = b[52];
        q[0x3D] = uint8_t(std::max(0, b[1] - 16));
        q[0x3E] = uint8_t(std::max(0, b[2] - 14));
        q[0x40] = uint8_t(koa_keyfollow(b[57]));
        q[0x50] = b[72];
        q[0x51] = b[73];
        q[0x52] = uint8_t(koa_keyfollow(b[74]));
        q[0x53] = b[77];
        q[0x55] = b[79];
        for (size_t j = 0; j < 4; j++) { q[0x5B + j] = b[85 + j]; q[0x5F + j] = b[90 + j]; }
        q[0x65] = b[0];
        q[0x77] = b[4];
        for (size_t j = 0; j < 7; j++) q[0x6E + j] = b[102 + j];
        if (b[43] == 1) { pt.xv_wave[t][0] = koa_wave(b + 44); pt.xv_wave[t][1] = koa_wave(b + 48); }
    }
    return pt;
}

void load_koa_rhythm(const SrcPtr &xv, const uint8_t *r, Instrument &inst) {
    std::map<int, SampleRefPtr> refs;
    for (size_t k = 0; k < K_NKEYS; k++) {
        const uint8_t *q = r + K_KEYS + K_KEY * k;
        const std::string name = name_at(std::vector<uint8_t>(q, q + 12), 0);   // unused keys: no name, a default wave
        const int w = koa_wave(q + 39) - 1;
        if (name.empty() || q[38] != 1 || w < 0 || w >= int(xv->waves.size())) continue;
        const int sid = pick(*xv, w, q[15]);
        if (sid < 0) continue;
        Zone z = make_zone(xv, sid, refs);
        if (!z.sample) continue;
        const Smp &s = xv->smps[size_t(sid)];
        const uint8_t eg[7] = {q[186], q[190], q[187], q[191], q[188], q[192], q[189]};
        double peak_db = 0;
        z.amp_env = envelope(eg, peak_db);
        if (!q[21]) z.amp_env.release = std::max(z.amp_env.release, z.amp_env.attack + z.amp_env.decay);   // rings out after a tap
        z.name = name;
        z.key_lo = z.key_hi = z.root = int(21 + k);
        z.tune = q[15] + (int(q[16]) - 64) / 100.0 - s.root + s.pinit / 1000.0;
        z.gain_db += velocity_to_db(q[14]) + peak_db + 6.0 * (std::min<int>(q[47], 3) - 1);
        z.pan = std::clamp((q[18] - 64) / 64.0, -1.0, 1.0);
        z.exclusive_group = q[13] & 0x1F;
        z.filter = jv_filter(q[162], q[163], 5, q[166]);
        const uint8_t feg[7] = {q[173], q[178], q[174], q[179], q[175], q[180], q[176]};
        jv_filter_env(z.filter, int(q[168]) - 64, feg, q[177], q[181]);
        inst.zones.push_back(std::move(z));
    }
}

bool probe_koa(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    return size == K_SIZE && n >= K_HEAD &&
           (!std::memcmp(h, "KoaBankFile00003PG-JV1080", K_HEAD) || !std::memcmp(h, "KoaBankFile00003PG-XV5080", K_HEAD));
}

std::vector<PresetInfo> list_koa(VolumePtr vol, const std::string &path) {
    std::vector<PresetInfo> out;
    for (const auto &rec : koa_records(read_koa(*vol, path))) {
        char num[16];
        std::snprintf(num, sizeof num, "%03d ", rec.first + 1);
        out.push_back({num + rec.second, rec.first});
    }
    return out;
}

Instrument load_koa(VolumePtr vol, const std::string &path, int index) {
    const std::vector<uint8_t> d = read_koa(*vol, path);
    if (index < 0 || size_t(index) >= K_RECS) throw ParseError("no such Roland Cloud patch");
    const std::string dir = find_beside(*vol, path, is_xv_dir, cached_xv);
    if (dir.empty())
        throw ParseError("Roland Cloud JV-1080: plays from the XV-3080's ROMs (xv3080_program_1.11.bin, xv3080_wave.bin beside the file or in the Plugin Library)");
    SrcPtr xv = open_xv(*vol, dir);
    const uint8_t *r = &d[K_HEAD + K_REC * size_t(index)];
    Instrument inst;
    inst.format = "Roland Cloud JV-1080";
    inst.library = xv->library;
    inst.name = name_at(d, K_HEAD + K_REC * size_t(index));
    if (koa_rhythm(r)) {
        inst.category = "Rhythm Sets";
        load_koa_rhythm(xv, r, inst);
    } else {
        inst.category = "Patches";
        load_patch1080(*vol, xv, dir, path_dir(path), koa_patch(r, inst.name), inst);
    }
    finish_jv(inst);
    return inst;
}

// ---- the reader ----

uint32_t head_crc(const uint8_t *h, size_t n) { return n >= 4096 ? tinf_crc32(h, 4096) : 0; }

bool probe(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    const uint32_t crc = head_crc(h, n);
    return (crc == CTRL_CRC && size == CTRL_SIZE) || (crc == WAVE1_CRC && size == WAVE_SIZE) || is_board(h, n, size);
}

// the tapped source (the JV-880, or a board) and the JV-880 beside it (null if absent)
SrcPtr open_source(Volume &vol, const std::string &path, SrcPtr &internal) {
    internal = open_internal(vol, path_dir(path));
    std::vector<uint8_t> h = vol.open(path)->head(4096);
    if (is_board(h.data(), h.size(), vol.open(path)->size())) return open_board(vol, path);
    if (!internal) throw ParseError("JV-880: its ROMs are missing (jv880_rom2, jv880_waverom1 and 2 in one folder)");
    return internal;
}

std::vector<PresetInfo> list(VolumePtr vol, const std::string &path) {
    SrcPtr internal;
    SrcPtr s = open_source(*vol, path, internal);
    std::vector<PresetInfo> out;
    for (size_t i = 0; i < s->patches.size(); i++) out.push_back({s->patch_names[i], PATCHES * KINDS + int(i)});
    for (size_t i = 0; i < s->rhythms.size(); i++) out.push_back({s->rhythm_names[i], RHYTHMS * KINDS + int(i)});
    for (size_t i = 0; i < s->waves.size(); i++) out.push_back({"Wave " + std::to_string(i + 1) + " " + s->waves[i].name, WAVES * KINDS + int(i)});
    return out;
}

Instrument load(VolumePtr vol, const std::string &path, int index) {
    SrcPtr internal;
    SrcPtr s = open_source(*vol, path, internal);
    const int kind = index % SOURCES / KINDS;
    const size_t i = size_t(index % KINDS);
    Instrument inst;
    inst.format = s->library + " ROM";
    inst.library = s->library;
    if (kind == PATCHES && i < s->patches.size()) {
        inst.name = s->patch_names[i];
        inst.category = "Patches";
        load_patch(s, internal, i, inst);
    } else if (kind == RHYTHMS && i < s->rhythms.size()) {
        inst.name = s->rhythm_names[i];
        inst.category = "Rhythm Sets";
        load_rhythm(s, internal, i, inst);
    } else if (kind == WAVES && i < s->waves.size()) {
        inst.name = s->waves[i].name;
        inst.category = "Waves";
        load_wave(s, i, inst);
    } else {
        throw ParseError("no such JV-880 preset");
    }
    std::sort(inst.warnings.begin(), inst.warnings.end());
    inst.warnings.erase(std::unique(inst.warnings.begin(), inst.warnings.end()), inst.warnings.end());
    if (inst.zones.empty())
        throw ParseError("nothing to play in " + inst.name + (inst.warnings.empty() ? "" : " (" + inst.warnings[0] + ")"));
    double top = -1e9;   // the loudest zone at 0 dB: the levels above are relative
    for (const Zone &z : inst.zones) top = std::max(top, z.gain_db);
    for (Zone &z : inst.zones) z.gain_db -= top;
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_roland_jv880() {
    register_reader({"Roland JV-880 / SR-JV80 ROM", "", probe, list, load});
    register_reader({"Roland JV-1080 ROM", "", probe_jv1080, list_jv1080, load_jv1080});
    register_reader({"Roland JV-1080 SysEx", "syx", probe_syx1080, list_syx1080, load_syx1080});
    register_reader({"Roland XV-3080 ROM", "", probe_xv, list_xv, load_xv});
    register_reader({"Roland Cloud JV-1080", "bin", probe_koa, list_koa, load_koa});
}

}  // namespace omni

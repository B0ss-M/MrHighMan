// Omni Sampler 2.4.32: Xfer Serum 2 presets (.SerumPreset) on Omni's SYNTH. From the user's serum.cpp (2026-11-22),
// checked on 239 presets (XTRA/GOGOi Downtempo For Serum 2, GOGOi LO-MO for Serum 2):
//   "XferJson\0", u64 LE metadata length, the metadata (JSON: name, author, tags), then u32 LE payload size, u32 LE
//   payload version (2), a zstd frame (src/third_party/zstd) holding one CBOR map (definite lengths only).
//   Oscillator0..2: plainParams (kParamEnable, kParamType absent = wavetable, kParamVolume 0..1, kParamPan -50..50,
//   kParamOctave, kParamPitch / kParamCoarsePit semitones, kParamFine cents, kParamUnison 1..16, kParamDetune 0..1,
//   kParamUnisonStereo 0..100) and WTOsc<n>: embeddedWTData (float32 array, 2048 per frame, 1..256 frames) or only
//   relativePathToWT (a Serum factory table, "Analog S2/Saw to Square.wav"), plainParams kParamTablePos (frame 1..256).
//   Env0 plainParams: kParamAttack / Hold / Decay / Release seconds, kParamSustain 0..1. A module whose settings are
//   all Serum's defaults stores plainParams as the text "default" (read as no fields: every default).
// 2.4.33/34: samples, multisamples, filter, envelopes, LFOs, modulation, macros, voice (formats/serum_sample.hpp).
// 2.4.35 (user: Flute / Piano and other presets silent on the Live III; Serum2.vst3 2.1.5 given to map them exactly),
// checked on all 626 factory presets + 239 GOGOi:
//   - MODULATION SOURCES: the Serum2 binary's source names, in order, ids from 1 (they match the library's use):
//       1 Mod Wheel, 2..5 Env 1..4 (Env 1 = the amp envelope), 6..15 LFO 1..10, 16 Velo, 17 Note#, 18 Aftertouch,
//       19 Poly Aftertouch, 20 Noise OSC, 21 / 22 NoteOn Rand 1 / 2, 23 / 24 NoteOn Alt 1 / 2, 25..32 Macro 1..8,
//       33 Pitch Bend, 34..36 MPE X / Y / Z, 37 Release Velo, 38 Fixed, 39..48 LFO 1..10 Y, 49..51 OSC A..C, 52 SUB,
//       53 / 54 Filter 1 / 2, 55 Active Voices, 56 / 57 Voice Mod 1 / 2, 58 Voice Index, 59 NoteOn Rand (Discrete).
//     (2.4.34 played 1 = Mod Wheel as the amp envelope and dropped 2 = the amp envelope, Macros 5..8 and LFOs 5..10.)
//   - a route's source is [id, aux]: the aux source scales it. Mod wheel / aftertouch / bend / MPE as aux: 0 when a note
//     is played as a preset loads, so the route does nothing (Ambient Piano: LFO 2 x wheel on OSC A's level, -92 %:
//     2.4.34 applied the whole LFO = silence); a macro as aux: its value; others kept as they are [G]
//   - an amount is a fraction of the parameter's whole range (kParamAmount -100..100 %): cutoff 8 Hz .. 22 kHz = 11.4
//     octaves (Omni: 4 octaves per unit), resonance 0..100, pan -50..50, fine +-100 ct; unipolar u = 0..1, bipolar
//     (kParamBipolar) as -1..1 at the full amount [G: 2u - 1, as Vital]; Omni clamps a route at +-1: the top end kept
//   - Omni has 4 macros and 4 LFOs: the ones a preset routes most stay live; the others' macros fold into the values
//     they move (their stored position), their LFOs are left out. Macros on an oscillator's level always fold [G]
//   - oscillators: the two that sound most (level + folded macros + half the dynamic level modulation) play; SAMPLE,
//     MULTISAMPLE, and GRANULAR / SPECTRAL (their source sample played plainly [G]) on the SYNTH's sample oscillator
//   - routing (RoutingSlot<k> = oscillator k; kParamRoutingDest Filter = 0 / Master / Direct / None, the binary's
//     order): OSC A goes to the filters by default, the others only when they say so; kParamFilterBalance -100 Filter 1
//     (the default, never stored) .. 0 both .. 100 Filter 2 (stored: 0, 100, -3.3). Omni's one filter: the filter the
//     louder routed oscillators use; an oscillator routed None (FX buses only) ranks last and plays at half level [G]
//   - filter types (the binary's list): MgL6..24, the L/B/H/P/N morphing types by kParamVar, ladders, combs /
//     flangers; phasers, EQs, S&H, reverbs and shapers are not filters: left off. Serum's default is MgL12
//   - LFOs: the drawn curve (curveData xVals / yVals / curveVals) plays as the SYNTH's CUSTOM shape
//     ("serumlfo:<n>:{path}", serum_table.hpp); kParamMode Envelope = One-Shot; kParamBeatSync absent = tempo synced
//   - warp: Sync / Bend / PWM / Mirror / FM from an oscillator onto Omni's warps (PD as Bend, RM as the ring) [G]
// Not played: FX racks, the other warp modes, a third oscillator, LFO delay / rise / smooth, envelope curves, MPE. [G] marks a
// mapping not checked against Serum itself.
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "../core/bytes.hpp"
#include "format.hpp"
#include "serum_table.hpp"
#include "serum_sample.hpp"
#include "synth_patch.hpp"

namespace omni {
namespace {

enum SerumSrc {
    SM_WHEEL = 1, SM_ENV1, SM_ENV2, SM_ENV3, SM_ENV4, SM_LFO1, SM_LFO10 = SM_LFO1 + 9, SM_VELO, SM_NOTE, SM_AT, SM_POLYAT,
    SM_NOISE, SM_RAND1, SM_RAND2, SM_ALT1, SM_ALT2, SM_MACRO1, SM_MACRO8 = SM_MACRO1 + 7, SM_BEND, SM_MPEX, SM_MPEY, SM_MPEZ,
    SM_RELVEL, SM_FIXED, SM_RAND_DISCRETE = 59
};
static_assert(SM_VELO == 16 && SM_MACRO1 == 25 && SM_BEND == 33 && SM_FIXED == 38, "Serum 2 source ids");
constexpr double SERUM_CUTOFF_OCT = 11.4291;   // log2(22050 / 8): the cutoff knob's whole range

bool probe_serum(const std::string &, const uint8_t *p, size_t n, uint64_t) {
    return n >= 17 && !std::memcmp(p, "XferJson\0", 9);
}

std::vector<PresetInfo> list_serum(VolumePtr vol, const std::string &path) {
    serum_unpack(*vol->open(path));   // refuses what is not a Serum 2 preset, so the browser shows why
    return {{path_stem(path), 0}};
}

bool starts(const std::string &s, const char *p) { return s.compare(0, std::strlen(p), p) == 0; }

// Serum 2's filter type (the binary's list) as the nearest Omni filter; -1: not a filter Omni has (left off) [G]
int serum_filter_type(const std::string &t, double var) {
    if (t.empty() || t == "MgL12" || t == "MgL6") return SynthPatch::LP12;   // MgL12: Serum's default (never stored)
    if (t == "MgL18" || t == "MgL24") return SynthPatch::LP24;
    if (t.find("Ladder") != std::string::npos || starts(t, "Dirty") || starts(t, "ZDF")) return SynthPatch::LADDER;
    if (starts(t, "Comb") || starts(t, "Flange") || starts(t, "DistComb")) return SynthPatch::COMB;
    if (t == "BandReject") return SynthPatch::NOTCH;
    if (t == "Scream3LP" || t == "Exp") return SynthPatch::LP24;
    // 2.4.35 (the binary's full list, XTRA/OmniSampler-Serum2-Source.zip binary-evidence.tsv): one-letter types
    // L6..L24 / H6..H24 / B12 B24 / N12 N24 (P12 P24 = peaks), the Korg 35 / PZ state variable (low), ExpBPF, Comb2
    if (t == "K35" || t == "PZ_SVF") return SynthPatch::LP12;   // [G]
    if (t == "ExpBPF") return SynthPatch::BP;
    if (t == "Comb2") return SynthPatch::COMB;
    if (t.size() >= 2 && std::strchr("LHBNP", t[0]) && std::isdigit(static_cast<unsigned char>(t[1]))) {
        const int slope = std::atoi(t.c_str() + 1);
        switch (t[0]) {
        case 'L': return slope > 12 ? SynthPatch::LP24 : SynthPatch::LP12;
        case 'H': return SynthPatch::HP;
        case 'B': return SynthPatch::BP;
        case 'N': return SynthPatch::NOTCH;
        default: return -1;   // a peak
        }
    }
    if (starts(t, "Scream") || starts(t, "Formant")) return SynthPatch::BP;
    // the morphing multi-mode filters: 2 or 3 letters (L low, B band, H high, P peak, N notch) + 12 / 24; kParamVar
    // (0..100) moves from the first letter to the last
    size_t n = 0;
    while (n < t.size() && std::strchr("LBHPN", t[n])) n++;
    if (n >= 2 && n <= 3 && (t.substr(n) == "6" || t.substr(n) == "12" || t.substr(n) == "24")) {
        const int i = clampi(int(std::lround(clampd(var, 0, 100) / 100.0 * double(n - 1))), 0, int(n) - 1);
        switch (t[size_t(i)]) {
        case 'L': return t.substr(n) == "24" ? SynthPatch::LP24 : SynthPatch::LP12;
        case 'B': return SynthPatch::BP;
        case 'H': return SynthPatch::HP;
        case 'N': return SynthPatch::NOTCH;
        default: return -1;   // a peak: no cut
        }
    }
    return -1;   // Phase.., EQs, SNH, RM / RMT, Wsp, DJMixer, Reverb1, Allpasses, Diffuser, ADD_BASS, the shapers (k..)
}

// an LFO's shape: Serum's random types, else its drawn curve (CUSTOM), else (no curve: Serum's default) a triangle [G]
int serum_lfo_shape(const SerumItem &lfo, bool &curve) {
    curve = false;
    const SerumItem t = lfo.field("plainParams").field("kParamType");
    if (t.equals("RandomSH")) return SynthPatch::L_SH;
    if (t.equals("Rossler") || t.equals("Lorenz")) return SynthPatch::L_RANDOM;
    VitalLine line;
    if (serum_lfo_curve(lfo, line)) { curve = true; return SynthPatch::L_CUSTOM; }
    return SynthPatch::L_TRI;
}

struct SRoute {
    int src = 0, aux = 0, did = 0;
    double amt = 0;   // -1..1 of the destination's whole range
    bool bip = false;
    std::string type, param;
};

Instrument load_serum(VolumePtr vol, const std::string &path, int) {
    const std::vector<uint8_t> decoded = serum_unpack(*vol->open(path));
    const SerumItem root = serum_root(decoded);

    Instrument inst;
    inst.name = path_stem(path);
    inst.format = "Serum 2 preset";
    SynthPatch patch;
    patch.source = "Serum 2";
    std::vector<std::string> notes, missing;
    int n_aux_rest = 0, n_skip_src = 0, n_skip_dst = 0, n_fx = 0, n_full = 0;

    double mval[8];   // the macros' stored positions, 0..1
    for (int k = 0; k < 8; k++) mval[k] = clampd(root.field("Macro" + std::to_string(k)).field("plainParams").number("kParamValue", 0) / 100.0, 0, 1);
    // the mod wheel's position the preset was saved with (Global0 kParamModWheel 0..100; 121 factory presets store one)
    const double wheel = clampd(root.field("Global0").field("plainParams").number("kParamModWheel", 0) / 100.0, 0, 1);
    auto is_macro = [](int s) { return s >= SM_MACRO1 && s <= SM_MACRO8; };
    auto is_lfo = [](int s) { return s >= SM_LFO1 && s <= SM_LFO10; };

    // ---- the routes, their aux sources applied ----
    std::vector<SRoute> routes;
    for (int m = 0; m < 64; m++) {
        const SerumItem slot = root.field("ModSlot" + std::to_string(m));
        const SerumItem src = slot.field("source");
        if (src.type != 4 || src.value < 1) continue;
        SRoute r;
        {
            const uint8_t *c = src.p;
            r.src = int(SerumItem::next(c, src.end).number(0));
            if (src.value >= 2 && c < src.end) r.aux = int(SerumItem::next(c, src.end).number(0));
        }
        const SerumItem pp = slot.field("plainParams");
        r.amt = clampd(pp.number("kParamAmount", 0) / 100.0, -1, 1);
        r.bip = pp.number("kParamBipolar", 0) >= 0.5;
        if (r.src < 1 || r.amt == 0) continue;
        if ((r.aux == SM_WHEEL && wheel <= 0) || r.aux == SM_AT || r.aux == SM_POLYAT || r.aux == SM_BEND || (r.aux >= SM_MPEX && r.aux <= SM_RELVEL)) {
            n_aux_rest++;   // scaled by a controller at rest: nothing
            continue;
        }
        if (is_macro(r.aux)) r.amt *= mval[r.aux - SM_MACRO1];
        if (r.aux == SM_WHEEL) r.amt *= wheel;   // at the saved wheel position
        r.type = slot.field("destModuleTypeString").text();
        r.param = slot.field("destModuleParamName").text();
        r.did = int(slot.field("destModuleID").number(-1));
        if (r.amt != 0) routes.push_back(r);
    }
    // a route on an oscillator's level (Oscillator 0..2, 3 the noise, 4 the sub)
    auto vol_route = [](const SRoute &r) { return r.type == "Oscillator" && r.param == "kParamVolume" && r.did >= 0 && r.did < 5; };

    // ---- the oscillators: their levels (macros folded), the two that sound most ----
    struct OscInfo { bool ok = false, sample = false, multi = false, default_table = false, filtered = false, fx_only = false; int frames = 0; double level = 0, dyn = 0, rank = 0, balance = -100; };
    OscInfo oi[3];
    bool other_type = false, warp = false, unison_capped = false, any_sample = false, any_grain = false, any_default = false;
    double vol_static[5] = {0, 0, 0, 0, 0}, vol_dyn[5] = {0, 0, 0, 0, 0};
    for (const SRoute &r : routes) {
        if (!vol_route(r)) continue;
        if (is_macro(r.src)) vol_static[r.did] += r.amt * (r.bip ? 2 * mval[r.src - SM_MACRO1] - 1 : mval[r.src - SM_MACRO1]);
        else if (r.src == SM_FIXED) vol_static[r.did] += r.amt;
        else vol_dyn[r.did] += std::max(0.0, r.amt);
    }
    // every source under -40 dB (BA - The Even Odds: 0.00017 each, its FX bring it up): raised so the loudest plays at 0.5 [G]
    double gain_up = 1;
    {
        double loudest = 0;
        for (int j = 0; j < 5; j++) {
            const SerumItem pp = root.field("Oscillator" + std::to_string(j)).field("plainParams");
            if (pp.number("kParamEnable", j == 0 ? 1 : 0) >= 0.5) loudest = std::max(loudest, clampd(pp.number("kParamVolume", 0.75) + vol_static[j], 0, 1) + vol_dyn[j]);
        }
        if (loudest > 0 && loudest < 0.01) { gain_up = 0.5 / loudest; notes.push_back("levels under -40 dB raised (the preset's FX bring them up)"); }
        for (int j = 0; j < 5; j++) vol_static[j] *= gain_up;
    }
    for (int j = 0; j < 3; ++j) {
        const std::string id = std::to_string(j);
        const SerumItem osc = root.field("Oscillator" + id), params = osc.field("plainParams");
        if (params.number("kParamEnable", j == 0 ? 1 : 0) < 0.5) continue;
        OscInfo &q = oi[j];
        q.level = clampd(params.number("kParamVolume", 0.75) * gain_up + vol_static[j], 0, 1);
        q.dyn = vol_dyn[j];
        if (q.level < 0.01 && q.dyn <= 0) continue;   // silent: never heard
        const SerumItem type = params.field("kParamType");
        q.sample = serum_plays_sample(type);
        q.multi = type.equals("kOsc_MultiSample");
        if (type.p && !type.equals("kOsc_Wavetable") && !q.sample && !q.multi) { other_type = true; continue; }
        try {
            if (q.sample) serum_library_file(*vol, path, "Samples", serum_sample_module(osc, type, id).field("samplePathRelative").text());
            else if (q.multi) {
                const SerumItem m = osc.field("MultiSampleOsc" + id);
                const std::vector<SerumSfzRegion> regs = serum_sfz_regions(m.field("embedded_sfz").text());
                std::string base = path_dir(m.field("sfzPathRelative").text());
                std::replace(base.begin(), base.end(), '\\', '/');
                if (regs.empty()) serum_library_file(*vol, path, "Multisamples", m.field("sfzPathRelative").text());
                else {
                    std::string rel = regs.front().sample;
                    std::replace(rel.begin(), rel.end(), '\\', '/');
                    while (rel.find("//") != std::string::npos) rel.replace(rel.find("//"), 2, "/");
                    serum_library_file(*vol, path, "Multisamples", base.empty() ? rel : base + "/" + rel);
                }
            } else {
                const SerumItem wt = osc.field("WTOsc" + id), data = wt.field("embeddedWTData");
                if (data.type == 4 && data.value >= uint64_t(WT_SIZE) && data.value % WT_SIZE == 0 && data.value <= uint64_t(WT_SIZE) * WT_MAX_FRAMES)
                    q.frames = int(data.value / WT_SIZE);
                else {
                    const std::string rel = wt.field("relativePathToWT").text();
                    if (rel.empty()) q.default_table = true;
                    else {
                        std::string found;
                        try { found = serum_table_path(*vol, path, rel); }
                        catch (const ParseError &) { throw ParseError("Serum 2: needs the factory wavetable \"" + rel + "\" (copy Serum's Tables folder beside the preset or above it)"); }
                        try {
                            AudioInfo ai = probe_audio(*vol->open(found));
                            if (ai.frames >= WT_SIZE && ai.frames % WT_SIZE == 0) q.frames = int(std::min<int64_t>(ai.frames / WT_SIZE, WT_MAX_FRAMES));
                        } catch (const std::exception &) {}
                        if (!q.frames) throw ParseError("Serum 2: " + rel + " is not a 2048-sample table");
                    }
                }
            }
        } catch (const ParseError &e) { missing.push_back(e.what()); continue; }
        // its routing: to the filters, or past them
        const SerumItem rs = root.field("RoutingSlot" + id).field("plainParams");
        const SerumItem dest = rs.field("kParamRoutingDest");
        q.filtered = dest.p ? dest.equals("kRoutingDestFilter") : j == 0;
        q.balance = rs.number("kParamFilterBalance", -100);   // -100 Filter 1 (the default: never stored) .. 100 Filter 2
        q.fx_only = dest.equals("kRoutingDestNone");           // only to the FX buses (a reverb-only layer): Omni has none
        q.rank = (q.level + 0.5 * std::min(1.0, q.dyn)) * (q.fx_only ? 0.1 : 1);
        q.ok = true;
    }
    int order[3] = {0, 1, 2}, omni_of[3] = {-1, -1, -1}, used = 0, skipped_extra = 0;
    std::stable_sort(order, order + 3, [&](int a, int b) { return (oi[a].ok ? oi[a].rank : -1) > (oi[b].ok ? oi[b].rank : -1); });
    for (int i = 0; i < 3; i++) {
        if (!oi[order[i]].ok) continue;
        if (used == 2) { skipped_extra++; continue; }
        omni_of[order[i]] = used++;
    }
    if (used == 2) {   // the two chosen in Serum's order: the lower Serum oscillator on A
        int a = -1, b = -1;
        for (int j = 0; j < 3; j++) if (omni_of[j] >= 0) { if (a < 0) a = j; else b = j; }
        omni_of[a] = 0; omni_of[b] = 1;
    }
    int serum_of[2] = {-1, -1};
    for (int j = 0; j < 3; j++) if (omni_of[j] >= 0) serum_of[omni_of[j]] = j;
    for (int a = 0; a < used; a++) {
        const int j = serum_of[a];
        const std::string id = std::to_string(j);
        const SerumItem osc = root.field("Oscillator" + id), params = osc.field("plainParams");
        const OscInfo &q = oi[j];
        auto &o = patch.osc[a];
        o.on = true;
        if (q.sample || q.multi) {
            o.sample = true;
            o.file = "serumsmp:" + id + ":{path}";
            o.position = clampd(params.number("kParamStart", 0) / 100.0, 0, 0.95);   // [G] the sample's start, percent
            any_sample = true;
            if (q.sample && !params.field("kParamType").equals("kOsc_Sample")) any_grain = true;
        } else if (q.default_table) {   // [G] Serum's default (basic shapes) table: Omni's classic saw
            o.wavetable = false;
            o.wave = SynthPatch::SAW;
            any_default = true;
        } else {
            const SerumItem wt = osc.field("WTOsc" + id);
            o.wavetable = true;
            o.file = "serum:" + id + ":{path}";
            const double pos = wt.field("plainParams").number("kParamTablePos", 1);   // frame 1..256
            o.position = q.frames > 1 ? clampd((pos - 1) / (q.frames - 1), 0, 1) : 0;
            // 2.4.35: the first warp onto Omni's (Bend, PWM, Mirror, Sync, FM from the other oscillator) [G: the amounts
            // 0..1 as Omni's; PD = phase distortion as Bend; RM from an oscillator = the SYNTH's ring]
            const SerumItem wm = wt.field("plainParams").field("kParamWarpMenu");
            const double wa = clampd(wt.field("plainParams").number("kParamWarp", 0), 0, 1);
            if (wa > 0.001) {
                const std::string m = wm.text();
                int mode = -1;
                if (m == "kSync" || m == "kHardSync" || m == "kSoftSync" || m == "kSofterSync") mode = SynthPatch::W_SYNC;
                else if (m.empty() || m.rfind("kBend", 0) == 0 || m == "kPD_OSC" || m == "kPD_OSC2" || m == "kSelfPD") mode = SynthPatch::W_BEND;
                else if (m == "kPWM") mode = SynthPatch::W_PWM;
                else if (m == "kMirror") mode = SynthPatch::W_MIRROR;
                else if (m == "kFM_OSC" || m == "kFM_OSC2" || m == "kFMX_OSC" || m == "kFMX_OSC2" || m == "kFMP_OSC" || m == "kFMP_OSC2") mode = SynthPatch::W_FM;
                else if (m == "kRM_OSC" || m == "kRM_OSC2") patch.ring = std::max(patch.ring, wa);
                if (mode >= 0) { o.warp_mode = mode; o.warp = wa; } else if (!(m == "kRM_OSC" || m == "kRM_OSC2")) warp = true;
            }
        }
        // its level: the stored one with the macros folded in; only dynamic modulation: half its reach [G]
        o.level = q.level >= 0.01 ? q.level : clampd(0.5 * q.dyn, 0, 1);
        if (q.fx_only) o.level *= 0.5;   // [G] heard only through Serum's FX (a send)
        o.pan = clampd(params.number("kParamPan", 0) / 50.0, -1, 1);   // [G] -50..50
        o.octave = clampi(int(std::lround(params.number("kParamOctave", 0))), -4, 4);
        const double semis = params.number("kParamPitch", 0) + params.number("kParamCoarsePit", 0);   // [G] both semitones
        o.semi = clampi(int(std::lround(semis)), -24, 24);
        o.fine = clampd(params.number("kParamFine", 0) + (semis - std::lround(semis)) * 100, -100, 100);
        const int uni = int(std::lround(params.number("kParamUnison", 1)));
        if (uni > SYN_MAX_UNISON) unison_capped = true;
        o.unison = clampi(uni, 1, SYN_MAX_UNISON);
        o.detune = clampd(params.number("kParamDetune", 0.25), 0, 1);   // [G] 0..1 as Omni's 0..100 ct
        o.spread = clampd(params.number("kParamUnisonStereo", 100) / 100.0, 0, 1);
    }
    // NOISE (Oscillator3) and SUB (Oscillator4). 2.4.35: Serum's noise plays a SAMPLE (NoiseOsc3.relativePathToNoiseSample,
    // under Samples/Factory Non-Tonal/Noises: 531 of 626 factory presets) unless kParamNoiseType names White / Pink /
    // Brown / Geiger: the sample plays on a free SYNTH oscillator; with none free, Omni's noise at half level [G]
    int noise_slot = -1;
    double noise_base = 0;
    bool noise_as_noise = false;
    {
        const SerumItem no = root.field("Oscillator3"), np = no.field("plainParams"), n = no.field("NoiseOsc3").field("plainParams");
        if (np.number("kParamEnable", 0) >= 0.5) {
            const double lv = clampd(np.number("kParamVolume", 0.75) * gain_up + vol_static[3], 0, 1);
            noise_base = lv;
            if (lv >= 0.01 || vol_dyn[3] > 0) {
                const SerumItem t = n.field("kParamNoiseType");
                const std::string rel = no.field("NoiseOsc3").field("relativePathToNoiseSample").text();
                bool sample = !t.p && !rel.empty();
                if (sample && used < 2) {
                    try { serum_library_file(*vol, path, "Samples", "Factory Non-Tonal/Noises/" + rel); }
                    catch (const ParseError &e) { missing.push_back(e.what()); sample = false; }
                    if (sample) {
                        noise_slot = used++;
                        auto &o = patch.osc[noise_slot];
                        o.on = true;
                        o.sample = true;
                        o.file = "serumsmp:3:{path}";
                        o.level = lv >= 0.01 ? lv : clampd(0.5 * vol_dyn[3], 0, 1);
                        any_sample = true;
                    }
                } else if (sample) noise_as_noise = true;
                if (noise_slot < 0) {
                    patch.noise_on = true;
                    patch.noise_level = (lv >= 0.01 ? lv : clampd(0.5 * vol_dyn[3], 0, 1)) * (noise_as_noise ? 0.5 : 1);
                    patch.noise_color = t.equals("Pink") || t.equals("Brown") ? 1 : t.equals("Geiger") ? 2 : 0;   // [G] Geiger = Crackle
                    patch.noise_tone = clampd(n.number("kParamColor", 0.5), 0, 1);
                }
            }
        }
        const SerumItem sp = root.field("Oscillator4").field("plainParams"), s = root.field("Oscillator4").field("SubOsc4").field("plainParams");
        if (sp.number("kParamEnable", 0) >= 0.5) {
            const double lv = clampd(sp.number("kParamVolume", 0.75) * gain_up + vol_static[4], 0, 1);
            if (lv >= 0.01 || vol_dyn[4] > 0) {
                patch.sub_on = true;
                patch.sub_level = lv >= 0.01 ? lv : clampd(0.5 * vol_dyn[4], 0, 1);
                patch.sub_oct = sp.number("kParamOctave", -1) <= -2 ? 2 : 1;
                const SerumItem sh = s.field("kParamShape");
                patch.sub_shape = sh.equals("kTriangle") ? 1 : (sh.equals("kSquare") || sh.equals("kSaw")) ? 2 : 0;   // [G] saw as square
            }
        }
    }
    if (!used && !patch.noise_on && !patch.sub_on) {   // nothing Omni plays sounds
        if (!missing.empty()) throw ParseError(missing.front());
        if (other_type) throw ParseError("Serum 2: no oscillator Omni plays");
        throw ParseError("Serum 2: no oscillator to play");
    }
    // ---- FILTER: the one the louder routed oscillators use ----
    int filter_id = -1;
    {
        double in_f = 0, past_f = 0, bal = -100, bal_w = -1;
        for (int j = 0; j < 3; j++) {
            if (omni_of[j] < 0) continue;
            const double w = patch.osc[omni_of[j]].level;
            if (oi[j].filtered) { in_f += w; if (w > bal_w) { bal_w = w; bal = oi[j].balance; } } else past_f += w;
        }
        bool en[2];
        for (int f = 0; f < 2; f++) en[f] = root.field("VoiceFilter" + std::to_string(f)).field("plainParams").number("kParamEnable", 0) >= 0.5;
        if (in_f > 0 && in_f >= past_f) {
            const int first = bal > 0 ? 1 : 0;   // [G] 0 = both: Filter 1
            filter_id = en[first] ? first : en[1 - first] ? 1 - first : -1;
        } else if (in_f > 0 && (en[0] || en[1])) notes.push_back("the louder oscillators bypass the filter: left off");
    }
    if (filter_id >= 0) {
        const SerumItem fp = root.field("VoiceFilter" + std::to_string(filter_id)).field("plainParams");
        const std::string ft = fp.field("kParamType").text();
        const int ty = serum_filter_type(ft, fp.number("kParamVar", 0));
        if (fp.number("kParamWet", 100) < 50) { notes.push_back("filter mixed in under 50 %: left off"); filter_id = -1; }   // [G] Omni's filter has no mix
        else if (ty < 0) { notes.push_back("filter type " + ft + " not played"); filter_id = -1; }
        else {
            patch.filter_on = true;
            patch.filter_type = ty;
            patch.cutoff_hz = 8.0 * std::pow(22050.0 / 8.0, clampd(fp.number("kParamFreq", 0.5), 0, 1));   // 0.5 = 420 Hz (Serum's 425)
            patch.resonance = clampd(fp.number("kParamReso", 0) / 100.0, 0, 1);
            patch.drive = clampd(fp.number("kParamDrive", 0) / 100.0, 0, 1);
            patch.keytrack = clampd(fp.number("kParamKeyTrack", 0), 0, 1);
            if (filter_id == 1) notes.push_back("filter 2 used as the filter");
        }
    }
    // ---- ENVELOPES: Env0 the amp envelope; Env1..3 the MOD ENVs (they act through the MATRIX) ----
    auto read_env = [&](const SerumItem &e, SynthPatch::Env &out) {
        out.attack = clampd(e.number("kParamAttack", 0.0005) + e.number("kParamHold", 0), 0, 30);
        out.decay = clampd(e.number("kParamDecay", 1), 0.001, 30);
        out.sustain = clampd(e.number("kParamSustain", 1), 0, 1);
        out.release = clampd(e.number("kParamRelease", 0.015), 0.001, 30);
    };
    SynthPatch::Env senv[4];
    for (int k = 0; k < 4; k++) read_env(root.field("Env" + std::to_string(k)).field("plainParams"), senv[k]);
    patch.amp = senv[0];
    patch.mod = senv[1];
    patch.xmod[0].env = senv[2];
    patch.xmod[1].env = senv[3];

    // ---- what each Serum destination is on the SYNTH: its MATRIX destination and the scale of a full-range amount
    // (1 = Serum's whole range) in that destination's units; D_OFF = not played ----
    double off_cut = 0, off_res = 0, off_drive = 0, off_pos[2] = {0, 0}, off_warp[2] = {0, 0}, off_pan = 0, off_pitch = 0, off_ntone = 0;
    auto dest_of = [&](const SRoute &r, double &scale, double *&offset) -> int {
        offset = nullptr;
        scale = 1;
        const int ab = r.did >= 0 && r.did < 3 ? omni_of[r.did] : -1;
        const std::string &t = r.type, &p = r.param;
        if (t == "VoiceFilter") {
            if (r.did != filter_id) return SynthPatch::D_OFF;
            if (p == "kParamFreq") { scale = SERUM_CUTOFF_OCT / 4; offset = &off_cut; return SynthPatch::D_CUTOFF; }
            if (p == "kParamReso") { scale = 2; offset = &off_res; return SynthPatch::D_RES; }   // Omni: + 0.5 per unit
            if (p == "kParamDrive") { offset = &off_drive; return SynthPatch::D_DRIVE; }
            return SynthPatch::D_OFF;
        }
        if (t == "WTOsc" && ab >= 0 && patch.osc[ab].wavetable && p == "kParamTablePos") { offset = &off_pos[ab]; return ab ? SynthPatch::D_POS_B : SynthPatch::D_POS_A; }
        if (t == "WTOsc" && ab >= 0 && patch.osc[ab].warp_mode != SynthPatch::W_OFF && p == "kParamWarp") { offset = &off_warp[ab]; return ab ? SynthPatch::D_WARP_B : SynthPatch::D_WARP_A; }
        if (t == "Oscillator" && ab >= 0) {
            if (p == "kParamFine") { scale = 2.0 / 12; offset = &off_pitch; return SynthPatch::D_PITCH; }   // +-100 ct; Omni 12 st per unit
            if (p == "kParamCoarsePit" || p == "kParamPitch" || p == "kParamOctave") { scale = 8; offset = &off_pitch; return SynthPatch::D_PITCH; }   // [G] +-48 st
            if (p == "kParamPan") { scale = 2; offset = &off_pan; return SynthPatch::D_PAN; }
            if (p == "kParamDetune") return ab ? SynthPatch::D_DET_B : SynthPatch::D_DET_A;   // [G]
            return SynthPatch::D_OFF;
        }
        if (t == "LFO" && p == "kParamRate") return SynthPatch::D_OFF;   // (set below, once the LFOs are placed)
        if (t == "NoiseOsc" && p == "kParamColor") { offset = &off_ntone; return SynthPatch::D_NOISE_TONE; }
        if (t == "Global" && p == "kParamPortamentoTime") return SynthPatch::D_GLIDE;   // [G]
        return SynthPatch::D_OFF;
    };

    // ---- which macros and LFOs stay live (Omni has 4 of each): the ones the played routes use most ----
    int mac_use[8] = {}, lfo_use[10] = {};
    for (const SRoute &r : routes) {
        double sc;
        double *of;
        const bool lfo_rate = r.type == "LFO" && r.param == "kParamRate", vol = vol_route(r);
        if (!vol && !lfo_rate && dest_of(r, sc, of) == SynthPatch::D_OFF) continue;
        if (is_macro(r.src) && !vol) mac_use[r.src - SM_MACRO1]++;   // (macros on a level fold)
        if (is_lfo(r.src)) lfo_use[r.src - SM_LFO1]++;
    }
    int mac_slot[8], lfo_slot[10], lfo_of[4] = {-1, -1, -1, -1};
    auto pick = [](const int *use, int n, int *slot, int *back) {
        std::vector<int> idx;
        for (int i = 0; i < n; i++) { slot[i] = -1; if (use[i] > 0) idx.push_back(i); }
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return use[a] > use[b]; });
        if (idx.size() > 4) idx.resize(4);
        std::sort(idx.begin(), idx.end());   // in Serum's order
        for (size_t k = 0; k < idx.size(); k++) { slot[idx[k]] = int(k); if (back) back[k] = idx[k]; }
    };
    int mac_of[4] = {-1, -1, -1, -1};
    pick(mac_use, 8, mac_slot, mac_of);
    pick(lfo_use, 10, lfo_slot, lfo_of);
    // the routes' amp-level LFOs that stay unused are not built; an LFO routed nowhere Omni plays is left default

    // ---- LFOs ----
    static const double SYNC_BEATS[] = {0, 16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3, 1.0 / 3, 1.0 / 6, 1.5, 0.75, 0.375};
    bool any_curve = false;
    for (int k = 0; k < 4; k++) {
        if (lfo_of[k] < 0) continue;
        const SerumItem lfo = root.field("LFO" + std::to_string(lfo_of[k])), lp = lfo.field("plainParams");
        SynthPatch::Lfo &l = patch.lfo[k];
        bool curve = false;
        l.shape = serum_lfo_shape(lfo, curve);
        if (curve) { l.ref = "serumlfo:" + std::to_string(lfo_of[k]) + ":{path}"; any_curve = true; }
        const SerumItem mode = lp.field("kParamMode");
        l.retrig = mode.p && !mode.equals("Free");
        l.one_shot = mode.equals("Envelope");   // runs once from each note
        const double rate = lp.number("kParamRate", 1);
        if (lp.number("kParamBeatSync", 1) < 0.5) { l.hz = clampd(rate, 0.05, 30); l.sync = 0; }
        else {   // [G] tempo: the rate as beats, the nearest of Omni's divisions (1/4 when unknown)
            int best = 5;
            if (rate >= 0.1 && rate <= 16) {
                double bd = 1e9;
                for (int i = 1; i < 15; i++) { const double d = std::fabs(std::log(SYNC_BEATS[i] / rate)); if (d < bd) { bd = d; best = i; } }
            }
            l.sync = best;
        }
    }

    // ---- MATRIX ----
    bool menv[3] = {false, false, false};
    // a Serum source on the SYNTH: Omni's source and u = a * s + b (Serum's 0..1 from Omni's value s); konst: a constant u
    auto src_of = [&](int s, int &omni, double &a, double &b, double &konst) -> bool {
        a = 1; b = 0; konst = -1; omni = SynthPatch::S_OFF;
        if (s == SM_WHEEL) omni = SynthPatch::S_WHEEL;
        else if (s == SM_ENV1) omni = SynthPatch::S_AMPENV;
        else if (s == SM_ENV2) { omni = SynthPatch::S_MODENV; menv[0] = true; }
        else if (s == SM_ENV3) { omni = SynthPatch::S_MODENV2; menv[1] = true; }
        else if (s == SM_ENV4) { omni = SynthPatch::S_MODENV3; menv[2] = true; }
        else if (is_lfo(s)) {
            const int k = lfo_slot[s - SM_LFO1];
            if (k < 0) return false;
            static const int L[4] = {SynthPatch::S_LFO1, SynthPatch::S_LFO2, SynthPatch::S_LFO3, SynthPatch::S_LFO4};
            omni = L[k]; a = 0.5; b = 0.5;   // Omni's LFOs -1..1, Serum's 0..1
        } else if (s == SM_VELO) omni = SynthPatch::S_VEL;
        else if (s == SM_NOTE) { omni = SynthPatch::S_NOTE; a = b = 60.0 / 127; }   // Omni (note - 60) / 60, Serum note / 127
        else if (s == SM_AT || s == SM_POLYAT) omni = SynthPatch::S_AT;
        else if (s == SM_RAND1 || s == SM_RAND2 || s == SM_RAND_DISCRETE) { omni = SynthPatch::S_RANDOM; a = 0.5; b = 0.5; }
        else if (is_macro(s)) {
            const int k = mac_slot[s - SM_MACRO1];
            if (k < 0) { konst = mval[s - SM_MACRO1]; return true; }
            omni = SynthPatch::S_MACRO1 + k;
        } else if (s == SM_FIXED) { konst = 1; return true; }
        else return false;
        return true;
    };
    bool fenv_used = false;
    bool lvl_reset[2] = {false, false};   // a SYNTH oscillator's level put back to its stored one (once)
    for (const SRoute &r : routes) {
        if (r.type.compare(0, 2, "FX") == 0) { n_fx++; continue; }
        int omni = 0;
        double a = 1, b = 0, konst = -1;
        if (!src_of(r.src, omni, a, b, konst)) { n_skip_src++; continue; }
        const bool is_k = konst >= 0;   // a constant source (a folded macro, Fixed)
        // bipolar [G]: the source's 0..1 as -1..1 at the full amount (the arc both ways, as Vital's): u' = 2u - 1
        if (r.bip) { a *= 2; b = 2 * b - 1; if (is_k) konst = 2 * konst - 1; }
        // the mod wheel: as saved (its position folded in), the rest of its travel on Omni's wheel (0 .. full = Serum's)
        if (r.src == SM_WHEEL && wheel > 0) { b += a * wheel; a *= 1 - wheel; }
        // the lowest value of Omni's source (LFOs, note, random: -1; the rest 0)
        const double s_lo = omni == SynthPatch::S_LFO1 || omni == SynthPatch::S_LFO2 || omni == SynthPatch::S_LFO3 || omni == SynthPatch::S_LFO4 ||
                            omni == SynthPatch::S_NOTE || omni == SynthPatch::S_RANDOM ? -1 : 0;
        int d = SynthPatch::D_OFF;
        double scale = 1, *offset = nullptr;
        if (vol_route(r)) {   // an oscillator's level: constants are folded already
            if (is_k) continue;
            const int j = r.did;
            const int slot = j < 3 ? omni_of[j] : j == 3 ? noise_slot : -1;   // the SYNTH oscillator it plays on
            if (slot >= 0) {
                auto &ol = patch.osc[slot].level;
                // 2.4.35: LEVEL A / B are absolute (Serum's: stored + amount x source, floored at 0). A level that came only
                // from modulation (stored ~0, set to half its reach above) goes back to the stored one: the route plays it
                const double stored = j < 3 ? oi[j].level : noise_base;
                if (stored < 0.01 && !lvl_reset[slot]) { ol = clampd(stored, 0, 1); lvl_reset[slot] = true; }
                // the part of u that does not move (an LFO's middle); a pull down past silence (a gating tremolo) keeps
                // half the level, so it pulses instead of never sounding [G]
                ol = clampd(std::max(ol + r.amt * b, r.amt * b < 0 ? 0.5 * ol : 0.0), 0, 1);
                d = slot ? SynthPatch::D_LEVEL_B : SynthPatch::D_LEVEL_A;
            } else if (j == 3 && patch.noise_on) { d = SynthPatch::D_NOISE; scale = 1 / std::max(0.05, patch.noise_level); }
            else if (j == 4 && patch.sub_on) { d = SynthPatch::D_SUB; scale = 1 / std::max(0.05, patch.sub_level); }
            else { n_skip_dst++; continue; }
        } else if (r.type == "LFO" && r.param == "kParamRate" && r.did >= 0 && r.did < 10 && lfo_slot[r.did] >= 0) {
            static const int RD[4] = {SynthPatch::D_LFO1_RATE, SynthPatch::D_LFO2_RATE, SynthPatch::D_LFO3_RATE, SynthPatch::D_LFO4_RATE};
            d = RD[lfo_slot[r.did]];   // [G] Omni: +-3 octaves per unit
        } else d = dest_of(r, scale, offset);
        if (d == SynthPatch::D_OFF) { n_skip_dst++; continue; }
        if (is_k) {   // a constant: its value moves the setting itself
            if (offset) *offset += r.amt * konst * scale;
            continue;
        }
        if (offset) *offset += r.amt * b * scale;   // the part of u that does not move
        // an envelope on the cutoff: Omni's FILTER ENV (+-6 octaves, that envelope's own times) the first time
        if (d == SynthPatch::D_CUTOFF && !fenv_used && (omni == SynthPatch::S_AMPENV || omni == SynthPatch::S_MODENV ||
                                                        omni == SynthPatch::S_MODENV2 || omni == SynthPatch::S_MODENV3)) {
            const double oct = r.amt * a * SERUM_CUTOFF_OCT, oct_c = clampd(oct, -6, 6);
            patch.flt = senv[omni == SynthPatch::S_AMPENV ? 0 : omni == SynthPatch::S_MODENV ? 1 : omni == SynthPatch::S_MODENV2 ? 2 : 3];
            patch.env_amount = oct_c / 6;
            if (oct > oct_c) off_cut += (oct - oct_c) / 4;   // past Omni's reach: the top kept (the open filter)
            fenv_used = true;
            continue;
        }
        const double full = r.amt * a * scale, amt = clampd(full, -1, 1);
        // past Omni's reach (+-1): the top of the sweep kept (the brightest / loudest end)
        if (offset && full != amt) *offset += (full - amt) * (full > 0 ? 1 : s_lo);
        if (std::fabs(amt) < 0.003) continue;
        if (int(patch.routes.size()) < SynthPatch::MAX_ROUTES) patch.routes.push_back({omni, d, amt});
        else n_full++;
    }
    for (int k = 0; k < 3; k++) menv[k] = false;   // on only when a route kept uses it
    for (const auto &rt : patch.routes) {
        if (rt.src == SynthPatch::S_MODENV) menv[0] = true;
        if (rt.src == SynthPatch::S_MODENV2) menv[1] = true;
        if (rt.src == SynthPatch::S_MODENV3) menv[2] = true;
    }
    patch.mod_env_on = menv[0];   // as MATRIX sources (no destination of their own)
    patch.xmod[0].on = menv[1];
    patch.xmod[1].on = menv[2];
    // the constant parts of the modulation, onto the settings they move (in the MATRIX's units)
    if (patch.filter_on) {
        patch.cutoff_hz = clampd(patch.cutoff_hz * std::exp2(off_cut * 4), 8, 22050);
        patch.resonance = clampd(patch.resonance + off_res * 0.5, 0, 1);
        patch.drive = clampd(patch.drive + off_drive, 0, 1);
    }
    for (int a = 0; a < 2; a++) if (patch.osc[a].on) {
        patch.osc[a].position = clampd(patch.osc[a].position + off_pos[a], 0, 1);
        if (patch.osc[a].warp_mode != SynthPatch::W_OFF) patch.osc[a].warp = clampd(patch.osc[a].warp + off_warp[a], 0, 1);
    }
    patch.pan = clampd(patch.pan + off_pan, -1, 1);
    if (off_pitch != 0)
        for (int a = 0; a < used; a++) {   // in semitones (12 per unit)
            auto &o = patch.osc[a];
            const double st = o.semi + o.fine / 100 + off_pitch * 12;
            o.semi = clampi(int(std::lround(st)), -24, 24);
            o.fine = clampd((st - o.semi) * 100, -100, 100);
        }
    if (patch.noise_on) patch.noise_tone = clampd(patch.noise_tone + off_ntone, 0, 1);
    // VOICE and output
    const SerumItem g = root.field("Global0").field("plainParams");
    patch.polyphony = clampi(int(std::lround(g.number("kParamPolyCount", 8))), 1, 16);
    if (g.number("kParamMonoToggle", 0) >= 0.5) patch.voice_mode = g.number("kParamLegato", 0) >= 0.5 ? 2 : 1;
    patch.glide = clampd(g.number("kParamPortamentoTime", 0), 0, 10);
    patch.glide_legato = g.number("kParamPortaAlways", 0) < 0.5;
    patch.bend = clampi(int(std::lround(std::fabs(g.number("kParamBendRangeUp", 2)))), 0, 24);
    patch.level_db = clampd(20 * std::log10(std::max(1e-3, g.number("kParamMasterVolume", 0.7)) / 0.7), -6, 3);   // [G] taper unknown: kept near 0 dB
    inst.settings = synth_settings(patch);
    for (int k = 0; k < 4; k++)   // MACRO 1..4: the live Serum macros' positions
        inst.settings.emplace_back("syn_macro" + std::to_string(k + 1), float(mac_of[k] >= 0 ? mval[mac_of[k]] * 100 : 0));
    {
        std::string mm;
        bool moved = false;
        for (int k = 0; k < 4; k++) if (mac_of[k] >= 0) { mm += std::string(mm.empty() ? "" : ", ") + std::to_string(k + 1) + "=" + std::to_string(mac_of[k] + 1); moved = moved || mac_of[k] != k; }
        if (moved) notes.push_back("MACROs " + mm + " (Omni = Serum)");
    }
    if (skipped_extra) notes.push_back("the quietest oscillator not played (the SYNTH has two)");
    if (!missing.empty()) notes.push_back(missing.front());
    if (unison_capped) notes.push_back("unison above 8 voices played as 8");
    if (warp) notes.push_back("some warp modes not played (from the sub / noise / filters, spectral, distortion)");
    if (any_default) notes.push_back("Serum's default table played as a saw");
    if (any_grain) notes.push_back("granular / spectral oscillators play their sample plainly");
    if (noise_slot >= 0) notes.push_back(std::string("the noise sample on OSC ") + char('A' + noise_slot));
    if (noise_as_noise) notes.push_back("the noise sample played as noise (no oscillator free)");
    if (other_type) notes.push_back("an oscillator type Omni does not play left out");
    if (n_skip_src || n_skip_dst || n_full) notes.push_back(std::to_string(n_skip_src + n_skip_dst + n_full) + " modulation routes not mapped");
    if (n_aux_rest) notes.push_back(std::to_string(n_aux_rest) + " routes scaled by the mod wheel / aftertouch (0 at rest)");
    (void)any_curve;
    std::string note = std::string("Serum 2: ") + (any_sample ? "samples and wavetables" : "wavetables") +
                       " on the SYNTH, filter, envelopes, LFO curves and modulation mapped; FX not read" +
                       (n_fx ? " (" + std::to_string(n_fx) + " FX routes)" : std::string());
    for (const std::string &n : notes) note += "; " + n;
    inst.warnings.push_back(note);
    return inst;
}

}  // namespace

void register_serum() {
    register_reader({"Serum 2 preset", "serumpreset", probe_serum, list_serum, load_serum});
}

}  // namespace omni

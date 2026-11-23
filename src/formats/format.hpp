// Omni Sampler: format readers. Each reader lists the presets in a file and loads one of them into the instrument
// model; samples stay undecoded SampleRefs until the engine's loader thread decodes them.
#pragma once
#include <string>
#include <vector>
#include "../core/audio.hpp"
#include "../core/model.hpp"
#include "../core/vfs.hpp"

namespace omni {

struct FormatReader {
    const char *name;     // shown on the display ("SFZ", "Akai S1000/S3000 program", ...)
    const char *exts;     // space-separated lower-case extensions this reader may take ("" = probe only)
    // Optional content check (null = the extension decides). lower_name is the file name in lower case.
    bool (*probe)(const std::string &lower_name, const uint8_t *head, size_t n, uint64_t size);
    std::vector<PresetInfo> (*list)(VolumePtr vol, const std::string &path);
    Instrument (*load)(VolumePtr vol, const std::string &path, int index);
    // 2.4.10: zero uses the shared 4 KB default; larger signatures request more bytes for this reader only.
    size_t probe_bytes = 0;
};

void register_all();                                          // formats and image types, once
void register_reader(const FormatReader &reader);
const std::vector<FormatReader> &formats();
// The reader for a file, by extension and content; null when nothing reads it.
const FormatReader *find_reader(Volume &vol, const std::string &path);
size_t reader_probe_bytes();

// Shared helpers for readers
// Host folders readers may search for a library's shared samples (the Plugin Library); set by the plugin
void set_search_dirs(const std::vector<std::string> &dirs);
// A folder of shared samples (an instrument's ROM dumps, a sample disk) that `is` recognises: the cached one; else
// beside the file (each folder above it and two levels under each); else in the search dirs (three levels). The
// cache is the caller's (one per kind of folder); folders are inner paths of the volume.
using IsFolder = bool (*)(Volume &, const std::string &);
std::string find_beside(Volume &vol, const std::string &file, IsFolder is, std::string &cache);
std::string omnisphere_note(VolumePtr vol, const std::string &path);   // 2.4.11: "factory" / "no patch" / ""
void finish_instrument(Instrument &inst);                     // clamp ranges, fill defaults, drop empty zones
double velocity_to_db(int value);                              // helpers for 0..127 scales
// A loop of `length` frames (the chip's own, maybe fractional: Ensoniq OTTO loops end between samples) from frame
// `start`, its last frame at most `last`: rounded to whole frames, the loop's tune keeping the pitch it has on the chip
Loop chip_loop(int64_t start, double length, int64_t last);
// One sample of Roland's LA32 PCM ROMs (MT-32, D-50, D-110, D-10/20), read as a big-endian word `hi`:`lo`: the data
// lines are wired out of order (Munt's [0,9,1,..,7,10,..,15,8]), giving sign + 15-bit log magnitude, 2048 per octave
int16_t la32_sample(uint8_t hi, uint8_t lo);
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

// Registration functions of the modules (each file adds its readers / image types)
void register_sfz();
void register_sf2();
void register_wav_folder();
void register_akai();
void register_emu();
void register_ni();
void register_roland();
void register_reason();
void register_ensoniq();
void register_korg();
void register_korg_triton();
void register_korg_trinity();
void register_korg_m1();
void register_korg_ws();
void register_ensoniq_sd1();
void register_drum_rom();
void register_alesis_drum();
void register_korg_dw8000();
void register_roland_d70();
void register_roland_d110();
void register_roland_d50();
void register_roland_mt32();
void register_tracker();
void register_roland_sc55();
void register_roland_sc88();
void register_roland_jv880();
void register_roland_jd800();
void register_ensoniq_kt();
void register_ensoniq_mr();
void register_kurzweil_k2000();
void register_augmented_strings();
void register_augmented_synth();   // 2.4.22: Arturia Augmented Strings synth layers (.agss) on the SYNTH engine // 2.4.21: Arturia Augmented Strings (.agsm -> SFZ map of .arta samples)
void register_yamaha();
void register_logic();
void register_maize();
void register_bliss();   // 2.1.1
void register_heatup();  // 2.1.2
void register_nxs();     // reFX Nexus .nxs multisamples
void register_nxs_klp(); // reFX Nexus 4.5 KLP1 multisamples
void register_srx_world(); // Roland SRX WORLD (plugin ROM + .fxb states)
void register_ujam_blob(); // UJAM split BLOB libraries (*_part1.blob + *_part2.blob, PCM)
void register_purity();
void register_kawai_k1();  // Kawai K1 / K1r / K1m singles from SysEx, played from the K1 wave ROM (2.4.5)
void register_kawai_k5();  // Kawai K5 / K5m singles from SysEx (an additive model, rendered per key range)
void register_kawai_k5000();  // Kawai K5000 singles (.KA1) and banks (.KAA): the ADD sources, rendered (2.4.5)
// 2.4.1: a UJAM split BLOB's style phrases ("MIDI Files": per event a tick and a packed event, the top byte the phrase,
// then status, data 1, data 2), for the ARP. Throws ParseError.
struct UjamMidi { std::string name; std::vector<uint32_t> ticks, events; };
std::vector<UjamMidi> ujam_midi_files(VolumePtr volume, const std::string &part1_path);
std::vector<Instrument> ujam_load_many(VolumePtr volume, const std::string &part1_path, const std::vector<int> &indexes,
                                       std::vector<std::string> &errors);   // 2.4.28: mapping * 16 + syllable each
void register_ujam_patch();   // 2.4.28: UJAM .patch presets (Symphonic Elements style: SE CHOIR), on the library's BLOB    // LUXONIX Purity .wvt waveform maps (+ .smp sample banks), .usr user banks (list only)
void register_rom_map(); // Omni ROM map (.omnirom JSON + raw PCM image)
void register_decent_sampler();   // 2.4.30: Decent Sampler presets (.dspreset; .dslibrary = ZIP)
void register_directwave();  // Image-Line DirectWave programs (.dwp)
void register_ableton();     // Ableton Sampler / Simpler presets and racks (.adv, .adg)
void register_omnisphere();  // Omnisphere patches / multis with user soundsources (.prt_omn, .mlt_omn, .zmap)
void register_omnisphere_factory_db();  // 2.4.14: Omnisphere .db banks (Waveforms / Human Voices / Omnisphere Library): browsable, were never registered
void register_dx7();         // 2.4.14: Yamaha DX7 voices (.syx) on the SYNTH engine (2-operator approximation)
void register_serum();       // 2.4.32: Serum 2 presets (.SerumPreset) on the SYNTH (formats/serum.cpp)
void register_vital();       // 2.4.11: Vital presets (.vital; .vitalbank = ZIP) on the SYNTH engine + its recording
// 2.1.3: the sample folder a Heat Up preset names (its intr=), from the file's first bytes; "" = none / not Heat Up
std::string heatup_sample_folder(const std::vector<uint8_t> &head);
void register_images();

}  // namespace omni

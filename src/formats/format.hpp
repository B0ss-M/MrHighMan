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
};

void register_all();                                          // formats and image types, once
void register_reader(const FormatReader &reader);
const std::vector<FormatReader> &formats();
// The reader for a file, by extension and content; null when nothing reads it.
const FormatReader *find_reader(Volume &vol, const std::string &path);
size_t reader_probe_bytes();

// Shared helpers for readers
void finish_instrument(Instrument &inst);                     // clamp ranges, fill defaults, drop empty zones
double velocity_to_db(int value);                              // helpers for 0..127 scales
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
void register_images();

}  // namespace omni

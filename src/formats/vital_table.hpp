// Omni Sampler 2.4.13: Vital's own shapes, read from its files (written from the files themselves: the Vital Pro set,
// 794 presets, 210 .vitaltable, 115 .vitallfo, and two user banks). Used by the preset reader (formats/vital.cpp) and by
// the SYNTH's oscillator / LFO loaders (plugin.cpp), which find a shape again from a reference:
//   "vital:<osc 1..3>:<full path of a .vital>"  a preset's wavetable     (a .vitaltable file: its own path)
//   "vlfo:<lfo 1..8>:<full path of a .vital>"   a preset's LFO shape     (a .vitallfo file: its own path)
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omni {

struct Json;

// A Vital line (an LFO's shape, a Line Source's cycle): points x 0..1, y 0..1 (0 = the TOP), a power per point (the
// curve from it to the next: 0 straight), smooth (eased segments)
struct VitalLine {
    std::vector<double> x, y, power;
    bool smooth = false;
    std::string name;
};
bool vital_line_parse(const Json &j, VitalLine &out);   // {num_points, points [x, y ...], powers, smooth}; false: unusable
double vital_line_value(const VitalLine &l, double ph);  // 0..1 (1 = the top) at phase 0..1

constexpr int VITAL_LFO_TAB = 256;   // the SYNTH's custom LFO table: VITAL_LFO_TAB + 1 values (a guard for interpolation)
// the shape as the SYNTH's LFOs play it: -1..1 (the top = 1), VITAL_LFO_TAB + 1 values (the last = the first)
std::vector<float> vital_lfo_table(const VitalLine &l);

constexpr int VITAL_CYCLE = 2048;    // Vital's (and Omni's) single cycle
constexpr int VITAL_MAX_FRAMES = 64; // Omni keeps 64 of Vital's 257 positions: 5.8 MB a table with its mip levels
// A Vital wavetable (a preset's "wavetables"[i], a .vitaltable's object) as frames x VITAL_CYCLE samples. frames is 1
// when nothing in it moves, else max_frames (Vital positions 0..256 spread over them). notes: what was approximated or
// skipped. False when no source in it could be read
bool vital_wavetable_render(const Json &table, int max_frames, std::vector<float> &cycles, int &frames, std::vector<std::string> *notes);

// From a file's text (JSON): a .vitaltable (index 0) or a preset's oscillator 1..3 / a .vitallfo (index 0) or a preset's
// LFO 1..8. name: the table's / shape's own. False with err set when it cannot be read
bool vital_wavetable_from_text(const std::string &text, int osc, int max_frames, std::vector<float> &cycles, int &frames,
                               std::string &name, std::string &err);
bool vital_lfo_from_text(const std::string &text, int lfo, VitalLine &out, std::string &err);

// base64 (Vital's binary fields), at most limit bytes out
std::vector<uint8_t> vital_base64(const std::string &s, size_t limit);

// a reference "vital:<n>:<path>" / "vlfo:<n>:<path>": its index and path; false for anything else (a plain file path)
bool vital_ref_parse(const std::string &ref, const char *kind, int &index, std::string &path);

}  // namespace omni

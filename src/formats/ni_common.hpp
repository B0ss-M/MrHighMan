// Omni Sampler: shared parts of the Native Instruments readers: the NI container ("hsin") that Kontakt 5+ and
// Maschine 2+ files use, and string helpers. Translated from ConvertWithMoss's format/ni/nicontainer (LGPL-3.0).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "../core/bytes.hpp"
#include "../core/vfs.hpp"

namespace omni {

enum : int { NI_APP_KONTAKT = 2, NI_APP_MASCHINE = 5, NI_APP_BATTERY = 9 };

struct NiContainer {
    std::vector<uint8_t> preset;   // the first preset chunk item's data
    int app = -1;                  // authoring application, -1 = none found
    bool encrypted = false;        // product IDs / undecodable sub trees: a protected library
};
NiContainer read_ni_container(const std::vector<uint8_t> &data);   // throws ParseError

std::string utf16_len(Reader &r);   // uint32 count, then UTF-16LE units

// Finds the sample files an NI preset references: the stored (often absolute, foreign) path relative to the preset,
// then by name next to it or two folders up (libraries), then by name anywhere below two folders up.
class NiSampleFinder {
public:
    NiSampleFinder(VolumePtr vol, const std::string &preset_path) : vol_(std::move(vol)), dir_(path_dir(preset_path)) {}
    std::string find(const std::string &ref);   // "" when not found

private:
    VolumePtr vol_;
    std::string dir_;
    std::map<std::string, std::string> by_name_;   // lower file name -> path
    bool indexed_ = false;
    void index(const std::string &dir, int depth, int &budget);
};

}  // namespace omni

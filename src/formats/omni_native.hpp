// Omni Sampler: the native instrument format (.omni + "<name> Samples/"), see omni_native.cpp.
#pragma once
#include <string>
#include <utility>
#include <vector>
#include "../core/model.hpp"

namespace omni {

constexpr int OMNI_VERSION = 2;   // 2: Roland S-500 loop ends and loop tune

// Write inst into out_dir (a host folder, created as needed) as <name>.omni and its samples; zone_pcm[i] is zone i's
// decoded audio (null: decoded here). source/source_preset record where it came from: a second extraction of the same
// source returns the existing file. Returns the .omni path. Throws ParseError.
// settings: the plugin's sound settings as a JSON object ("" = none); force_path: rewrite this .omni (its samples
// stay) instead of choosing a new file in out_dir.
std::string write_omni(const Instrument &inst, const std::vector<PcmPtr> &zone_pcm, const std::string &out_dir,
                       const std::string &source, int source_preset, const std::string &settings = "",
                       const std::string &force_path = "");
using Settings_list = std::vector<std::pair<std::string, float>>;
// The sound settings stored in an .omni file on the host (empty if none).
Settings_list omni_settings_of(const std::string &host_path);
// "<source>\n<preset>" recorded in an .omni file on the host, "" if none.
std::string omni_source_of(const std::string &host_path);
// True for an .omni written by an older version whose source format has since been read better: reload it from the
// source (when that is still there) and rewrite it.
bool omni_stale(const std::string &host_path);
void register_omni_native();

}  // namespace omni

// Omni Sampler: a single audio file as an instrument: one zone over the keyboard, root and loop from the file's own
// metadata (WAV smpl/inst, AIFF INST/MARK), C3 otherwise.
#include "format.hpp"

namespace omni {
namespace {

std::vector<PresetInfo> list_audio(VolumePtr, const std::string &path) { return {PresetInfo{path_stem(path), 0}}; }

Instrument load_audio(VolumePtr vol, const std::string &path, int) {
    AudioInfo ai = probe_audio(*vol->open(path));
    Instrument inst;
    inst.name = path_stem(path);
    Zone z;
    z.name = inst.name;
    z.sample = file_sample_ref(vol, path);
    z.sample->rate = ai.rate;
    z.sample->channels = ai.channels;
    z.sample->frames = ai.frames;
    z.root = ai.root >= 0 ? ai.root : 60;
    z.tune = ai.fine / 100.0;
    if (ai.key_lo >= 0 && ai.key_hi >= ai.key_lo && !(ai.key_lo == 0 && ai.key_hi == 0)) { z.key_lo = ai.key_lo; z.key_hi = ai.key_hi; }
    if (ai.vel_lo >= 1 && ai.vel_hi >= ai.vel_lo) { z.vel_lo = ai.vel_lo; z.vel_hi = ai.vel_hi; }
    z.loops = ai.loops;
    z.gain_db = ai.gain_db;
    if (z.loops.empty() && ai.frames < ai.rate * 4) z.one_shot = false;
    inst.zones.push_back(z);
    std::string ext = path_ext(path);
    inst.format = ext == "ncw" ? "NI NCW" : ext == "flac" ? "FLAC" : ext == "ogg" ? "Ogg Vorbis" : ext[0] == 'a' ? "AIFF" : "WAV";
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_wav_folder() {
    register_reader({"Audio file", "wav wave aif aiff aifc flac ogg ncw", nullptr, list_audio, load_audio});
}

}  // namespace omni

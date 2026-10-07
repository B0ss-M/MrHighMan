// Omni Sampler: audio decoding to int16 Pcm. Files: WAV (PCM/float/extensible, RF64, RIFX), AIFF/AIFC (none, sowt,
// fl32, fl64, raw, in24, in32), FLAC (dr_flac), Ogg Vorbis (stb_vorbis), NI NCW. Raw helpers for sample data
// embedded in banks and disk images.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "model.hpp"
#include "vfs.hpp"

namespace omni {

// What a sample file says about itself besides its audio (WAV smpl/inst, AIFF INST/MARK).
struct AudioInfo {
    int root = -1;            // -1 = not given
    double fine = 0;          // cents
    std::vector<Loop> loops;
    int key_lo = -1, key_hi = -1, vel_lo = -1, vel_hi = -1;
    double gain_db = 0;
    int rate = 0, channels = 0, bits = 0;
    int64_t frames = 0;
};

enum class Enc { S8, U8, S16LE, S16BE, S24LE, S24BE, S32LE, S32BE, F32LE, F32BE, F64LE, F64BE };
int enc_bytes(Enc e);

// Interleaved raw samples to int16 (24/32-bit and float are rounded down to 16 bits).
void convert_raw(const uint8_t *src, size_t samples, Enc enc, int16_t *dst);
PcmPtr pcm_from_raw(const uint8_t *src, int64_t frames, int channels, Enc enc, int rate);
// Planar (one channel after another) raw samples to interleaved Pcm.
PcmPtr pcm_from_planar(const uint8_t *src, int64_t frames, int channels, Enc enc, int rate);
// A lazily decoded slice of raw audio inside a blob (bank files, disk images).
SampleRefPtr raw_sample_ref(BlobPtr blob, uint64_t offset, int64_t frames, int channels, Enc enc, int rate,
                            const std::string &name, bool planar = false);

// Decode a whole audio file (format from its magic). info may be null. Throws ParseError.
PcmPtr decode_audio(const Blob &blob, AudioInfo *info = nullptr);
// Header only: rate, channels, frames, loops, root. Cheap for WAV/AIFF/NCW; FLAC/Ogg read their stream info.
AudioInfo probe_audio(const Blob &blob);
bool is_audio_ext(const std::string &ext);
// A sample file in a volume, decoded on demand.
SampleRefPtr file_sample_ref(VolumePtr volume, const std::string &path);

// NI NCW (Kontakt compressed wave)
PcmPtr decode_ncw(const Blob &blob, AudioInfo *info = nullptr);

}  // namespace omni

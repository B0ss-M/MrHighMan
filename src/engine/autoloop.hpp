// Omni Sampler: automatic loop points (see autoloop.cpp).
#pragma once
#include <vector>
#include "../core/model.hpp"

namespace omni {

// The best sustain loop in [start, stop) of p (frames; stop <= 0 = the end). False when the sample is too short or
// nothing matches well enough.
bool find_loop(const Pcm &p, int64_t start, int64_t stop, int64_t &loop_start, int64_t &loop_end);
// Loops for the zones that have none and sustain (not one-shot, not reversed, not release triggers, not decaying to
// silence); pcm[i] is zone i's audio. Returns how many zones got one.
int auto_loop(Instrument &inst, const std::vector<PcmPtr> &pcm);

}  // namespace omni

// Omni Sampler: the Akai readers (S900..S6000, MPC60..MPC 3) and their disk images.
#include "format.hpp"

namespace omni {

void register_akai_s();
void register_akai_akp();
void register_akai_mpc();

void register_akai() {
    register_akai_s();
    register_akai_akp();
    register_akai_mpc();
}

}  // namespace omni

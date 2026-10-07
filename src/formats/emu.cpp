// Omni Sampler: the E-mu readers (Emulator I/II/III/IV/X, Emax, EOS) and their disk images. SoundFont 2 is sf2.cpp.
#include "format.hpp"

namespace omni {

void register_emu_e4();
void register_emu_e3();
void register_emu_emax();
void register_emu_ex();
void register_emu_e1();
void register_emu_e2();

void register_emu() {
    register_emu_e4();
    register_emu_e3();
    register_emu_emax();
    register_emu_ex();
    register_emu_e1();
    register_emu_e2();
}

}  // namespace omni

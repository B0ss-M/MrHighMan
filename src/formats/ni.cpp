// Omni Sampler: Native Instruments readers (Kontakt, Maschine).
#include "format.hpp"
namespace omni {
void register_ni_kontakt();
void register_ni_maschine();
void register_ni() {
    register_ni_maschine();   // before Kontakt: both use the NI container
    register_ni_kontakt();
}
}  // namespace omni

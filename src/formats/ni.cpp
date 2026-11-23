// Omni Sampler: Native Instruments readers (Kontakt, Maschine, Battery 1-3).
#include "format.hpp"
namespace omni {
void register_ni_kontakt();
void register_ni_maschine();
void register_ni_battery();
void register_ni() {
    register_ni_maschine();   // before Kontakt: both use the NI container
    register_ni_kontakt();
    register_ni_battery();
}
}  // namespace omni

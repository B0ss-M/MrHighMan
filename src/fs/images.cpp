// Omni Sampler: the disk image types, most specific probes first.
#include "../formats/format.hpp"
#include "fs.hpp"

namespace omni {

void register_images() {
    register_cue();
    register_floppy_containers();
    register_roland_images();
    register_emu_images();
    register_ensoniq_images();
    register_iso9660();
    register_omnisphere_pkg();   // 2.4.9
    // 2.4.10: ZIP archives are mountable folders.
    register_zip();
    register_fat();
    register_akai_images();
}

}  // namespace omni

// Omni Sampler 2.4.39 / public 2.3.1: the MV-8800 effects in FX BANK slot A (PUBLIC build only; the private build keeps
// them in slot B). The user (2026-11-24): append them to bank A, light ones first so the numbers match on every MPC:
// A 118.. = the effects that run on the Live II (32-bit ARM), then the heavy ones, which the 32-bit builds list but play dry
// ("64-bit MPC only": over 2.5 % of a Live II core, reports/bench-2437/live2-mv-b.tsv). design.py reads MV_HEAVY_ARMV7 from
// this file for the popup's order; fx_expansion.cpp and plugin.cpp use the same lists.
#pragma once
#include <cstring>

namespace omni {

// the 13 effects over the Live II's 2.5 % gate (best of two runs, after the 2.4.37 CPU pass)
constexpr const char *MV_HEAVY_ARMV7[] = {"2Vo Pitch", "Bass Multi", "EP Multi", "Fbk Pitch", "Graphic EQ", "Gtr Amp Sim",
                                          "Hexa-Chorus", "Humanizer", "Kbd Multi", "MultiTap Dly", "Reverse Dly", "Spectrum",
                                          "Super Filter"};
constexpr int MV_HEAVY_ARMV7_COUNT = int(sizeof(MV_HEAVY_ARMV7) / sizeof(MV_HEAVY_ARMV7[0]));
inline bool mv_heavy_armv7(const char *name) {
    for (const char *h : MV_HEAVY_ARMV7) if (!std::strcmp(h, name)) return true;
    return false;
}
// bank A's MV programs are catalog numbers from MV_A_BASE + 1 (the factory catalog is far below it)
constexpr int MV_A_BASE = 1000;

}  // namespace omni

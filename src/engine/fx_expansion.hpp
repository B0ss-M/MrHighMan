// Omni Sampler 2.4.33: FX BANK expansion slots. The bay on the FX BANK page has four slots: A = the factory bank
// (omni_fx_bank.hpp, built in), B / C / D = expansion banks of up to 999 programs each (2.4.35: was 256): the user's
// C++ algorithms. 2.4.35: B = Roland hardware (236), C = other hardware (186) (hardware_fx.hpp), D = Camel (891, camel_fx.hpp).
//
// An expansion bank implements FxExpansion and is created by make_fx_expansion() (fx_expansion.cpp) for its slot.
// Rules for a bank (the audio thread calls everything but the constructor and prepare()):
//   - prepare() allocates everything; select / set / process / reset never allocate, lock or do I/O
//   - process() works in place on the mix (l, r, frames <= max_block), stereo, at the sample rate given to prepare()
//   - select() takes 1..count() and clears the tails (no sound from the program before); set() is called only when a
//     control changes, so per-block work stays in process()
//   - output must stay finite (the mix is checked once per block; a NaN / inf block is muted and every FX state cleared)
//   - CPU: one program runs at a time; each program must stay under 2.5 % of a Live II core (device bench before shipping)
#pragma once
#include <memory>
#include "omni_fx_bank.hpp"   // omnisampler::fxbank::Controls: the page's 13 controls, shared by every slot

namespace omni {

struct FxExpansion {
    static constexpr int MAX_PROGRAMS = 999;   // 2.4.35: was 256
    virtual ~FxExpansion() = default;
    virtual const char *title() const = 0;              // the bank's name, on the OLED's second line
    virtual int count() const = 0;                      // 1 .. MAX_PROGRAMS
    virtual const char *name(int program) const = 0;    // program 1 .. count(); short (the OLED shows about 24 characters)
    virtual void prepare(double sample_rate, int max_block) = 0;
    virtual void select(int program) = 0;               // 1 .. count(); resets the tails
    virtual void set(const omnisampler::fxbank::Controls &c) = 0;
    virtual void process(float *l, float *r, int frames) = 0;
    virtual void reset() = 0;
    // 2.4.35 (optional; audio thread): the song tempo before each process(), and a note played (key-synced effects)
    virtual void tempo(double bpm) { (void)bpm; }
    virtual void note_on() {}
};

using FxExpansionFactory = std::unique_ptr<FxExpansion> (*)(int slot);   // slot: 1 = B, 2 = C, 3 = D
// the bank in a slot, or nullptr: an empty slot (the bay shows it EMPTY, the sound passes dry)
std::unique_ptr<FxExpansion> make_fx_expansion(int slot);
// 2.4.35: all three slots at once (B and C share one hardware rack: only one slot plays at a time). out[0..2] = B..D
void make_fx_expansions(std::unique_ptr<FxExpansion> out[3]);
// 2.4.39 / public 2.3.1: the MV-8800 effects as FX BANK slot A's programs after the factory ones (the PUBLIC build: the
// sampler creates it only with OMNI_PUBLIC; private keeps them in slot B). Light ones first (mv8800_bank_a.hpp); on 32-bit
// ARM the heavy ones are listed but play dry
std::unique_ptr<FxExpansion> make_mv_bank_a();
// tests only: another factory for the Sampler instances created after it (nullptr = the built-in one)
void set_fx_expansion_factory(FxExpansionFactory f);

}  // namespace omni

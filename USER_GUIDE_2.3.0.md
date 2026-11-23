# Omni Sampler 2.3.0 – User Guide

Omni Sampler is a multi-format sampler and synth for the MPC standalone hardware, by MR HighMan. It plays instruments
from hardware samplers, sample libraries, hardware ROMs and software-synth presets, straight from their files or the
disk images they came on.

---

## What's new in 2.3.0

Everything below is new compared with the public 1.8 releases.

### A built-in synth (SYNTH page)
- **Two oscillators, OSC A and OSC B**, each Classic (analog waves), Wavetable, Sample or Macro, with up to 8 unison
  voices, detune and stereo spread.
- **Warp modes:** Bend, PWM, Mirror, Sync and FM (from the other oscillator).
- **SUB and NOISE oscillators**, plus FM, ring modulation and feedback between the oscillators.
- **Filter:** LP 12, LP 24, Ladder, BP, HP, Notch and Comb, with drive, key tracking and its own envelope.
- **Modulation:**
  - an amp envelope, a filter envelope and three mod envelopes;
  - four LFOs (Sine, Tri, Saw, Square, S&H, Random, or a Custom drawn shape) with free, tempo-synced and one-shot modes;
  - a **16-route MATRIX**, with new **Level A / Level B** destinations to fade each oscillator on its own;
  - four MACRO knobs.
- **Voice:** Poly, Mono or Legato, glide, and a pitch bend range of up to 24 semitones.
- **Synth presets play on it:** Xfer Serum 2, Vital, LUXONIX Purity and Initial Audio Heat Up 3 presets are translated
  onto the SYNTH, including their wavetables, samples, envelopes, LFO shapes and modulation.

### ARP page
- **Seq and Arp modes**, a step grid, and latch.
- **MIDI-file phrases:** put `.mid` files in the **MIDI** folder of your Plugin Library and they appear on the ARP page.

### FX page and FX BANK
- **FX page:** EQ, compressor, reverb, drive, chorus, delay and modulation effects.
- **FX BANK:** 117 factory effect programs (reverbs, delays, choruses, flangers, phasers, distortion, EQ and more), with
  a 3-digit keypad, program stepping and a DRY/WET fader. Slots B to D are empty in this release.

### New formats
Serum 2, Vital, LUXONIX Purity (waveforms, user banks and **`.pth` patch banks, including drum kits**), Heat Up 3,
Decent Sampler, Ableton Sampler/Simpler, DirectWave, discoDSP Bliss, Alesis drum ROMs, Roland JV-880 / SR-JV80 /
JV-1080 / XV-3080 and Roland Cloud JV-1080, Kurzweil K2000 ROM sets and `.krz` files, Ensoniq KT-76 and MR wave ROMs,
drum-machine ROM dumps, Omni ROM maps, and tracker modules. The full list is below.

### Saving and patches
- **SAVE TO LIBRARY** keeps a preset in your Plugin Library. See *Saving* below for what is stored.
- **Patches** (`.omnipatch`) save all four slots (A–D) with their ranges and the sound settings, and reopen in one tap.
- Saved sampler instruments also get an **MPC keygroup program (`.xpm`)** beside their samples, so MPC's own browser
  can load them without Omni.

### Playing and controls
- Every knob is reachable from the **Q-Links**, in banks per page.
- **Velocity curves:** Soft, Linear, Hard or Fixed.
- **Layers and keyswitches:** up to four instruments (slots A–D), layered or switched by key.

### Four packages
Separate packages for Hakai 32-bit, Hakai 64-bit, official MPC OS 32-bit and official MPC OS 64-bit (see *Installing*).

---

## Installing

| Package | For | How |
|---|---|---|
| `Omni-Sampler-2.3.0-mpc-armv7.zip` | modded 32-bit MPCs (Live, Live II, One, X, Key 61 / 37, Force) with SSH root access | unzip, copy the `Omni-Sampler-2.3.0` folder to the MPC, run `install.sh` (see its INSTALL.md) |
| `Omni-Sampler-2.3.0-mpc-aarch64.zip` | modded 64-bit MPCs with SSH root access | the same, with this package |
| `Omni-Sampler-2.3.0-public-official-32bit.zip` | official MPC OS 3.10.1 or later, 32-bit MPCs | copy the folder `MR HighMan - VST - Omni Sampler` into `VST2 Plugins` at the root of a USB drive, SD card or the internal drive |
| `Omni-Sampler-2.3.0-public-official-64bit.zip` | official MPC OS 3.10.1 or later, 64-bit MPCs (Live III, XL and later) | the same, with this package |

**On the official firmware:** turn on Preferences › Enhancements › Enable Enhancements and External Plugin Support.
Omni Sampler is then listed under External Plugins.

**Before you install:**
- Keep **one** copy of Omni Sampler per MPC. Remove older copies from every drive first, otherwise the MPC may load the
  wrong one.
- Save your project first: the Hakai installer restarts MPC.

**Your existing projects:** projects made with Omni Sampler 1.8 open in 2.3.0 (same plugin and parameter order).

---

## First steps

1. Put your sounds on an **SD card, USB drive or SSD**: sample libraries, disk images, ROM dumps and presets.
2. Add Omni Sampler to a track and open the **BROWSE** page.
3. Open the folder that holds your sounds and tap **SET LIBRARY HERE**. Omni remembers this Plugin Library for every
   project. It uses its `Extracted`, `Patches` and `MIDI` folders, and finds shared ROMs and wavetables there.
4. Tap a file to list its presets, then tap a preset to load it into the **Target Slot** (A–D, on the SETUP page).
   Disk images open like folders.

ROM dumps, sample libraries and synth presets are **not** included with Omni Sampler. Use your own legally obtained
copies. Keep each set's files together: a ROM set finds its other chips, and a patch bank finds its ROMs or waveforms,
in the same folder or the folders around it.

---

## Supported formats

| Family | Files |
|---|---|
| Akai | S612 / S900 / S1000 / S3000 programs and samples, S5000 / S6000 / Z4 / Z8 `.akp` / `.akm`, MPC `.xpm` / `.pgm` / `.snd` and projects; Akai hard-disk, CD and floppy images |
| E-mu | EOS / E4 `.e4b`, Emulator III, Emax / Emax II, Emulator I / II, Emulator X `.exb`; EOS and EIII CD images |
| Native Instruments | Kontakt `.nki` / `.nkm` (all generations), NCW samples, Maschine sounds, kits and projects, Battery kits |
| Roland | S-700 series volumes, performances and patches, S-50 / S-550 / S-330 / W-30 disk images, MV-8000 / MV-8800 patches; JV-880 / SR-JV80 boards, JV-1080 (ROM and SysEx), XV-3080 and Roland Cloud JV-1080 |
| Korg | Trinity / Triton / Karma `.kmp` + `.ksf`, TRITON content dumps, TRINITY `.pcg`, M1 / M1R / M1EX and Wavestation SysEx (played from their ROM dumps) |
| Ensoniq | EPS / EPS-16+ / ASR-10 / ASR-88 instruments and disk images, SD-1 / VFX-SD SysEx (from the SD-1 wave ROM), SQ-1, KT-76 and MR wave ROMs |
| Kurzweil | K2000 / K2000R ROM sets, `.krz` files |
| Yamaha | Motif XS / XF, MOXF, Montage, Montage M and MODX libraries; Motif ES waveforms |
| LUXONIX Purity | waveforms `.wvt`, user banks `.usr`, patch banks `.pth` (melody patches and drum kits; keep the `.wvt` files beside them) |
| Synth presets | Xfer Serum 2 `.SerumPreset` (with Serum's Tables / Samples / Multisamples folders beside or above), Vital `.vital`, Initial Audio Heat Up 3 `.heatUpPreset` |
| Sample players | Decent Sampler `.dspreset` / `.dslibrary`, Ableton Sampler / Simpler `.adv` / `.adg`, DirectWave `.dwp`, discoDSP Bliss `.zbp` / `.zbb` |
| Apple Logic, Reason | EXS24 `.exs`, NN-XT `.sxt` and ReFills |
| Drum machines | sound-chip dumps (LinnDrum, Linn 9000, Oberheim DMX / DX, Roland TR-505 / 626 / 707, Alesis HR-16 / HR-16:B / SR-16, Yamaha PSR-60 / 70, Casio RZ-1, Sequential Drumtraks, E-mu Drumulator, Simmons SDS and third-party EPROMs), Omni ROM maps `.omnirom` |
| Trackers | FastTracker 2 `.xi` / `.xm`, Impulse Tracker `.iti` / `.its` / `.it`, Scream Tracker 3 `.s3m`, ProTracker `.mod` |
| Open formats | SoundFont 2 `.sf2` and 1 `.sbk`, SFZ `.sfz`; samples in WAV, AIFF, CAF, FLAC and Ogg Vorbis; single-cycle waves and wavetables (Serum / Surge / WaveEdit WAVs, `.wt`) |
| Disk images | ISO 9660, CUE / BIN, FAT12/16/32 (floppies, cards, ZIP disks), Akai and E-mu file systems, HFE / IMD / Teledisk floppies, Nero `.nrg`; images inside images |

---

## MIDI

| MIDI message | What Omni does |
|---|---|
| Notes and velocity | play the loaded slots (or the ARP when it is on). Velocity follows the **Velocity Curve** (Soft / Linear / Hard / Fixed) |
| Pitch bend | bends by **Bend Range** (sampler) and **Synth Bend Range** (SYNTH), up to ±24 semitones |
| CC 1 Mod Wheel | a modulation source on the MOD page and in the SYNTH MATRIX |
| Channel and poly aftertouch | modulation sources on the MOD page and in the SYNTH MATRIX |
| CC 7 Volume, CC 10 Pan, CC 11 Expression | level, pan and expression |
| CC 64 Sustain | holds notes while the pedal is down |
| CC 120 All Sound Off | silences everything at once and resets the ARP |
| CC 121 Reset Controllers | resets bend, mod wheel, aftertouch, volume, pan and expression |
| CC 123 to 127 | all notes off |
| Program Change | with **Program Change** on (SETUP), loads preset number *n* of the current file into the Target Slot |

**Layers and keyswitches:**
- **Layer** mode (SETUP) plays slots A–D together, each within its own key and velocity range (LAYERS page).
- **Keyswitch** mode picks the playing slot with the four keys from **Keyswitch Base** upward (C1 = note 24 by default).
  Those keys make no sound.

**ARP:**
- With the ARP on, it takes the notes you play and plays its pattern or MIDI phrase, following the MPC's tempo.
- Phrases come from `.mid` files in the Plugin Library's `MIDI` folder.

**Automation:** every parameter can be automated and mapped from the MPC, by name.

---

## OSC: the SYNTH's oscillators

OSC A and OSC B are the SYNTH's two oscillators. Turn the SYNTH on with the SYNTH page's power switch. It plays on its
own or together with the sampler slots.

- **Type:**
  - **Classic:** sine, triangle, saw, square, pulse and other analog-style waves.
  - **Wavetable:** a factory table or your own single-cycle waves and wavetables; POSITION scans through it.
  - **Sample:** plays a sample or a loaded instrument as an oscillator.
  - **Macro:** a complete sound engine in one oscillator.
- **Pitch:** octave, semitone and fine tune.
- **Unison:** up to 8 voices, with DETUNE and SPREAD.
- **Warp:** Bend, PWM, Mirror, Sync, or FM from the other oscillator, with an amount.
- **Level and pan** per oscillator, and the **A/B balance**.
- **SUB:** sine, triangle or square, one or two octaves down. **NOISE:** white, pink or crackle, with tone.
- **FM, RING and FEEDBACK** between the oscillators.
- **Modulating the oscillators:** in the MATRIX, route any source (LFOs, envelopes, velocity, note, mod wheel,
  aftertouch, macros, random) to wavetable position, warp, detune, spread, pitch, or **Level A / Level B**.

When you load a Serum, Vital, Purity or Heat Up preset, Omni sets these oscillators up for you. The info line says
what was translated and what was approximated.

*Note: Omni Sampler does not use the Open Sound Control (OSC) network protocol. It is controlled by MIDI and the MPC.*

---

## Saving

| Preset kind | SAVE TO LIBRARY / patches store |
|---|---|
| Sampler presets whose samples are their own (Akai, E-mu, Roland, Korg, Ensoniq and Yamaha disks, SoundFonts, SFZ, WAV, Ableton, DirectWave, Bliss, Alesis, trackers, Kurzweil `.krz`) | an Omni instrument (`.omni`) **with its samples**, plus an MPC keygroup program: a project keeps working without the original disk |
| Presets that play from ROMs, `.bin` files or a synth's library (ROM sets, drum-machine ROMs, JV / XV, K2000 ROMs, Korg M1 / Trinity / Triton / Wavestation, Ensoniq SD-1 / SQ-1 / KT / MR, Serum, Vital, Purity, Heat Up), and Kontakt, Logic, Reason and Decent Sampler presets | a small **reference** (`.omni`): the settings and where the sounds are. **Keep those files where they are**: the reference loads them from there |

- **Auto Extract** (SETUP): a preset from inside a disk image is kept in `Extracted` automatically, once. Presets that
  save a reference are not extracted.
- **Projects** store each slot's file and the sound settings. A slot from a ROM or a synth preset needs that file on
  the drive when the project opens.

---

## Important information

- **Memory:** the Memory Limit (SETUP) caps how much audio Omni holds for its slots. Raise it for large multisamples;
  lower it if the MPC runs short.
- **CPU:**
  - Sampler instruments are light: a 16-voice drum kit used about 13 % of one core on an MPC Live II and 3 % on an MPC
    Live III (see `tested.json`).
  - Dense synth patches are heavy on 32-bit MPCs: a big Vital patch on the SYNTH used up to about 60 % of a core on a
    Live II (15 % on a Live III). On 32-bit MPCs, play one such instance at a time, or lower its voices or unison.
  - If you hear dropouts, lower the polyphony or unison, or use a lighter effect.
- **Translated synth presets are approximations.** Serum's effects racks, some warp modes and some modulation are not
  played. The info line lists what was left out.
- **ROMs and libraries are not included.** Use your own legally obtained dumps and libraries.
- **One install per MPC**, and keep the folder name `MR HighMan - VST - Omni Sampler` unchanged.
- **Licence:** see `LICENSE.txt` and `NOTICE.txt` in the package.
  - Omni Sampler's own code may be installed and used on your own MPC, and customised for your own use.
  - It may not be copied, redistributed, ported to other platforms, or used to train AI models without permission.
  - The open-source parts keep their own licences, listed in `NOTICE.txt`.
- **Support:** mrhighmanproductions@gmail.com (bug reports and format requests welcome).

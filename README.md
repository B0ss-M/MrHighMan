# Omni Sampler 2.3

A multi-format sampler and synth for the MPC standalone hardware, by MR HighMan. It plays instruments from
hardware samplers, sample libraries, hardware ROMs and software-synth presets, straight from their files or the
disk images they came on, through its own engine. Nothing is converted on disk unless you save it.

Licence: see LICENSE.txt (Omni Sampler's own code) and NOTICE.txt (the open-source parts and their licences).
Contact: mrhighmanproductions@gmail.com (bug reports and format requests welcome).

## Packages

| Package | For |
|---|---|
| `Omni-Sampler-2.3.1-mpc-armv7.zip` (Hakai 32-bit) | modded 32-bit MPCs (Live, Live II, One, X, Key 61 / 37, Force) with SSH root access: run its `install.sh` |
| `Omni-Sampler-2.3.1-mpc-aarch64.zip` (Hakai 64-bit) | modded 64-bit MPCs with SSH root access: run its `install.sh` |
| `Omni-Sampler-2.3.1-public-official-32bit.zip` | official MPC OS 3.10.1+ on 32-bit MPCs: copy the folder into `VST2 Plugins` |
| `Omni-Sampler-2.3.1-public-official-64bit.zip` | official MPC OS 3.10.1+ on 64-bit MPCs (Live III, XL and later): copy the folder into `VST2 Plugins` |

On the official firmware, use the `VST2 Plugins` folder: a copy installed anywhere else is not listed in the plugin menu.

Each package has an INSTALL file. Tested on: see `tested.json` in the release (devices, firmware, builds, benchmark).

## Formats

| Family | Files |
|---|---|
| Akai | S612 / S900 / S1000 / S3000 programs and samples, S5000 / S6000 / Z4 / Z8 `.akp` / `.akm`, MPC `.xpm` / `.pgm` / `.snd` / projects; Akai hard-disk, CD and floppy images |
| E-mu | EOS / E4 `.e4b`, Emulator III, Emax / Emax II, Emulator I / II, Emulator X; EOS and EIII CD images |
| Native Instruments | Kontakt `.nki` / `.nkm` (all generations), NCW samples, Maschine sounds / kits / projects, Battery kits |
| Roland | S-700 series, S-50 / S-550 / S-330 / W-30 images, MV-8000 / MV-8800 patches; JV-880 / SR-JV80 boards, JV-1080, XV-3080 and Roland Cloud JV-1080 ROMs and SysEx |
| Korg | Trinity / Triton / Karma `.kmp` + `.ksf`, TRITON content dumps, TRINITY `.pcg`, M1 / M1R / M1EX and Wavestation SysEx (played from their ROM dumps) |
| Ensoniq | EPS / EPS-16+ / ASR-10 / ASR-88 instruments and disk images, SD-1 / VFX-SD SysEx, SQ-1, KT-76 and MR wave ROMs |
| Kurzweil | K2000 / K2000R ROM sets and `.krz` files |
| Yamaha | Motif XS / XF, MOXF, Montage, Montage M, MODX libraries; Motif ES waveforms |
| Drum machines | sound-chip dumps (LinnDrum, Oberheim DMX, Roland TR-505 / 626 / 707, Alesis HR-16 / SR-16, Casio RZ-1, E-mu Drumulator and more), Omni ROM maps `.omnirom` |
| Synth presets | Xfer Serum 2 `.SerumPreset`, Vital `.vital`, LUXONIX Purity, Initial Audio Heat Up 3: played on Omni's SYNTH |
| Sample players | Decent Sampler `.dspreset` / `.dslibrary` / `.dsbundle`, Ableton Sampler / Simpler `.adv` / `.adg`, DirectWave `.dwp`, discoDSP Bliss `.zbp` / `.zbb` |
| Apple Logic, Reason | EXS24 `.exs`, NN-XT `.sxt` and ReFills |
| Trackers | FastTracker 2, Impulse Tracker, Scream Tracker 3 and ProTracker instruments and modules |
| Open formats | SoundFont 2 / 1, SFZ; WAV, AIFF, CAF, FLAC, Ogg Vorbis; single-cycle waves and wavetables |
| Disk images | ISO 9660, CUE / BIN, FAT12/16/32, Akai and E-mu file systems, HFE / IMD / Teledisk floppies, Nero `.nrg` |

ROM dumps, sample libraries and synth presets are not part of Omni Sampler: use your own.

## Saving

- A preset whose samples are its own (sample libraries, sampler disks, SoundFonts, SFZ, WAV, tracker modules)
  saves as an Omni instrument with its samples, so a project keeps working without the original disk.
- A preset that plays from ROMs, `.bin` files or a synth's own library (ROM sets, drum machine ROMs, Serum, Vital,
  Purity, Heat Up), and Kontakt, Logic, Reason and Decent Sampler presets (their samples are separate files they
  point to), saves a small reference instead: its settings and where its sounds are. Keep those files where they are.

## FX BANK

The FX BANK page carries the factory bank (slot A): 117 programs, plus 30 MV programs (118-147) since 2.3.1. On
32-bit MPCs, the 13 heaviest MV programs (2-Voice Pitch to Super Filter) are marked "(64-bit MPC only)" and pass the
sound through dry. Every knob changes the sound: the knobs a program does not use act on the program as a whole
(drive, EQ, tone, dynamics, tremolo, echo, pitch). At their defaults they change nothing. Slots B to D are empty in this
release.

## What's new in 2.3.1

- ARP: 128 Omni Style patterns in GENERATE that follow your key, your scale and a loaded MIDI file; VOICE and INVERT
  now change ARP patterns and melodies.
- FX BANK: 30 MV programs in bank A, and every knob now changes the sound.
- MPC Live III: the internal drive is listed as Internal on the BROWSE page (open, set as library, save).
- Decent Sampler: fuller mapping (knob settings, effects, modulators, oscillators, buses); sealed `.dsproduct` files
  are listed and not opened.

See USER_GUIDE_2.3.1 for details.

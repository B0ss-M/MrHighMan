# Omni Sampler

A multi-format sampler instrument for the MPC OS standalone hardware (VST2, armv7), by MR HighMan. It loads
instruments from Akai, E-mu and Native Instruments samplers, SoundFonts and SFZ, straight from their files or from
the disk images they shipped on (sampler CDs, floppies, hard disks, CF/ZIP dumps), and plays them through its own
engine. Nothing is converted on disk.

Requires a modded unit with root access (the installer edits `MPC.settings`). Tested on an MPC Live (Gen1, armv7).

## Formats

| Family | Files |
|---|---|
| Akai S900 / S1000 / S3000 | `.p .p1 .p3 .s .s1 .s3 .p9 .s9 .s9c`, MESA `.s3p`; Akai hard-disk, CD and floppy images |
| Akai S5000 / S6000 / Z4 / Z8 | `.akp`, `.akm` |
| Akai MPC | `.xpm` (XML, MPC 2/3 JSON, and the `<Program>`/`<Keygroups>` layout preset generators write), `.xpj` / `.xty` projects, MPC1000 `.pgm`, MPC2000/3000 `.pgm` / `.snd`, MPC60 `.set` |
| E-mu | EOS / E4 `.e4b`, Emulator III `.e3b .e3x .esi`, Emax / Emax II, Emulator I / II, Emulator X `.exb` + `.ebl`; EOS and EIII CD images |
| Native Instruments | Kontakt `.nki` / `.nkm` (Kontakt 1, 2-4.1, 4.2, 5-8 and both monolith kinds), NCW samples, Maschine 1 `.msnd` / `.mgrp`, Maschine 2/3 sounds `.mxsnd`, kits (groups) `.mxgrp` and projects `.mxprj`, Battery 4 kits `.nbkt` (with the library's `Samples` folder) |
| Roland | S-700 series (S-750 / S-760 / S-770, SP-700, DJ-70) CD-ROM, hard-disk and floppy images: volumes (banks), performances and patches; S-50 / S-550 / S-330 / W-30 floppy images and S-500 "LAND" CD-ROMs; MV-8000 / MV-8800 patches `.MV0` |
| Reason | NN-XT patches `.sxt`, with their samples as files next to the patch (or in a folder named after it) |
| Open formats | SoundFont 2 `.sf2`, SFZ `.sfz`; samples in WAV (PCM, float, ADPCM, RF64), AIFF/AIFC, FLAC, Ogg Vorbis |
| Synth waves | single-cycle waveforms (a WAV of 256, 512, 600, 1024, 2048 or 4096 frames) and wavetables (WAV with a Serum `clm ` or Surge `srge` chunk) |
| Disk images | ISO 9660 (Joliet, Rock Ridge; `.iso`, raw `.bin`), FAT12/16/32 (floppies, partitioned cards and ZIP disks), Akai and E-mu sampler file systems, HFE and IMD floppy images, Nero `.nrg` CD images. Images open like folders, also nested (an Akai CD image on a FAT card) |

MV-8000 / MV-8800 patches (`.MV0`) load straight from the file, samples and all, stereo samples included. Drum kits
(and percussion) start on the MV's pad 1 (A0); they are moved up so pad 1 plays on MPC's pad 1 (C1, note 36) and a
second pad bank on MPC's bank B. Checked against the 103 MV-8000 factory patches and a third-party kit. The filter
follows ConvertWithMoss's reading of the MV's TVF; some patches (e.g. PNO_Grand Piano, a low-pass at cutoff 0 opened by
its envelope) may come out darker than on the MV.

Roland images mount like folders: S-700 volumes (banks) hold their patches and a Performances folder, "All Patches"
lists every patch; S-500 disks list their patches (P11-P28), a LAND CD one folder per disk. Multi-floppy S-700 sets
are not read yet. S-500 patches play with their loop tune (the per-tone correction for short loops). Checked
against a real S-50 CD (L-CD1, all disks); the S-700 readers follow ConvertWithMoss's notes and were checked with
generated images only: please report how real S-700 discs load.

Maschine kits (`.mxgrp`, Maschine 1 `.mgrp`) load as a drum program: each sound on its pad, pad 1 on MPC's pad 1 (C1,
note 36), every pad its own group. A project (`.mxprj`) plays its first group the same way. Sounds made with a drum
synth or a plugin have no samples and stay silent (their pads stay where they were in Maschine); a kit of only those
says so. NI Massive presets (`.nmsv`) are synth patches, not samples, and are reported as such. Checked against the
Astral Flutter library (kits, projects, sounds).

**Previews**: tapping a kit or sound that has a preview (NI's `.previews/<file>.ogg` next to Maschine files, or
`previews/<file>.ogg` next to Battery kits) plays it at once, so you hear the kit before (and while) it loads, even
when its samples are missing. Another tap, PANIC or **PREVIEW OFF** (BROWSE) stops it.

Not possible: encrypted commercial Kontakt libraries (Kontakt Player / NKS protected) and Reason ReFills (`.rfl`),
which Propellerhead encrypts so only Reason opens them. Both are detected and reported as such. An NN-XT patch whose
samples come from a ReFill plays when those samples also exist as files (e.g. exported from Reason) next to it.

Checked so far against test files made with ConvertWithMoss, real Kontakt 6.8 and Maschine files and generated
disk images: SFZ, SF2, XPM, Akai S1000/S3000 CD images, Akai S6000 AKP programs (101 from a real CD), E4B and EOS CDs, Emulator III / X, Emax / Emax II,
Emulator I / II floppies, Kontakt 1 and 5+, Maschine 1-3 (sounds, kits, projects), ISO and FAT images. Written but not yet checked against
real files: S900, AKM, Battery 4 kits (parsed; their library samples not at hand), MPC JSON programs and projects, MPC1000/2000/60, MESA, Kontakt 2-4 and monoliths, encrypted-library detection, Akai
floppies, IMD. Reports and sample files welcome.

## Single cycles and wavetables

A WAV that is one cycle of a waveform (256-4096 frames, no loop) loads as an oscillator: it loops the cycle at each key's
pitch, split by octaves into band-limited copies (the harmonics a key's pitch would fold back above half the sample rate
are removed), so high notes stay clean. It gets a synth envelope (attack 3 ms, decay 1 s, sustain full, release 0.25 s)
for the PLAY page's ADSR faders to shape, and everything else (filter, drive, LFOs, the MOD matrix, layers, patches)
works as for any instrument. A wavetable (a WAV whose Serum `clm ` or Surge `srge` chunk gives its cycle length) plays
its cycles with **WT POSITION** (SETUP, a Q-Link) crossfading between them; **WT Position** is also a MOD matrix
destination, so an LFO, the envelope, velocity or the mod wheel can sweep it. SAVE TO LIBRARY and SAVE PATCH keep it.

## Pages

**PANIC** (top right, on every page) stops every sound at once (a 5 ms fade) and clears whatever is still held: keys,
the sustain pedal, the skin's pads, bend and mod wheel. Use it if a note ever hangs. (1.6 also fixes a cause of hanging
notes: a note-off that arrived on another MIDI channel than its note-on was ignored, so a looped sound rang forever.)

- **PLAY**: the front panel. Top: instrument stepper and name, format. PITCH (transpose, bend range, fine tune),
  VOICE (glide, voices, drive), REVERB (mix, size, damping); filter type, voice mode and interpolation buttons; the
  display shows the zone the last note played (name, root, keys, velocity range, hit velocity) and the instrument;
  volume, pan, status; FILTER (cutoff, resonance), ENVELOPE faders, velocity sensitivity and filter velocity.
  Along the bottom, 16 pads play notes 36-51 (MPC bank A layout; PAD BANK shifts them by 16) at the pad
  velocity (small fader), and light while their note plays, also from MPC's own pads. The ENVELOPE faders show the
  loaded preset's own envelope (attack, decay and release in ms / s, sustain in %, from the zone that plays middle C) and
  play it as set: moved, every zone of the instrument changes by as much, so multisamples keep their differences.
  Loading another preset shows its envelope; a project, patch or saved instrument keeps the one you set.
  The display also draws the loaded sound: the waveform of the zone last played, and a keyboard strip showing which
  keys each layer covers (lit while a key plays).
- **LAYERS**: up to four instruments (slots A-D) at once. Load into a slot by choosing it on BROWSE (SLOT A-D)
  before tapping a preset. Each slot has a key range (low / high), **key shift**, volume, transpose and mute; the
  keyboard map shows the ranges in the slot colours. **Key Shift** (-48..+48 keys) moves the slot's whole instrument
  up or down the keyboard without changing its pitch: a kit that starts on C1 at -12 starts on C0, leaving C1 and up
  for a bass in another slot. The slot's key range moves with it (an open end, Low at C-2 or High at G8, stays open). **Layer Mode** Layer plays every slot whose range holds the key; Keyswitch plays only
  the slot last selected with the four keyswitch notes from **Keyswitch Base** (default C0, 24-27). **AUTO SPLIT**
  divides the keyboard evenly between the loaded slots; **CLEAR SLOT** empties the selected slot. **SAVE PATCH** keeps
  the whole setup (see Patches below).
- **BROWSE**: drives (Plugin Library, Internal, every USB/SD volume under `/media`), folders, files and disk
  images. The list scrolls one row at a time with the **▲ / ▼ buttons** beside it or **Q-Link 1** (one row per click),
  and a page (ten rows) at a time with the arrows below it. **SELECTED** shows the full name of what was tapped (a
  long folder or file name wraps over up to three lines; the list rows shorten long names in the middle). Tap a file with several instruments (an SF2 bank, a multi, a disk partition) to list them. The arrows
  next to the instrument name step through the presets of a file, or the files of a folder.
- **SETUP**: polyphony, voice mode (poly / mono / legato) and glide, bend range ("Inst" = the instrument's own),
  volume, pan, transpose, fine tune, velocity sensitivity, filter velocity, interpolation, reverb damping,
  memory limit, pad bank note and MIDI program change (selects the n-th preset of the loaded file). **Auto Loop**
  finds a sustain loop for samples that sustain but have none (a zero-crossing match near the end of the sample;
  drums, one-shots and decaying sounds are left alone); applies to the next load.
- **MOD**: two LFOs (sine, triangle, saw up / down, square, sample & hold; rate 0.02-20 Hz or synced to MPC's tempo,
  4 bars to 1/16 with triplets and dotted; free-running or restarted by every note) and an 8-slot modulation matrix:
  source (LFO 1/2, mod wheel, aftertouch, pitch bend, velocity, key, per-note random, amp envelope) to destination
  (pitch +-12 semitones, cutoff +-4 octaves, resonance, volume (silent at -100%, +6 dB at +100%), pan, sample start,
  drive, reverb mix, LFO 1/2 rate +-3 octaves), amount -100..100%. The slots come routed (LFO 1 to pitch, LFO 2 to
  cutoff, mod wheel to cutoff, ...) at amount 0: turn an amount up to use one. Drive, reverb mix and LFO rates follow
  only the global sources (LFOs, wheel, aftertouch, bend).
- **INFO**: every supported format with its file extensions, on two pages (PG 1 / PG 2 in the title bar); a green dot
  marks formats checked against test files, an amber one formats supported but not yet tested on real files, a red one
  formats that are recognised but encrypted.

Q-Links follow the screen sections: each Q-Link bank (the Q-Link button on 4-knob MPCs) is one section of the
page, in this order. PLAY: envelope (attack, decay, sustain, release) / filter (cutoff, resonance, filter velocity,
velocity) / pitch (transpose, bend, fine tune, glide) / drive, reverb mix, volume, pan. SETUP: voice / output /
playback / effects and system. MOD: amounts 1-4 / amounts 5-8 / LFO 1 / LFO 2. LAYERS: one bank per slot A-D (low
key, high key, volume, tune).

## Disk images, extraction and projects

Presets inside a disk image play straight from it: tap the image, open a bank (folder), tap a patch, play. With
**Auto Extract** on (BROWSE, default Disk Images) the plugin then writes that one preset into the Plugin Library,
`Extracted/<image>/<preset>.omni` with its samples in `<preset> Samples/`, and the loaded instrument refers to that
file. A project saves only that, so reopening it loads the one preset without the image (which may be gone). The
instrument stepper and MIDI program changes still walk the image's presets while the image is there; a preset
extracted before is reused, not written again. **SAVE TO LIBRARY** does the same for anything loaded (e.g. one
preset of a big SF2 bank) and also stores the current sound settings (envelope, filter, pitch, MOD page, ...) in
the file: loading that instrument later brings them back, so a customised sound can be kept. Instruments extracted
by an older version from a format since read better (Roland S-500 loops before 1.4) are re-read from their image
when it is still there and rewritten in place, keeping their settings. `.omni` is the plugin's own lossless format (JSON: every zone, envelope, filter and LFO
setting; 16-bit WAV samples) and loads like any other instrument.

## Patches

**SAVE PATCH** (LAYERS) saves every loaded slot as one patch: `Patches/<A> + <B> + ....omnipatch` in the Plugin
Library, with the slots' key ranges, volume, tune and mute, the layer mode and keyswitch base, and the sound settings
(envelope, filter, pitch, MOD page, ...). Each press writes a new file (numbered when the name is taken). A slot that
is already an `.omni` file on a drive (an extracted or saved instrument) is referred to as it is; anything else (a
preset of an SF2 bank, a Kontakt file, a preset inside a disk image) is written as an `.omni` into
`Patches/<patch> Instruments/`, so the patch keeps working without its source. Paths inside the library are stored
relative, so the library can move to another drive. Tap a `.omnipatch` on BROWSE to load it: its instruments go into
their slots (slots it does not use are cleared) and its settings come back.

## Where to put sound files

Anywhere the browser reaches: the **Plugin Library**, the internal storage, or a USB stick / SD card. The Plugin
Library defaults to `/sdcard/vst/omni-sampler/library`; internal storage is small, so move it to an SD card or SSD:
browse to a folder there and tap **SET LIBRARY HERE** (BROWSE). Extracted instruments go to its `Extracted` folder.
If that drive is missing later the plugin falls back to the internal library. Keep sample folders next to the instrument files that use them (Kontakt and Maschine
paths are also searched two folders up, as libraries are laid out). Samples are loaded into memory up to the
**Memory Limit** (SETUP, default 512 MB); samples over it are skipped and reported.

## CPU (MPC Live, Gen1)

`tools/bench.sh` with a 61-zone Kontakt instrument: 16 voices 8% of an audio block (p99); with all 8 matrix slots in
use 12%. The Q-Link sweep stage, which also hits the pads (up to 16 pad notes on top of its 8-voice chords), reaches
19% p99 / 20% worst block: WARN, fine for one or two instances. A SoundFont with a filter, filter envelope and
vibrato on every voice costs about twice as much per voice; lower Polyphony on SETUP for more instances.

## Install

Download `Omni-Sampler-<version>-mpc-armv7.zip` from [Releases](../../releases), unzip it, copy the folder to the
MPC and run `sh install.sh` as root (it stops MPC, backs up `MPC.settings`, installs and restarts MPC). The zip's
`INSTALL.md` has the full steps, including a manual install. `uninstall.sh` removes it again.

## Build and test

The build uses the VST2 wrapper and skin tools of [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins):
check that out next to this repo and point `MPC_VST` at it.

```
git clone https://github.com/sd88me/mpc-vst-plugins
export MPC_VST=$PWD/mpc-vst-plugins
python3 design.py                       # params.json, layout.conf, skin.css, art/ (the skin design)
./build.sh                              # build/omni_sampler.so (armhf), build/skin/, pluginlist-entry.xml
"$MPC_VST/tools/test_port.sh" vst.json  # x86 host test (ASan)
tests/build_x86.sh                      # build/x86/probe, test_engine, test_patch (ASan; --fast for -O2)
build/x86/probe <file|image/path>       # list / dump what a reader makes of a file
build/x86/test_engine <file> [preset] [notes]  # load, play, browse, save/restore state
```

Without Docker, `build.sh` needs `arm-linux-gnueabihf-g++-12` and a Python with Playwright (`HTML_ART_PYTHON`).
A release zip is made with `$MPC_VST/tools/release.py` (see its `docs/RELEASING.md`). The PLAY page design is drawn
by `design.py` at MPC's 1280 x 628 plugin area. The title font is Michroma (OFL, `fonts/`).

## License

LGPL-3.0-or-later. The format readers are C++ translations of
[ConvertWithMoss](https://github.com/git-moss/ConvertWithMoss) by Jürgen Moßgraber (LGPL-3.0); keep this
attribution and the included [LGPL-3.0.txt](LGPL-3.0.txt) and [GPL-3.0.txt](GPL-3.0.txt) when distributing it.
Bundled libraries keep their own licences: dr_flac (public domain / MIT-0), stb_vorbis (public domain / MIT),
tinf (zlib). See [src/third_party/VENDORED.md](src/third_party/VENDORED.md).

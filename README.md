# Omni Sampler

A multi-format sampler instrument for the MPC OS standalone hardware (VST2, armv7), by MR HighMan. It loads
instruments from Akai, E-mu, Native Instruments, Roland, Ensoniq, Korg and Yamaha samplers, SoundFonts and SFZ, straight from their files or from
the disk images they shipped on (sampler CDs, floppies, hard disks, CF/ZIP dumps), and plays them through its own
engine. Nothing is converted on disk.

Requires a modded unit with root access (the installer edits `MPC.settings`). Tested on an MPC Live (Gen1, armv7).

Contact: mrhighmanproductions@gmail.com (bug reports and format requests welcome).

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
| Apple Logic | EXS24 / Sampler instruments `.exs` (both byte orders), with their samples (WAV, AIFF, CAF) found by name next to the instrument, around it or in the folders below it; consolidated instruments (one sample file for all zones) included |
| Ensoniq | EPS / EPS-16+ / ASR-10 / ASR-88 instruments `.efe`, and their disks: raw floppy and hard-disk images (`.img`), Giebler `.gkh`, `.eda`, `.ede`, HFE floppies |
| Korg | Trinity / Triton / Karma multisamples `.kmp` with their `.ksf` samples (8 and 16 bit), single samples `.ksf`, load scripts `.ksc`; TRITON content dumps (the ROM and EXB-PCM banks as FLAC + JSON cards): `programs/manifest.csv` shows as the folder **TRITON by Category** (the programs in the TRITON's 16 categories), `multisamples/manifest.csv` lists the multisamples; TRINITY program banks `.pcg` (KORG Collection TRINITY): the programs that play the TFD sample disks, by category; M1 / M1R / M1EX SysEx `.syx` (programs, combinations, drum kits), played from the M1's sample ROM dumps or Korg's M1 sample disk |
| Yamaha | Motif XS `.X0A .X0W .X0V`, Motif XF `.X3A .X3W .X3V`, MOXF `.X6A .X6W .X6V`, Montage `.X7U .X7L .X7A`, Montage M `.Y2L .Y2U`, MODX `.X8U .X8L .X8A` libraries: waveforms, and Montage / MODX performances; Motif ES waveforms `.W7A` + `.W8A` |
| Open formats | SoundFont 2 `.sf2`, SFZ `.sfz`; samples in WAV (PCM, float, ADPCM, RF64), AIFF/AIFC, CAF (linear PCM), FLAC, Ogg Vorbis |
| Synth waves | single-cycle waveforms (a WAV of 256, 512, 600, 1024, 2048 or 4096 frames) and wavetables (WAV with a Serum `clm ` or Surge `srge` chunk) |
| Disk images | ISO 9660 (Joliet, Rock Ridge; `.iso`, raw `.bin`), CUE sheets (`.cue` + its `.bin` files: a data CD opens as its file system, CD audio tracks as `Track NN.wav`; a renamed `.bin` is found by the `.cue`'s name), FAT12/16/32 (floppies, partitioned cards and ZIP disks), Akai and E-mu sampler file systems, HFE and IMD floppy images, Nero `.nrg` CD images. Images open like folders, also nested (an Akai CD image on a FAT card) |

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

Ensoniq disks open like folders: the disk's directories, each instrument as `<NAME>.efe`. An instrument lists one
preset per patch select (none, L, R, L+R on the ASR's buttons) that switches on a different set of layers; layers in the
key-up set play on note-off, copied wave samples share their source's audio, and loop modes (forward, backward,
back-and-forth, loop-until-release) play as set. Checked against the ASR-10 Essential Sound Disks (all 38 instruments
on the EDA set, and the IMG / GKH copies). Two notes from that set: AD-002.img / .GKH are damaged dumps (209 blocks
differ from the EDA; ASR FLUTE loses four of its five layers), so the plugin plays the layers that are there and says so;
and the sound follows the ASR10 Sampler's mapping, whose envelope-time and filter-cutoff curves are fitted by ear (the
OS's own tables are not decoded yet).

Korg KMP multisamples find their KSF samples next to them, in a folder named after them, in any folder beside them (the
M1 library's `00SAMPLE/`, PBS's `ACD_SAMP/`) or one folder up (sets split over floppies, `disk 1` ... `disk 7`). A
KSC script lists its multisamples as presets (a script of samples only maps them one per key from C1). Checked against
the Trinity / Triton content in the test library: all 561 KMP and 4165 KSF files load. Zones that use the keyboard's ROM
samples (`INTERNALnnnn`) are skipped and reported; program banks (`.pcg`) are not read yet. The 8-bit samples carry a
per-sample value (attribute bits 0-3, 0-5 in practice) whose meaning is not known yet; it is not applied.

A Korg TRITON content dump (a folder with `samples/`, `multisamples/` and `programs/`, each with a `manifest.csv`) opens
from its manifests: `programs/manifest.csv` shows as the folder **TRITON by Category**, one folder per TRITON category
(Keyboard, Strings, Bass, ..., from the dump's `program_catalog`) holding its programs. SAVE TO LIBRARY files a program
in `Extracted/TRITON/<category>/` and records the category in the `.omni`; Auto Extract leaves TRITON programs alone,
since they play straight from the dump. The view lists 1,604 of the 1,792 programs (INT-A to E, EXB-A to I; drum kits and empty
`InitlProgram` slots are left out) and `multisamples/manifest.csv` the 1,406 multisamples. A program brings its
oscillators (single or double), each oscillator's high / low multisample split by velocity, velocity zones, octave /
transpose / tune, levels, pan, amp envelope, velocity sensitivity and bend range; the filter, LFOs and effects are left
to the sampler's own. The dump's FLACs are 48 kHz audio labelled 44.1 kHz and play at 48 kHz, its zone `tune_cents` is
not applied (the samples already sound at their root key), and its program `oscillator_mode` is not used: the real mode
of 1,720 programs was taken from the matching records in the TRITON `.pcg` banks. Drum kits need the TRITON's drum
sample table, which the dump doesn't have. EG times use a fitted curve (0 = instant, 50 = 0.23 s, 99 = 17 s).

A Korg TRINITY program bank (`.PCG`, as KORG Collection TRINITY keeps them in `Presets/PCGData/`) shows as a folder of
the TRINITY's 16 categories. It lists the programs whose oscillators play a TFD sample disk (1.8): the disk's
`Resource/PCM/KSC/<bank name>/Disk1/ALL_INST.KSC` loads them, with the KMP and KSF files spread over its `Disk1`-`Disk6`
folders. Each oscillator's multisample is mapped (a stereo pair panned left / right, both oscillators in double mode);
the program's envelopes, filter, tuning and velocity split are not read, so the sampler's own apply. An oscillator
that plays the TRINITY's ROM is left out (with a warning); programs that only use the ROM, drum programs and banks
without a TFD disk (`TRIRACK`, `GENERAL`) are not listed. SAVE TO LIBRARY files a program in `Extracted/TRINITY/<category>/`.

Korg M1 SysEx files (`.syx`: a 100-program bank, a single program, combinations and the global data with the drum kits,
from an M1, M1R or M1EX) list their programs (`P00`-`P99`) and combinations (`C00`-`C99`) as presets (1.8). An M1 file
holds settings only; the sound comes from one of these, found beside the `.syx` files (in a folder above them or up to
two levels under one) or anywhere up to three levels into the Plugin Library:
- the M1's own sample ROM: dumps of its eight sample EPROMs, `ic36.bin` ... `ic43.bin` (512 KB each, the names MAME uses
  for the korgm1 set), unzipped into one folder. Omni knows where each multisound and drum sound lies in them (a table
  read from the M1 OS; no Korg audio is part of Omni) and plays them at the M1's 31.25 kHz, the 14 "NT" multisounds and
  the DWGS waves included. No OS chips are needed.
- with the Plus+1 upgrade's four sample ROMs beside those (`U201.bin` ... `U204.bin`, 1 MB each), its 46 extra
  multisounds and 45 drum sounds too, for the Plus+1 program card (`plus+1_prog_card.bin`, the 32 KB card image: its
  100 programs and 100 combinations list like a bank).
- Korg's own copy of the M1 ROM, the TRINITY M1 sample disk (TFD-4S, the folder with `00_PIANO.KMP` ... `85_DWGS_.KMP`
  and the drum sounds `01_KICK_.KSF` ... `44_METRO.KSF`). The 14 "NT" multisounds, which the disk leaves out, play
  their full multisound (from its loop when it has one).

A program brings its oscillators (single, double or drums), octave (16' / 8' / 4'), OSC2 interval and detune,
and each oscillator's amp envelope (attack, break point, slope, sustain, release; times on a fitted curve). Multisounds
from PCM cards (100 and up) and from the M1EX's expansion bank (multisounds and drum sounds) are left out with a warning.
A drum program plays its kit from the file's global data (sound, key, pan, tune, level; on the Plus+1 card also the key
a sound plays as); a file without one lays the 44 drum sounds out from C2. A combination layers or splits its programs
(level, transpose, detune, pan, key and velocity range); its programs come from the same file (on a card, its own
programs), or from the program bank beside it whose name ends in `P` instead of `C`. A sound with nothing to play says
why (an M1EX or card multisound, a program not in the file, an empty kit). The filter, LFOs, velocity
sensitivity and effects are not read: the sampler's own apply.
All-data dumps (function 50: the whole memory, programs, combinations and kits) list like a bank; a file that holds
only the global data (the drum kits, `ORIGGLOB.SYX`) lends its kits to the program banks beside it that have none
(`ORIGPROG.SYX`), and says so when opened itself; sequencer data (function 48, songs) has no sounds and says so. Files
with a Mac header before the SysEx (MacBinary) read too. SAVE TO LIBRARY files a sound in `Extracted/Korg M1/<file name>/`.

Yamaha libraries (YSFC: Motif XS / XF, MOXF, Montage, Montage M, MODX) list their performances or voices as presets,
each element or part a layer with its key and velocity range, tuning, pan, filter and amplitude envelope. Parts that use
the keyboard's own preset waveforms are not in the library, so they are left out and the plugin says how many. Motif
ES `.W7A` (voices and names) and `.W8A` (sample data) load as a pair: open either one; each waveform is a preset.
Checked against Motif XS (Digital Sound Factory Symphonic Strings), Motif XF (Easy Sounds FM Xpanded), Montage (Easy
Sounds Dance Pro) and Motif ES (Sonic Reality Sampler) libraries: every preset loads and every sample decodes. MOXF,
Montage M and MODX files follow the same layout but have not been tried.

Yamaha libraries list their waveforms (multisamples) and, for Montage and MODX, their performances first, read as
ConvertWithMoss reads them: each part a group, each of its elements its waveform within the element's key and velocity
limits with its level, pan, tune, amplitude envelope, filter and filter envelope (XA cycle / random as round robin /
random, XA key-off as a release trigger). Closer to the hardware than ConvertWithMoss: key-bank and element levels add
up, pans add (stereo pairs stay wide), and the part's key / velocity limits, note shift and detune apply. Performances
that only use the keyboard's preset (ROM) waveforms are not listed (an expansion library often has many: Dance Pro
lists 77 of its 216). Libraries are read where they lie (a 470 MB .X0V loads a waveform in a quarter of a second).
Motif ES `.w7a` files need their `.w8a` next to them (ConvertWithMoss does not read these; their layout was worked out
from the Sonic Reality Sampler library, 44.1 kHz checked by pitch). The pitch envelope is not read; compressed (WXC)
and encrypted waveforms are refused with a message. Checked against Motif XS (.X0V, 24 waveforms), Motif XF (.X3A, 48),
Montage (.X7L, 77 performances + 36 waveforms) and Motif ES (12 waveforms) libraries.

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
  keyboard map shows the ranges in the slot colours, where they play (a closed range shifted off the keyboard comes back
  when shifted back, 1.8): a shifted slot is drawn at its shifted keys (1.8;
  before, the map drew the instrument's own keys, so a slot shifted down never showed below its lowest key). Each slot
  also has a **Pan** (1.8, L100..R100; it moves held notes too). **Key Shift** (-48..+48 keys) moves the slot's whole instrument
  up or down the keyboard without changing its pitch: a kit that starts on C1 at -12 starts on C0, leaving C1 and up
  for a bass in another slot. The slot's key range moves with it (an open end, Low at C-2 or High at G8, stays open). **Layer Mode** Layer plays every slot whose range holds the key; Keyswitch plays only
  the slot last selected with the four keyswitch notes from **Keyswitch Base** (default C0, 24-27). **AUTO SPLIT**
  divides the keyboard evenly between the loaded slots; **CLEAR SLOT** empties the selected slot. **SAVE PATCH** keeps
  the whole setup (see Patches below).
- **BROWSE**: drives (Plugin Library, Internal, every USB/SD/SSD volume under `/media`), folders, files and disk
  images. The list scrolls one row at a time with the **▲ / ▼ buttons** beside it or **Q-Link 1** (one row per click),
  and a page (ten rows) at a time with the arrows below it. **SELECTED** shows the full name of what was tapped (a
  long folder or file name wraps over up to three lines; the list rows shorten long names in the middle). Tap a file with several instruments (an SF2 bank, a multi, a disk partition) to list them. The arrows
  next to the instrument name step through the presets of a file, or the files of a folder. A folder that mixes
  instruments with loose samples (an MPC expansion: hundreds of programs among thousands of WAVs) is shown by type
  (1.8): **[Keygroup Programs (n)]**, **[Drum Programs (n)]**, other instrument types, **[Samples (n)]**; tap one to
  list its files, `..` goes back. Stepping through a folder's files stays within the loaded file's type. Folders of
  only samples or only instruments are listed as before.
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
- **FX** (1.8): before the reverb, in this order: a 3-band **EQ** (low shelf 150 Hz, mid 1 kHz, high shelf 6 kHz,
  +-12 dB), a one-knob **Compressor** (threshold and ratio rise together, with makeup gain), a stereo **Chorus** (mix,
  rate 0.05-5 Hz, depth) and a **Delay** synced to MPC's tempo (1/32 to 1 bar with triplets and dotted, feedback,
  damping, stereo or ping-pong); then the reverb and drive (also here). Each is skipped while it is off. Saved with
  projects, patches and instruments like the other sound settings. Q-Links: all 16 FX knobs on one bank.
- **INFO**: every supported format with its file extensions, on two pages (PG 1 / PG 2 in the title bar); a green dot
  marks formats checked against test files, an amber one formats supported but not yet tested on real files, a red one
  formats that are recognised but encrypted. PG 3 has the notes on what can't be read and the contact address.

Q-Links follow the screen sections: each Q-Link bank (the Q-Link button on 4-knob MPCs) is one section of the
page, in this order. PLAY: envelope (attack, decay, sustain, release) / filter (cutoff, resonance, filter velocity,
velocity) / pitch (transpose, bend, fine tune, glide) / drive, reverb mix, volume, pan. SETUP: voice / output /
playback / effects and system. MOD: amounts 1-4 / amounts 5-8 / LFO 1 / LFO 2. LAYERS: one bank per slot A-D (low
key, high key, volume, pan; 1.8, was tune). FX: EQ and compressor / chorus and delay mix / delay / reverb and drive.

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

Anywhere the browser reaches: the **Plugin Library**, the internal storage, or a USB stick / SD card. Since 1.8.0 the
Plugin Library is always on an SD card, USB drive or SSD, never the internal storage (sample libraries are large):
browse to a folder there and tap **SET LIBRARY HERE** (BROWSE). The plugin remembers it for every project and through
updates (`omni-sampler/library_path.txt` in the plugin's folder). Until one is set, SAVE TO LIBRARY, Auto Extract and
SAVE PATCH say so instead of saving. If the drive is not connected the library is kept, not replaced, and comes back
when the drive does. A library set on the internal storage before 1.8.0 is not used; its files stay where they are
(BROWSE > Internal) and can be copied to the new library. Extracted instruments go to the library's `Extracted`
folder. Keep sample folders next to the instrument files that use them (Kontakt and Maschine
paths are also searched two folders up, as libraries are laid out). Samples are loaded into memory up to the
**Memory Limit** (SETUP, default 512 MB); samples over it are skipped and reported.

## Loop crossfades (1.8)

Loops that come with a crossfade (Kontakt, Maschine, SFZ `loop_crossfade`, `.omni`) now get it: the blend is worked
into the audio once when the instrument loads, as the hardware samplers did it, so the voice still plays plain audio.
Before 1.8 the crossfade was read and ignored, and such loops clicked at the loop point. SFZ `loop_crossfade` is
converted at the sample's own rate (it assumed 44.1 kHz). An instrument saved after loading keeps the blended audio
(and a crossfade of 0), so it is not blended twice.

## CPU (MPC Live, Gen1)

1.8: backward loops, the backward half of back-and-forth loops (Ensoniq loop mode 3, Akai / SF2 alternating loops)
and reversed samples now take the same 32-bit fast path as forward playback (they ran the generic path, where every
sample costs double <-> int64 library calls on ARMv7): measured on the ARM build under qemu, 8 voices of a backward
loop 305 -> 181 us per block, back-and-forth 253 -> 180, reversed 161 -> 107, bit-identical output. The drive's
saturation no longer calls tanh per sample (a rational fit). The FX page costs nothing while its effects are off.


`tools/bench.sh` with a 61-zone Kontakt instrument: 16 voices 8% of an audio block (p99); with all 8 matrix slots in
use 12%. The Q-Link sweep stage, which also hits the pads (up to 16 pad notes on top of its 8-voice chords), reaches
19% p99 / 20% worst block: WARN, fine for one or two instances. A SoundFont with a filter, filter envelope and
vibrato on every voice costs about twice as much per voice; lower Polyphony on SETUP for more instances.

## Install

Download `Omni-Sampler-<version>-mpc-armv7.zip` from [Releases](../../releases) and install it with the
[MPC plugin installer](https://github.com/sd88me/mpc-vst-plugins), or unzip it, copy the folder to the MPC and run
`sh install.sh` as root (it stops MPC, backs up `MPC.settings`, installs and restarts MPC). The zip's `INSTALL.md`
has the full steps, including a manual install. `uninstall.sh` removes it again.

Since 1.8.0 the plugin is one folder, `/sdcard/Synths/MR HighMan - VST - Omni Sampler/` (the portable layout the
current installer needs; older zips used `/sdcard/vst` and the installer refuses them with "this zip uses the old
/sdcard/vst layout"). Installing over an older version removes `/sdcard/vst/omni_sampler.so` and moves your
`/sdcard/vst/omni-sampler` folder (the Plugin Library and the library location you set) into the plugin folder.

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
build/x86/test_patch <file> <image/preset> <file>  # save a 3-slot patch, reopen it; project settings, mono pads
```

Without Docker, `build.sh` needs `arm-linux-gnueabihf-g++-12` and a Python with Playwright (`HTML_ART_PYTHON`).
A release zip is made with upstream's current `tools/release.py` (see its `docs/RELEASING.md`), which makes the portable
layout; `omni-sampler/` is the user's data, so pass it as `--user-data`:

```sh
python3 $MPC_VST/tools/release.py --so build/omni_sampler.so --skin "build/skin/MR HighMan - VST - Omni Sampler" \
  --entry build/pluginlist-entry.xml --version X.Y.Z --id omni-sampler --repo B0ss-M/MrHighMan \
  --license LGPL-3.0-or-later --extra build/licenses:licenses --user-data omni-sampler --bench build/bench.json -o dist
```
 The PLAY page design is drawn
by `design.py` at MPC's 1280 x 628 plugin area. The title font is Michroma (OFL, `fonts/`).

## License

LGPL-3.0-or-later. The format readers are C++ translations of
[ConvertWithMoss](https://github.com/git-moss/ConvertWithMoss) by Jürgen Moßgraber (LGPL-3.0); keep this
attribution and the included [LGPL-3.0.txt](LGPL-3.0.txt) and [GPL-3.0.txt](GPL-3.0.txt) when distributing it.
Bundled libraries keep their own licences: dr_flac (public domain / MIT-0), stb_vorbis (public domain / MIT),
tinf (zlib). See [src/third_party/VENDORED.md](src/third_party/VENDORED.md).

# Writing a format reader for Omni Sampler

This guide is for an agent or developer adding a new instrument format to **Omni Sampler**. Omni is a VST2 sampler for
Akai MPC OS (armv7 Gen1, aarch64 Gen2), written in C++17 under LGPL-3.0. A reader is one `.cpp` file that turns a
format's files into Omni's instrument model. When it is registered, Omni lists the files in BROWSE, loads the presets
into a slot and plays them with the full engine. Nothing is converted on disk.

Everything below is taken from the source (`src/formats/format.hpp`, `src/core/*.hpp`). If something here disagrees
with those headers, the headers win: tell the maintainer so the guide can be fixed.

---

## 1. Ground rules

1. **Read only, nothing written.** A reader only reads the user's files. It never converts, caches to disk or edits
   them. (SAVE TO LIBRARY, which writes Omni's own format, is handled by the plugin.)
2. **Facts only.** Document every offset and field in the header comment of your `.cpp` as *measured* (checked against
   real files or audio) or *unknown*. Never guess a field's meaning and present it as fact. Read unknown fields
   conservatively, or not at all.
3. **No crashes on bad input.** Every read is bounds-checked: use `Reader`, `Blob::read`/`head` or explicit size checks.
   A truncated or corrupt file must throw `ParseError` with a short message. It must never read out of bounds, loop
   forever or allocate gigabytes. Check every count and size against the file size and sane limits (zones ≤ 4096,
   names ≤ 256 characters and so on).
4. **No circumventing protection.** If the payload is encrypted or tied to a host application's licence (Kontakt
   Player/NKS, Reason ReFills, Roland Cloud), do not try to break it. Throw a clear `ParseError` such as "this library
   is encrypted", and list the format on INFO's "NOT READABLE" panel.
5. **Licences.** Omni is LGPL-3.0. Do not paste or translate code from GPL projects (libgig, libsfark, Vital, Polyphone
   and others). You may use public specifications, your own measurements, and code under MIT, BSD, zlib or LGPL
   licences, credited in `NOTICE.txt`. ConvertWithMoss's model units are mirrored by Omni's model, so its documentation
   is a fine reference.
6. **No commercial content in the repository.** Test files stay outside it. Record names and SHA-256 hashes in notes.
7. **Small, self-contained.** One file, `src/formats/<name>.cpp`, with everything except its registration in an
   anonymous namespace. Use the shared helpers in section 5; don't add new dependencies.

---

## 2. What a reader is

```cpp
// src/formats/format.hpp
struct FormatReader {
    const char *name;   // shown on the display: "SFZ", "Akai S1000/S3000 program", ...
    const char *exts;   // space-separated lower-case extensions it may take ("" = probe only)
    bool (*probe)(const std::string &lower_name, const uint8_t *head, size_t n, uint64_t size);   // may be null
    std::vector<PresetInfo> (*list)(VolumePtr vol, const std::string &path);
    Instrument (*load)(VolumePtr vol, const std::string &path, int index);
};
```

| Member | Contract |
|---|---|
| `name` | Short and human-readable; it is shown in the format window and on the info line. Up to about 30 characters. |
| `exts` | Lower-case extensions **without dots**, separated by spaces: `"dspreset dslibrary"`. **The browser only lists files whose extension appears in some reader's `exts`**, so a probe-only reader (`""`) is never offered in the list. Give real extensions whenever the format has them. |
| `probe` | Optional content check. It receives the lower-case file name, up to **4096** head bytes (`n` may be smaller for small files) and the file's size. It must be fast, pure and never throw: check magic bytes or a tag, and return false when unsure. Without a probe, the extension alone decides. |
| `list` | Returns the presets inside the file: `{name, index}` pairs. Most formats return one entry (`{{name, 0}}`). Banks return one per program. It runs when the user taps the file, so it must be **fast**: parse headers and names only, and never decode audio. Throws `ParseError` when the file is not readable. |
| `load` | Builds the full `Instrument` for preset `index` (an index from `list`). Audio is **not** decoded here: each zone gets a `SampleRef` with a decode function that the loader thread calls later. It must call `finish_instrument(inst)` before returning. Throws `ParseError` with a message the user can act on. |

**How a reader is chosen** (`find_reader`): readers are tried in registration order. A reader whose `exts` lists the
file's extension and has no probe wins outright. A reader with a probe wins when its probe returns true. A reader with
`exts == ""` probes every file. The first match wins, so a generic extension (`.bin`, `.syx`, `.xml`, `.img`) **must**
come with a strict probe, or it will steal files from other readers.

---

## 3. The instrument model (`src/core/model.hpp`)

Units follow ConvertWithMoss's model: **times in seconds, gains in dB, pan −1..1, tuning in semitones** (fractions are
cents), **filter cutoff in Hz, resonance 0..1** (0..1 = 0..40 dB), **modulation depths −1..1**, where 1 = 12000 cents
for pitch and cutoff envelopes and LFOs, and velocity to cutoff 1 = 9600 cents.

### Instrument
| Field | Meaning |
|---|---|
| `name` | Preset name (an empty name becomes "Instrument") |
| `format` | Usually the reader's `name`; shown on the display |
| `library`, `category` | Optional. SAVE TO LIBRARY files the preset under `Extracted/<library>/<category>/` |
| `groups` | Group list (at least one); `Zone::group` indexes it. A `Group` has a `name` and a `trigger` |
| `zones` | The playable zones (below) |
| `polyphony` | 0 = engine default |
| `gain_db` | Whole-instrument gain |
| `warnings` | Short notes shown on the info line and logged, such as "filter and effects are not read". **Always say what you skip.** |

### Zone — one sample on a key/velocity range
| Field | Default | Meaning |
|---|---|---|
| `name` | "" | Shown in the slot's details |
| `sample` | null | **Required.** A `SampleRefPtr` (section 4). Zones without one are dropped by `finish_instrument` |
| `key_lo`, `key_hi`, `root` | 0, 127, 60 | MIDI notes (C4 = 60). `root` is the key the sample plays at its own pitch |
| `key_xfade_lo/hi` | 0 | Keys of fade-in above `key_lo` and fade-out below `key_hi` |
| `vel_lo`, `vel_hi` | 0, 127 | Velocity range (inclusive) |
| `vel_xfade_lo/hi` | 0 | Velocity crossfades |
| `group` | 0 | Index into `Instrument::groups` |
| `gain_db`, `pan`, `tune` | 0 | dB, −1..1, semitones |
| `key_tracking` | 1 | 0 = fixed pitch (drums that keep their pitch on every key), 1 = normal |
| `amp_key_tracking` | 0 | dB per key around the root (1 = 1 dB per key) |
| `amp_vel_depth`, `amp_vel_curve` | 1, 0 | Velocity sensitivity 0..1; curve x^(3^curve) |
| `reverse`, `one_shot` | false | Play reversed; play to the end ignoring note-off (drums) |
| `exclusive_group`, `off_by` | 0, −1 | Choke group (> 0); `off_by` = choked by notes of that group (−1 = its own) |
| `trigger` | Attack | `Attack`, `Release` (release samples), `First`, `Legato` |
| `play_logic`, `seq_position`, `seq_length` | Always, 1, 1 | Round robin: `RoundRobin` + `seq_position` (1-based). `seq_length` is worked out by `finish_instrument` when left at 1 |
| `rand_lo`, `rand_hi` | 0, 1 | `PlayLogic::Random`: plays when the note's random number is in [lo, hi) |
| `start`, `stop` | 0, −1 | Sample frames; `stop` is exclusive, −1 = to the end |
| `loops` | empty | `Loop{type, start, end, until_release, crossfade, tune}`: frames, **`end` inclusive** (as in WAV `smpl`); `type` is Forward, Backward or Alternating; `until_release` loops only while the key is held; `crossfade` in frames; `tune` in semitones applied inside the loop |
| `amp_env` | not set | `Envelope` (below). Without `set = true` the engine uses its default envelope |
| `amp_lfo_depth`, `amp_lfo` | 0 | Tremolo (depth 1 = 96 dB) |
| `pitch_env_depth`, `pitch_env` | 0 | Pitch envelope (depth 1 = 12000 cents) |
| `pitch_lfo_depth`, `pitch_lfo` | 0 | Vibrato |
| `bend_up`, `bend_down` | 200, −200 | Pitch-bend range in cents |
| `filter` | none | `Filter{type, poles, cutoff Hz, resonance 0..1, key_tracking, env_depth + env, vel_depth, lfo_depth + lfo, modwheel_depth}`; type is None, LowPass, HighPass, BandPass or BandReject |
| `midi_channel` | −1 | −1 = any; 0..15 for multis that put parts on channels |
| `cc_num`, `cc_lo`, `cc_hi` | −1 | Plays only while that controller is in range (pedal-down piano layers) |
| `wt_size`, `wt_count` | 0 | Wavetable: the sample is `wt_count` cycles of `wt_size` frames each |
| `seq_step` | false | A wave-sequence step (Wavestation): the sample waits out the amp delay and ends once faded to 0 |
| `slot` | 0 | Set by the plugin; leave it alone |

**Envelope:** `set`, `delay`, `start_level`, `attack`, `hold`, `decay`, `sustain` (level 0..1), `release`, `end_level`,
the slopes `attack_slope`, `decay_slope` and `release_slope` (−1..1; 0 = linear), and `time_key_tracking` and
`time_vel_tracking` (−1..1). Times are in seconds.

**Lfo:** `set`, `wave` (Sine, Triangle, Square, SawUp, SawDown, Random), `rate_hz`, `delay`, `fade_in`, `start_phase`,
`key_sync`.

The engine plays every one of these fields. Map as many of your format's parameters as you can measure with
confidence. Where a parameter has no counterpart, leave the default and add a warning.

**What `finish_instrument` does:** it clamps keys and velocities to 0..127 (swapping reversed ranges) and pan to −1..1,
fixes bad group indices, swaps reversed loop points, drops zones without a sample, fills round-robin lengths and names
an unnamed instrument. Call it last in `load`.

---

## 4. Samples: `SampleRef` and audio decoding

Zones refer to audio through `SampleRef`. It is decoded **once, on the loader thread, when the preset is played**:

```cpp
struct SampleRef {
    std::string key;                  // cache identity: zones sharing a key share one decoded Pcm
    std::string name;
    int rate = 0, channels = 0;       // 0 = unknown until decoded
    int64_t frames = 0;
    std::function<PcmPtr()> decode;   // throws ParseError on failure
};
```

- **`key` must be unique per distinct audio**, for example `"<reader>:" + path` or `+ "@" + offset`. Zones that share a
  sample share one `SampleRefPtr`, or at least the same key, so the audio is decoded and stored once.
- **`decode` runs on another thread, later.** Capture what it needs **by value** (`VolumePtr`, path, offsets), never by
  reference to locals. It must be thread-safe and must not touch globals without a lock.
- Decoded audio is **`Pcm`: interleaved int16** with any sample rate and 1 or 2 channels. 24-bit and float sources are
  rounded to 16 bits by the helpers. The engine repitches by `rate` and `root`, so don't resample.
- Memory is limited: SETUP's MEMORY setting defaults to 512 MB of decoded audio on a device with about 2 GB of RAM.
  Never hold whole files in memory in `list` or `load`.

Ready-made constructors (`src/core/audio.hpp`):

| Helper | Use |
|---|---|
| `file_sample_ref(vol, path)` | A standalone audio file: WAV (PCM, float, ADPCM, extensible, RF64, RIFX), AIFF/AIFC, FLAC, Ogg Vorbis, CAF (linear PCM), NI NCW. The type is detected from the content. |
| `raw_sample_ref(blob, offset, frames, channels, Enc, rate, name, planar=false)` | Raw PCM inside a bank or disk image. `Enc`: S8, U8, S16LE/BE, S24LE/BE, S32LE/BE, F32LE/BE, F64LE/BE |
| `decode_audio(blob, &AudioInfo)` | Inside your own `decode` lambda for embedded files (a WAV inside a ZIP: `make_mem_blob(bytes)`). `AudioInfo` returns the root, fine tune, loops and ranges from WAV `smpl`/`inst` and AIFF `INST`/`MARK` chunks |
| `probe_audio(blob)` | Header only: rate, channels, frames, root and loops, without decoding (cheap for WAV/AIFF) |
| `pcm_from_raw`, `pcm_from_planar`, `convert_raw` | Build a `Pcm` from bytes you have already decrypted or descrambled |

If your format has its own codec, put it in your `.cpp` and call it from `decode`. For example, `heatup.cpp` patches
a FLAC header and then calls `decode_audio`.

---

## 5. Shared helpers you should use

**Files and paths** (`src/core/vfs.hpp`). Readers only ever see a `Volume` plus an inner path. The same reader then works
on host folders and inside ISO/FAT/Akai/E-mu disk images.
- `vol->open(path)` gives a `BlobPtr`; `blob->size()`, `read(off, dst, n)`, `bytes(off, n)`, `head(n, off)` (never throws
  on a short tail) and `all(limit)`.
- `vol->list(dir)`, `vol->is_dir(p)`, `vol->exists(p)`; `make_slice(blob, off, size)`, `make_mem_blob(vector)`.
- `path_join`, `path_dir`, `path_name`, `path_stem`, `path_ext` (lower case, no dot), `path_resolve(base_dir, ref)`
  (handles `../` and backslashes from Windows-made files) and `find_ci(vol, path, found)` (a case-insensitive file
  lookup: always use it for sample paths named inside a preset).
- `find_beside(vol, file, is_folder, cache)` finds a shared folder (ROM dumps, a sample folder) next to the file, in any
  folder above it, two levels below each of those, or in the user's Plugin Library. Pass a `static std::string cache`
  per kind of folder.

**Binary parsing** (`src/core/bytes.hpp`): `Reader` (`u8/u16le/u32be/f32le/str(n)/cstr()/utf16le(n)/sub/seek/starts`,
all bounds-checked and throwing `ParseError`); the free functions `le16/le32/be32...`; `lower`, `trim`, `equals_ci`,
`ends_with_ci`.

**Text and compression** (`src/core/parse.hpp`): `parse_xml(text)` gives an `XmlNode` (`child`, `all`, `attr`,
`attr_num`, `find`, `child_text`); `parse_json(text)` gives a `Json` (`["key"]`, `as_num`, `as_str`, `arr`, `obj`);
`inflate_zlib`, `inflate_raw` and `inflate_gzip`; `fastlz_decompress`. For ZIP containers (`.multisample`, `.dslibrary`,
`.xrni`...), reuse the small ZIP reader in `bliss.cpp` (stored and deflate entries).

**Numbers** (`format.hpp`): `clampi`, `clampd`, `velocity_to_db(0..127)` and `chip_loop(start, length, last)` for
fractional ROM loops.

---

## 6. Layout on disk: presets and their samples

- Find samples **relative to the preset** with `path_resolve` and `find_ci`. If they are missing, look further with
  `find_beside`.
- **SAMPLE FOLDERS: HIDE** (BROWSE) hides an instrument's own sample folder so presets stand alone. A folder is hidden
  when it sits beside an instrument file and is named `<stem>`, `<stem> Samples` or `<stem>_Samples` (case-insensitive),
  or when it holds only Heat Up `.ignitex` files. If your format uses a different convention (samples in a folder
  whose name is written inside the preset), tell the maintainer: `companion_folders()` in `src/plugin.cpp` can learn it.
- **Disk-like containers** (a format that is really a file system or a bank of files) can be an `ImageType` instead
  (`register_image_type`: `probe` plus `mount(BlobPtr)` returning a `Volume`). The image then opens like a folder in
  BROWSE and your readers run on the files inside it. See `src/fs/`.

---

## 7. Template

```cpp
// Omni Sampler: <Product> <kind of file> (.ext). Measured <date> on <N> files (<names>); facts only:
//   - <offset/field>: <meaning> (measured: <how>)
//   - ...
// Not read: <what is skipped and why>.
#include <algorithm>
#include <cstring>
#include "format.hpp"
#include "../core/parse.hpp"     // only if XML / JSON / inflate are used

namespace omni {
namespace {

bool probe_xyz(const std::string &, const uint8_t *h, size_t n, uint64_t size) {
    return n >= 8 && size >= 64 && !std::memcmp(h, "XYZ1", 4);   // strict: magic + plausible size
}

struct Header { std::string name; uint32_t zones = 0; uint64_t first = 0; };

Header read_header(const Blob &f) {
    std::vector<uint8_t> b = f.head(4096);
    Reader r(b);
    if (!r.starts("XYZ1")) throw ParseError("not an XYZ file");
    r.skip(4);
    Header h;
    h.zones = r.u32le();
    if (h.zones == 0 || h.zones > 4096) throw ParseError("XYZ: implausible zone count");
    h.name = r.str(32);
    h.first = r.pos();
    return h;
}

std::vector<PresetInfo> list_xyz(VolumePtr vol, const std::string &path) {
    Header h = read_header(*vol->open(path));                        // headers only: fast
    return {{h.name.empty() ? path_stem(path) : h.name, 0}};
}

Instrument load_xyz(VolumePtr vol, const std::string &path, int /*index*/) {
    BlobPtr f = vol->open(path);
    Header h = read_header(*f);
    Instrument inst;
    inst.name = h.name.empty() ? path_stem(path) : h.name;
    inst.format = "XYZ instrument";
    uint64_t at = h.first;
    for (uint32_t i = 0; i < h.zones; i++) {
        std::vector<uint8_t> rec = f->bytes(at, 32);                 // throws ParseError when cut short
        Reader r(rec);
        Zone z;
        z.root = r.u8(); z.key_lo = r.u8(); z.key_hi = r.u8();
        z.vel_lo = r.u8(); z.vel_hi = r.u8();
        uint32_t frames = r.u32le(), rate = r.u32le(), data = r.u32le();
        if (!frames || frames > (1u << 28) || data + uint64_t(frames) * 2 > f->size()) throw ParseError("XYZ: bad zone");
        z.sample = raw_sample_ref(f, data, frames, 1, Enc::S16LE, int(rate), inst.name + " " + std::to_string(i));
        z.amp_env.set = true; z.amp_env.release = 0.3;
        inst.zones.push_back(std::move(z));
        at += 32;
    }
    inst.warnings.push_back("XYZ: filter and effects are not read");
    finish_instrument(inst);
    return inst;
}

}  // namespace

void register_xyz() {
    register_reader({"XYZ instrument", "xyz", probe_xyz, list_xyz, load_xyz});
}

}  // namespace omni
```

For samples in separate files, replace `raw_sample_ref` with:

```cpp
std::string found;
std::string want = path_resolve(path_dir(path), sample_name_from_preset);
if (!find_ci(*vol, want, found)) throw ParseError("XYZ: sample \"" + sample_name_from_preset + "\" not found");
z.sample = file_sample_ref(vol, found);
```

Reuse one `SampleRefPtr` for zones that share a file (keep a `std::map<std::string, SampleRefPtr>`, as `sfz.cpp` does).

---

## 8. Wiring it in (four edits)

1. **`src/formats/<name>.cpp`**: the reader.
2. **`src/formats/format.hpp`**: declare `void register_<name>();` next to the others.
3. **`src/formats/format.cpp`**: call `register_<name>();` inside `register_all()`, **before** `register_omni_native()`,
   `register_wav_folder()` and `register_images()`. Order matters: probe-only and generic-extension readers go after
   the specific ones.
4. **`vst.json`**: add `"src/formats/<name>.cpp"` to `build.sources`. Only the Docker build (`build_port.sh`) reads
   this list. Without Docker, `build.sh` and `build_aarch64.sh` compile every `src/{core,formats,fs,engine}/*.cpp`, and
   so do the desktop tests. Add the file anyway, so both builds contain the same code. (`nxs_klp.cpp`, `srx_world.cpp`
   and `rom_map.cpp` are missing from the list at the moment.)

Then document it:
- **`README.md`**: a row in the format table ("Family | Files") saying what is read **and what is not**.
- **INFO page** (`design.py`, `INFO_PANELS` / `INFO_PANELS2`): `(label, ".ext", True|False|None)`. True = tested on real
  files, False = supported but untested, None = recognised but not readable. The panels are full: ask the maintainer
  where a new entry goes.
- **`NOTICE.txt`**: credit any third-party code or specification you used.

---

## 9. Testing (desktop first, then the device)

```sh
# x86 desktop tools (ASan/UBSan by default; --fast = -O2, no sanitizers; keep JOBS low, the container has little RAM)
MPC_VST=_toolcache/mpc-vst-plugins-20261030 JOBS=2 tests/build_x86.sh [--fast]

build/x86/probe <file>              # what list() returns
build/x86/probe <file> 0 -d         # the zones of preset 0, every sample decoded (peak, frames)
build/x86/probe <file> -all -d      # every preset
build/x86/test_engine <file> [preset] [notes...]   # load as a project would, play notes, measure; prints PASSED
```

What counts as done:
- `probe -all -d` on **every test file** shows plausible zones (keys, roots, ranges, loops) and decodes every sample
  with a non-zero peak and **no ASan/UBSan reports**.
- `test_engine` prints `PASSED` and the state round trip is "ok".
- **Robustness:** truncate a test file at several points (`head -c N`) and flip random bytes. `probe` must throw a clean
  `ParseError` every time: no crash, hang or huge allocation.
- **Listening:** a reference render or the original product agrees on pitch (roots, tune), key and velocity layout,
  loops (no clicks beyond the original's) and level, within reason.
- The existing tests still pass: `build/x86/test_wheel`, `test_patch`, `test_playback`, `test_sinc`, and `probe` on a
  few files of other formats, to check you didn't steal them through extension order or a loose probe.

**Batch test a whole library**, not just a few files. Count the results by error message. That is how the SRX bug
(section 10) showed up: every preset in 10 banks failed with the same message.

```sh
P=build/x86/probe; find "$LIB" -iname '*.ext' | while IFS= read -r f; do
  out=$(timeout 60 $P "$f" -all -d 2>&1) && echo OK || echo "$out" | grep -m1 -iE 'error|not|unsupported'
done | sort | uniq -c | sort -rn
```

**Device build:** this is the maintainer's job; install on the user's MPC only when they say so. Raise
`"version"` in `vst.json` for every build you hand over (2113 → 2114 shows as 2.1.14 in `version.xml`), so the user
can see which build is running. Gen1 (armv7) is built with
the bullseye gcc-10 cross compiler (glibc ≤ 2.30) through `_toolcache/omni-fixes/build_omni.sh`, and Gen2 (aarch64)
through `build_aarch64.sh`. Both must compile your file without warnings. The plugin must still pass `build_omni.sh`'s
checks ("highest glibc: GLIBC_2.30", "shipped skin check: … 0 EDGE").

---

## 10. Mistakes that have shipped, and how to avoid them

Each of these got into a release and was fixed afterwards.

**1. One value, two numbering schemes** (SRX, fixed in 2.1.14). The Koa bank files name an expansion by its Roland
board number (9 = SRX-09 World, 6 = SRX-06 Orchestra). The ROM names itself by its internal group (35, 31). The reader
compared against the ROM's number only, so it rejected every tone, and every preset failed with "no supported mono wave
regions".
*Avoid it:* before deciding which values a field may take, count the values that actually occur across **every** test
file. A short Python loop that prints a histogram is enough. If you see values you didn't expect, find out what they
mean before you filter on them. Record the counts in the header comment (`srx_world.cpp` does:
"9 / 6 in 3,072 tones of the ten SR-JV80 banks").

**2. One odd record threw away the whole preset** (SRX, 2.1.14). Sixteen waves contain a region that repeats the
previous key range. The reader threw `ParseError` for it, so every preset using one of those waves failed.
*Avoid it:* decide what kind of problem you're looking at.
- **The file is damaged** (offsets out of range, impossible counts): throw `ParseError`, as rule 3 says.
- **The file is valid but has something you don't understand or support** (an unknown flag, an extra region, an
  unsupported ROM): skip that zone or tone, add a warning that names what was skipped, and keep going. Partial sound
  with a warning is better than an error.

**3. A cached folder search found another library's folder** (Nexus, fixed in 2.1.13). `find_beside`'s `static` cache
is shared by **every** file the reader opens. When the user had a self-contained library (`Nexus Test/Samples`) next to
a split layout (preset folders beside `Rom/`, `XpDnc1/` …), the first folder found was cached and used for every preset.
Nearly every sample came up "not found".
*Avoid it:* resolve sample references **nearest first**: from the preset's own folder, walk upward and check each
layout the format allows (`find_nxs_nearby` in `nxs.cpp`). Use the cached `find_beside` only as a fallback. Test with
two libraries side by side.

**4. Tested only on a tidy layout.** Users organise folders their own way: split layouts, renamed folders, mixed case
(`XPDNC2` vs `XpDnc2`), several libraries side by side.
*Avoid it:* ask how the user's folders are laid out (or list them over SSH) and mirror that tree on the desktop. For
path lookups, small placeholder files are enough to check that every reference resolves. Then copy a few real samples in
for an end-to-end test. Always look up names with `find_ci`.

**5. An error message that doesn't say why.** "No supported mono wave regions" was accurate, but it hid the real cause:
every tone had been rejected for its group number.
*Avoid it:* when a preset ends up with no zones, say **why**. Name the most common reason you skipped things, with a
count (for example "all 4 tones use ROM group 9, expected 35"). The user and the next developer can then fix it without
a debugger.

**6. A warning repeated per zone.** A reader that warns once for each skipped zone can fill the info line with 40
copies of the same text. Warn once per kind of problem, with a count if it helps.

---

## 11. Hand-over checklist

- [ ] `src/formats/<name>.cpp`, with a header comment: what was measured, on which files, and what is not read
- [ ] `register_<name>` declared, called in `register_all()` in the right order, and listed in `vst.json` sources
- [ ] Strict probe for any generic or shared extension; `exts` filled in so BROWSE lists the files
- [ ] `list` is headers-only and fast; `load` decodes nothing and calls `finish_instrument`
- [ ] Every read bounds-checked; every count and size sanity-checked; corrupt input only ever throws `ParseError`
- [ ] `SampleRef` keys unique per audio; decode lambdas capture by value
- [ ] `inst.warnings` says what is skipped, once per kind; a preset with no zones says why
- [ ] Field values were counted across all test files before being filtered (section 10.1)
- [ ] An unsupported record skips that zone with a warning; only damaged data throws (section 10.2)
- [ ] Sample paths are resolved nearest first; tested with two libraries side by side and on the user's real layout
- [ ] A whole-library batch probe, with failures counted by message
- [ ] Probe and engine tests pass with ASan; the truncation and corruption tests pass; other formats unaffected
- [ ] README row, INFO entry proposal, NOTICE credit
- [ ] Notes and file hashes for the maintainer; no commercial files committed; no encryption circumvented

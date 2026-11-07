# Play verified ROM samples in Omni Sampler

A version-1 `.omnirom` map lets Omni Sampler play raw PCM in a companion image without exporting WAV files. Keep the map and image together in the plugin library and select the map in Omni's browser. Each map can list multiple presets.

The map is a working format for analysis. Derive its values from an identified reader, ROM tables, or a verified dump. Use a logical image after any required deinterleave or address permutation. The map never changes the image.

```json
{
  "version": 1,
  "image": "waves.bin",
  "presets": [
    {
      "name": "Example voice",
      "zones": [
        {
          "name": "Middle zone",
          "offset": 4096,
          "frames": 1024,
          "channels": 1,
          "rate": 31250,
          "codec": "s16be",
          "key_lo": 48,
          "key_hi": 72,
          "root": 60,
          "loop_start": 128,
          "loop_end": 1023
        }
      ]
    }
  ]
}
```

The offsets and loop points above are examples. The image is a filename beside the map. Offsets are absolute byte offsets in that image. Frames are per channel and stereo samples are interleaved. Codecs are signed 8-bit PCM (`s8`), signed 16-bit little-endian PCM (`s16le`), and signed 16-bit big-endian PCM (`s16be`). Optional MIDI fields are `key_lo`, `key_hi`, `root`, `vel_lo` and `vel_hi`. Both loop endpoints are required when specifying a forward loop, and the end is inclusive. The reader checks sample bounds before creating the sample references. Decoding runs through Omni's normal loader.

For a newly identified format, put its preset/table parser and storage/codec logic in a dedicated C++ reader once the layout is verified. The reader can reuse the same `Instrument`, `Zone` and deferred `SampleRef` interfaces. A format-specific encoder still needs the inverse codec and physical chip packing rules before producing a hardware image.

## Desktop verification

Build the usual desktop probe, then run the synthetic regression fixture:

```sh
MPC_VST=/path/to/mpc-vst-plugins bash tests/build_x86.sh --fast
python3 tests/test_rom_map.py build/x86/probe
build/x86/probe /path/to/bank.omnirom -all -d
```

## MPC Live II compatibility

Use the armv7 build and retain the project's existing glibc ceiling. For each candidate `omni_sampler.so`, inspect it off-device:

```sh
readelf -h omni_sampler.so
readelf -A omni_sampler.so
readelf -d omni_sampler.so
readelf -V omni_sampler.so
```

Check the ARM architecture and ABI, then compare every needed library and versioned symbol requirement against libraries from each target firmware. Check MPC OS 3.9.1 and 3.10 separately. The [inMusic G1 3.10 source archives](https://github.com/inmusic-software/az0x-open-source/tree/mpc-g1/3.10) document the latter's open-source components. The target rootfs libraries and a playback test establish runtime compatibility.

Baseline audit of the published v1.8.1 armv7 ZIP (SHA-256
`78f620f1af2ce45000304d0b94eb129297a140be12866eae8977241a20682255`):
ELF32 ARMv7, EABI5 hard-float, VFP register arguments. Its declared dependencies are
`libpthread.so.0`, `libm.so.6`, `libc.so.6`, and `ld-linux-armhf.so.3`.
The highest requested GLIBC version is `GLIBC_2.30`. This is the existing release
baseline. A new build needs the same audit and device verification.

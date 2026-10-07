# Vendored third-party code

Copied in unchanged unless noted. Re-vendoring = replace the files and diff against this list.

| Path | Project | Version | License | Local changes |
|---|---|---|---|---|
| `dr_flac.h` | [dr_libs](https://github.com/mackron/dr_libs) dr_flac | v0.13.4 (dr_libs commit dfe8377) | Public domain (Unlicense) or MIT-0 | none |
| `stb_vorbis.c` | [stb](https://github.com/nothings/stb) stb_vorbis | v1.22 (stb commit 1ee679ca) | Public domain or MIT | none (compiled inside `core/audio.cpp`) |
| `tinf/` | [tinf](https://github.com/jibsen/tinf) by Joergen Ibsen | commit 57ffa1f1 | zlib (`tinf/LICENSE`) | none; each `.c` compiles as its own C unit |

## Code translated (not copied) from other projects

The format readers are C++ translations of the file-format knowledge in
[ConvertWithMoss](https://github.com/git-moss/ConvertWithMoss) by Jürgen Moßgraber (LGPL-3.0), checked against
commit 31b0f8ab603aa1dd7a369e7ba141b8688fd4c3d2: Akai S1000/S3000/S900/AKP/MPC, E-mu EOS/Emulator III/Emax/
Emulator I and II/Emulator X, NI Kontakt (all generations, NI container, FastLZ, NCW) and Maschine, SF2 and SFZ.
They are LGPL-3.0, like the rest of this port (see the README). Corrections made against the Java original are
listed in the source comments where they apply (e.g. S3000 header size, keygroup stride, NCW block handling).

The Akai S3000 sample/program layouts were also checked against Takashi Ohsaki's published S3000 format notes
(documentation only, no code).

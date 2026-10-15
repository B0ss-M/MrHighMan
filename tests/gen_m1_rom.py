#!/usr/bin/env python3
"""Generates src/formats/korg_m1_rom.inc: where each Korg M1 multisound and drum sound lies in the M1's 4 MB sample
ROM (MAME korgm1 set: ic36.bin ... ic43.bin), read from the M1EX OS v1.29 EPROMs and, for the drums, the M1 OS rev 19
(MAME korgm1: rev19-ic32 / rev19-ic23; ic32 = even bytes, ic23 = odd); and the extra multisounds and drum sounds of
InVision's Plus+1 upgrade (U201.bin ... U204.bin, 16-bit little-endian words) from its OS (MAME korgm1p1: plus-1_lsb
= even bytes, plus-1_msb = odd). Only numbers (addresses, lengths, keys) go into the table; the audio stays in the
user's own chip dumps.

    gen_m1_rom.py <v1.29 ic32> <v1.29 ic23> <rev19 ic32> <rev19 ic23> <plus-1_lsb> <plus-1_msb>
                  [<folder with ic36.bin..ic43.bin and U201.bin..U204.bin> to check]

What the OS holds (decoded 2026-10-15 against the TRINITY M1 disk and the ROM audio):
  - Multisound table (190 x 14 bytes, before "Piano     "): zone list pointer u16, type u8 (2 = DWGS), zone count
    u8, name[10]. Pointers are offsets in the data segment DS (file offset 0x15F60).
  - Zone (8 bytes at DS + pointer): sample pointer u16, top key, 5, tune s8 (cents), original key, level s8, 0.
  - Sample (8 bytes at DS + sample pointer - 4): start u24 (a ROM word address; bit 22 = the M1EX card bank, not in
    the internal ROM), flags (bit 2 = loop), length u16, loop length u16 (the loop is the last n words). Samples follow
    each other in the ROM 2 words apart; the first one after a table holds a placeholder start (0x3FC, 0x4003FC),
    so its start comes from the next one. All play at 31250 Hz (MAME's korgm1.cpp says 48000 Hz: at 48 kHz 3% of the
    loops hold whole cycles of their zone's pitch, at 31250 Hz 98%; Korg's TRINITY M1 disk is 31250 Hz too).
  - DWGS multisounds (77-99) share one zone list; their waves are 0x400-word blocks at the ROM start, block
    = multisound - 77 (single cycles of 512 words at 0, 256 at 0x200, ... 4 at 0x3F8: n words at 0x400 - 2n), the
    list's start addresses being offsets in the block; its first zone holds a placeholder for the 512-word cycle.
  - Plus+1 OS: the same layout with DS = 0x17150. Its multisounds 100-145 (the card bank: a program's byte 11 bit 4
    / 6, multisound 100 + n) play from the U chips (start bit 22); 143-145 (Sync 1-3) are DWGS-like, blocks 0-2 of the
    U chips. Its drum list (0x1CB98) has 89 sounds: the M1's 44, then 45 more (in the U chips; 44, Kick 4, in an
    unused part of the M1's ROM; continue address unknown: played from the start).
  - Drum sounds 0-43: samples (as above) at file offset 0x1C168, one-shots. A drum plays 2 words at its start and then
    goes on at another address, most often start + 2 but not always (Pole, Block, Ride, Flexatone ...: shared
    material). v1.29 leaves that address out; rev 19 has it (14-byte records: start u24, -, -, -, continue u20 + 1
    byte, end u20 + 1 byte; bit 20 of the ROM address is not stored). The table plays from continue - 2: the half of
    the ROM where that is a sample start in the v1.29 tables, else the start's. The TRINITY disk's drums begin there.
"""
import hashlib
import os
import struct
import sys

DS = 0x15F60
DRUMS = 0x1C168
P1_DS = 0x17150
P1_DRUMS = 0x1CB98


def interleave(lo, hi):
    a, b = open(lo, "rb").read(), open(hi, "rb").read()
    return bytes(x for p in zip(a, b) for x in p)


def sample(os_, ptr, ds=DS):
    o = ds + ptr - 4
    r = os_[o:o + 16]
    start = int.from_bytes(r[0:3], "little")
    nxt = int.from_bytes(r[8:11], "little")
    ln, lp = struct.unpack("<HH", r[4:8])
    return dict(start=start, flags=r[3], len=ln, loop=lp, next=nxt)


def fix_start(s):
    # a placeholder start: the sample ends 2 words before the next one
    if s["start"] in (0x3FC, 0x4003FC):
        s["start"] = (s["next"] & 0x3FFFFF) - s["len"] - 2
        s["start"] |= s["next"] & 0x400000
    return s


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    os_ = interleave(sys.argv[1], sys.argv[2])
    rev19 = interleave(sys.argv[3], sys.argv[4])
    names_at = os_.find(b"Piano     ") - 4
    if names_at < 0 or len(os_) != 0x20000:
        sys.exit("not the M1 OS v1.29")
    ms = []
    for k in range(100):
        e = os_[names_at + 14 * k:names_at + 14 * k + 14]
        ptr, kind, count = struct.unpack("<HBB", e[:4])
        ms.append((e[4:14].decode("latin1").rstrip(), ptr, kind, count))

    samples, index = [], {}

    def add(s):
        key = (s["start"], s["len"], s["loop"], s["flags"] & 4)
        if key not in index:
            index[key] = len(samples)
            samples.append(s)
        return index[key]

    zones, multis, lists = [], [], {}

    def read_multis(os_, ds, entries, bank, dwgs_block):
        for k, (name, ptr, kind, count) in entries:
            dwgs = bank | dwgs_block(k) * 0x400 if kind == 2 else -1   # the wave block's first word
            if (ds, ptr) in lists:   # the DWGS multisounds share one zone list
                multis.append((name, lists[(ds, ptr)], count, dwgs))
                continue
            first = lists[(ds, ptr)] = len(zones)
            for z in range(count):
                r = os_[ds + ptr + 8 * z:ds + ptr + 8 * z + 8]
                sp, top, _, tune, orig, level, _ = struct.unpack("<HBBbBbB", r)
                s = sample(os_, sp, ds)
                if kind == 2:
                    # one cycle of the wave: n words at 0x400 - 2n in the block; else the 512-word cycle at its start
                    off = s["start"] & 0x3FF
                    if 4 <= s["loop"] <= 256 and off == 0x400 - 2 * s["loop"]:
                        s = dict(start=off, len=s["loop"], loop=s["loop"], flags=4)
                    else:
                        s = dict(start=0, len=512, loop=512, flags=4)
                else:
                    s = fix_start(s)
                    if (s["start"] & 0x400000) != bank:
                        sys.exit("%s: a sample in the other bank" % name)
                zones.append((top, orig, tune, level, add(s)))
            multis.append((name, first, count, dwgs))

    read_multis(os_, DS, list(enumerate(ms)), 0, lambda k: k - 77)

    starts = {s["start"] & 0x3FFFFF for s in samples}
    for o in range(DRUMS, DRUMS + 44 * 8, 8):
        starts.add(int.from_bytes(os_[o:o + 3], "little") & 0x3FFFFF)
    v129 = [sample(os_, DRUMS - DS + 4 + 8 * d) for d in range(44)]
    base = rev19.find(v129[1]["start"].to_bytes(3, "little")) - 14   # Kick 2, the first with a start of its own
    if base < 0:
        sys.exit("not the M1 OS rev 19")
    drums = []
    for d in range(44):
        r = rev19[base + 14 * d:base + 14 * d + 14]
        start, cont = int.from_bytes(r[0:3], "little"), int.from_bytes(r[6:9], "little") & 0xFFFFF
        hits = [a for a in ((cont | half) - 2 for half in (0, 0x100000)) if a in starts]
        body = hits[0] if len(hits) == 1 else (cont | (start & 0x100000)) - 2
        drums.append(add(dict(start=body, len=v129[d]["len"], loop=0, flags=0)))

    # Plus+1
    p1 = interleave(sys.argv[5], sys.argv[6])
    at = p1.find(b"Piano     ") - 4
    if at < 0 or p1.find(b"E. Piano 3") < 0:
        sys.exit("not the Plus+1 OS")
    p1ms = []
    for k in range(100, 146):
        e = p1[at + 14 * k:at + 14 * k + 14]
        ptr, kind, count = struct.unpack("<HBB", e[:4])
        p1ms.append((k, (e[4:14].decode("latin1").rstrip(), ptr, kind, count)))
    n_internal = len(multis)
    read_multis(p1, P1_DS, p1ms, 0x400000, lambda k: k - 143)
    p1drums = []
    for d in range(44, 89):
        s = fix_start(sample(p1, P1_DRUMS - P1_DS + 4 + 8 * d, P1_DS))   # (44, Kick 4: in the M1's own ROM)
        p1drums.append(add(dict(start=s["start"], len=s["len"], loop=0, flags=0)))

    if len(sys.argv) > 7:
        check(sys.argv[7], samples, zones, multis)

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "formats", "korg_m1_rom.inc")
    with open(out, "w") as f:
        f.write("// Generated by tests/gen_m1_rom.py from the Korg M1EX OS v1.29 (sha1 %s): where each multisound and\n"
                "// drum sound lies in the M1 sample ROM. Numbers only; the audio is read from the user's chip dumps.\n"
                % hashlib.sha1(os_).hexdigest()[:12])
        f.write("// start (ROM word, bit 22: the Plus+1 U chips; for DWGS: offset in the wave block), length, loop length\n"
                "// (0 = one-shot)\n")
        f.write("const M1Sample M1_SAMPLES[%d] = {\n" % len(samples))
        for i in range(0, len(samples), 4):
            f.write("    " + " ".join("{0x%06X, %d, %d}," % (s["start"], s["len"], s["loop"] if s["flags"] & 4 else 0)
                                      for s in samples[i:i + 4]) + "\n")
        f.write("};\n// top key, original key, tune (cents), level, sample\n")
        f.write("const M1Zone M1_ZONES[%d] = {\n" % len(zones))
        for i in range(0, len(zones), 6):
            f.write("    " + " ".join("{%d, %d, %d, %d, %d}," % z for z in zones[i:i + 6]) + "\n")
        f.write("};\n// name, first zone, zones, DWGS wave block (its first word; -1: not DWGS)\n")
        f.write("const M1Multi M1_MULTIS[100] = {\n")
        for name, first, count, dwgs in multis[:n_internal]:
            f.write('    {"%s", %d, %d, %d},\n' % (name.replace('"', ""), first, count, dwgs))
        f.write("};\n// Plus+1: multisounds 100-145 (the card bank)\n")
        f.write("const M1Multi P1_MULTIS[%d] = {\n" % (len(multis) - n_internal))
        for name, first, count, dwgs in multis[n_internal:]:
            f.write('    {"%s", %d, %d, %d},\n' % (name.replace('"', ""), first, count, dwgs))
        f.write("};\n// the 44 drum sounds (one-shots)\n")
        f.write("const uint16_t M1_DRUMS[44] = {" + ", ".join(str(d) for d in drums) + "};\n")
        f.write("// Plus+1: drum sounds 44-88\n")
        f.write("const uint16_t P1_DRUMS[45] = {" + ", ".join(str(d) for d in p1drums) + "};\n")
    print("wrote %s: %d samples, %d zones" % (os.path.normpath(out), len(samples), len(zones)))


def check(folder, samples, zones, multis):
    """Loops of whole cycles at the zone's own pitch (31250 Hz) and seamless loop points, on the ROM itself."""
    import numpy as np
    def chip(n):
        for f in os.listdir(folder):
            if f.lower() == "ic%d.bin" % n:
                return np.fromfile(os.path.join(folder, f), dtype=np.uint8)
        sys.exit("no ic%d.bin in %s" % (n, folder))
    hi = np.concatenate([chip(n) for n in range(40, 44)]).astype(np.int32)
    lo = np.concatenate([chip(n) for n in range(36, 40)]).astype(np.int32)
    banks = {0: ((hi << 8) | lo).astype(np.int16).astype(np.float64)}
    if all(os.path.exists(os.path.join(folder, "U20%d.bin" % n)) for n in range(1, 5)):
        banks[0x400000] = np.concatenate([np.fromfile(os.path.join(folder, "U20%d.bin" % n), dtype="<i2")
                                          for n in range(1, 5)]).astype(np.float64)
    for bank, w in banks.items():
        whole, seams = [], []
        for name, first, count, dwgs in multis:
            if dwgs >= 0:
                continue
            for top, orig, tune, level, si in zones[first:first + count]:
                s = samples[si]
                if (s["start"] & 0x400000) != bank or not s["flags"] & 4 or not 8 <= s["loop"] <= 600:
                    continue
                m = 440 * 2 ** ((orig - 69) / 12) * 2 ** (-tune / 1200) * s["loop"] / 31250
                whole.append(abs(m - round(m)) if m >= 0.5 else 0.5)
                end = (s["start"] & 0x3FFFFF) + s["len"]
                a, b = w[end - s["loop"]:end], w[end - 2 * s["loop"]:end - s["loop"]]
                seams.append(float(np.mean(np.abs(a - b)) / (np.std(a) + 1e-9)))
        print("%s: loops of whole cycles: %.0f%%, median loop-vs-cycle-before error %.3f (n %d)"
              % ("Plus+1" if bank else "M1", 100 * np.mean(np.array(whole) < 0.1), np.median(seams), len(whole)))


if __name__ == "__main__":
    main()

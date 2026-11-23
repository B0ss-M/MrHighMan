#!/usr/bin/env python3
"""Generates src/formats/drum_rom_table.inc: the sound chips of vintage drum machines (and the sound EPROMs sold for
them) that Omni Sampler recognises, and the drum sounds each holds. A chip is known by the CRC-32 of its first 4 KB
and its size, so the user's dumps work under any file name; only numbers and names go into the table, the audio stays
in the user's own dumps.

    gen_drum_roms.py <ROM Firmware folder> <Vintage-Synth-Firmware-Collection folder> [out.inc]

The first folder holds MAME's sets (linndrum, obdmx, tr707: the MAME 0.289 file names), the second the archive.org
"vintage-synth-firmware-collection" dumps, one folder per zip as downloaded.

What the chips hold (2026-10-21):
  - LinnDrum, Oberheim DMX / DX, Sequential Drumtraks, E-mu Drumulator, and the Drumware / DigiDrums / Linn 9000
    EPROMs: 8-bit samples for an AM6070 (PMI DAC-76) companding DAC: bit 7 sign (1 = +), bits 6-4 the segment, 3-0 the
    step; value = (0, 33, 99, 231, 495, 1023, 2079, 4191)[segment] + step * (2 << segment), full scale 8031. Matches
    the WAV renders shipped with the DMX, DX, Drumtraks, Drumware and DigiDrums dumps exactly (correlation 1.000).
  - Rates. LinnDrum (MAME linndrum.cpp at its default trimmer and knob settings): the multiplexed drums (hat,
    cymbals, tambourine, cowbell ...) 24836 Hz, snare and sidestick 23286 Hz, toms 34261 / 40827 / 47392 Hz (low /
    mid / high: one sample), congas 40827 / 42140 Hz. DMX (MAME dmx.cpp, its 555 timers at mid trim): 27179 Hz, the
    snare card 40769 Hz, the Perc 2 card 19083 Hz, the tom cards 27179 / 31332 / 36847 Hz (three pitches of one
    sample). Other chips: the rate of the WAV render shipped beside them, else the set's default below (estimates).
  - DMX Perc 1 / 2 and cymbal cards, and the Drumtraks / Drumware "two sound" chips, hold two sounds, one per half;
    multi-chip sounds (cymbals, long hats) are their chips one after another (DMX cymbal card: 4 crash then 4 ride).
  - TR-707 (MAME roland_tr707.cpp): IC34 + IC35 = 8 voices of 8 KB (voice << 13): bass, snare, low / mid / high tom,
    hi-hat, rimshot / cowbell, handclap / tambourine; bass, snare and the two pairs interleave two sounds (even bytes
    the first, odd the second; 4096 each at 25 kHz); toms 12.5 kHz, the hat 25 kHz (8192). 8-bit DAC08: larger values
    are more negative. Crash (IC19) and ride (IC22): 32 KB each, the top 6 bits to a 6-bit DAC, 25 kHz.
  - Casio RZ-1 (MAME rz1.cpp): signed 8-bit at 20 kHz; sound_a (Toms 1-3, Bass Drum, Snare, Rim, Closed / Open Hat,
    Click) and sound_b (Claps, Ride, Cowbell, Crash), each sound from its start address in the program ROM (written
    to the uPD934G at boot) to the next one.
  - Solton Programmer 24 (the dump's own mapping sheet): 4 x 16 KB in 2 KB blocks, unsigned 8-bit, a sample ending at
    the first 0x00 or 0xFF; 24.5 kHz, congas and toms 12.25 kHz.
  - Simmons SDS EPROMs: unsigned 8-bit (centred 0x80), rate unknown (25 kHz assumed).
  - Roland TR-505 / TR-626 (Harry Axten's maps, hkadesign.org.uk tr505expansion / tr626expansion): unsigned 8-bit,
    25 kHz, one sound per 8 KB, or two sounds per 8 KB on its even / odd bytes, cymbals as 2 or 4 blocks of 8 KB
    read one byte from each in turn. The TR-626 mask ROM has address lines 6 and 8 swapped. The community dumps
    "TR-505_RawRomDump_Decoded" and "TR-626_Sound_Rom_UnScrambled" are already laid out plainly (the pair's even
    sound in the first 4 KB, its odd one in the second, each cymbal contiguous); the raw TR-626 dump is turned into
    that layout on load (chip transform 1: byte-identical to the UnScrambled file). The raw TR-505 mask ROM dump needs
    its non-standard pinout, not known here, so only its decoded copy is read.
  - Yamaha PSR-60 / PSR-70 (MAME psr60 / psr70): the YM2154 "RYP4" plays its two sample ROMs, YM21908 (drum kit) and
    YM21909 (Tom 3 and Latin percussion), at 20.833 kHz (2.25 MHz / 18 / 6), one byte a sample: bit 7 the sign, bits
    4-6 a shift, bits 0-3 the mantissa: (0x21 | m << 1) << shift (MAME ym2154.cpp). Sounds are 64-byte blocks from a
    start to an end block; names and blocks from Jari Kangas's RYP4 Programmer's Guide (github.com/JKN0/PSR70-reverse,
    table 3), the same 21 entries as the sound table at 0x4000 of the PSR-60 / PSR-70 ROM2 (yamaha_psr60_pgm_ic110).
"""
import os
import re
import struct
import sys
import wave
import zlib

LEV = [0, 33, 99, 231, 495, 1023, 2079, 4191]
DAC76, U8, S8, U8NEG, U6, RYP4 = 0, 1, 2, 3, 4, 5
FMT_NAMES = {DAC76: 'DAC76', U8: 'U8', S8: 'S8', U8NEG: 'U8NEG', U6: 'U6', RYP4: 'RYP4'}

# drum categories: the reader turns them into GM keys
CATS = ['other', 'kick', 'snare', 'rim', 'clap', 'hat_closed', 'hat_open', 'hat_pedal', 'tom', 'crash', 'ride',
        'cowbell', 'tambourine', 'cabasa', 'shaker', 'conga', 'bongo', 'timbale', 'agogo', 'clave', 'block',
        'triangle', 'china', 'click', 'fx', 'cuica']
CAT_RE = [('hat_open', r'open.?h|ohh|open hat|long.?hat|longhat'), ('hat_pedal', r'foot.?hat|pedal'),
          ('hat_closed', r'hat|hh'), ('kick', r'kick|bass(?! ?clar)|bd\b|kik|bunk|wallbang'),
          ('rim', r'rim|stick|stik|x-?stick'), ('snare', r'snar|snr|sd\b|^s\d'), ('clap', r'clap|claps|snap'),
          ('china', r'china'), ('ride', r'ride|perid'), ('crash', r'crash|crsh|cr1f|sync.?cym|cym'),
          ('cowbell', r'cow|cwbl'), ('tambourine', r'tamb'), ('cabasa', r'cabasa|cbsa'),
          ('shaker', r'shak|maraca'), ('conga', r'conga|cnga|cga'), ('bongo', r'bongo'), ('timbale', r'timb'),
          ('agogo', r'agogo|ago\b|bell'), ('clave', r'clave|clv'), ('block', r'block|blk'),
          ('triangle', r'tri\b|triangle'), ('tom', r'tom|^t\d|timpani|talking'), ('click', r'click|metronome|klak'),
          ('kick', r'^k\d')]


def category(name):
    n = name.lower()
    for cat, rx in CAT_RE:
        if re.search(rx, n):
            return CATS.index(cat)
    return CATS.index('other')


def dac76(b):
    c = (b >> 4) & 7
    v = LEV[c] + (b & 15) * (2 << c)
    return v if b & 0x80 else -v


def decode(fmt, b):
    if fmt == DAC76:
        return dac76(b) * 4                     # 8031 -> ~32124
    if fmt == U8:
        return (b - 128) * 256
    if fmt == S8:
        return (b - 256 if b > 127 else b) * 256
    if fmt == U8NEG:
        return (128 - b) * 256
    if fmt == U6:
        return ((b & 0xFC) - 128) * 256
    if fmt == RYP4:
        v = (0x21 | (b & 15) << 1) << ((b >> 4) & 7)
        return (-v if b & 0x80 else v) * 4      # 8064 -> 32256
    raise ValueError(fmt)


def quiet(fmt, b):
    return abs(decode(fmt, b)) < 600


def crc_key(data):
    return zlib.crc32(data[:4096]) & 0xFFFFFFFF, len(data)


# chip transforms (DrumChip.xform), applied to the whole chip on load
XF_NONE, XF_TR626 = 0, 1
TR626_PAIRS = (0x8000, 0xA000, 0xC000, 0x28000, 0x2A000, 0x2C000)
TR626_CYMBALS = ((0x10000, 4), (0x18000, 2), (0x30000, 4), (0x38000, 2))


def tr626_plain(raw):
    """the raw TR-626 mask ROM dump laid out plainly (= TR-626_Sound_Rom_UnScrambled.bin)"""
    n = len(raw)
    s = bytearray(n)
    for a in range(n):
        s[a] = raw[(a & ~0x140) | ((a >> 6) & 1) << 8 | ((a >> 8) & 1) << 6]
    out = bytearray(s)
    for b in TR626_PAIRS:
        out[b:b + 4096] = s[b:b + 8192:2]
        out[b + 4096:b + 8192] = s[b + 1:b + 8192:2]
    for b, nb in TR626_CYMBALS:
        for k in range(nb * 8192):
            out[b + k] = s[b + (k % nb) * 8192 + k // nb]
    return bytes(out)


class Chip:
    def __init__(self, path, xform=XF_NONE):
        self.path = path
        raw = open(path, 'rb').read()
        self.crc, self.size = crc_key(raw)
        self.xform = xform
        self.data = tr626_plain(raw) if xform == XF_TR626 else raw
        self.id = None


chips = {}          # (crc, size) -> Chip
sets = []           # [name, [sound dicts]]


def chip(path, xform=XF_NONE):
    c = Chip(path, xform)
    key = (c.crc, c.size)
    if key not in chips:
        chips[key] = c
    return chips[key]


def trimmed(fmt, data, lo, hi, stride=1, phase=0):
    """frames of data[lo:hi] (every stride-th byte from phase) without the trailing silence"""
    n = (hi - lo - phase + stride - 1) // stride
    at = lambda k: data[lo + phase + k * stride]
    if fmt == U8:                                   # silence rests near (not always at) 0x80: a DC offset
        rest = at(n - 1)
        while n > 1 and abs(at(n - 1) - rest) <= 3:
            n -= 1
        return min(n + 1, (hi - lo - phase + stride - 1) // stride)
    while n > 1 and quiet(fmt, at(n - 1)):
        n -= 1
    return n


def sound(name, cat, rate, fmt, parts, offset=0, length=None, stride=1, phase=0, exact=None, decay=0):
    """parts: the chips, one after another; offset / length in bytes of their concatenation"""
    data = b''.join(c.data for c in parts)
    if length is None:
        length = len(data) - offset
    frames = exact if exact is not None else trimmed(fmt, data, offset, offset + length, stride, phase)
    pcm = bytes(data[offset + phase + k * stride] for k in range(frames))
    cat = CATS.index(cat) if cat in CATS else category(cat or name)
    return dict(name=name, cat=cat,
                rate=int(rate), fmt=fmt, parts=parts, offset=offset, frames=frames, stride=stride, phase=phase, decay=decay,
                hash=zlib.crc32(pcm) & 0xFFFFFFFF)


def wav_rate(folder, base):
    """the rate of a WAV render shipped beside a chip (same name, any case, or in preview/)"""
    key = re.sub(r'[^a-z0-9]', '', base.lower())
    for d in (folder, os.path.join(folder, 'preview')):
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if f.lower().endswith('.wav') and re.sub(r'[^a-z0-9]', '', os.path.splitext(f)[0].lower()) == key:
                try:
                    with wave.open(os.path.join(d, f)) as w:
                        return w.getframerate()
                except (wave.Error, EOFError):
                    return None
    return None


# display names (or the two names of a two-sound chip) for the 8.3 names of the EPROM dumps
NAMES = {# 2.4.18: the Linn 9000's factory sound EPROMs (archive.org "Linn 9000 roms 1"): one sound a chip
         'bass01': 'Bass Drum', 'caba0125': 'Cabasa', 'clap0125': 'Hand Claps', 'cnga01': 'Conga', 'cowb0128': 'Cowbell',
         'hat0124': 'Hi-Hat', 'snar01': 'Snare', 'sstk01': 'Sidestick', 'tamb0128': 'Tambourine', 'tomm0125': 'Tom',
         'DXCR1F': 'DX Crash', 'PERID5': 'Ride (PE)', 'LNOISE': 'Long Noise', 'LONGHAT': 'Long Hat', 'DXTOM': 'DX Tom',
         'DXHATS': 'DX Hats', 'DXSHAKE': 'DX Shaker', 'FATSNARE': 'Fat Snare', 'KICK64': 'Kick 64',
         'REGSNAR': 'Regular Snare', 'TAMBRIM': ('Tambourine', 'Rimshot'), 'TIMBALE': 'Timbale', 'CONGA': 'Conga',
         'COWBELL': 'Cowbell', 'CLAVE915': 'Clave', 'CLIKCLAP': 'Click Clap', 'COWBCLAV': ('Cowbell', 'Clave'),
         'ELECSNAR': 'Electric Snare', 'ELKICK3': 'Electric Kick 3', 'ELSNAR3': 'Electric Snare 3',
         'ELTOM3': 'Electric Tom 3', 'OLCNGA': 'Old Conga', 'SHAKE1': ('Shaker 1', 'Shaker 2'), 'BONES': 'Bones',
         'SCRATCH': 'Scratch', 'SHOT': 'Shot', 'beatkick': 'Beat Kick', 'beatsnar': 'Beat Snare', 'punch': 'Punch',
         'sci_crash': 'Crash', 'sci_ride': 'Ride', 'AF_BELL': 'African Bell', 'AGOGO': 'Agogo',
         'CONGSLAP': 'Conga Slap', 'MOUTH': 'Mouth Click', 'SCI_BASS': 'Bass', 'SCI_HAT': 'Hi-Hat',
         'SCI_PERC1': ('Perc 1A', 'Perc 1B'), 'SCI_PERC2': ('Perc 2A', 'Perc 2B'), 'SCI_PERC1A': 'Perc 1A',
         'SCI_PERC1B': 'Perc 1B', 'SCI_PERC2A': 'Perc 2A', 'SCI_PERC2B': 'Perc 2B', 'SCI_RIM': 'Rimshot',
         'SCI_SNARE': 'Snare', 'SCI_SNRIM': ('Snare', 'Rimshot'), 'SCI_TOM': 'Tom', 'SNAPS': 'Snaps',
         'WOODBLK': 'Wood Block', 'conga_slap_ago_bell': ('Conga Slap', 'Agogo Bell'),
         'mouth_click_snaps': ('Mouth Click', 'Snaps'), 'wood_block_african_bell': ('Wood Block', 'African Bell'),
         'talking_drum': 'Talking Drum', 'timpani': 'Timpani', 'RIDECYM': 'Ride Cymbal', 'SFTCRSH': 'Soft Crash',
         'AM60SNAR': 'AM60 Snare', 'CONGMOOS': 'Conga Moos', 'DEEPSNAR': 'Deep Snare', 'EQ_KICK': 'EQ Kick',
         'EURKASNR': 'Eureka Snare', 'KLAK': 'Klak', 'LONGTAMB': 'Long Tambourine', 'MIDRMTOM': 'Mid Room Tom',
         'DigiDrums_ELSNR3': 'Electric Snare 3', 'DigiDrums_ELTOM3': 'Electric Tom 3',
         'DigiDrums_CLVAGO': ('Clave', 'Agogo'), 'beefykik': 'Beefy Kick', 'bunkik': 'Bun Kick',
         'doopkick': 'Doop Kick', 'hiphopkik': 'Hip Hop Kick', 'junglesnare': 'Jungle Snare',
         'policysnare': 'Policy Snare', 'resohi': 'Reso Hi', 'resokik': 'Reso Kick', 'wallbang': 'Wall Bang'}


def pretty(base):
    n = NAMES.get(base)
    if isinstance(n, str):
        return n
    s = re.sub(r'_(27|2)\d{2,3}$', '', base)                 # Simmons: "Fat Kick_2764"
    s = re.sub(r'^(Linndrum|DigiDrums)_', '', s, flags=re.I)
    s = s.replace('_', ' ').replace('-', ' ').strip()
    s = re.sub(r'\s+', ' ', s)
    return s if any(ch.islower() for ch in s) else s.title()


def is_split(fmt, data):
    """two sounds, one per half: the first half ends quiet and the second starts loud"""
    h = len(data) // 2
    if h < 1024:
        return False
    tail = max(abs(decode(fmt, b)) for b in data[h - 48:h])
    head = max(abs(decode(fmt, b)) for b in data[h:h + 64])
    return tail < 1500 and head > 6000


def continues(fmt, a, b):
    """chip b carries on chip a's sound: a does not end quiet"""
    return max(abs(decode(fmt, x)) for x in a[-64:]) > 1200


def guess_fmt(data):
    """DAC-76 data rests at 0x00 / 0x80 (+-0), unsigned 8-bit around 0x80 (0x7F, 0x81 ...), signed around 0x00 / 0xFF"""
    t = data[len(data) * 3 // 4:]
    dac = sum(1 for b in t if b in (0, 1, 0x80, 0x81))
    u8 = sum(1 for b in t if 0x7C <= b <= 0x84 and b not in (0x80, 0x81))
    return U8 if u8 > dac else DAC76


def folder_set(name, folder, fmt, rate, rates=None, splits=None, groups=(), skip=()):
    """a set of one-sound (or two-sound) chips, one file each; groups: name stems whose numbered / lettered chips
    are one sound"""
    files = sorted(f for f in os.listdir(folder) if os.path.isfile(os.path.join(folder, f)) and
                   (f.lower().endswith('.bin') or '.' not in f))
    known = {k for k in chips}          # chips an earlier set already describes (MAME's sets: better rates)
    used = set()
    out = []

    def fresh(f):
        return crc_key(open(os.path.join(folder, f), 'rb').read()) not in known

    files = [f for f in files if fresh(f)]
    for stem in groups:
        members = [f for f in files if re.fullmatch(re.escape(stem) + r'[0-9A-Ha-h]\.bin', f, re.I)]
        if not members:
            continue
        parts = [chip(os.path.join(folder, f)) for f in members]
        r = wav_rate(folder, stem) or rate
        out.append(sound(pretty(stem), None, r, fmt, parts))
        used.update(members)
    for f in files:
        if f in used or f in skip:
            continue
        c = chip(os.path.join(folder, f))
        base = os.path.splitext(f)[0]
        if len(set(c.data)) <= 2:                              # blank chips (silent4k.bin)
            continue
        r = (rates or {}).get(base) or wav_rate(folder, base) or rate
        f_fmt = guess_fmt(c.data) if fmt is None else fmt
        split = (splits or {}).get(base)
        if split is None and isinstance(NAMES.get(base), tuple):
            split = NAMES[base]
        if split is None:
            split = is_split(f_fmt, c.data)
        if split:
            h = c.size // 2
            na, nb = split if isinstance(split, tuple) else (pretty(base) + ' A', pretty(base) + ' B')
            ra = wav_rate(folder, base + 'A') or wav_rate(folder, base + '_A') or r
            rb = wav_rate(folder, base + 'B') or wav_rate(folder, base + '_B') or r
            out.append(sound(na, None, ra, f_fmt, [c], 0, h))
            out.append(sound(nb, None, rb, f_fmt, [c], h, h))
        else:
            out.append(sound(pretty(base), None, r, f_fmt, [c]))
    seen = set()                       # the same audio twice (a whole-chip dump beside its parts): keep the first
    out = [x for x in out if not (x['hash'] in seen or seen.add(x['hash']))]
    if out:
        sets.append([name, out])


def main():
    rom, coll = sys.argv[1], sys.argv[2]
    out_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), '..', 'src', 'formats',
                                                                    'drum_rom_table.inc')
    R = lambda f: chip(os.path.join(rom, f))
    C = lambda *p: os.path.join(coll, *p)

    # ---- LinnDrum (MAME linndrum; cabasa, claps and bass are not dumped) ----
    MUX, SN = 24836, 23286
    ld = [sound('Tambourine', 'tamb', MUX, DAC76, [R('tamb1.tamb.u25')]),
          sound('Cowbell', 'cowbell', MUX, DAC76, [R('cwbl1.cwbl.u28')]),
          sound('Closed Hi-Hat', 'hat_closed', MUX, DAC76, [R(f'hat1{x}.hat{i}.u{30 + i - 1}') for i, x in enumerate('abcd', 1)],
                decay=90),
          sound('Open Hi-Hat', 'hat_open', MUX, DAC76, [R(f'hat1{x}.hat{i}.u{30 + i - 1}') for i, x in enumerate('abcd', 1)]),
          sound('Ride', 'ride', MUX, DAC76, [R(f) for f in ['ride1a.rid1.u9', 'ride1b.rid2.u10', 'ride1c.rid3.u8',
                                                           'ride1d.rid4.u7', 'ride1e.rid5.u6', 'ride1f.rid6.u5',
                                                           'ride1g.rid7.u4', 'ride1h.rid8.u3']]),
          sound('Crash', 'crash', MUX, DAC76, [R(f) for f in ['crsh1a.crs1.u14', 'crsh1b.crs2.u13', 'crsh1c.crs3.u12',
                                                             'crsh1d.crs4.u15', 'crsh1e.crs5.u16', 'crsh1f.crs6.u17',
                                                             'crsh1g.crs7.u18', 'crsh1h.crs8.u11']]),
          sound('Snare', 'snare', SN, DAC76, [R('snar9.snar.u79')]),
          sound('Sidestick', 'rim', SN, DAC76, [R('sstk1.sstk.u78')])]
    tom = [R('tom6a.tom1.u68'), R('tom6b.tom2.u69')]
    conga = [R('cnga1a.cga1.u66'), R('cnga1b.cga2.u67')]
    ld += [sound('Lo Tom', 'tom', 34261, DAC76, tom), sound('Mid Tom', 'tom', 40827, DAC76, tom),
           sound('Hi Tom', 'tom', 47392, DAC76, tom),
           sound('Lo Conga', 'conga', 40827, DAC76, conga), sound('Hi Conga', 'conga', 42140, DAC76, conga)]
    sets.append(['LinnDrum', ld])

    # ---- Oberheim DMX (MAME obdmx) ----
    B, SNR, P2 = 27179, 40769, 19083
    TOMS = (27179, 31332, 36847)
    cym = [R(f) for f in ['crash1.u12', 'crash2.u11', 'crash3.u10', 'crash4.u9',
                          'ride2a1.u8', 'ride2a2.u7', 'ride2a3.u6', 'ride2a4.u5']]
    dmx = [sound('Bass', 'kick', B, DAC76, [R('21kick.u7')]), sound('Snare', 'snare', SNR, DAC76, [R('snare6.u7')]),
           sound('Closed Hi-Hat', 'hat_closed', B, DAC76, [R('hat1a.u7')], decay=60),
           sound('Open Hi-Hat', 'hat_open', B, DAC76, [R('hat1a.u7')])]
    for i, r in enumerate(TOMS):
        dmx.append(sound(f'Tom {4 + i}', 'tom', r, DAC76, [R('sfltom2.u7')]))
    for i, r in enumerate(TOMS):
        dmx.append(sound(f'Tom {1 + i}', 'tom', r, DAC76, [R('stom5.u7')]))
    dmx += [sound('Shaker', 'shaker', B, DAC76, [R('shake6.u7')], 0, 2048),
            sound('Tambourine', 'tamb', B, DAC76, [R('shake6.u7')], 2048, 2048),
            sound('Rimshot', 'rim', P2, DAC76, [R('stik.u7')], 2048, 2048),
            sound('Sidestick', 'rim', P2, DAC76, [R('stik.u7')], 0, 2048),
            sound('Crash', 'crash', B, DAC76, cym, 0, 16384), sound('Ride', 'ride', B, DAC76, cym, 16384, 16384)]
    sets.append(['Oberheim DMX', dmx])

    # ---- Roland TR-707 (MAME tr707) ----
    v = [R('hn61256p_c71_15179694.ic34'), R('hn61256p_c72_15179695.ic35')]
    tr = []
    for voice, names, cats in [(0, ('Bass Drum 1', 'Bass Drum 2'), ('kick', 'kick')),
                               (1, ('Snare Drum 1', 'Snare Drum 2'), ('snare', 'snare')),
                               (6, ('Rimshot', 'Cowbell'), ('rim', 'cowbell')),
                               (7, ('Handclap', 'Tambourine'), ('clap', 'tamb'))]:
        for ph in (0, 1):
            tr.append(sound(names[ph], cats[ph], 25000, U8NEG, v, voice << 13, 8192, 2, ph))
    for voice, nm in [(2, 'Low Tom'), (3, 'Mid Tom'), (4, 'Hi Tom')]:
        tr.append(sound(nm, 'tom', 12500, U8NEG, v, voice << 13, 8192))
    tr.append(sound('Closed Hi-Hat', 'hat_closed', 25000, U8NEG, v, 5 << 13, 8192, decay=90))
    tr.append(sound('Open Hi-Hat', 'hat_open', 25000, U8NEG, v, 5 << 13, 8192))
    tr.append(sound('Crash Cymbal', 'crash', 25000, U6, [R('hn61256p_c73_15179696.ic19')]))
    tr.append(sound('Ride Cymbal', 'ride', 25000, U6, [R('hn61256p_c74_15179697.ic22')]))
    sets.append(['Roland TR-707', tr])

    # ---- Casio RZ-1 (MAME rz1): the sound start addresses its program ROM gives the two uPD934G (program.bin
    # 0x3B8F: 10 for pg1, then 0x3BA3: 4 for pg0, 16-bit little-endian); a sound ends where the next starts ----
    a, b = R('sound_a.cm5'), R('sound_b.cm6')
    rz = []
    for chip_, starts, names in [
            (a, [0x0000, 0x0ECF, 0x1DD3, 0x2DCA, 0x3425, 0x40BD, 0x45D4, 0x4A9B, 0x7984, 0x7B7F],
             [('Tom 1', 'tom'), ('Tom 2', 'tom'), ('Tom 3', 'tom'), ('Bass Drum', 'kick'), ('Snare Drum', 'snare'),
              ('Rim Shot', 'rim'), ('Closed Hi-Hat', 'hat_closed'), ('Open Hi-Hat', 'hat_open'), ('Click', 'click')]),
            (b, [0x0000, 0x09CF, 0x403E, 0x47DD, 0x7FFF],
             [('Claps', 'clap'), ('Ride', 'ride'), ('Cowbell', 'cowbell'), ('Crash', 'crash')])]:
        for k, (nm, cat) in enumerate(names):
            rz.append(sound(nm, cat, 20000, S8, [chip_], starts[k], starts[k + 1] - starts[k]))
    rz.sort(key=lambda x: ['Bass Drum', 'Snare Drum', 'Rim Shot', 'Claps', 'Closed Hi-Hat', 'Open Hi-Hat', 'Tom 1',
                           'Tom 2', 'Tom 3', 'Ride', 'Crash', 'Cowbell', 'Click'].index(x['name']))
    sets.append(['Casio RZ-1', rz])

    # ---- archive.org dumps ----
    folder_set('Oberheim DMX (stock EPROMs)', C('Stock DMX Sounds EPROM'), DAC76, 24000,
               splits={'SHAKE6': ('Shaker', 'Tambourine'), 'STIK': ('Sidestick', 'Rimshot')})
    folder_set('Oberheim DMX cymbals', C('Standard DMX Cymbals Sound EPROM'), DAC76, 27179, groups=['CRASH', 'RIDE2A'])
    folder_set('Oberheim DMX new cymbals', C('Oberheim DMX New DMX Cymbals EPROM dump'), DAC76, 24000,
               groups=['DXCR1F', 'PERID5'])
    folder_set('Oberheim DX (stock)', C('Oberheim DMX Stock DX Sounds EPROM dump'), DAC76, 24000)
    folder_set('Oberheim DMX / DX extras', C('Oberheim DMX DMX and DX Other Sounds EPROM dump'), DAC76, 24000,
               groups=['LNOISE', 'LONGHAT', 'DXTOM'], skip=('silent4k.bin', 'silent8k.bin'))
    folder_set('Sequential Drumtraks', C('Sequential Circuits (SCI) Drumtraks Sounds EPROM dump'), DAC76, 24000,
               groups=['sci_crash', 'sci_ride'])
    folder_set('Drumware', C('Drumware - Sound ROMS', 'Drumware - Sound ROMS'), DAC76, 32000,
               groups=['LONGHAT', 'RIDECYM', 'SFTCRSH'])
    folder_set('DigiDrums', C('DigiDrums - Sound ROMS', 'DigiDrums - ROMS - Clv_Ago_Elsnare3_Eltom3'), DAC76, 32000,
               rates={'DigiDrums_CLVAGO': 32000}, splits={'DigiDrums_CLVAGO': ('Clave', 'Agogo')})
    folder_set('LinnDrum (factory EPROMs)', C('Linndrum Factory ROMs'), DAC76, MUX,
               rates={'Linndrum_SNARE9': SN, 'Linndrum_SIDESTICK1': SN},
               groups=['Linndrum_CRASH1', 'Linndrum_RIDE1', 'Linndrum_HAT1', 'Linndrum_TOM6', 'Linndrum_CONGA1'])
    folder_set('Linn 9000 (user EPROMs)', C('Linn 9000 roms 2'), None, 28000)
    # 2.4.18: the Linn 9000's factory EPROMs ("Linn 9000 roms 1" on archive.org: not in the collection folder here, so
    # beside it or at $LINN9000_FACTORY; left out when not found). The snare chip is a LinnDrum chip already known.
    l9f = os.environ.get('LINN9000_FACTORY') or C('Linn 9000  roms 1')
    if not os.path.isdir(l9f):
        l9f = '/workspaces/MPC_Mod/_toolcache/archive-vsfc/ex/Linn 9000  roms 1'
    if os.path.isdir(l9f):
        folder_set('Linn 9000 (factory EPROMs)', l9f, None, 28000)
    folder_set('Simmons SDS EPROMs', C('Simmons - Sound ROMS', 'Simmons - Sound ROMS'), U8, 25000)
    dru = C('Emu Drumulator Sound EPROM Collection')
    for kit in sorted(os.listdir(dru)):
        if os.path.isdir(os.path.join(dru, kit)):
            folder_set('Drumulator ' + re.sub(r'^\d+-', '', kit), os.path.join(dru, kit), DAC76, 27000, splits={})
    # Solton Programmer 24: the mapping sheet, in 2 KB blocks
    sol = [chip(C('Solton_Programmer24-Sample_ROMs', f'Solton_P24_Sample_ROM{i}.BIN')) for i in range(1, 5)]
    st = []
    for rom_i, blocks in enumerate([[(1, 1, 'Bass'), (2, 2, 'Snare'), (4, 1, 'Foot Hat'), (5, 1, 'Rim Shot'),
                                     (6, 2, 'Clap')],
                                    [(0, 1, 'Closed Hat'), (1, 1, 'Clave'), (2, 2, 'Cowbell'), (4, 4, 'Open Hat')],
                                    [(0, 2, 'Low Conga'), (2, 2, 'Hi Conga'), (4, 2, 'Low Tom'), (6, 2, 'Hi Tom')],
                                    [(0, 8, 'Ride')]]):
        for blk, nblk, nm in blocks:
            r = 12250 if re.search('conga|tom', nm, re.I) else 24500
            st.append(sound(nm, None, r, U8, [sol[rom_i]], blk * 2048, nblk * 2048))
    sets.append(['Solton Programmer 24', st])

    # ---- Roland TR-505 (the decoded dump) and TR-626 (the raw dump or the unscrambled one) ----
    tr505 = [('Timbale', 'timbale', 0x0000, 8192), ('Low Tom', 'tom', 0x2000, 8192), ('High Tom', 'tom', 0x4000, 8192),
             ('Mid Tom', 'tom', 0x6000, 8192), ('Bass Drum', 'kick', 0x8000, 4096), ('Low Conga', 'conga', 0x9000, 4096),
             ('Snare Drum', 'snare', 0xA000, 4096), ('High Conga', 'conga', 0xB000, 4096),
             ('Hand Clap', 'clap', 0xC000, 4096), ('Low Cowbell', 'cowbell', 0xD000, 4096),
             ('Rimshot', 'rim', 0xE000, 4096), ('High Cowbell', 'cowbell', 0xF000, 4096),
             ('Crash Cymbal', 'crash', 0x10000, 32768), ('Ride Cymbal', 'ride', 0x18000, 16384),
             ('Open Hi-Hat', 'hat_open', 0x1C000, 8192), ('Closed Hi-Hat', 'hat_closed', 0x1E000, 8192)]
    c505 = chip(C('Roland TR-505_Sound_Rom', 'TR-505_RawRomDump_Decoded.BIN'))
    sets.append(['Roland TR-505', [sound(n, cat, 25000, U8, [c505], off, ln) for n, cat, off, ln in tr505]])
    tr626 = [('Open High Conga', 'conga', 0x0000, 8192), ('Low Tom 1', 'tom', 0x2000, 8192),
             ('High Tom 1', 'tom', 0x4000, 8192), ('Mid Tom 1', 'tom', 0x6000, 8192),
             ('Hand Claps', 'clap', 0x8000, 4096), ('Cowbell', 'cowbell', 0x9000, 4096),
             ('Claves', 'clave', 0xA000, 4096), ('Tambourine', 'tambourine', 0xB000, 4096),
             ('Rimshot', 'rim', 0xC000, 4096), ('Bass Drum 1', 'kick', 0xD000, 4096),
             ('Open Hi-Hat', 'hat_open', 0xE000, 8192), ('Crash Cymbal', 'crash', 0x10000, 32768),
             ('Ride Cymbal', 'ride', 0x18000, 16384), ('Snare Drum 1', 'snare', 0x1C000, 8192),
             ('Snare Drum 2', 'snare', 0x1E000, 8192), ('Low Conga', 'conga', 0x20000, 8192),
             ('Low Tom 2', 'tom', 0x22000, 8192), ('High Tom 2', 'tom', 0x24000, 8192), ('Mid Tom 2', 'tom', 0x26000, 8192),
             ('Mute High Conga', 'conga', 0x28000, 4096), ('Low Agogo', 'agogo', 0x29000, 4096),
             ('Shaker', 'shaker', 0x2A000, 4096), ('High Agogo', 'agogo', 0x2B000, 4096),
             ('Snare Drum 3', 'snare', 0x2C000, 4096), ('Bass Drum 2', 'kick', 0x2D000, 4096),
             ('Closed Hi-Hat', 'hat_closed', 0x2E000, 8192), ('China Cymbal', 'china', 0x30000, 32768),
             ('Ride Cup', 'ride', 0x38000, 16384), ('Low Timbale', 'timbale', 0x3C000, 8192),
             ('High Timbale', 'timbale', 0x3E000, 8192)]
    for title, c626 in (('Roland TR-626', chip(C('Roland TR-626_Sound_Rom', 'TR-626_Sound_Rom_UnScrambled.bin'))),
                        ('Roland TR-626 (raw ROM)', chip(C('Roland TR-626_Sound_Rom', 'TR-626_Sound_Rom.bin'), XF_TR626))):
        sets.append([title, [sound(n, cat, 25000, U8, [c626], off, ln) for n, cat, off, ln in tr626]])

    # ---- Yamaha PSR-60 / PSR-70: YM21908 + YM21909 (start / end 64-byte blocks) ----
    psr = [(R('ym21908.bin'), [('Ride', 'ride', 0x000, 0x0F7), ('HH Open', 'hat_open', 0x0F8, 0x157),
                               ('HH Closed', 'hat_closed', 0x158, 0x163), ('SD Hi', 'snare', 0x164, 0x173),
                               ('SD Lo', 'snare', 0x174, 0x18F), ('Rimshot', 'rim', 0x190, 0x197),
                               ('BD', 'kick', 0x198, 0x1A7), ('Tom 1', 'tom', 0x1A8, 0x1CF), ('Tom 2', 'tom', 0x1D0, 0x1FF)]),
           (R('ym21909.bin'), [('Tom 3', 'tom', 0x000, 0x077), ('Bongo 1', 'bongo', 0x078, 0x087),
                               ('Bongo 2', 'bongo', 0x088, 0x08F), ('Bongo 3', 'bongo', 0x090, 0x0AF),
                               ('Claves', 'clave', 0x0B0, 0x0BB), ('Timbale Hi', 'timbale', 0x0BC, 0x0EB),
                               ('Timbale Lo', 'timbale', 0x0EC, 0x11B), ('Cuica Hi', 'cuica', 0x11C, 0x137),
                               ('Cuica Lo', 'cuica', 0x138, 0x187), ('Agogo Hi', 'agogo', 0x188, 0x19B),
                               ('Agogo Lo', 'agogo', 0x19C, 0x1EB), ('Claps', 'clap', 0x1EC, 0x1FF)])]
    sets.append(['Yamaha PSR-60 / PSR-70', [sound(n, cat, 20833, RYP4, [c], a * 64, (b - a + 1) * 64)
                                            for c, snd in psr for n, cat, a, b in snd]])

    write(out_path)


def write(out_path):
    order = sorted(chips.values(), key=lambda c: (c.crc, c.size))
    # sounds name their chips by index into one list of chip runs (a multi-chip sound's chips one after another)
    runs, run_of = [], {}
    for _, snd in sets:
        for s in snd:
            key = tuple(id(c) for c in s['parts'])
            if key not in run_of:
                run_of[key] = len(runs)
                runs.append(s['parts'])
    for i, c in enumerate(order):
        c.id = i
    lines = ['// Generated by tests/gen_drum_roms.py: the vintage drum machine sound chips Omni Sampler knows (by the',
             '// CRC-32 of their first 4 KB and their size) and the drum sounds each holds. Numbers and names only; the',
             '// audio is read from the user\'s chip dumps. Sounds: name, category (gm_keys in drum_rom.cpp), format, stride, phase,',
             '// rate (Hz), chip run, byte offset in the run, frames, closed-hat decay (ms, 0 = none). Chips: CRC, size,',
             '// transform on load (1: the raw TR-626 mask ROM laid out plainly).',
             f'const DrumChip DRUM_CHIPS[{len(order)}] = {{']
    for c in order:
        lines.append(f'    {{0x{c.crc:08X}, {c.size}, {c.xform}}},  // {os.path.basename(c.path)}')
    lines.append('};')
    flat = []
    for r in runs:
        flat += [c.id for c in r]
    lines.append(f'const uint16_t DRUM_RUN_CHIPS[{len(flat)}] = {{')
    for i in range(0, len(flat), 16):
        lines.append('    ' + ', '.join(str(x) for x in flat[i:i + 16]) + ',')
    lines.append('};')
    lines.append(f'const DrumRun DRUM_RUNS[{len(runs)}] = {{')
    pos = 0
    for r in runs:
        lines.append(f'    {{{pos}, {len(r)}}},')
        pos += len(r)
    lines.append('};')
    total = sum(len(s) for _, s in sets)
    lines.append(f'const DrumSound DRUM_SOUNDS[{total}] = {{')
    first = []
    for si, (name, snd) in enumerate(sets):
        first.append(len([1 for _, s in sets[:si] for _ in s]))
        names_used = set()
        for s in snd:
            run = run_of[tuple(id(c) for c in s['parts'])]
            nm = s['name'].replace('"', "'")
            k = 2
            while nm in names_used:
                nm = f"{s['name']} {k}"
                k += 1
            names_used.add(nm)
            lines.append(f'    {{"{nm}", {s["cat"]}, {FMT_NAMES[s["fmt"]]}, {s["stride"]}, {s["phase"]}, {s["rate"]}, '
                         f'{run}, {s["offset"]}, {s["frames"]}, {s["decay"]}}},')
    lines.append('};')
    lines.append(f'const DrumSet DRUM_SETS[{len(sets)}] = {{')
    for si, (name, snd) in enumerate(sets):
        lines.append(f'    {{"{name}", {first[si]}, {len(snd)}}},')
    lines.append('};')
    open(out_path, 'w').write('\n'.join(lines) + '\n')
    print(f'{out_path}: {len(order)} chips, {len(runs)} runs, {total} sounds, {len(sets)} sets')
    for name, snd in sets:
        print(f'  {name}: ' + ', '.join(f"{s['name']}({CATS[s['cat']]},{s['rate']},{s['frames']})" for s in snd))


if __name__ == '__main__':
    main()

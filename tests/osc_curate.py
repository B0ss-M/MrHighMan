#!/usr/bin/env python3
"""Omni Sampler: curate oscx's single cycles into the Oscillators folder's "ROM Waves".
  osc_curate.py <oscx out dir> <oscx log dir> <dest dir>
Keeps a wave only when it has a real fundamental (a quarter of its energy in harmonics 1-8, its first strong harmonic
at most the 3rd: a dense loop played as one cycle would sound octaves too high or as noise); drops near-copies across
the sets (the Roland GM ROMs share most waves); caps each set, keeping the most varied (farthest-point picks); names
address-only samples ("Sample 228400") after their preset. Writes <dest>/<set>/<NNN name>.wav (16-bit, 2048, clm)."""
import os, re, sys, shutil, struct, wave
import numpy as np

SRC, LOGS, DEST = sys.argv[1:4]
# set order = priority (the synth ROMs first: their waves win a near-copy), cap per set
SETS = [("Roland D-50", 40), ("Roland JD-800", 120), ("Korg DW-8000 EX-8000", 70), ("Korg M1", 110),
        ("Korg Wavestation EX", 120), ("Roland D-110", 10), ("Roland D-70", 80), ("Ensoniq SD-1 VFX", 80),
        ("Ensoniq SQ-1", 10), ("Ensoniq KT-76", 70), ("Ensoniq MR Rack", 50), ("Kurzweil K2000", 40),
        ("Roland MT-32", 0), ("Roland CM-32L", 0), ("Roland SC-55mkII", 50), ("Roland SC-88", 60),
        ("Roland SC-88VL", 20), ("Roland VE-GS Pro", 50), ("Roland XP-GS", 15), ("E-mu EXB Banks", 80)]
# (2.3.5: the Oscillators list holds 4096 entries; the user's publicsamples take ~2970, so ROM Waves stay near 1050)
SHORT = {"Korg DW-8000 EX-8000": "Korg DW-8000"}


def read(path):
    w = wave.open(path)
    n = w.getnframes()
    return np.array(struct.unpack("<%dh" % n, w.readframes(n)), dtype=np.float64) / 32768


def write(path, y):
    clm = b"<!>2048 00000000 wavetable (Omni oscx)"
    data = struct.pack("<%dh" % len(y), *np.clip(np.round(y * 32767), -32768, 32767).astype(int))
    body = b"WAVE" + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, 44100, 88200, 2, 16) + b"clm " + struct.pack("<I", len(clm)) + clm + \
        b"data" + struct.pack("<I", len(data)) + data
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def features(y):
    m = np.abs(np.fft.rfft(y))[1:1024]
    e = m ** 2
    tot = e.sum()
    if tot <= 0:
        return None
    first = int(np.argmax(m > 0.1 * m.max())) + 1
    low = e[:8].sum() / tot
    v = m[:64] / (np.linalg.norm(m[:64]) or 1)
    return first, low, v


def preset_names(set_name):
    """oscx's log: '<NNN> <name>  loop ..., <preset>' -> {file number: preset}"""
    out = {}
    try:
        for line in open(os.path.join(LOGS, set_name + ".txt"), errors="replace"):
            m = re.match(r"(\d{3}) .*harmonics +\d+, (.*)$", line.rstrip("\n"))
            if m:
                out[int(m.group(1))] = m.group(2).strip()
    except OSError:
        pass
    return out


def clean(s):
    s = re.sub(r"[^A-Za-z0-9 +\-._]", " ", s)
    s = re.sub(r"^(P\d\d|Pf\d\d|[0-9]{2,3}(\.[0-9]{3})?)\s+", "", s.strip())   # bank numbers off the front
    return re.sub(r"\s+", " ", s).strip()[:36] or "Wave"


kept_vecs = []
total = 0
if os.path.isdir(DEST):
    shutil.rmtree(DEST)
for set_name, cap in SETS:
    d = os.path.join(SRC, set_name)
    if not os.path.isdir(d) or cap == 0:
        continue
    presets = preset_names(set_name)
    cands = []
    rej = dup = 0
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".wav"):
            continue
        y = read(os.path.join(d, fn))
        f = features(y)
        if not f or f[0] > 3 or f[1] < 0.25:
            rej += 1
            continue
        if kept_vecs and max(float(np.dot(f[2], k)) for k in kept_vecs[-3000:]) > 0.985:
            dup += 1
            continue
        num = int(fn[:3])
        name = fn[4:-4]
        if re.match(r"^(Sample|Wave|Smpl)\s*\d+", name) or re.fullmatch(r"[\d .]+", name):
            name = presets.get(num, name)
        cands.append((y, f[2], clean(name)))
    # the most varied first (farthest-point), then the set's own order for those kept
    picked = []
    if cands:
        V = np.array([c[1] for c in cands])
        picked = [0]
        dist = 1 - V @ V[0]
        while len(picked) < min(cap, len(cands)):
            i = int(np.argmax(dist))
            if dist[i] <= 0.004:
                break
            picked.append(i)
            dist = np.minimum(dist, 1 - V @ V[i])
        picked.sort()
    out = os.path.join(DEST, SHORT.get(set_name, set_name))
    os.makedirs(out, exist_ok=True)
    names = set()
    for n, i in enumerate(picked, 1):
        y, v, name = cands[i]
        base, k = name, 2
        while base.lower() in names:
            base, k = "%s %d" % (name, k), k + 1
        names.add(base.lower())
        write(os.path.join(out, "%03d %s.wav" % (n, base)), y)
        kept_vecs.append(v)
    total += len(picked)
    print("%-22s %4d kept  (%d not single-cycle, %d copies of earlier sets, %d over the cap)" %
          (set_name, len(picked), rej, dup, len(cands) - len(picked)))
print("total", total)

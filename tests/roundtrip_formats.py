#!/usr/bin/env python3
"""2.4.9: every supported format through a project round trip (the user: "ensure all supported formats load and save
correctly", after ISO instruments went missing from reopened projects).

For each file extension a reader or disk image type takes, up to N real files from the library folders: load the file
the way a project does (test_engine: state -> loader), save the project's state, restore it in a fresh instance and
check that the same instrument comes back. Disk images: a preset inside (found with probe). Run twice: without a
Plugin Library, and with one (Auto Extract: the project then points at the extracted .omni).
One test_engine at a time (the container has run out of memory before).

  python3 -I tests/roundtrip_formats.py [--per-ext N] [--roots dir ...] [--out report.tsv]
"""
import argparse, os, re, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
X86 = os.path.join(PORT, "build", "x86")
ROOTS = ["/workspaces/MPC_Mod/omni_sampler", "/workspaces/MPC_Mod/_toolcache/omni-fixes/testdata"]
IMAGE_EXTS = {"iso", "img", "hfe", "imd", "td0", "cue", "hds", "efe_disk", "eiii", "e4", "dsk", "ima", "raw"}
SKIP_EXTS = {"txt", "pdf", "jpg", "png", "gif", "htm", "html", "json", "md", "nfo", "url", "doc", "exe", "csv_", "js",
             "zip", "rar", "7z", "cab", "sfv", "mid", "py", "log", "xml", "ini", "dll", "vst3"}
MAX_BYTES = 80 << 20


def reader_exts():
    exts = set()
    for root, _, files in os.walk(os.path.join(PORT, "src", "formats")):
        for f in files:
            if f.endswith(".cpp"):
                for m in re.finditer(r'register_reader\(\{"[^"]+", "([^"]*)"', open(os.path.join(root, f), errors="replace").read()):
                    exts.update(m.group(1).split())
    return exts


def run(cmd, env=None, timeout=300):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=timeout, env=env)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return -9, "TIMEOUT"


def probe_list(path):
    rc, out = run([os.path.join(X86, "probe"), path], timeout=120)
    entries = []
    for line in out.splitlines():
        m = re.match(r"^([d-])\s+\d+\s+(.+)$", line)
        if m:
            entries.append((m.group(1) == "d", m.group(2)))
    return entries


def inner_files(image, want=3, depth=0):
    """a few files inside a disk image (depth first, the first entries)"""
    out = []
    if depth > 4:
        return out
    for is_dir, name in probe_list(image)[:40]:
        p = image + "/" + name
        if is_dir:
            out += inner_files(p, want - len(out), depth + 1)
        else:
            out.append(p)
        if len(out) >= want:
            break
    return out


def collect(roots, per_ext, exts):
    found = {}
    for root in roots:
        for d, dirs, files in os.walk(root):
            dirs.sort()
            for f in sorted(files):
                e = f.rsplit(".", 1)[-1].lower() if "." in f else ""
                if not e or e in SKIP_EXTS or (e not in exts and e not in IMAGE_EXTS):
                    continue
                if len(found.setdefault(e, [])) >= per_ext:
                    continue
                p = os.path.join(d, f)
                try:
                    if os.path.getsize(p) > MAX_BYTES or os.path.getsize(p) == 0:
                        continue
                except OSError:
                    continue
                found[e].append(p)
    return found


def round_trip(path, lib):
    env = dict(os.environ)
    if lib:
        env["OMNI_SAMPLER_LIBRARY"] = lib
    else:
        env.pop("OMNI_SAMPLER_LIBRARY", None)
    rc, out = run([os.path.join(X86, "test_engine"), path, "0", "60"], env=env)
    loaded = re.search(r"^loaded in [\d.]+s: (?:\d+/\d+ )?A: (.*)$", out, re.M)   # (a multi-preset file: "1/2 A: ...")
    rt = re.search(r"^state round trip: (\S+)", out, re.M)
    if rc == -9:
        return "TIMEOUT", ""
    if rc != 0 and not rt:
        return "CRASH(%d)" % rc, out.strip().splitlines()[-1][:120] if out.strip() else ""
    if not loaded or loaded.group(1).startswith("(empty)") or "Error" in (loaded.group(1) if loaded else ""):
        err = re.search(r"Error: ([^\n|]*)", out)
        return "NOLOAD", (err.group(1) if err else (loaded.group(1) if loaded else "?"))[:120]
    if not rt or rt.group(1) != "ok":
        restored = re.search(r"^restored: (.*)$", out, re.M)
        return "LOST", (restored.group(1) if restored else "?")[:120]
    return "ok", loaded.group(1)[:80]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-ext", type=int, default=2)
    ap.add_argument("--roots", nargs="*", default=ROOTS)
    ap.add_argument("--out", default=os.path.join(PORT, "..", "roundtrip-formats.tsv"))
    a = ap.parse_args()
    exts = reader_exts()
    files = collect(a.roots, a.per_ext, exts)
    print("extensions with files: %d (readers know %d)" % (len(files), len(exts)), flush=True)
    rows, bad = [], 0
    fo = open(a.out, "w")   # written as it goes (a crash keeps what ran)
    fo.write("ext\tmode\tresult\tsecs\tfile\tinfo\n")
    for e in sorted(files):
        for f in files[e]:
            targets = [f]
            if e in IMAGE_EXTS:
                targets = inner_files(f) or [f]
            for t in targets[:1] if e not in IMAGE_EXTS else targets:
                for mode in ("nolib", "lib"):
                    lib = None
                    tmp = None
                    if mode == "lib":
                        tmp = tempfile.mkdtemp(prefix="omni-rt-")
                        lib = os.path.join(tmp, "library")
                        os.makedirs(lib)
                    t0 = time.time()
                    res, info = round_trip(t, lib)
                    if tmp:
                        shutil.rmtree(tmp, ignore_errors=True)
                    rows.append((e, mode, res, "%.1f" % (time.time() - t0), t, info))
                    fo.write("\t".join(rows[-1]) + "\n"); fo.flush()
                    if res not in ("ok", "NOLOAD"):
                        bad += 1
                    print("%-6s %-5s %-8s %s | %s" % (e, mode, res, t[-90:], info), flush=True)
                if e in IMAGE_EXTS and res == "ok":
                    break   # one working preset per image is enough
    fo.close()
    noload = sum(1 for r in rows if r[2] == "NOLOAD")
    print("\n%d runs: %d ok, %d not loadable (not a reader's file / no preset), %d LOST / CRASH / TIMEOUT" %
          (len(rows), sum(1 for r in rows if r[2] == "ok"), noload, bad))
    print("report: %s" % os.path.abspath(a.out))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

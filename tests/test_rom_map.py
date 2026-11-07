"""Exercise the registered ROM reader through the normal desktop probe."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def main():
    probe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        image = bytes([0, 0x80, 0x7f, 0xff]) + struct.pack(">hh", -30000, 500) + struct.pack("<hh", 12, -25000)
        (root / "waves.bin").write_bytes(image)
        zone = dict(offset=0, frames=4, channels=1, rate=31250, codec="s8",
                    root=60, key_lo=48, key_hi=72, loop_start=1, loop_end=3)
        data = {"version": 1, "image": "waves.bin", "presets": [
            {"name": "Eight", "zones": [zone]},
            {"name": "Sixteen", "zones": [
                dict(offset=4, frames=1, channels=2, rate=32000, codec="s16be"),
                dict(offset=8, frames=2, channels=1, rate=44100, codec="s16le"),
            ]},
        ]}
        path = root / "fixture.omnirom"

        def run(*args):
            path.write_text(json.dumps(data), encoding="utf-8")
            return subprocess.run([str(probe), str(path), *args], text=True, capture_output=True)

        result = run("-all", "-d")
        assert result.returncode == 0, result.stdout + result.stderr
        for expected in ("2 presets", "key 48-72 root 60", "loop fwd 1-3",
                         "31250Hz 1ch 4 frames peak 32768", "32000Hz 2ch 1 frames peak 30000",
                         "44100Hz 1ch 2 frames peak 25000"):
            assert expected in result.stdout, result.stdout
        zone["frames"] = 999
        result = run("0", "-d")
        assert result.returncode == 1 and "outside the image" in result.stdout, result.stdout
        zone["frames"] = 4
        data["image"] = "../waves.bin"
        result = run("0", "-d")
        assert result.returncode == 1 and "filename beside the map" in result.stdout, result.stdout
    print("Omni ROM map: registration, PCM decoding, mapping, loops and invalid ranges passed")


if __name__ == "__main__":
    main()

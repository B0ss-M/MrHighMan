#!/usr/bin/env python3
"""Generate bounded ZIP reader fixtures under _toolcache/omni-fixes/testdata/zip/."""
import pathlib
import struct
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[2] / "testdata" / "zip"
WAV = (
    b"RIFF"
    + struct.pack("<I", 40)
    + b"WAVEfmt "
    + struct.pack("<IHHIIHH", 16, 1, 1, 44100, 88200, 2, 16)
    + b"data"
    + struct.pack("<I", 4)
    + struct.pack("<hh", 12000, -12000)
)


def write_standard(path, compression):
    with zipfile.ZipFile(path, "w", compression=compression) as archive:
        archive.writestr("a.wav", WAV)
        archive.writestr("sub/b.wav", WAV)
        archive.writestr("empty/", b"")


def force_zip64_directory(path):
    data = bytearray(path.read_bytes())
    eocd = data.rfind(b"PK\x05\x06")
    if eocd < 0:
        raise RuntimeError("ZIP64 fixture has no classic end record")
    _, disk, directory_disk, disk_count, count, directory_size, directory_offset, comment = struct.unpack_from(
        "<IHHHHIIH", data, eocd
    )
    if disk or directory_disk or disk_count != count:
        raise RuntimeError("unexpected multi-disk ZIP fixture")
    zip64_offset = eocd
    zip64 = struct.pack(
        "<IQHHIIQQQQ",
        0x06064B50,
        44,
        45,
        45,
        0,
        0,
        count,
        count,
        directory_size,
        directory_offset,
    )
    locator = struct.pack("<IIQI", 0x07064B50, 0, zip64_offset, 1)
    classic = bytearray(data[eocd : eocd + 22 + comment])
    struct.pack_into("<HHII", classic, 8, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
    path.write_bytes(data[:eocd] + zip64 + locator + classic)


def force_zip64_entry_extra(path):
    data = bytearray(path.read_bytes())
    locator = data.rfind(b"PK\x06\x07")
    if locator < 0:
        raise RuntimeError("ZIP64 fixture has no locator")
    zip64_offset = struct.unpack_from("<Q", data, locator + 8)[0]
    directory_size = struct.unpack_from("<Q", data, zip64_offset + 40)[0]
    directory_offset = struct.unpack_from("<Q", data, zip64_offset + 48)[0]
    central = data.find(b"PK\x01\x02", directory_offset)
    if central < 0 or data[central + 46 : central + 51] != b"a.wav":
        raise RuntimeError("ZIP64 fixture central entry not found")
    name_len, extra_len = struct.unpack_from("<HH", data, central + 28)
    extra_at = central + 46 + name_len + extra_len
    extra = struct.pack("<HHQQ", 1, 16, len(WAV), len(WAV))
    struct.pack_into("<II", data, central + 20, 0xFFFFFFFF, 0xFFFFFFFF)
    struct.pack_into("<H", data, central + 30, extra_len + len(extra))
    data[extra_at:extra_at] = extra
    locator = data.rfind(b"PK\x06\x07")
    zip64_offset += len(extra)
    struct.pack_into("<Q", data, locator + 8, zip64_offset)
    struct.pack_into("<Q", data, zip64_offset + 40, directory_size + len(extra))
    path.write_bytes(data)


def patch_name_to_cp437(path):
    data = bytearray(path.read_bytes())
    old = b"Caf_.wav"
    new = b"Caf\x82.wav"
    for signature, flag_offset, name_offset, length_offset in (
        (b"PK\x03\x04", 6, 30, 26),
        (b"PK\x01\x02", 8, 46, 28),
    ):
        at = data.find(signature)
        if at < 0 or data[at + name_offset : at + name_offset + len(old)] != old:
            raise RuntimeError("CP437 fixture header not found")
        flags = struct.unpack_from("<H", data, at + flag_offset)[0] & ~0x0800
        struct.pack_into("<H", data, at + flag_offset, flags)
        name_len = struct.unpack_from("<H", data, at + length_offset)[0]
        if name_len != len(old):
            raise RuntimeError("unexpected CP437 fixture name length")
        data[at + name_offset : at + name_offset + name_len] = new
    path.write_bytes(data)


def patch_encrypted_flag(path):
    data = bytearray(path.read_bytes())
    for signature, flag_offset in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):
        at = data.find(signature)
        if at < 0:
            raise RuntimeError("encrypted fixture header not found")
        flags = struct.unpack_from("<H", data, at + flag_offset)[0] | 1
        struct.pack_into("<H", data, at + flag_offset, flags)
    path.write_bytes(data)


def corrupt_first_payload(path):
    data = bytearray(path.read_bytes())
    local = data.find(b"PK\x03\x04")
    if local < 0:
        raise RuntimeError("bad-CRC fixture has no local header")
    name_len, extra_len = struct.unpack_from("<HH", data, local + 26)
    payload = local + 30 + name_len + extra_len
    central = data.find(b"PK\x01\x02")
    compressed = struct.unpack_from("<I", data, central + 20)[0]
    if not compressed:
        raise RuntimeError("bad-CRC fixture has empty compressed data")
    data[payload + compressed // 2] ^= 0x40
    path.write_bytes(data)


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    (ROOT / "expected.wav").write_bytes(WAV)
    with zipfile.ZipFile(ROOT / "empty.zip", "w"):
        pass
    write_standard(ROOT / "stored.zip", zipfile.ZIP_STORED)
    write_standard(ROOT / "deflate.zip", zipfile.ZIP_DEFLATED)

    zip64 = ROOT / "zip64.zip"
    with zipfile.ZipFile(zip64, "w", allowZip64=True) as archive:
        info = zipfile.ZipInfo("a.wav")
        with archive.open(info, "w", force_zip64=True) as output:
            output.write(WAV)
        archive.writestr("sub/b.wav", WAV)
        archive.writestr("empty/", b"")
    force_zip64_directory(zip64)
    force_zip64_entry_extra(zip64)

    with zipfile.ZipFile(ROOT / "utf8.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("Ünïcode drum.wav", WAV)
    cp437 = ROOT / "cp437.zip"
    with zipfile.ZipFile(cp437, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("Caf_.wav", WAV)
    patch_name_to_cp437(cp437)

    with zipfile.ZipFile(ROOT / "traversal.zip", "w") as archive:
        archive.writestr("../evil.wav", WAV)
        archive.writestr("/abs.wav", WAV)
        archive.writestr("safe.wav", WAV)

    badcrc = ROOT / "badcrc.zip"
    write_standard(badcrc, zipfile.ZIP_DEFLATED)
    corrupt_first_payload(badcrc)
    truncated = ROOT / "truncated.zip"
    truncated.write_bytes((ROOT / "deflate.zip").read_bytes()[: len((ROOT / "deflate.zip").read_bytes()) // 2])

    with zipfile.ZipFile(ROOT / "bomb.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("bomb.bin", bytes(100 * 1024 * 1024))
    with zipfile.ZipFile(ROOT / "many.zip", "w", allowZip64=True) as archive:
        for i in range(200000):
            archive.writestr("e%06d" % i, b"")

    encrypted = ROOT / "encrypted.zip"
    write_standard(encrypted, zipfile.ZIP_DEFLATED)
    patch_encrypted_flag(encrypted)
    (ROOT / "garbage.zip").write_bytes(b"PK\x03\x04" + bytes(range(1, 129)))
    print("ZIP fixtures written to", ROOT)


if __name__ == "__main__":
    main()

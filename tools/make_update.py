#!/usr/bin/env python3
"""Pack firmware and storage image into one update package.

The format is described in main/update_pkg.h, which is also what reads it.
The storage image is a whole partition, half of it erased flash; the erased
tail is cut off here and restored by the device erasing what it did not write.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"O3EUPD1\0"
HEADER = 64
BLOCK = 4096
APP_DESC_MAGIC = 0xABCD5432


def app_version(image: bytes) -> str:
    """The version string out of the image's application descriptor."""
    magic, = struct.unpack_from("<I", image, 32)
    if image[0] != 0xE9 or magic != APP_DESC_MAGIC:
        raise SystemExit("the firmware file is not an ESP32 application image")
    return image[48:80].split(b"\0")[0].decode("utf-8", "replace")


def trim(storage: bytes) -> bytes:
    """Cut the erased tail, down to a whole flash block."""
    end = len(storage.rstrip(b"\xff"))
    end = (end + BLOCK - 1) // BLOCK * BLOCK
    return storage[:max(end, BLOCK)]


def pack(firmware: bytes, storage: bytes, version: str | None = None) -> bytes:
    ver = (version or app_version(firmware)).encode()[:31]
    st = trim(storage)
    head = struct.pack("<8s32sIIIII", MAGIC, ver,
                       len(firmware), zlib.crc32(firmware),
                       len(st), zlib.crc32(st), len(storage))
    head += struct.pack("<I", zlib.crc32(head))
    assert len(head) == HEADER
    return head + firmware + st


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--firmware", type=Path, default=root / "build" / "open3e-gateway.bin")
    ap.add_argument("--storage", type=Path, default=root / "build" / "storage.bin")
    ap.add_argument("--version", default=None,
                    help="override the version read from the firmware (tests)")
    ap.add_argument("-o", "--output", type=Path,
                    default=root / "build" / "open3e-update.bin")
    a = ap.parse_args()
    for f in (a.firmware, a.storage):
        if not f.exists():
            print(f"{f} missing -- run idf.py build first", file=sys.stderr)
            return 1
    fw, st = a.firmware.read_bytes(), a.storage.read_bytes()
    pkg = pack(fw, st, a.version)
    a.output.write_bytes(pkg)
    ver = pkg[8:40].split(b"\0")[0].decode()
    print(f"{a.output}: {ver}, {len(pkg) / 1024:.0f} KiB "
          f"(firmware {len(fw) / 1024:.0f}, storage {(len(pkg) - HEADER - len(fw)) / 1024:.0f} "
          f"of {len(st) / 1024:.0f} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

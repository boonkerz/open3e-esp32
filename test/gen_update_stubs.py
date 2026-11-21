"""Two stand-in images for the update package test: 5000 bytes that begin
like an application image, and a 32 KiB "partition" with 9000 bytes used."""
import sys
from pathlib import Path

out = Path(sys.argv[1])
(out / "fw_test.bin").write_bytes((b"\xe9" + bytes(range(256)) * 20)[:5000])
(out / "st_test.bin").write_bytes(bytes(range(250)) * 36 + b"\xff" * (32768 - 9000))

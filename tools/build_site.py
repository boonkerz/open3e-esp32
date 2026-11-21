#!/usr/bin/env python3
"""Assemble the browser-flashing site from site/ and a finished firmware build.

The manifest is generated from the build's own flasher_args.json rather than
written by hand. A wrong offset there produces no error message -- just a board
that does not start -- and partitions.csv is exactly the kind of file that gets
edited without anyone remembering a web page depends on it.
"""
import argparse
import json
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SITE = ROOT / "site"
BUILD = ROOT / "build"

# esp-web-tools names chips this way; ours is fixed by the board.
CHIP_FAMILY = "ESP32-S3"


# Where esp_app_desc_t sits in an image, and its magic word. Same numbers as
# tools/fwinfo.py.
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432


def version(app: Path) -> str:
    """Read the version out of the image itself, not out of git.

    The firmware carries the string it will report on its status page, fixed at
    the moment CMake configured. Asking git instead would let the manifest
    claim one version while the board announces another -- and that difference
    is invisible until somebody tries to work out which build they are running.
    """
    raw = app.read_bytes()[APP_DESC_OFFSET:APP_DESC_OFFSET + 256]
    (magic,) = struct.unpack_from("<I", raw, 0)
    if magic != APP_DESC_MAGIC:
        raise SystemExit(f"{app}: no application descriptor -- not a firmware image?")
    return raw[16:48].split(b"\0")[0].decode("utf-8", "replace")


def manifest(channel: str, ver: str, args: dict) -> dict:
    """Turn flasher_args.json into an esp-web-tools manifest.

    Offsets arrive as hex strings ("0x820000") and must go out as JSON numbers,
    since JSON has no hexadecimal literals.
    """
    parts = [{"path": f"bin/{channel}/{Path(f).name}", "offset": int(off, 16)}
             for off, f in sorted(args["flash_files"].items(),
                                  key=lambda kv: int(kv[0], 16))]
    return {
        "name": "open3e-Gateway",
        "version": ver,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": CHIP_FAMILY, "improv": False, "parts": parts}],
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", default="dev", choices=("release", "dev"))
    ap.add_argument("--version", default=None)
    ap.add_argument("--build", type=Path, default=BUILD)
    ap.add_argument("--out", type=Path, default=BUILD / "site")
    a = ap.parse_args()

    args_file = a.build / "flasher_args.json"
    if not args_file.exists():
        print(f"{args_file} missing -- run idf.py build first", file=sys.stderr)
        return 1
    args = json.loads(args_file.read_text())

    app = a.build / args["app"]["file"]
    ver = a.version or version(app)
    a.out.mkdir(parents=True, exist_ok=True)

    # Static files. The README documents the directory for whoever edits it and
    # has no business on a public web server.
    for src in sorted(SITE.iterdir()):
        if src.is_file() and src.name != "README.md":
            shutil.copy2(src, a.out / src.name)

    bindir = a.out / "bin" / a.channel
    bindir.mkdir(parents=True, exist_ok=True)
    total = 0
    for off, rel in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
        src = a.build / rel
        if not src.exists():
            print(f"{src} missing", file=sys.stderr)
            return 1
        shutil.copy2(src, bindir / src.name)
        total += src.stat().st_size
        print(f"  {off:>10}  {src.name:<24} {src.stat().st_size / 1024:8.0f} KiB")

    # The update package and a note of what it is. Both sit beside the flash
    # binaries, so the server's pull moves them with the channel and a device
    # asking for an update never sees a description of a package that is not
    # there yet.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import make_update
    fw = (a.build / args["app"]["file"]).read_bytes()
    pkg = make_update.pack(fw, (a.build / "storage.bin").read_bytes())
    (bindir / "open3e-update.bin").write_bytes(pkg)
    (bindir / "update.json").write_text(json.dumps(
        {"version": make_update.app_version(fw), "size": len(pkg),
         "channel": a.channel}) + "\n")
    print(f"  {'':>10}  {'open3e-update.bin':<24} {len(pkg) / 1024:8.0f} KiB")

    out_manifest = a.out / f"manifest-{a.channel}.json"
    out_manifest.write_text(json.dumps(manifest(a.channel, ver, args), indent=2) + "\n")

    print(f"\n{a.channel} {ver}: {total / 1024 / 1024:.1f} MiB -> {a.out}")
    print(f"  {out_manifest.relative_to(a.out.parent)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

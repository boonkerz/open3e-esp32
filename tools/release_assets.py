#!/usr/bin/env python3
"""Collect what a release offers for download, and its notes.

    open3e-gateway-<ver>-update.bin    firmware and storage in one, for
                                       Einstellungen -> Wartung -> Update
    open3e-gateway-<ver>.bin           the application alone (OTA)
    open3e-gateway-<ver>-storage.bin   the storage partition alone
    open3e-gateway-<ver>-factory.bin   everything from offset 0, for a first
                                       installation by cable
    SHA256SUMS
    NOTES.md                           this version's section of CHANGELOG.md

The file names come from the build's own flasher_args.json, as in
build_site.py. The factory image is made by `idf.py merge-bin` and only
picked up here: esptool sets the flash parameters in the bootloader header
while merging, and redoing that by hand would be a second implementation to
keep in step.
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import make_update  # noqa: E402


def notes(changelog: str, ver: str) -> str:
    """The section of the changelog that belongs to `ver`, without its heading."""
    m = re.search(rf"^## {re.escape(ver)}\b[^\n]*\n(.*?)(?=^## |\Z)", changelog, re.M | re.S)
    if not m:
        raise SystemExit(f"CHANGELOG.md has no section for {ver}")
    return m.group(1).strip() + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("version", help="without the leading v")
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--factory", type=Path, default=None,
                    help="output of idf.py merge-bin; left out of the release if not given")
    ap.add_argument("--out", type=Path, default=ROOT / "dist")
    a = ap.parse_args()

    args = json.loads((a.build / "flasher_args.json").read_text())
    app = a.build / args["app"]["file"]
    storage = a.build / args["storage"]["file"]
    built = make_update.app_version(app.read_bytes())
    # The firmware reports "<version>+<commit>". A tag on a tree built from
    # something else would put one version in the file names and another on
    # the status page; a dirty tree is nothing anybody can rebuild.
    if built.split("+")[0] != a.version or built.endswith("-dirty"):
        raise SystemExit(f"the firmware reports {built}, the release is {a.version}")

    a.out.mkdir(parents=True, exist_ok=True)
    base = f"open3e-gateway-{a.version}"
    shutil.copy2(app, a.out / f"{base}.bin")
    shutil.copy2(storage, a.out / f"{base}-storage.bin")
    (a.out / f"{base}-update.bin").write_bytes(
        make_update.pack(app.read_bytes(), storage.read_bytes()))
    if a.factory:
        shutil.copy2(a.factory, a.out / f"{base}-factory.bin")

    files = sorted(p for p in a.out.iterdir() if p.suffix == ".bin")
    (a.out / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n" for p in files))
    (a.out / "NOTES.md").write_text(notes((ROOT / "CHANGELOG.md").read_text(), a.version))

    for p in files:
        print(f"  {p.name:<44} {p.stat().st_size / 1024:8.0f} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())

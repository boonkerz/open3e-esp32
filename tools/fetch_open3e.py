#!/usr/bin/env python3
"""Fetch the open3e sources this firmware derives its datapoint database from.

The database is *not* vendored into this repo: it is regenerated at build time
from a pinned upstream commit.  Bumping to a newer open3e release is therefore a
one-line change to OPEN3E_REF below.

open3e is Apache-2.0 licensed; see NOTICE.
"""
import shutil
import subprocess
import sys
from pathlib import Path

OPEN3E_URL = "https://github.com/open3e/open3e.git"
# Pinned to an exact commit, not a branch.
#
# The datapoint database decides what the device names, decodes and shows, so
# two builds of the same firmware commit must not be able to disagree about it.
# A branch name cannot promise that. (open3e's default branch is `master`, not
# `main` -- cloning the wrong one fails outright.)
OPEN3E_REF = "1cbd58378c77dcdcc12aab97c5e5a7add50e75d4"   # master, 2026-10-14

# E3onCAN adds the passively broadcast datapoints of the Viessmann energy
# meters, which open3e does not cover: the E380 answers no UDS request, it only
# announces itself on the bus.
E3ONCAN_URL = "https://github.com/MyHomeMyData/E3onCAN.git"
E3ONCAN_REF = "3ebf7551822f5aef71a2ae89c1966aa1f3721e74"   # main, 2026-11-04

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / ".cache" / "open3e"
CACHE_E3ONCAN = ROOT / ".cache" / "e3oncan"


def run(*args, **kw):
    return subprocess.run(args, check=True, **kw)


def head_of(dest: Path) -> str | None:
    """The commit checked out at `dest`, or None if it is not a git tree."""
    if not (dest / ".git").exists():
        return None
    out = subprocess.run(("git", "-C", str(dest), "rev-parse", "HEAD"),
                         capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else None


def fetch(url: str, ref: str, dest: Path, name: str) -> None:
    """Put `url` at exactly `ref` under `dest`.

    `git clone --branch` only accepts branches and tags, so a commit is
    fetched into an empty repository instead. Fetching one commit by its hash
    needs uploadpack.allowReachableSHA1InWant on the server; GitHub has it.
    """
    if head_of(dest) == ref:
        print(f"{name} already at {ref[:12]}")
        return
    if dest.exists() and head_of(dest) is None:
        # A tree with no .git -- there is no way to tell what is in it.
        print(f"{name}: {dest} is not a checkout, replacing it")
        shutil.rmtree(dest)

    dest.mkdir(parents=True, exist_ok=True)
    print(f"fetching {name}@{ref[:12]} -> {dest}")
    if not (dest / ".git").exists():
        run("git", "-C", str(dest), "init", "--quiet")
        run("git", "-C", str(dest), "remote", "add", "origin", url)
    run("git", "-C", str(dest), "fetch", "--quiet", "--depth", "1", "origin", ref)
    run("git", "-C", str(dest), "checkout", "--quiet", "--force", "FETCH_HEAD")


def main() -> int:
    fetch(OPEN3E_URL, OPEN3E_REF, CACHE, "open3e")
    fetch(E3ONCAN_URL, E3ONCAN_REF, CACHE_E3ONCAN, "E3onCAN")
    return 0


if __name__ == "__main__":
    sys.exit(main())

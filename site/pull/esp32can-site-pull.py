#!/usr/bin/env python3
"""Fetch the flash page from the latest successful GitHub Actions run.

The web server sits on a home network, so GitHub's runners cannot push to it
(issue #3: the workflow's deploy step silently skipped for a month). Instead
the server pulls: every few minutes this script asks the GitHub API for the
newest successful `site` run per channel, downloads its `site-<channel>`
artifact when the run id changed, and unpacks it over the web root.

Channels: `dev` is the latest push to main, `release` the latest tag `v*`.
Both live in the same tree, so only the files of the channel being updated
are touched -- the other channel's binaries stay as they are.

Only the standard library: the host is a Caddy box with python3 and nothing
else worth installing for a 150-line script.
"""
import io
import json
import os
import shutil
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

REPO = os.environ.get("SITE_REPO", "boonkerz/open3e-esp32")
WORKFLOW = os.environ.get("SITE_WORKFLOW", "site.yml")
WEBROOT = Path(os.environ.get("SITE_WEBROOT", "/var/www/esp32can"))
TOKEN_FILE = Path(os.environ.get("SITE_TOKEN_FILE", "/etc/esp32can-site/token"))
STATE_FILE = Path(os.environ.get("SITE_STATE_FILE", "/var/lib/esp32can-site/state.json"))
API = "https://api.github.com"


def log(msg: str) -> None:
    print(msg, flush=True)


def token() -> str:
    try:
        tok = TOKEN_FILE.read_text().strip()
    except FileNotFoundError:
        sys.exit(f"{TOKEN_FILE} missing -- a fine-grained token with read access "
                 f"to Actions on {REPO}, mode 0600")
    if not tok:
        sys.exit(f"{TOKEN_FILE} is empty")
    return tok


class _StripAuthOnRedirect(urllib.request.HTTPRedirectHandler):
    """Drop the GitHub token when the download bounces to blob storage.

    The artifact endpoint answers 302 to a pre-signed Azure URL. urllib
    forwards every header to the new location, Azure sees a bearer token it
    did not issue and answers 401 -- which looks exactly like a bad GitHub
    token until one reads the www-authenticate header.
    """

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and (urllib.parse.urlsplit(newurl).netloc
                                != urllib.parse.urlsplit(req.full_url).netloc):
            new.remove_header("Authorization")
        return new


_opener = urllib.request.build_opener(_StripAuthOnRedirect)


def api(path: str, tok: str, raw: bool = False) -> bytes | dict:
    req = urllib.request.Request(
        path if path.startswith("http") else API + path,
        headers={
            "Authorization": f"Bearer {tok}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "esp32can-site-pull",
        },
    )
    with _opener.open(req, timeout=120) as resp:
        data = resp.read()
    return data if raw else json.loads(data)


def latest_run(tok: str, channel: str) -> dict | None:
    """Newest successful run for the channel, or None if there is none yet."""
    q = f"/repos/{REPO}/actions/workflows/{WORKFLOW}/runs?status=success&per_page=20"
    if channel == "dev":
        q += "&branch=main"
    runs = api(q, tok)["workflow_runs"]
    # Documented as newest first, but the first run on the server picked a
    # four-day-old one over three newer successes. Sort rather than trust.
    runs.sort(key=lambda r: r["run_number"], reverse=True)
    for run in runs:
        if channel == "dev":
            return run
        # A tag run reports the tag as head_branch; the workflow only
        # publishes `release` for tags v*.
        if run["head_branch"].startswith("v"):
            return run
    return None


def artifact_url(tok: str, run_id: int, name: str) -> str | None:
    arts = api(f"/repos/{REPO}/actions/runs/{run_id}/artifacts", tok)["artifacts"]
    for a in arts:
        if a["name"] == name and not a["expired"]:
            return a["archive_download_url"]
    return None


def install(zip_bytes: bytes, channel: str) -> None:
    """Unpack the artifact over the web root, this channel's files only.

    Everything is extracted into a staging directory beside the web root
    first, then moved into place file by file. The binaries directory is
    swapped by rename, so a browser never sees a half-written channel.
    """
    staging = Path(tempfile.mkdtemp(prefix=f".pull-{channel}-", dir=WEBROOT.parent))
    try:
        with zipfile.ZipFile(io.BytesIO(zip_bytes)) as zf:
            for member in zf.namelist():
                # Belt and braces: the artifact is our own, but a zip can
                # still name "../" entries, and this runs as root.
                if member.startswith("/") or ".." in Path(member).parts:
                    raise SystemExit(f"refusing suspicious path in artifact: {member}")
            zf.extractall(staging)

        manifest = staging / f"manifest-{channel}.json"
        bins = staging / "bin" / channel
        if not manifest.exists() or not bins.is_dir():
            raise SystemExit(f"artifact lacks manifest-{channel}.json or bin/{channel}/")

        WEBROOT.mkdir(parents=True, exist_ok=True)
        (WEBROOT / "bin").mkdir(exist_ok=True)

        # Binaries first, then the manifest that points at them: a page loaded
        # in between gets old manifest + old binaries, or new + new, never a
        # manifest for binaries that are not there yet.
        target = WEBROOT / "bin" / channel
        old = WEBROOT / "bin" / f".{channel}.old"
        if old.exists():
            shutil.rmtree(old)
        if target.exists():
            target.rename(old)
        shutil.move(str(bins), str(target))
        if old.exists():
            shutil.rmtree(old)

        # The documentation is one tree for both channels, newest wins. It is
        # swapped whole, like the binaries: pages removed from the docs must
        # not linger, and a reader must not get a page without its stylesheet.
        docs = staging / "doku"
        if docs.is_dir():
            target = WEBROOT / "doku"
            old = WEBROOT / ".doku.old"
            if old.exists():
                shutil.rmtree(old)
            if target.exists():
                target.rename(old)
            shutil.move(str(docs), str(target))
            if old.exists():
                shutil.rmtree(old)

        for name in (f"manifest-{channel}.json", "index.html", "LICENSE", "NOTICE"):
            src = staging / name
            if src.exists():
                os.replace(src, WEBROOT / name)
        # Anything else at the top level of the artifact (stylesheets, images)
        # is shared between channels and newest-wins.
        for src in staging.iterdir():
            if src.is_file():
                os.replace(src, WEBROOT / src.name)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def main() -> int:
    tok = token()
    state = {}
    if STATE_FILE.exists():
        state = json.loads(STATE_FILE.read_text())

    changed = False
    for channel in ("dev", "release"):
        try:
            run = latest_run(tok, channel)
        except urllib.error.HTTPError as e:
            log(f"{channel}: GitHub API {e.code} listing runs -- {e.read()[:200]!r}")
            return 1
        if run is None:
            log(f"{channel}: no successful run yet")
            continue
        if state.get(channel) == run["id"]:
            continue

        url = artifact_url(tok, run["id"], f"site-{channel}")
        if url is None:
            log(f"{channel}: run {run['id']} ({run['head_sha'][:7]}) has no "
                f"site-{channel} artifact (expired?)")
            continue
        log(f"{channel}: fetching run {run['id']} ({run['head_branch']} @ "
            f"{run['head_sha'][:7]}, {run['updated_at']})")
        install(api(url, tok, raw=True), channel)
        state[channel] = run["id"]
        changed = True
        log(f"{channel}: installed into {WEBROOT}")

    if changed:
        STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        STATE_FILE.write_text(json.dumps(state))
    return 0


if __name__ == "__main__":
    sys.exit(main())

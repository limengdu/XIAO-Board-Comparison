#!/usr/bin/env python3
"""Download the pinned npm files listed in assets/manifest.json and verify their SHA-256.

Files come from cdn.jsdelivr.net; hashes come from the jsDelivr package API. The result is
assets/cache/resolved.json: every pack with the local source path and on-board destination
of each file. Re-running skips files already downloaded with the right hash.

Example:
    python tools/fetch_assets.py
"""

import base64
import fnmatch
import hashlib
import json
import sys
import urllib.request
from pathlib import Path

ASSETS = Path(__file__).resolve().parent.parent / "assets"
CACHE = ASSETS / "cache"
API = "https://data.jsdelivr.com/v1/packages/npm/{pkg}?structure=flat"
CDN = "https://cdn.jsdelivr.net/npm/{pkg}/{path}"


def get(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=60) as resp:
        return resp.read()


def package_index(pkg: str, memo={}):
    """Map of file path (without leading slash) -> base64 SHA-256 for one npm package version."""
    if pkg not in memo:
        files = json.loads(get(API.format(pkg=pkg)))["files"]
        memo[pkg] = {f["name"].lstrip("/"): f["hash"] for f in files}
    return memo[pkg]


def fetch(pkg: str, path: str, expected_b64: str) -> Path:
    dest = CACHE / pkg / path
    digest = lambda data: base64.b64encode(hashlib.sha256(data).digest()).decode()  # noqa: E731
    if dest.exists() and digest(dest.read_bytes()) == expected_b64:
        return dest
    data = get(CDN.format(pkg=pkg, path=path))
    if digest(data) != expected_b64:
        sys.exit(f"hash mismatch for {pkg}/{path}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    return dest


def resolve_entry(entry):
    """Expand one manifest entry into [{"src", "dest", "gzip"}]."""
    gzip = bool(entry.get("gzip"))
    if "local" in entry:
        return [{"src": str(ASSETS / entry["local"]), "dest": entry["dest"], "gzip": gzip}]
    pkg = entry["npm"]
    index = package_index(pkg)
    if "path" in entry:
        if entry["path"] not in index:
            sys.exit(f"{pkg} has no {entry['path']}")
        return [{"src": str(fetch(pkg, entry["path"], index[entry["path"]])), "dest": entry["dest"], "gzip": gzip}]
    paths = sorted(p for p in index if fnmatch.fnmatch(p, entry["glob"]))
    if not paths:
        sys.exit(f"{pkg} has no files matching {entry['glob']}")
    return [{"src": str(fetch(pkg, p, index[p])), "dest": f"{entry['dest_dir']}/{p}", "gzip": gzip} for p in paths]


def main():
    manifest = json.loads((ASSETS / "manifest.json").read_text())
    packs = []
    for pack in manifest["packs"]:
        files = [f for entry in pack["files"] for f in resolve_entry(entry)]
        size = sum(Path(f["src"]).stat().st_size for f in files)
        packs.append({"id": pack["id"], "title": pack["title"], "files": files})
        print(f"{pack['id']:<12} {len(files):>4} files  {size / 1024:8.1f} KiB before gzip")
    CACHE.mkdir(parents=True, exist_ok=True)
    (CACHE / "resolved.json").write_text(json.dumps({"packs": packs}, indent=2))
    print(f"wrote {CACHE / 'resolved.json'}")


if __name__ == "__main__":
    main()

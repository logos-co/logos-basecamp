#!/usr/bin/env python3
"""Add version-independent download names without changing release binaries."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
from urllib.parse import quote


ALIASES = (
    "LogosBasecamp-Desktop-x86_64.AppImage",
    "LogosBasecamp-Desktop-aarch64.AppImage",
    "LogosBasecamp-Desktop-aarch64.dmg",
    "LogosBasecamp-Desktop-x86_64-windows-setup.exe",
)


def gh(*args: str) -> str:
    return subprocess.check_output(["gh", *args], text=True)


def select_sources(release: dict) -> dict[str, str]:
    if release["prerelease"]:
        raise ValueError("Download aliases require a full release, not a prerelease")
    names = [asset["name"] for asset in release["assets"] if asset["state"] == "uploaded"]
    sources = {}
    for alias in ALIASES:
        suffix = alias.removeprefix("LogosBasecamp-Desktop-")
        pattern = re.compile(r"LogosBasecamp-Desktop-v[\w.+-]+-" + re.escape(suffix))
        matches = [name for name in names if pattern.fullmatch(name)]
        if not matches:
            raise FileNotFoundError(f"Missing versioned source for {alias}")
        if len(matches) != 1:
            raise ValueError(f"Ambiguous sources for {alias}: {matches}")
        sources[alias] = matches[0]
    return sources


def load_release(repo: str, tag: str) -> dict:
    return json.loads(gh("api", f"repos/{repo}/releases/tags/{quote(tag, safe='')}"))


def wait_for_sources(repo: str, tag: str, wait_seconds: int) -> tuple[dict, dict[str, str]]:
    deadline = time.monotonic() + wait_seconds
    while True:
        release = load_release(repo, tag)
        try:
            return release, select_sources(release)
        except FileNotFoundError:
            if time.monotonic() >= deadline:
                raise
            # Publication can precede the final binary upload.
            time.sleep(min(30, max(0, deadline - time.monotonic())))


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(chunk)
    return checksum.hexdigest()


def publish_aliases(repo: str, tag: str, release: dict, sources: dict[str, str]) -> None:
    if release.get("immutable") and not release["draft"]:
        raise ValueError("This release is immutable; add aliases before publication")
    existing = {asset["name"] for asset in release["assets"]}
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        pending = []
        for alias, source in sources.items():
            gh("release", "download", tag, "--repo", repo, "--pattern", source, "--dir", directory)
            if alias in existing:
                gh("release", "download", tag, "--repo", repo, "--pattern", alias, "--dir", directory)
                if digest(root / alias) != digest(root / source):
                    raise ValueError(f"Existing alias differs from its versioned source: {alias}")
            else:
                shutil.copyfile(root / source, root / alias)
                pending.append(str(root / alias))
        if pending:
            # No --clobber: existing downloads must never be deleted or replaced.
            gh("release", "upload", tag, "--repo", repo, *pending)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--wait-seconds", type=int, default=0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    release, sources = wait_for_sources(args.repo, args.tag, args.wait_seconds)
    if args.dry_run:
        print(json.dumps(sources, indent=2))
    else:
        publish_aliases(args.repo, args.tag, release, sources)


if __name__ == "__main__":
    main()

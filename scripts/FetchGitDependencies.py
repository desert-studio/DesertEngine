#!/usr/bin/env python3
"""Fetch the third-party source trees that are not submodules, each at its pinned commit.

    python3 scripts/FetchGitDependencies.py [--check]

ThirdParty/GitDependencies.json lists them (directory, URL, full commit SHA, a sentinel file the configure reads).
This one script and that one list serve every platform: scripts/MacOS/Setup.sh, scripts/Windows/Setup.bat and the
Linux server image (Server/Dockerfile) call it, so the set and the pins cannot drift apart (UE's GitDependencies
pattern: one manifest, one fetcher, run by Setup on every platform and by CI).

A directory is current when its stamp file (.desert-git-pin) holds the pinned commit and the sentinel exists, or when
it is a git checkout whose HEAD is the pinned commit (the stamp is then written). Anything else is deleted and fetched
again: `git init` + `git fetch --depth 1 <url> <commit>` + checkout, so exactly the pinned commit lands, never a
branch tip. Idempotent: a current directory is not touched and needs no network.

    --check   Verify only; fetch nothing.

Exit codes: 0 every dependency current; 1 a fetch failed or left no sentinel; 3 the manifest is invalid;
4 (--check) a dependency is missing or at another commit.
"""

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "ThirdParty" / "GitDependencies.json"
STAMP = ".desert-git-pin"


def git(*args, cwd=None):
    return subprocess.run(["git", *args], cwd=cwd, check=False, capture_output=True, text=True)


def is_current(directory: Path, commit: str, sentinel: Path) -> bool:
    if not sentinel.is_file():
        return False
    stamp = directory / STAMP
    if stamp.is_file() and stamp.read_text().strip() == commit:
        return True
    if (directory / ".git").exists():
        head = git("rev-parse", "HEAD", cwd=directory)
        if head.returncode == 0 and head.stdout.strip() == commit:
            stamp.write_text(commit + "\n")
            return True
    return False


def fetch(directory: Path, url: str, commit: str) -> str:
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    for args in (("init", "-q"), ("remote", "add", "origin", url), ("fetch", "-q", "--depth", "1", "origin", commit),
                 ("checkout", "-q", "--detach", "FETCH_HEAD")):
        result = git(*args, cwd=directory)
        if result.returncode != 0:
            return f"git {' '.join(args)}: {result.stderr.strip()}"
    (directory / STAMP).write_text(commit + "\n")
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="verify only; fetch nothing")
    options = parser.parse_args()

    try:
        dependencies = json.loads(MANIFEST.read_text())["Dependencies"]
        for dep in dependencies:
            if len(dep["Commit"]) != 40:
                raise ValueError(f"{dep['Name']}: Commit must be a full SHA, got {dep['Commit']!r}")
    except (OSError, KeyError, ValueError) as error:
        print(f"[ERROR] ManifestInvalid {MANIFEST.relative_to(ROOT)}: {error}", file=sys.stderr)
        return 3

    status = 0
    for dep in dependencies:
        directory = ROOT / dep["Directory"]
        sentinel = directory / dep["Sentinel"]
        commit = dep["Commit"]
        if is_current(directory, commit, sentinel):
            print(f"--- {dep['Directory']}: present at {commit[:12]}")
            continue
        if options.check:
            print(f"[ERROR] NotFetched {dep['Directory']}: not at {commit[:12]}", file=sys.stderr)
            status = max(status, 4)
            continue
        print(f"--- Fetching {dep['Directory']} at {commit[:12]} from {dep['Url']}")
        error = fetch(directory, dep["Url"], commit)
        if not error and not sentinel.is_file():
            error = f"fetched, but {dep['Directory']}/{dep['Sentinel']} is missing"
        if error:
            print(f"[ERROR] FetchFailed {dep['Directory']}: {error}", file=sys.stderr)
            status = max(status, 1)
    return status


if __name__ == "__main__":
    sys.exit(main())

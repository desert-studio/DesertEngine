#!/usr/bin/env python3
"""Build artifacts as GitHub Releases of desert-studio/DesertEngine-artifacts.

Binaries are release assets, never commits (a binary in git history stays forever).
One release per platform build: tag `<platform>-<sha12>`, platform = win | mac.
Each release carries `<tag>-bin.zip` (executables + shared libraries) and, unless
--no-symbols, `<tag>-symbols.zip` (.pdb / .dSYM).

  publish <tree> [--bin DIR] [--config Release] [--no-symbols]
  fetch   <tag | latest> <dir> [--platform win|mac] [--symbols]
  list    [--platform win|mac]
  prune   --keep N [--platform win|mac]

Needs `gh` logged in with repo scope.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path

REPO = "desert-studio/DesertEngine-artifacts"

# Static libraries and intermediates are not artifacts; only what runs.
WIN_BIN = {".exe", ".dll"}
WIN_SYM = {".pdb"}
MAC_BIN = {".dylib"}
MAC_SYM = {".dsym"}


def host_platform():
    return "mac" if sys.platform == "darwin" else "win"


def gh(*args, capture=True):
    r = subprocess.run(["gh", *args], capture_output=capture, text=True)
    if r.returncode != 0:
        sys.exit(f"gh {' '.join(args[:2])} failed: {(r.stderr or '').strip()}")
    return r.stdout


def git(tree, *args):
    return subprocess.run(["git", "-C", str(tree), *args], capture_output=True, text=True, check=True).stdout.strip()


def is_mac_executable(p: Path):
    return p.is_file() and p.suffix == "" and os.access(p, os.X_OK)


def collect(bin_dir: Path, platform: str):
    """Returns (binaries, symbols) as lists of (path_on_disk, path_in_zip)."""
    binaries, symbols = [], []
    for p in sorted(bin_dir.iterdir()):
        ext = p.suffix.lower()
        if platform == "win":
            if ext in WIN_BIN:
                binaries.append((p, p.name))
            elif ext in WIN_SYM and any((bin_dir / (p.stem + e)).exists() for e in (".exe", ".dll")):
                symbols.append((p, p.name))
        else:
            target = symbols if ext in MAC_SYM else binaries
            if p.is_dir() and ext in (".app", ".dsym", ".framework"):
                for f in sorted(p.rglob("*")):
                    if f.is_file():
                        target.append((f, str(f.relative_to(bin_dir))))
            elif ext in MAC_BIN or is_mac_executable(p):
                binaries.append((p, p.name))
    return binaries, symbols


def write_zip(path: Path, entries):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for src, arc in entries:
            z.write(src, arc)


def cmd_publish(a):
    tree = Path(a.tree).resolve()
    platform = host_platform()
    bin_dir = Path(a.bin) if a.bin else tree / "build" / "Bin" / a.config
    if not bin_dir.is_dir():
        sys.exit(f"no build output at {bin_dir}")
    sha = git(tree, "rev-parse", "HEAD")
    branch = git(tree, "rev-parse", "--abbrev-ref", "HEAD")
    subject = git(tree, "log", "-1", "--format=%s")
    tag = f"{platform}-{sha[:12]}"
    binaries, symbols = collect(bin_dir, platform)
    if not binaries:
        sys.exit(f"nothing to publish in {bin_dir}")

    manifest = {
        "platform": platform, "sha": sha, "branch": branch, "subject": subject,
        "config": a.config, "built": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "files": [arc for _, arc in binaries],
    }
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        assets = [tmp / f"{tag}-bin.zip"]
        write_zip(assets[0], binaries + [(tmp / "manifest.json", "manifest.json")])
        if symbols and not a.no_symbols:
            assets.append(tmp / f"{tag}-symbols.zip")
            write_zip(assets[1], symbols)
        for asset in assets:
            if asset.stat().st_size >= 2 * 1024**3:
                sys.exit(f"{asset.name} is over the 2 GiB release asset limit")
        notes = f"{platform} {a.config} build of DesertEngine `{sha}` ({branch})\n\n{subject}\n\n" + \
                "\n".join(f"- {arc}" for _, arc in binaries)
        exists = subprocess.run(["gh", "release", "view", tag, "-R", REPO], capture_output=True).returncode == 0
        if exists:
            gh("release", "upload", tag, *map(str, assets), "--clobber", "-R", REPO)
        else:
            gh("release", "create", tag, *map(str, assets), "-R", REPO, "--target", "main",
               "--title", f"{platform} {sha[:12]} — {subject[:80]}", "--notes", notes)
    print(f"published {tag}: {len(binaries)} binaries, {len(symbols)} symbol files")


def releases(platform=None):
    out = gh("release", "list", "-R", REPO, "--limit", "1000", "--json", "tagName,createdAt")
    rows = sorted(json.loads(out), key=lambda r: r["createdAt"], reverse=True)
    return [r for r in rows if platform is None or r["tagName"].startswith(platform + "-")]


def cmd_fetch(a):
    platform = a.platform or host_platform()
    tag = a.tag
    if tag == "latest":
        rows = releases(platform)
        if not rows:
            sys.exit(f"no {platform} releases")
        tag = rows[0]["tagName"]
    dest = Path(a.dir)
    dest.mkdir(parents=True, exist_ok=True)
    patterns = ["*-bin.zip"] + (["*-symbols.zip"] if a.symbols else [])
    with tempfile.TemporaryDirectory() as tmp:
        args = ["release", "download", tag, "-R", REPO, "-D", tmp]
        for p in patterns:
            args += ["-p", p]
        gh(*args)
        for z in Path(tmp).glob("*.zip"):
            with zipfile.ZipFile(z) as zf:
                zf.extractall(dest)
    if platform == "mac":
        for p in dest.rglob("*"):
            if p.is_file() and p.suffix in ("", ".dylib"):
                p.chmod(p.stat().st_mode | 0o111)
    print(f"fetched {tag} into {dest}")


def cmd_list(a):
    for r in releases(a.platform):
        print(r["tagName"], r["createdAt"])


def cmd_prune(a):
    for platform in ([a.platform] if a.platform else ["win", "mac"]):
        for r in releases(platform)[a.keep:]:
            gh("release", "delete", r["tagName"], "-R", REPO, "--yes", "--cleanup-tag")
            print("deleted", r["tagName"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("publish")
    p.add_argument("tree")
    p.add_argument("--bin")
    p.add_argument("--config", default="Release")
    p.add_argument("--no-symbols", action="store_true")
    p.set_defaults(fn=cmd_publish)
    p = sub.add_parser("fetch")
    p.add_argument("tag")
    p.add_argument("dir")
    p.add_argument("--platform", choices=["win", "mac"])
    p.add_argument("--symbols", action="store_true")
    p.set_defaults(fn=cmd_fetch)
    p = sub.add_parser("list")
    p.add_argument("--platform", choices=["win", "mac"])
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("prune")
    p.add_argument("--keep", type=int, required=True)
    p.add_argument("--platform", choices=["win", "mac"])
    p.set_defaults(fn=cmd_prune)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()

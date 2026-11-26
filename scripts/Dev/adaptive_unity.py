#!/usr/bin/env python3
"""Adaptive unity: the sources this branch changed compile alone, outside the unity batches.

A unity batch recompiles all of its sixteen sources when one of them changes; the file being edited is the one
that changes again and again. This writes <build dir>/adaptive_unity.cmake with

    set(DESERT_ADAPTIVE_UNITY_SKIP "path/a.cpp;path/b.cpp")

the repository-relative .cpp/.cc/.cxx/.mm sources that differ from the merge base with dev (committed or not),
which cmake/DesertWorkspace.cmake reads on configure (desert_finalize_targets drops them from the batches).
The file is rewritten only when the list changes, so an unchanged list costs no reconfigure.

usage: adaptive_unity.py <build dir> [--base dev]
"""
import argparse
import os
import subprocess
import sys

SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".mm")


def git_lines(root, *args):
    out = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True)
    if out.returncode != 0:
        return None, out.stderr.strip()
    return [line.strip() for line in out.stdout.splitlines() if line.strip()], ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("build_dir")
    parser.add_argument("--base", default="dev")
    args = parser.parse_args()

    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    base, err = git_lines(root, "merge-base", "HEAD", args.base)
    if base is None or not base:
        print(f"adaptive_unity: no merge base with {args.base}: {err}", file=sys.stderr)
        return 1
    changed, err = git_lines(root, "diff", "--name-only", base[0])
    if changed is None:
        print(f"adaptive_unity: git diff failed: {err}", file=sys.stderr)
        return 1
    untracked, err = git_lines(root, "ls-files", "--others", "--exclude-standard")
    if untracked is None:
        print(f"adaptive_unity: git ls-files failed: {err}", file=sys.stderr)
        return 1

    skip = sorted({p for p in changed + untracked
                   if p.endswith(SOURCE_SUFFIXES) and os.path.isfile(os.path.join(root, p))})
    text = "# Written by scripts/Dev/adaptive_unity.py: the branch's changed sources, compiled outside the unity batches.\n"
    text += 'set(DESERT_ADAPTIVE_UNITY_SKIP "' + ";".join(skip) + '")\n'

    os.makedirs(args.build_dir, exist_ok=True)
    path = os.path.join(args.build_dir, "adaptive_unity.cmake")
    old = None
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as f:
            old = f.read()
    if old != text:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"adaptive_unity: {len(skip)} source(s) outside the batches -> {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

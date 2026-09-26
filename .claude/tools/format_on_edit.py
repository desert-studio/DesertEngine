#!/usr/bin/env python3
"""PostToolUse hook: after Edit/Write/MultiEdit of a C/C++ file, format the lines that differ from HEAD with
clang-format 18 — the version CI pins (scripts/CI/CheckFormat.sh). Only the changed lines: formatting a whole
file would rewrite code nobody touched and hand a reviewer a diff of whitespace.

A file git does not track yet is formatted whole (every line of it is new). Anything else — a Lua file, a file
outside the repository, no clang-format 18 on this machine — is left alone and, where it matters, said once on
stderr. The hook never blocks the edit: exit 0 always.
"""
import json
import os
import shutil
import subprocess
import sys

SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".m", ".mm"}

# clang-format 18 wherever each machine keeps it. DESERT_CLANG_FORMAT overrides; the list is macOS Homebrew's
# llvm@18 keg and the Windows portable install (F:\tools\llvm18). A different major version is NOT accepted:
# 19 and 22 both disagree with 18 on lines CI then rejects.
CANDIDATES = [
    os.environ.get("DESERT_CLANG_FORMAT", ""),
    "/opt/homebrew/opt/llvm@18/bin/clang-format",
    "/usr/local/opt/llvm@18/bin/clang-format",
    "F:/tools/llvm18/bin/clang-format.exe",
]


def find_clang_format():
    for candidate in CANDIDATES:
        if candidate and os.path.isfile(candidate):
            try:
                version = subprocess.run([candidate, "--version"], capture_output=True, text=True, timeout=10).stdout
            except (OSError, subprocess.SubprocessError):
                continue
            if "version 18." in version:
                return candidate
    return None


def main():
    try:
        event = json.load(sys.stdin)
    except (ValueError, OSError):
        return 0

    if event.get("tool_name") not in ("Edit", "Write", "MultiEdit"):
        return 0
    path = (event.get("tool_input") or {}).get("file_path") or ""
    if os.path.splitext(path)[1].lower() not in SOURCE_EXTENSIONS or not os.path.isfile(path):
        return 0

    directory = os.path.dirname(os.path.abspath(path))
    top = subprocess.run(["git", "-C", directory, "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    if top.returncode != 0:
        return 0  # not in a repository: not ours to format
    root = top.stdout.strip()
    relative = os.path.relpath(os.path.abspath(path), root)
    if relative.startswith("ThirdParty") or os.sep + "ThirdParty" + os.sep in relative:
        return 0

    binary = find_clang_format()
    if binary is None:
        print("[format_on_edit] clang-format 18 not found (set DESERT_CLANG_FORMAT); the edit was NOT formatted — "
              "CI's format gate may reject it.", file=sys.stderr)
        return 0

    tracked = subprocess.run(["git", "-C", root, "ls-files", "--error-unmatch", relative], capture_output=True).returncode == 0
    if not tracked:
        subprocess.run([binary, "-i", os.path.join(root, relative)], cwd=root, capture_output=True)
        return 0

    git_clang_format = os.path.join(os.path.dirname(binary), "git-clang-format")
    command = [git_clang_format] if os.path.isfile(git_clang_format) else [shutil.which("git-clang-format") or ""]
    if not command[0]:
        print("[format_on_edit] git-clang-format not found beside clang-format 18; the edit was NOT formatted.",
              file=sys.stderr)
        return 0
    if not os.access(command[0], os.X_OK) or os.name == "nt":
        command = [sys.executable] + command  # Windows: the script's #! line cannot be executed directly
    result = subprocess.run(command + ["--force", "--binary", binary, "HEAD", "--", relative], cwd=root,
                            capture_output=True, text=True)
    if result.returncode not in (0, 1):
        print(f"[format_on_edit] git-clang-format failed on {relative}: {result.stderr.strip()[:300]}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

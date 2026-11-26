#!/usr/bin/env bash
# Emit compile_commands.json for ONE configuration of the workspace, from the database CMake writes.
#
# WHY THIS STEP EXISTS AT ALL: each configure preset is one configuration in its own tree (CMakePresets.json:
# single-config Ninja, <host>-dev / <host>-debug / <host>-ship), so build/cmake/<preset>/compile_commands.json
# already holds one command per source. What a tool reading it cannot take is the rest: ThirdParty, the
# configure-written sources and the PCH binary a --gen-only tree does not have. This script drops those.
#
# The flags are CMake's own, by construction: nothing here re-derives a compile line.
#
# ThirdParty is excluded BY PATH COMPONENT, not by a list of names: any source whose path contains a
# `ThirdParty/` component is dropped. A new submodule lands under ThirdParty/ and is excluded the day
# it arrives, with nothing to remember to edit.
#
# Usage: scripts/CI/GenCompileCommands.sh [Development|DebugGame|Shipping] [-o <output>]
#   The tree is the host's preset of that configuration: mac-<dev|debug|ship> on macOS, win-<...> on Windows.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT="$PWD"

CONFIG="DebugGame"
OUT="$ROOT/compile_commands.json"
while [ $# -gt 0 ]; do
    case "$1" in
        Development|DebugGame|Shipping) CONFIG="$1"; shift ;;
        -o) OUT="$2"; shift 2 ;;
        *) echo "GenCompileCommands: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

case "$CONFIG" in Development) SUFFIX=dev ;; DebugGame) SUFFIX=debug ;; Shipping) SUFFIX=ship ;; esac
case "$(uname -s)" in
    Darwin) PRESET="mac-$SUFFIX" ;;
    MINGW*|MSYS*|CYGWIN*) PRESET="win-$SUFFIX" ;;
    *) echo "GenCompileCommands: no configure preset for host '$(uname -s)' in CMakePresets.json" >&2; exit 2 ;;
esac
SRC_DB="$ROOT/build/cmake/$PRESET/compile_commands.json"

# EVERY FAILURE BELOW EXITS 2 AND NAMES ITSELF. An empty or partial database is the exact shape this
# project keeps paying for — a tool that answers a different question with no sign that it did — and
# downstream the answer would be "0 files analysed, no warnings", i.e. a silent pass.
if [ ! -f "$SRC_DB" ]; then
    echo "GenCompileCommands: $SRC_DB is missing — configure first: cmake --preset $PRESET" >&2
    exit 2
fi

SRC_DB="$SRC_DB" OUT_DB="$OUT" CONFIG="$CONFIG" python3 - <<'PY' || exit 2
import json, os, sys, tempfile

src, out, config = os.environ["SRC_DB"], os.environ["OUT_DB"], os.environ["CONFIG"]
entries = json.load(open(src, encoding="utf-8"))
kept, total = [], 0

# THE PCH IS USED AS ITS HEADER, NOT AS ITS BINARY. A clang line from CMake carries
# `-Xclang -include-pch -Xclang <...>/cmake_pch.hxx.pch` AND `-Xclang -include -Xclang <...>/cmake_pch.hxx`: the
# .pch exists only after a build, and a tool reading this database (clang-tidy on a --gen-only tree) has none.
# Dropping the -include-pch pair keeps the forced include of cmake_pch.hxx (written at configure), so the TU sees
# exactly the headers the build gives it, parsed instead of loaded. MSVC lines (/Yu /Fp) are left as they are.
import shlex
def strip_pch(command):
    if "-include-pch" not in command:
        return command
    args = shlex.split(command, posix=True)
    out, i = [], 0
    while i < len(args):
        if args[i] == "-Xclang" and i + 3 < len(args) and args[i + 1] == "-include-pch" and args[i + 2] == "-Xclang":
            i += 4
            continue
        out.append(args[i])
        i += 1
    return shlex.join(out)
for e in entries:
    total += 1
    path = os.path.normpath(os.path.join(e["directory"], e["file"])).replace("\\", "/")
    # build/generated holds third-party sources the configure step writes (dav1d's bit-depth
    # instances, assimp's config): ThirdParty code at a build path, never a file a change can touch.
    if "/ThirdParty/" in "/" + path or "/build/generated/" in path:
        continue
    # The precompiled-header TU (cmake_pch.hxx.cxx, CMake writes it at configure) is the build's cache, not a source.
    if os.path.basename(path).startswith("cmake_pch."):
        continue
    kept.append({"directory": e["directory"], "file": path, "command": strip_pch(e["command"])})

if total == 0:
    sys.exit(f"GenCompileCommands: {src} holds no entries (configuration {config})")
if not kept:
    sys.exit(f"GenCompileCommands: {total} {config} entries, all under ThirdParty/ — an EMPTY first-party database")

fd, partial = tempfile.mkstemp(prefix=os.path.basename(out) + ".partial.", dir=os.path.dirname(out) or ".")
with os.fdopen(fd, "w", encoding="utf-8") as f:
    json.dump(kept, f, indent=1)
os.replace(partial, out)
print(f"GenCompileCommands: {len(kept)} first-party {config} translation units ({total - len(kept)} ThirdParty dropped) -> {out}")
PY

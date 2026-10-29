#!/usr/bin/env bash
# Which of our translation units NO test suite compiles.
#
# WHY THIS EXISTS. "69 suites green" and "this code is covered" are two different statements, and until
# this script there was nothing that said which one was true. A file no suite compiles is INVISIBLE to a
# green sweep: a defect planted in it changes nothing anybody runs. That is not a hypothesis — it was
# measured. Deliberate sabotage of the scene deserializer's identity stitch, of the texture importer's
# compatibility branch and of four other files left every suite in the repository passing.
#
# WHY IT DERIVES THE ANSWER INSTEAD OF CARRYING A LIST. Same reason DEV_CONTRACT §2.4.5a deleted its list of
# tool names: a list has to be maintained, and a list that is one entry out of date lies in exactly the
# direction nobody checks. The configured build already states every source each test runner compiles
# (compile_commands.json) and every library it links whole (the Ninja link lines) — the build IS the ground
# truth, and this is a set difference over it.
#
# It is a REPORT, not a gate. There is no threshold to fail against: some of these files cannot be reached
# without a GPU and never will be, and a number that fails a build would only be argued down. What it is
# for is to be read — beside the sweep, in the developer's report, so that "green" is qualified by "over
# what".
#
# Usage: scripts/CI/UnreachedSources.sh
#   Requires a configured host preset:  cmake --preset macos-clang-ninja   (windows-msvc-ninja on Windows)
#   (the presets configure the test runners; without them every file would look unreached).

set -euo pipefail
cd "$(dirname "$0")/../.."

case "$(uname -s)" in
    Darwin) PRESET=macos-clang-ninja ;;
    *)      PRESET=windows-msvc-ninja ;;
esac
BUILD_DIR="build/cmake/$PRESET"
if [ ! -f "$BUILD_DIR/compile_commands.json" ] || [ ! -f "$BUILD_DIR/CMakeFiles/impl-Debug.ninja" ]; then
    echo "No configured build in $BUILD_DIR. Run: cmake --preset $PRESET" >&2
    exit 2
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# Git Bash on Windows: python3 is a native program and cannot open /tmp/...; give it the C:/... spelling.
command -v cygpath >/dev/null 2>&1 && TMP=$(cygpath -m "$TMP")

# --- Every test suite and the runner that holds it: build/TestManifest.txt, "<Runner> <Suite>" per line, written by
#     Desert/Tests/CMakeLists.txt at configure time (suites are directories in per-layer runners, not targets).
MANIFEST=build/TestManifest.txt
if [ ! -f "$MANIFEST" ]; then
    echo "No $MANIFEST. Run: cmake --preset $PRESET" >&2
    exit 2
fi
SUITES=$(grep -c . "$MANIFEST")
awk 'NF { print $1 }' "$MANIFEST" | sort -u > "$TMP/runners"

# --- Every source file those runners compile, plus every source of a static library a runner links WHOLE
#     (desert_link_whole_archive: $<LINK_LIBRARY:WHOLE_ARCHIVE,..>, i.e. -force_load / --whole-archive /
#     /WHOLEARCHIVE: on its Debug link line -- the five editor libraries in EditorTests): such a library's objects
#     are all in the runner, as if compiled there. A library linked by reference (Desert, Common) contributes only
#     the objects something pulls, which the build cannot tell, so its sources are not counted. A target's
#     sources are the compile_commands.json entries whose object lies under `<Target>.dir/Debug/`.
MISSING=$(BUILD_DIR="$BUILD_DIR" RUNNERS="$TMP/runners" OUT="$TMP/compiled" python3 - <<'PY'
import json, os, re
build = os.environ["BUILD_DIR"]
# The source root as CMake spells it in the database (F:/... on Windows, where the shell says /f/...).
cache = open(os.path.join(build, "CMakeCache.txt"), encoding="utf-8", errors="replace").read()
root = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", cache, re.M).group(1).strip().replace("\\", "/").rstrip("/") + "/"
sources = {}
for e in json.load(open(os.path.join(build, "compile_commands.json"), encoding="utf-8")):
    m = re.search(r"/([^/]+)\.dir/Debug/", e.get("output", "").replace("\\", "/"))
    if not m:
        continue
    path = os.path.normpath(os.path.join(e["directory"], e["file"])).replace("\\", "/")
    if path.lower().startswith(root.lower()):
        sources.setdefault(m.group(1), set()).add(path[len(root):])
# The Debug link statements: `build <binary>: CXX_EXECUTABLE_LINKER__<Target>_Debug ...`, then indented variables.
links, current = {}, None
for line in open(os.path.join(build, "CMakeFiles", "impl-Debug.ninja"), encoding="utf-8", errors="replace"):
    # `.*`, not `[^:]*`: Ninja spells a Windows drive as `F$:`, a colon inside the output path.
    m = re.match(r"build .*: CXX_EXECUTABLE_LINKER__(\w+?)_Debug\b", line)
    if m:
        current = m.group(1)
    elif current and line.startswith("  LINK_LIBRARIES = "):
        links[current] = line
    elif not line.startswith("  "):
        current = None
whole = re.compile(r"(?:-force_load,|-force_load |--whole-archive |/WHOLEARCHIVE:)(\S+)")
compiled, missing = set(), []
for runner in open(os.environ["RUNNERS"]).read().split():
    if runner not in sources:
        missing.append(runner)
        continue
    compiled |= sources[runner]
    for lib in whole.findall(links.get(runner, "")):
        name = re.sub(r"^lib", "", os.path.splitext(os.path.basename(lib.replace("\\", "/")))[0])
        if name in sources:
            compiled |= sources[name]
        else:
            missing.append(name)
# newline="\n": on Windows text mode writes CRLF, and a "path<CR>" never equals find's "path" in comm,
# which is how every source read as unreached (0 of 1029).
open(os.environ["OUT"], "w", newline="\n").write("".join(p + "\n" for p in sorted(compiled)))
print(" ".join(missing))
PY
)
sort -u "$TMP/compiled" -o "$TMP/compiled"

# --- Every translation unit we own. Third-party trees are not ours to test.
find Desert/Common/Source Desert/Desert/Source Editor/Source Runtime/Source Tools \
    \( -name '*.cpp' -o -name '*.mm' \) -print |
    sort -u > "$TMP/ours"

comm -23 "$TMP/ours" "$TMP/compiled" > "$TMP/unreached"

OURS=$(wc -l < "$TMP/ours" | tr -d ' ')
UNREACHED=$(wc -l < "$TMP/unreached" | tr -d ' ')
REACHED=$((OURS - UNREACHED))

echo "=== Reachability of our sources by the test suites ==="
echo "suites:                 $SUITES (in $(wc -l < "$TMP/runners" | tr -d ' ') runners)"
echo "our translation units:  $OURS"
echo "compiled by >=1 suite:  $REACHED"
echo "compiled by NO suite:   $UNREACHED"
if [ -n "$MISSING" ]; then
    echo "NOT IN THE BUILD (not configured?): $MISSING"
fi
echo ""
echo "--- unreached, by area ---"
# Group by the directory two levels below the source root, which is how the code is actually laid out.
sed -E 's#^(Desert/Common/Source/Common|Desert/Desert/Source/Engine|Editor/Source/Editor|Runtime/Source|Tools/[^/]+)/?([^/]*)/.*#\1/\2#; s#^([^/]+/[^/]+)/[^/]+\.(cpp|mm)$#\1#' \
    "$TMP/unreached" | sort | uniq -c | sort -rn
echo ""
echo "--- unreached, in full ---"
cat "$TMP/unreached"

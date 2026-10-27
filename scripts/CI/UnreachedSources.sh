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
# direction nobody checks. The generated `*.make` files already state, line by line, every source each test
# project compiles — the makefiles ARE the ground truth, and this is a set difference over them.
#
# It is a REPORT, not a gate. There is no threshold to fail against: some of these files cannot be reached
# without a GPU and never will be, and a number that fails a build would only be argued down. What it is
# for is to be read — beside the sweep, in the developer's report, so that "green" is qualified by "over
# what".
#
# Usage: scripts/CI/UnreachedSources.sh
#   Requires the makefiles to exist:  CI=true premake5 gmake
#   (without CI the test projects are not generated at all, and every file would look unreached).

set -euo pipefail
cd "$(dirname "$0")/../.."

# premake writes the makefiles into build/Projects/ (BuildScripts/Workspace.lua), with sources relative to it.
PROJ_DIR=build/Projects
if ! ls "$PROJ_DIR"/*.make >/dev/null 2>&1; then
    echo "No generated makefiles in $PROJ_DIR. Run: CI=true premake5 gmake" >&2
    exit 2
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- Every test suite and the runner that holds it: build/TestManifest.txt, "<Runner> <Suite>" per line, written by
#     Desert/Tests/premake5.lua (suites are directories in per-layer runners, not projects of their own).
MANIFEST=build/TestManifest.txt
if [ ! -f "$MANIFEST" ]; then
    echo "No $MANIFEST. Run: CI=true premake5 gmake" >&2
    exit 2
fi
SUITES=$(grep -c . "$MANIFEST")
awk 'NF { print $1 }' "$MANIFEST" | sort -u > "$TMP/runners"

# --- Every source file those runners compile, plus every source of a static library a runner links WHOLE
#     (DesertPlatform.LinkWholeArchive: -force_load / --whole-archive on its link line -- the five editor
#     libraries in EditorTests): such a library's objects are all in the runner, as if compiled there. A library linked by
#     reference (Desert, Common) contributes only the objects something pulls, which a makefile cannot tell, so
#     its sources are not counted. Premake writes one dependency line per object:
#       $(OBJDIR)/<name>.o: <source path>
: > "$TMP/compiled"
MISSING=""
sources_of() {
    awk '/^\$\(OBJDIR\)\/[^:]+: /{ sub(/^\.\.\/\.\.\//, "", $2); print $2 }' "$PROJ_DIR/$1.make"
}
while read -r runner; do
    if [ ! -f "$PROJ_DIR/$runner.make" ]; then
        MISSING="$MISSING $runner"
        continue
    fi
    sources_of "$runner" >> "$TMP/compiled"
    for lib in $(grep -oE '(-force_load,|--whole-archive )[^ ]*/lib[A-Za-z0-9_]+\.a' "$PROJ_DIR/$runner.make" |
                 sed -E 's#.*/lib([A-Za-z0-9_]+)\.a$#\1#' | sort -u); do
        if [ -f "$PROJ_DIR/$lib.make" ]; then
            sources_of "$lib" >> "$TMP/compiled"
        else
            MISSING="$MISSING $lib"
        fi
    done
done < "$TMP/runners"
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
    echo "NO MAKEFILE (not generated?):$MISSING"
fi
echo ""
echo "--- unreached, by area ---"
# Group by the directory two levels below the source root, which is how the code is actually laid out.
sed -E 's#^(Desert/Common/Source/Common|Desert/Desert/Source/Engine|Editor/Source/Editor|Runtime/Source|Tools/[^/]+)/?([^/]*)/.*#\1/\2#; s#^([^/]+/[^/]+)/[^/]+\.(cpp|mm)$#\1#' \
    "$TMP/unreached" | sort | uniq -c | sort -rn
echo ""
echo "--- unreached, in full ---"
cat "$TMP/unreached"

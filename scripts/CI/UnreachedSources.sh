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

# --- Every test suite, named the way the sweep names them: one directory with a premake5.lua under
#     Desert/Tests, excluding the aggregate at the root.
find Desert/Tests -mindepth 2 -name premake5.lua -print0 |
    xargs -0 -n1 dirname |
    xargs -n1 basename |
    sort -u > "$TMP/suites"

SUITES=$(wc -l < "$TMP/suites" | tr -d ' ')

# --- Every source file those suites compile. Premake writes one dependency line per object:
#       $(OBJDIR)/<name>.o: <source path>
: > "$TMP/compiled"
MISSING=""
while read -r suite; do
    if [ ! -f "$PROJ_DIR/$suite.make" ]; then
        MISSING="$MISSING $suite"
        continue
    fi
    awk '/^\$\(OBJDIR\)\/[^:]+: /{ sub(/^\.\.\/\.\.\//, "", $2); print $2 }' "$PROJ_DIR/$suite.make" >> "$TMP/compiled"
done < "$TMP/suites"
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
echo "suites:                 $SUITES"
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

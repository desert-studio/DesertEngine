#!/usr/bin/env bash
# Diff-based "text is formatted, never glued" gate: only the lines ADDED vs the base are checked, exactly as
# CheckFormat.sh does for clang-format. The rule (contract, "Text is formatted, never glued", owner 2026-09-29):
# a string built by `"[" + id + "] " + text` is returned; text is made by std::format / std::format_to with a
# literal format string, a repeated shape has ONE named home, paths are std::filesystem::path joined with `/`.
# About 1,700 glued lines predate the rule, so a whole-tree check would be red forever; the lines you touch
# follow it and the tree converges.
#
# What counts as a finding: a BINARY `+` (so `+=` of a glued expression too) with a string literal on one side
# and a non-literal on the other — `"..." + x`, `x + "..."`, `out += "..." + x`. Deliberately NOT findings:
#   - two literals (`"a" "b"`, `"a" + "b"` never compiles alone, but `std::string( "a" ) + "b"` is still a
#     literal next to an expression and IS a finding — the exception is literal-to-literal only);
#   - std::filesystem::path operators (`root / "Assets" / name`) — `/` is the RIGHT way to join a path;
#   - character literals (`'0' + digit` is arithmetic, not text);
#   - `out += "literal";` alone (appending one literal is not a glued expression);
#   - comments and #include lines.
# The one escape is explicit and names its reason on the line: `// glued-ok: <reason>`. Every such pass is
# printed in a register at the end, so the exceptions stay visible instead of becoming a silent allowlist.
#
# Usage:
#   scripts/CI/CheckGluedText.sh [<base-ref-or-sha>]   changed lines vs merge-base(HEAD, base); default origin/dev
#   scripts/CI/CheckGluedText.sh --file <path>...       every line of the named files (fixtures, ad-hoc checks)
# Exit: 0 clean, 1 findings (file:line + the std::format hint), 2 could not run (never a verdict about code).
set -euo pipefail
CALLER_DIR=$PWD
cd "$(dirname "$0")/../.."

PY=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then PY="$candidate"; break; fi
done
if [ -z "$PY" ]; then
    echo "glued-text: MISSING TOOL — python3 not found. This is an environment failure, not a finding." >&2
    exit 2
fi

MODE=diff
if [ "${1:-}" = "--file" ]; then
    shift
    [ $# -gt 0 ] || { echo "glued-text: --file needs at least one path" >&2; exit 2; }
    FILES=()
    for f in "$@"; do
        case "$f" in /*) ;; *) f="$CALLER_DIR/$f" ;; esac
        FILES+=("$f")
    done
    set -- "${FILES[@]}"
    for f in "$@"; do
        [ -f "$f" ] || { echo "glued-text: no such file: $f" >&2; exit 2; }
    done
    MODE=file
    INPUT=$(for f in "$@"; do
        n=0
        while IFS= read -r line || [ -n "$line" ]; do
            n=$((n + 1))
            printf '%s\t%s\t%s\n' "$f" "$n" "$line"
        done <"$f"
    done)
else
    BASE_INPUT="${1:-origin/dev}"
    git rev-parse --git-dir >/dev/null 2>&1 || { echo "glued-text: not inside a git repository" >&2; exit 2; }
    # Same base resolution as CheckFormat.sh: the given ref, else HEAD~1 (a first push sends an all-zero sha).
    if git rev-parse --verify -q "$BASE_INPUT^{commit}" >/dev/null 2>&1; then
        BASE=$(git merge-base HEAD "$BASE_INPUT" 2>/dev/null || echo "$BASE_INPUT")
    else
        BASE=$(git rev-parse HEAD~1 2>/dev/null || git rev-parse HEAD)
    fi
    set +e
    DIFF=$(git diff -U0 --no-color --no-ext-diff "$BASE" -- '*.cpp' '*.hpp' '*.h' '*.mm' 2>&1)
    RC=$?
    set -e
    if [ $RC -ne 0 ]; then
        echo "$DIFF" >&2
        echo "glued-text: git diff against $BASE FAILED (exit $RC) — environment failure, not a finding." >&2
        exit 2
    fi
    # Added lines only, as file<TAB>line<TAB>text; ThirdParty/ and Generated/ are not ours to reformat.
    INPUT=$(printf '%s\n' "$DIFF" | awk '
        /^\+\+\+ / { f = substr($0, 5); sub(/^b\//, "", f); skip = (f ~ /(^|\/)(ThirdParty|Generated)\//); next }
        /^@@ /     { split($3, a, ","); n = substr(a[1], 2) + 0; next }
        /^\+/      { if (!skip) printf "%s\t%d\t%s\n", f, n, substr($0, 2); n++; next }
    ')
fi

set +e
printf '%s\n' "$INPUT" | "$PY" -c '
import re, sys

findings, passes = [], []
IDENT_END = re.compile(r"[A-Za-z0-9_\)\]]$")

def mask(text):
    """Replace string literals by S and char literals by C; drop // and single-line /* */ comments.
    Returns (masked, comment) where comment is the // tail (for the glued-ok marker)."""
    out, i, n, comment = [], 0, len(text), ""
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            comment = text[i:]
            break
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        m = re.match(r"(?:u8|u|U|L)?R\"([^()\\\s]{0,16})\(", text[i:])
        if m and (i == 0 or not (text[i-1].isalnum() or text[i-1] == "_")):
            end = text.find(")" + m.group(1) + "\"", i + m.end())
            i = n if end < 0 else end + len(m.group(1)) + 2
            out.append(" S ")
            continue
        m = re.match(r"(?:u8|u|U|L)?\"", text[i:])
        if m and (i == 0 or not (text[i-1].isalnum() or text[i-1] == "_") or m.group(0) == "\""):
            j = i + m.end()
            while j < n and text[j] != "\"":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            # a user-defined-literal suffix ("x"s, "x"sv) belongs to the literal
            while i < n and (text[i].isalnum() or text[i] == "_"):
                i += 1
            out.append(" S ")
            continue
        if c == "\x27" and not (i > 0 and text[i-1].isalnum()):   # char literal, not a digit separator
            j = i + 1
            while j < n and text[j] != "\x27":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            out.append(" C ")
            continue
        out.append(c)
        i += 1
    return "".join(out), comment

def glued(masked):
    s = masked.strip()
    for m in re.finditer(r"\+", s):
        k = m.start()
        if (k + 1 < len(s) and s[k+1] in "+=") or (k > 0 and s[k-1] == "+"):
            continue                                  # ++, +=
        left, right = s[:k].rstrip(), s[k+1:].lstrip()
        binary = (left == "") or bool(IDENT_END.search(left)) or left.endswith("S") or left.endswith("C")
        if not binary:
            continue                                  # unary plus
        lit_left, lit_right = left.endswith(" S") or left == "S", right.startswith("S ") or right == "S"
        if lit_left and lit_right:
            continue                                  # literal next to literal
        if lit_left or lit_right:
            return True
    return False

for raw in sys.stdin.read().splitlines():
    if not raw.strip():
        continue
    parts = raw.split("\t", 2)
    if len(parts) < 3:
        continue
    path, line, text = parts
    t = text.lstrip()
    if t.startswith("#include") or t.startswith("*") or t.startswith("/*"):
        continue
    masked, comment = mask(text)
    if not glued(masked):
        continue
    ok = re.search(r"glued-ok:\s*(\S.*)$", comment)
    if ok:
        passes.append((path, line, ok.group(1).strip()))
    else:
        findings.append((path, line, text.strip()))

for p, l, r in passes:
    print(f"glued-ok  {p}:{l}  ({r})")
if passes:
    print(f"glued-text: {len(passes)} line(s) passed by an explicit // glued-ok: <reason> (register above)")
if findings:
    for p, l, t in findings:
        print(f"{p}:{l}: glued text: {t}")
    print("")
    print(f"glued-text: {len(findings)} changed line(s) build text with + on a string literal.")
    print("Use std::format(\"[{}] {}\", id, text) / std::format_to(std::back_inserter(out), ...) with a literal")
    print("format string; a shape used twice gets ONE named function or constexpr std::string_view k...Format;")
    print("paths are std::filesystem::path joined with /. A justified exception: // glued-ok: <reason>")
    sys.exit(1)
print("glued-text: changed lines are clean")
'
RC=$?
set -e
if [ $RC -gt 1 ]; then
    echo "glued-text: the checker FAILED TO RUN (exit $RC) — environment failure, not a finding." >&2
    exit 2
fi
[ "$MODE" = diff ] && [ $RC -eq 0 ] && echo "glued-text: (vs $BASE)"
exit $RC

#!/bin/bash
# iwyu.sh <file.cpp|file.hpp>... [--fix] — include-what-you-use on the tree's own sources: what each file (and its own
# header) should include, forward-declare or drop. A header is checked through its own .cpp. The compile line is the
# Debug one from compile_commands.json with the precompiled header turned back into a plain prefix header; symbols the
# prefix header (pch.hpp / EditorPCH.hpp) already gives are never proposed, ThirdParty and the C++ library map to their
# public headers through scripts/Dev/iwyu.imp. --fix applies the edits (fix_includes.py, comments kept, headers only
# gain lines) -- compile_check.sh after it. Needs `brew install include-what-you-use`. Prints per file; exit 0.
set -u
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
case "${1:-}" in -h|--help|"") dev_help "$0"; exit 0 ;; esac
command -v include-what-you-use >/dev/null || { echo "iwyu: include-what-you-use not installed (brew install include-what-you-use)"; exit 1; }
FIX=0; FILES=()
for a in "$@"; do [ "$a" = --fix ] && FIX=1 || FILES+=("$a"); done
[ -f "$DEV_BUILD/compile_commands.json" ] || { echo "iwyu: no $DEV_BUILD/compile_commands.json -- configure first"; exit 1; }
LOG="$(dev_logdir iwyu)"

# file -> its compile line, rewritten for include-what-you-use (one line per file in $LOG/commands.sh).
python3 - "$DEV_ROOT" "$DEV_BUILD" "$LOG" "${FILES[@]}" <<'EOF' || exit 1
import json, os, shlex, sys
root, build, log, files = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
db = {}
for e in json.load(open(os.path.join(build, 'compile_commands.json'))):
    if '/Debug/' in e.get('output', ''):
        db.setdefault(os.path.normpath(os.path.join(e['directory'], e['file'])), e)
imp = os.path.join(root, 'scripts', 'Dev', 'iwyu.imp')
out = open(os.path.join(log, 'commands.sh'), 'w')
for f in files:
    a = os.path.normpath(os.path.join(root, f) if not os.path.isabs(f) else f)
    if a.endswith(('.hpp', '.h')):  # a header is checked through its own source file
        a = next((os.path.splitext(a)[0] + x for x in ('.cpp', '.mm') if os.path.splitext(a)[0] + x in db), a)
    e = db.get(a)
    if not e:
        print(f'iwyu: {f}: not compiled in the Debug build (compile_commands.json) -- a header without its own .cpp: name an includer'); continue
    args = shlex.split(e['command'])[1:]
    keep, i = [], 0
    while i < len(args):
        x = args[i]
        if x == '-o' or x in ('-MF', '-MT', '-MQ'): i += 2; continue
        if x in ('-c', '-MD', '-MMD', '-Winvalid-pch', '-fcolor-diagnostics') or x == e['file']: i += 1; continue
        if x == '-Xclang' and i + 1 < len(args):
            y = args[i + 1]
            if y == '-fno-pch-timestamp': i += 2; continue
            if y == '-include-pch': i += 4; continue              # -Xclang -include-pch -Xclang <pch>
            if y == '-include':  # cmake_pch.hxx -> the header it wraps (clang would load the .pch next to it)
                hxx = args[i + 3]
                wrapped = [l.split('"')[1] for l in open(hxx) if l.startswith('#include "')] if os.path.exists(hxx) else []
                for w in wrapped or [hxx]: keep += ['-include', w]
                i += 4; continue
        keep.append(x); i += 1
    iwyu = ['-Xiwyu', f'--mapping_file={imp}', '-Xiwyu', '--prefix_header_includes=keep', '-Xiwyu', '--quoted_includes_first',
            '-Xiwyu', '--max_line_length=160', '-Wno-everything', '-DFMT_CONSTEVAL=']
    report = os.path.join(log, os.path.basename(a) + '.iwyu')
    script = report[:-5] + '.sh'
    open(script, 'w').write(shlex.join(['include-what-you-use'] + keep + iwyu + [a]) + f' > {shlex.quote(report)} 2>&1\n')
    out.write(f'{os.path.relpath(a, root)}\t{script}\n')
EOF

cd "$DEV_BUILD" || exit 1
# All files at once, IWYU_JOBS (default 3: ~1.5 GB each) at a time; then the reports in the order given.
start=$(date +%s)
cut -f2 "$LOG/commands.sh" | tr '\n' '\0' | xargs -0 -n1 -P "${IWYU_JOBS:-3}" sh
while IFS=$'\t' read -r file script; do
    out="${script%.sh}.iwyu"
    # Only the report: "<file> should add/remove ..." blocks and "The full include-list"; compiler errors if any.
    if grep -qE ' (should add|should remove) these lines' "$out"; then
        perl -pe "s#\\Q$DEV_ROOT/\\E##g" "$out" | grep -vE '^\s*$' | grep -vE '^---$'
    elif grep -q 'has correct #includes/fwd-decls' "$out"; then
        echo "$file: correct #includes/fwd-decls"
    else
        echo "$file: include-what-you-use did not finish:"; grep -E 'error:' "$out" | perl -pe "s#\\Q$DEV_ROOT/\\E##g" | head -8
    fi
    if [ "$FIX" = 1 ]; then
        (cd "$DEV_ROOT" && python3 "$(dirname "$(readlink -f "$(command -v include-what-you-use)")")/fix_includes.py" \
            --comments --safe_headers --nosort --basedir="$DEV_ROOT" < "$out") | tail -2
    fi
done < "$LOG/commands.sh"
echo "iwyu: $(wc -l < "$LOG/commands.sh" | tr -d ' ') file(s), $(( $(date +%s) - start ))s; reports in $LOG"

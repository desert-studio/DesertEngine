#!/usr/bin/env bash
# sym.sh <name> [tree]        — where <name> is defined/declared: class/struct/enum/using, Class::name(, functions, constants.
# sym.sh --refs <name> [tree] — how many lines use <name>, and the top 15 files by use count.
# <name> may be qualified (AssetRegistry::FindByGuidReference): the last part is searched, the qualifier filters.
# Searches tracked C/C++/GLSL sources (ThirdParty and Generated excluded); prints file:line + context, ≤15 hits.
# Exit 1 when nothing is found.
set -u
source "$(dirname "$0")/_common.sh"
[ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ] || [ $# -eq 0 ] && { dev_help "$0"; exit 0; }
refs=0; [ "$1" = "--refs" ] && { refs=1; shift; }
full="$1"; tree="${2:-$DEV_ROOT}"
name="${full##*::}"; qual=""; [ "$name" != "$full" ] && qual="${full%::*}"; qual="${qual##*::}"
cd "$tree" || exit 2
specs=( -- '*.cpp' '*.hpp' '*.h' '*.inl' '*.glslh' '*.glsl' ':!ThirdParty' ':!**/Generated/**' )
search() { git grep -n -I "$@" "${specs[@]}"; }

if [ $refs -eq 1 ]; then
    hits=$(search -w -e "$name")
    [ -z "$hits" ] && { echo "sym --refs $name: no uses"; exit 1; }
    n=$(printf '%s\n' "$hits" | wc -l | tr -d ' ')
    files=$(printf '%s\n' "$hits" | cut -d: -f1 | sort -u | wc -l | tr -d ' ')
    echo "$name: $n line(s) in $files file(s); top files:"
    printf '%s\n' "$hits" | cut -d: -f1 | sort | uniq -c | sort -rn | head -15 | awk '{printf "  %4d %s\n",$1,$2}'
    exit 0
fi

n="$name"
# Definitions/declarations: type heads, qualified out-of-line definitions, column-0 free functions,
# indented member declarations ending the line in ; or {, and constants / aliases.
pat="((^|[^A-Za-z0-9_:])(class|struct|enum( class)?|union|namespace|concept)[[:space:]]+([A-Z_]+[[:space:]]+)?$n([^A-Za-z0-9_]|$)"
pat+="|using[[:space:]]+$n[[:space:]]*="
pat+="|^[[:space:]]*([A-Za-z_][A-Za-z0-9_:<>,*& ]*[[:space:]*&])?[A-Za-z0-9_:<>]*[A-Za-z0-9_>]::$n[[:space:]]*\\([^;]*$"
pat+="|^[A-Za-z_][A-Za-z0-9_:<>,*& ]*[[:space:]*&]$n[[:space:]]*\\("
pat+="|^[[:space:]]+(static |virtual |inline |constexpr |explicit |friend |\\[\\[nodiscard\\]\\] )*[A-Za-z_][A-Za-z0-9_:<>,*& ]*[[:space:]*&]$n[[:space:]]*\\([^;]*\\)[^;=]*(;|\\{|$)"
# an indented definition whose parameter list continues on the next line (inside a namespace block):
# `    Report MigrateTextureGuidsV28ToV29( std::vector<X>& entities,` was not found (T6f).
pat+="|^[[:space:]]+[A-Za-z_][A-Za-z0-9_:<>,*& ]*[[:space:]*&]$n[[:space:]]*\\([^;)]*,[[:space:]]*$"
pat+="|(constexpr|const|inline|static)[^=(;]*[[:space:]]$n[[:space:]]*(=|\\{|;|\\[))"
hits=$(search -E -e "$pat" | grep -v -E '^[^:]+:[0-9]+:[[:space:]]*(//|\*|return |if |else|while |for |case )')
if [ -n "$qual" ]; then
    q=$(printf '%s\n' "$hits" | grep -E "$qual::$n|/$qual\\.(h|hpp|cpp):|(class|struct) $qual")
    if [ -n "$q" ]; then hits="$q"; else [ -n "$hits" ] && echo "no $qual::$n; unqualified matches:"; fi
fi
[ -z "$hits" ] && { echo "sym $full: no definition/declaration found (try --refs)"; exit 1; }
total=$(printf '%s\n' "$hits" | wc -l | tr -d ' ')
# Headers first (declarations), then sources; context trimmed to 110 characters.
printf '%s\n' "$hits" | awk -F: '{k=($1 ~ /\.(h|hpp|inl|glslh)$/)?0:1; print k"\t"$0}' | sort -s -k1,1n | cut -f2- | head -15 |
    awk '{i=index($0,":"); f=substr($0,1,i-1); r=substr($0,i+1); j=index(r,":"); l=substr(r,1,j-1); c=substr(r,j+1);
          sub(/^[ \t]+/,"",c); printf "%s:%s  %s\n", f, l, substr(c,1,110)}'
[ "$total" -gt 15 ] && echo "... $((total - 15)) more (qualify the name, e.g. Class::$name)"
exit 0

#!/usr/bin/env bash
# CheckWindowsDlls.sh — refuse a packaged Windows executable that imports a DLL outside the allow-list.
#
#   scripts/CI/CheckWindowsDlls.sh <allow-list.txt> <exe>...
#
# WHY. A game that imports VCRUNTIME140.dll / MSVCP140.dll does not start on a Windows install that
# never ran the Visual C++ Redistributable, and nothing on the build machine shows it: the machine that
# builds the game has the redistributable by definition. The only honest question is the binary's own
# import table, so this reads it with `dumpbin /dependents` (normal AND delay-load imports) and compares
# every entry against scripts/CI/windows_allowed_dlls.txt, where each allowed DLL carries its reason.
#
# dumpbin: $DUMPBIN if set, otherwise located through vswhere (it is not on PATH on a GitHub runner —
# microsoft/setup-msbuild adds MSBuild only). A missing dumpbin is a refusal, never a skip.
#
# A POSITIVE CONTROL, because a parser that finds nothing would pass everything: an image that
# imports zero DLLs does not exist (KERNEL32 at the least), so an empty import list fails the check.
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <allow-list.txt> <exe>..." >&2
    exit 2
fi

allowList="$1"
shift
if [ ! -f "$allowList" ]; then
    echo "CheckWindowsDlls: allow-list '$allowList' not found" >&2
    exit 2
fi

findDumpbin() {
    if [ -n "${DUMPBIN:-}" ]; then
        echo "$DUMPBIN"
        return 0
    fi
    local vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [ ! -x "$vswhere" ]; then
        echo "CheckWindowsDlls: dumpbin not found — set DUMPBIN or install Visual Studio (no vswhere at '$vswhere')" >&2
        return 1
    fi
    local found
    found="$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
        -find 'VC/Tools/MSVC/**/bin/Hostx64/x64/dumpbin.exe' | tr -d '\r' | head -n 1)"
    if [ -z "$found" ]; then
        echo "CheckWindowsDlls: vswhere found no dumpbin.exe (VC tools x64 component missing?)" >&2
        return 1
    fi
    echo "$found"
}

dumpbin="$(findDumpbin)"

# Allow-list rows as "PATTERN FLAG" (FLAG = app-local | system), comments and blank lines dropped.
allowed="$(sed -e 's/#.*//' "$allowList" | awk 'NF { print $1, ( $2 == "app-local" ? "app-local" : "system" ) }')"

# The DLL names from a `dumpbin /dependents` listing: the indented lines under "Image has the following
# [delay load ]dependencies:" up to the "Summary" block.
parseDependents() {
    awk '
        /Image has the following( delay load)? dependencies:/ { inList = 1; next }
        /^[[:space:]]*Summary/                              { inList = 0 }
        inList {
            gsub( /\r/, "" )
            gsub( /^[[:space:]]+|[[:space:]]+$/, "" )
            if ( tolower( $0 ) ~ /\.dll$/ ) print
        }
    '
}

# Prints the flag of the first allow-list row whose pattern matches $1 (case-insensitive), or nothing.
allowFlag() {
    local dll="$1" pattern flag
    shopt -s nocasematch
    while read -r pattern flag; do
        # shellcheck disable=SC2053 # the pattern IS a glob
        if [[ "$dll" == $pattern ]]; then
            shopt -u nocasematch
            echo "$flag"
            return 0
        fi
    done <<< "$allowed"
    shopt -u nocasematch
}

# Is a file named $2 (case-insensitive) in directory $1?
presentIn() {
    find "$1" -maxdepth 1 -type f -iname "$2" | grep -q .
}

status=0
checkBinary() {
    local bin="$1" gameDir="$2"
    local listing deps refused="" missing="" count flag
    listing="$("$dumpbin" /nologo /dependents "$bin" | tr -d '\r')"
    deps="$(printf '%s\n' "$listing" | parseDependents | sort -u -f)"
    if [ -z "$deps" ]; then
        echo "CheckWindowsDlls: '$bin' — parsed ZERO imported DLLs; the dumpbin output was not understood:" >&2
        printf '%s\n' "$listing" >&2
        status=1
        return
    fi
    while IFS= read -r dll; do
        flag="$(allowFlag "$dll")"
        if [ -z "$flag" ]; then
            refused="$refused $dll"
        elif [ "$flag" = "app-local" ] && ! presentIn "$gameDir" "$dll"; then
            missing="$missing $dll"
        fi
    done <<< "$deps"

    count="$(printf '%s\n' "$deps" | wc -l | tr -d ' ')"
    if [ -n "$refused" ]; then
        echo "CheckWindowsDlls: '$bin' imports DLL(s) outside $allowList:$refused" >&2
        echo "  all $count imports: $(printf '%s ' $deps)" >&2
        echo "  A *D.dll / ucrtbased entry is the debug CRT (never shipped); anything else needs a reason in" >&2
        echo "  the allow-list or must go." >&2
        status=1
    fi
    if [ -n "$missing" ]; then
        echo "CheckWindowsDlls: '$bin' imports app-local DLL(s) that are NOT in the package ($gameDir):$missing" >&2
        echo "  the player's machine has no such file; GamePackager must ship it next to the exe." >&2
        status=1
    fi
    if [ -z "$refused" ] && [ -z "$missing" ]; then
        echo "CheckWindowsDlls: '$bin' — $count imports, all allowed and present: $(printf '%s ' $deps)"
    fi
}

for exe in "$@"; do
    if [ ! -f "$exe" ]; then
        echo "CheckWindowsDlls: '$exe' does not exist" >&2
        status=1
        continue
    fi
    gameDir="$(dirname "$exe")"
    checkBinary "$exe" "$gameDir"
    while IFS= read -r dll; do
        [ -n "$dll" ] && checkBinary "$dll" "$gameDir"
    done <<< "$(find "$gameDir" -maxdepth 1 -type f -iname '*.dll' | sort)"
done

exit "$status"

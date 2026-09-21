#!/usr/bin/env bash
# ── IS THE DEBUG SURFACE ACTUALLY ABSENT FROM THE SHIPPED BINARY? ────────────────────────────────────
#
# Desert/Tests/Runtime/ShippingBoundary reads the SOURCES and asserts each instrument is behind
# `DESERT_DEV_INSTRUMENTS`. This reads the LINKED ARTIFACT and asserts the names are not in it. The two
# answer different questions and neither replaces the other: a source census cannot see a static
# library's archive members being pulled in by a reference nobody wrote down, and a symbol dump cannot
# tell you which file was supposed to have a guard.
#
# WHY A SCRIPT AND NOT A TEST. A test binary is compiled in ONE configuration, and it is never the
# Shipping one (the sweep runs Debug). A Debug test cannot build a Shipping engine to look inside it, and
# a test that shelled out to `make` would take forty minutes and would be the build, not a test of it.
#
# USAGE
#   scripts/CI/ShippingSymbols.sh [path/to/Runtime]      (default: build/Bin/Shipping/Runtime)
#
# EXIT CODES ARE THE INTERFACE
#   0  clean — every forbidden name absent, the positive control present
#   1  a forbidden name is in the binary
#   2  THE CHECK COULD NOT RUN — no binary, no nm, or an empty symbol table
#
# 2 IS A SEPARATE CODE ON PURPOSE, and it is the whole reason this script is not three lines of grep.
# `nm` on a stripped binary prints nothing, and "nothing" passes every absence check ever written. That
# is Ф4's shape at the one gate whose entire job is to say what is present, so the positive control below
# is not a nicety: without a name that MUST be found, a green here would be indistinguishable from a
# binary this script failed to read.
set -uo pipefail

BIN="${1:-build/Bin/Shipping/Runtime}"

if [ ! -f "$BIN" ]; then
    echo "ShippingSymbols: '$BIN' does not exist." >&2
    echo "  Build it first: make Runtime config=shipping -j8" >&2
    exit 2
fi
if ! command -v nm >/dev/null 2>&1; then
    echo "ShippingSymbols: nm is not on PATH; the check cannot run." >&2
    exit 2
fi

# THE DUMP GOES TO A FILE, NOT INTO A SHELL VARIABLE, and that is a bug fix rather than a style. With
# the table in a variable every check reads `printf ... | grep -q ...`, and `grep -q` exits the instant
# it matches — which hands `printf` a SIGPIPE, which under `pipefail` makes the PIPELINE fail. The first
# run of this script reported its own positive control missing for exactly that reason, on a binary that
# contained it. §8.5 of the agent brief has this same shape twice over: the last command in a pipeline
# answers for the pipeline, and `-q`/`head` are the two that end it early.
SYMBOLS="$(mktemp -t desert_shipping_syms)"
trap 'rm -f "$SYMBOLS"' EXIT
nm -U "$BIN" >"$SYMBOLS" 2>/dev/null || nm "$BIN" >"$SYMBOLS" 2>/dev/null
SYMCOUNT="$(grep -c . "$SYMBOLS" || true)"
if [ "$SYMCOUNT" -lt 100 ]; then
    echo "ShippingSymbols: '$BIN' yielded $SYMCOUNT symbols — that is a stripped or unreadable binary," >&2
    echo "  and an absence check over an empty list passes on nothing at all." >&2
    exit 2
fi

# THE POSITIVE CONTROL. `DrawIndexedCounted` is the draw funnel itself: it must be in every build of this
# engine, and it lives in the very translation unit the draw COUNTER was cut out of. So it proves two
# things at once — that this dump is readable, and that the file the counter was removed from is still
# here and still linked. A control in a file unrelated to the cut would prove only the first.
CONTROL="DrawIndexedCounted"
if [ "$(grep -cF "$CONTROL" "$SYMBOLS" || true)" -eq 0 ]; then
    echo "ShippingSymbols: the positive control '$CONTROL' is NOT in '$BIN'." >&2
    echo "  Every absence reported below would be meaningless, so nothing is reported." >&2
    exit 2
fi

# ── THE REGISTER: one named row per instrument, never a count ───────────────────────────────────────
#
# Each row is a name fragment that can only come from that instrument. A count of forbidden symbols
# could be satisfied by editing the count; a row can only be satisfied by removing the instrument or by
# deleting a line somebody has to sign for.
#
# `Optick` is a row even though nothing in the engine calls it any more: it is still on the shipping link
# line (BuildScripts/ThirdParty/Optick.lua builds in every configuration), and the claim being made is
# precisely that an unreferenced archive contributes nothing. That claim is worth checking rather than
# assuming — it is the difference between "we stopped calling it" and "it is not there".
FORBIDDEN=(
    "RuntimeShot"          # the runtime frame capture's flag parser
    "ParseRuntimeShot"     # ditto, by name
    "RecordShotIfDue"      # its half inside the layer
    "DrawCounter"          # the draw-call counter
    "MemoryWatch"          # the per-frame memory watch
    "MemoryReadout"        # the device/heap/process readout it samples
    "SyncLoadLedger"       # the synchronous-load detector
    "LoadTimingScope"      # its per-asset scope
    "VulkanGpuProfiler"    # the GPU timestamp query pools
    # MANGLED, not "Profiling": the plain word is also inside a vendored shaderc table
    # (`pygen_variable_KernelProfilingInfoEntries`), and a row that matches somebody else's symbol is a
    # row that will be deleted as a false alarm — taking the real check with it.
    "6Common9Profiling"    # Common::Profiling — the in-engine CPU aggregator
    "Optick"               # the external profiler

    # ── THE DEVELOPER-ONLY GRAPHICS PIPELINES (В12) ─────────────────────────────────────────────────
    #
    # Four of the 67 pipelines a Shipping boot used to create were reachable ONLY through
    # Graphic::DebugViewState, which nothing in the player's source set writes: StaticMeshWireframe,
    # DebugLinePipeline, OverdrawPipeline and OverdrawResolvePipeline. Their creation sites now sit
    # behind DESERT_DEV_INSTRUMENTS; Desert/Tests/Runtime/ShippingPipelines is the source-side census
    # and these rows are the linked-artifact half of the same claim.
    #
    # MANGLED, AND THAT IS NOT PEDANTRY. The plain word "DebugLine" matches THREE symbols of vendored
    # SPIRV-Tools and spirv-cross in this very binary (spv::Function::setDebugLineInfo,
    # spvtools::opt::Instruction::AddDebugLine, ::IsDebugLineInst) — a row that matches somebody else's
    # symbol is a row that gets deleted as a false alarm, taking the real check with it, and this
    # script's header already carries one instance of exactly that. "7Graphic" pins the namespace, and
    # the Itanium length prefixes ("17", "16", "23") pin the whole identifier, so
    # "7Graphic16MaterialOverdraw" cannot match MaterialOverdrawResolve.
    "7Graphic17MaterialDebugLine"         # the AABB-wireframe material
    "7Graphic16MaterialOverdraw"          # the overdraw accumulation material
    "7Graphic23MaterialOverdrawResolve"   # its fullscreen heat-map resolve
    "MeshRenderer18SetupDebugLinePass"    # the debug-line pipeline's builder
    "MeshRenderer17SetupOverdrawPass"     # the two overdraw pipelines' builder
    "MeshRenderer20RenderOverdrawManual"  # the pass that would draw them
)

FOUND=0
for name in "${FORBIDDEN[@]}"; do
    HITS="$(grep -F "$name" "$SYMBOLS" || true)"
    if [ -n "$HITS" ]; then
        FOUND=1
        echo "ShippingSymbols: '$name' is in the shipping binary:" >&2
        printf '%s\n' "$HITS" | head -8 | sed 's/^/    /' >&2
    fi
done

if [ "$FOUND" -ne 0 ]; then
    echo "" >&2
    echo "  A development instrument reached the artifact a player runs. The boundary is" >&2
    echo "  DESERT_DEV_INSTRUMENTS (Common/Core/DevInstruments.hpp); the source-side census is" >&2
    echo "  Desert/Tests/Runtime/ShippingBoundary." >&2
    exit 1
fi

echo "ShippingSymbols: clean — ${#FORBIDDEN[@]} registered instruments absent from '$BIN'"
echo "  ($SYMCOUNT symbols read; positive control '$CONTROL' present)"
exit 0

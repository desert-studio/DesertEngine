-- Debug / Release / Shipping build configurations (workspace scope, applies to all projects).

filter "configurations:Debug"
    runtime "Debug"
    symbols "On"

filter "configurations:Release"
    runtime "Release"
    optimize "On"
    defines { "NDEBUG" }

-- ── THE SHIPPING CONFIGURATION — WHAT A PLAYER RUNS ────────────────────────────────────────────────
--
-- WHY THIS EXISTS. Everything this engine grew in order to develop itself — the runtime frame capture
-- (`--shot`), the Optick profiler and the GPU timestamp readout, the draw-call counter, the per-frame
-- memory watch and the per-asset synchronous-load ledger — was compiled into the player binary
-- unconditionally, in BOTH existing configurations. There was no configuration without them because
-- there was no third configuration at all. A game shipped from this repository therefore shipped every
-- development instrument it had, and the surface grew with every task: four of the five instruments
-- named above were added in a single week.
--
-- WHY A CONFIGURATION AND NOT A FLAG. "Off by default, switched on with --shot" is a flag, and a flag
-- is a property of the RUN, not of the artifact: whoever has the binary can turn it on. The boundary
-- that cannot be crossed is the one the compiler draws. `DESERT_CONFIG_SHIPPING` is what the sources
-- test, and under it the instruments are not disabled — their translation units are not compiled and
-- their call sites are not emitted.
--
-- WHY IT IS DEFINED HERE AND NOT IN EACH PROJECT, unlike DESERT_CONFIG_DEBUG / _RELEASE. Those are
-- spelled out in five project scripts, which is five places to forget; this one is the gate for a
-- SECURITY-shaped property ("the debug surface is absent"), and a project that silently did not get the
-- define would silently get the instruments back. Workspace scope is what makes that impossible, and it
-- is the same argument ENTT_USE_ATOMIC is declared at workspace scope in Workspace.lua for.
--
-- `symbols "Off"` DROPS THE DWARF, NOT THE SYMBOL TABLE, and the difference is what makes the proof
-- possible. scripts/CI/ShippingSymbols.sh reads the linked binary's own table with `nm` and asserts the
-- instruments' names are not in it; a genuinely stripped binary would answer that question with an
-- empty list, which is indistinguishable from an answer of "none" — Ф4's shape, at the one gate whose
-- whole job is to say what is present. So the script carries a POSITIVE CONTROL: a symbol that must be
-- found. If stripping is ever wanted it belongs in a release step after the census, never before it.
filter "configurations:Shipping"
    runtime "Release"
    optimize "Full"
    symbols "Off"
    defines { "NDEBUG", "DESERT_CONFIG_SHIPPING" }

filter {}

-- Platform detection helpers shared by every premake script.
-- Included once from the root premake5.lua, before anything else.

DesertPlatform = {}

DesertPlatform.IsWindows = os.target() == "windows"
DesertPlatform.IsMacOS   = os.target() == "macosx"
DesertPlatform.IsLinux   = os.target() == "linux"

-- Writes a file premake generates for the compile (a third-party config.h, a template instantiation) only when its
-- text changed. io.writefile restamped every one on every generation, so each `premake5 gmake` recompiled every
-- Assimp, OpenEXR and dav1d source that includes one, and relinked Desert behind them (BLD-DEVTESTS measured ~190
-- TUs per run once suite.sh --dev-suites regenerates on a change of suite list).
function DesertPlatform.WriteGenerated(path, text)
    os.mkdir(path:match("^(.*)/[^/]*$"))
    if os.writefile_ifnotequal(text, path) < 0 then
        error("cannot write " .. path, 0)
    end
end

-- Extension of built executables on the target platform ("" on unix-likes).
DesertPlatform.ExeExt = DesertPlatform.IsWindows and ".exe" or ""

-- Homebrew prefix on macOS (arm64 -> /opt/homebrew, intel -> /usr/local).
-- nil when not on macOS or when Homebrew is not installed.
DesertPlatform.HomebrewPrefix = nil
if DesertPlatform.IsMacOS then
    for _, prefix in ipairs( { os.getenv( "HOMEBREW_PREFIX" ), "/opt/homebrew", "/usr/local" } ) do
        if prefix and os.isdir( prefix .. "/include" ) then
            DesertPlatform.HomebrewPrefix = prefix
            break
        end
    end
end

-- Returns the full command line for a tool built into build/Bin, with the
-- platform-correct executable extension (DesertHeaderTool.exe vs DesertHeaderTool).
-- Uses the absolute workspace path: in pre/postbuildcommands gmake expands
-- %{wks.location} relative to the project dir, which breaks the command.
function DesertPlatform.BuiltToolPath( name )
    return '"' .. _MAIN_SCRIPT_DIR .. '/build/Bin/%{cfg.buildcfg}/' .. name .. DesertPlatform.ExeExt .. '"'
end

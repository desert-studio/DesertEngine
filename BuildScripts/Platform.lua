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

-- LINKS EVERY OBJECT OF A STATIC LIBRARY, NOT ONLY THE ONES SOMETHING REFERENCES (BLD-EDITORCORE). A linker
-- takes an object out of a static library only to resolve a symbol, so a file whose one job is a static
-- initialiser -- a self-registration (DESERT_REGISTER_REFLECTED_COMPONENT, a component editor's registrar) --
-- is referenced by nothing and silently left out of the binary: no build error, the registry is just short.
-- Linking the whole archive makes "compiled into the library" mean "in the binary", so a new registering file
-- needs nothing named anywhere. Unreal's UnrealEd gets the same from its DLL in a modular build.
--   macOS ld64: -force_load <archive>; GNU ld / lld: --whole-archive ... --no-whole-archive; MSVC:
--   /WHOLEARCHIVE:<lib> (matched against the library the project reference puts on the link line).
-- The library stays in `links` as well, for the build order and the project reference; loading it whole first
-- leaves the later plain reference nothing to resolve. Call at project scope, with no filter active.
function DesertPlatform.LinkWholeArchive( name )
    local archive = _MAIN_SCRIPT_DIR .. "/build/Bin/%{cfg.buildcfg}/lib" .. name .. ".a"
    filter "system:macosx"
        linkoptions { '-Wl,-force_load,"' .. archive .. '"' }
    filter "system:linux"
        linkoptions { '-Wl,--whole-archive "' .. archive .. '" -Wl,--no-whole-archive' }
    filter "system:windows"
        linkoptions { "/WHOLEARCHIVE:" .. name .. ".lib" }
    filter {}
end

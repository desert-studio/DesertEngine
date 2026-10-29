-- meshoptimizer (zeux/meshoptimizer, MIT). Mesh simplification / LOD generation +
-- vertex/index optimization. Sources live in ThirdParty/meshoptimizer (cloned by
-- scripts/MacOS/Setup.sh — not a submodule). Single public header src/meshoptimizer.h.

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/meshoptimizer"

if not os.isfile( root .. "/src/meshoptimizer.h" ) then
    error( "ThirdParty/meshoptimizer sources are missing. Run scripts/MacOS/Setup.sh (macOS) " ..
           "or clone https://github.com/zeux/meshoptimizer into ThirdParty/meshoptimizer." )
end

-- The pinned release (scripts/MacOS/Setup.sh, scripts/Windows/Setup.bat): LOD reduction relies on its
-- Permissive / Prune simplification options, so a stale clone is an error, not a silent older reducer.
if not io.readfile( root .. "/src/meshoptimizer.h" ):find( "#define MESHOPTIMIZER_VERSION 1030", 1, true ) then
    error( "ThirdParty/meshoptimizer is not v1.3. Re-run scripts/MacOS/Setup.sh (macOS) or " ..
           "scripts\\Windows\\Setup.bat (Windows); they re-fetch a mismatched version." )
end

project "MeshOptimizer"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/" .. "MeshOptimizer" )

    files
    {
        root .. "/src/**.cpp",
        root .. "/src/**.h",
    }

    includedirs { root .. "/src" }

    filter "system:windows"
        systemversion "latest"

    filter "system:not windows"
        pic "On"

    filter {}

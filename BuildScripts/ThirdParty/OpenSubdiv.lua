-- OpenSubdiv (PixarAnimationStudios/OpenSubdiv, Modified Apache 2.0; ThirdParty/OpenSubdiv/LICENSE.txt and
-- NOTICE.txt, listed in ThirdParty/THIRD_PARTY_LICENSES.md). Pinned submodule at the v3_6_0 release tag.
--
-- Why it is here: UE's Subdivide (ModelingComponentsEditorOnly/Operations/SubdividePoly.cpp) is built on it, and
-- our port (Engine/Geometry/MeshCore/DynamicMesh/Operations/SubdividePoly.cpp) is too.
--
-- ONLY the CPU refinement layers are compiled: sdc (schemes), vtr (topology tables) and far (refiner, primvar
-- interpolation, patch/stencil tables). No osd (GPU evaluators), bfr, hbr, examples or CMake: the upstream
-- CMakeLists is not run, so nothing here depends on it.

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/OpenSubdiv"

if not os.isfile( root .. "/opensubdiv/far/topologyRefiner.h" ) then
    error( "ThirdParty/OpenSubdiv sources are missing. Run `git submodule update --init ThirdParty/OpenSubdiv`." )
end

project "OpenSubdiv"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/" .. "OpenSubdiv" )
    warnings "Off"

    files
    {
        root .. "/opensubdiv/version.cpp",
        root .. "/opensubdiv/version.h",
        root .. "/opensubdiv/sdc/*.cpp",
        root .. "/opensubdiv/sdc/*.h",
        root .. "/opensubdiv/vtr/*.cpp",
        root .. "/opensubdiv/vtr/*.h",
        root .. "/opensubdiv/far/*.cpp",
        root .. "/opensubdiv/far/*.h",
    }

    -- Sources include each other as "../far/x.h"; consumers include <opensubdiv/far/x.h>.
    includedirs { root }

    filter "system:windows"
        systemversion "latest"
        -- sdc/loopScheme.h, sdc/catmarkScheme.h and far/loopPatchBuilder.cpp use M_PI; upstream CMake passes
        -- the same define for MSVC (CMakeLists.txt:263).
        defines { "_USE_MATH_DEFINES" }

    filter "system:not windows"
        pic "On"

    filter {}

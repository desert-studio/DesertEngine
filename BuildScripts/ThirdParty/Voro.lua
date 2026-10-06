-- voro++ 0.4.6 (Chris H. Rycroft, LBNL; modified-BSD licence in ThirdParty/voro++/LICENSE). The 3D Voronoi
-- cell computation the fracture bake cuts with (Engine/Destruction/FractureCells.cpp) — the same library and
-- version UE vendors for its Voronoi module (Engine/Source/Runtime/Experimental/Voronoi/Private/voro++). The
-- sources are the upstream v0.4.6 tag (github.com/chr1shr/voro), vendored unmodified; UE's thread-safety edits
-- are not needed because the bake computes one container serially.

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/voro++"

project "Voro"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/" .. "Voro" )

    files
    {
        root .. "/src/**.cc",
        root .. "/src/**.hh",
    }

    -- v_base.cc #includes the generated worklist table; compiling it on its own would define it twice.
    removefiles { root .. "/src/v_base_wl.cc" }

    includedirs { root .. "/src" }

    warnings "Off"

    filter "system:windows"
        systemversion "latest"

    filter "system:not windows"
        pic "On"

    filter {}

-- MikkTSpace (Morten S. Mikkelsen, 2011; zlib licence, in the header of ThirdParty/mikktspace/mikktspace.h).
-- The tangent-space generator UE's mesh build calls (Engine/Source/Runtime/MeshUtilitiesCommon, ComputeMikktTangents)
-- and Blender bakes normal maps against; the engine's one tangent generator (Engine/Geometry/MikkTangents.cpp)
-- drives it. Vendored unmodified from github.com/mmikk/MikkTSpace at 3e895b49d05ea07e4c2133156cfa94369e19e409.

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/mikktspace"

project "MikkTSpace"
    kind "StaticLib"
    language "C"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/" .. "MikkTSpace" )

    files
    {
        root .. "/mikktspace.c",
        root .. "/mikktspace.h",
    }

    includedirs { root }

    warnings "Off"

    filter "system:windows"
        systemversion "latest"

    filter "system:not windows"
        pic "On"

    filter {}

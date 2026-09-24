local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMesh.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshAttributes.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshConversion.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshNormals.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshPolyGroups.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshSelection.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshOperations.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshTopologyOperations.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Geometry/EditMeshModelOperations.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",        -- <Engine/Geometry/EditMeshOperations.hpp>
        "%{wks.location}/Desert/Tests/Engine/EditMesh", -- EditMeshTestSupport.hpp
    }
    externalincludedirs {
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    links { "Common", "Optick" }

    filter "configurations:Debug"
        for name, path in pairs(deps.TestSpecific.Libraries.Debug) do
            links { path }
        end

    filter "configurations:Release"
        for name, path in pairs(deps.TestSpecific.Libraries.Release) do
            links { path }
        end

    filter {}

print("Configured test project: " .. test_name)

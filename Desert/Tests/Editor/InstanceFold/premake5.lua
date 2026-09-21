-- "NOBODY TYPES FIVE HUNDRED MATRICES BY HAND."
--
-- The InstancedStaticMeshComponent, its serialization, its draw command and its shadow flag all existed
-- before this suite; what did not exist was any way to PRODUCE one from a selection, which left the
-- instance list a feature with no author. The fold is that producer, and its DECISION -- refuse a mixed
-- selection, refuse to drop a property an ISM cannot carry, keep the world matrices in selection order --
-- is what this suite asks about.
--
-- Editor/Core/Commands/InstanceFold.cpp is std + glm: no entt, no ImGui, no renderer. That is what lets
-- the decision be asked without a device, and it is why the mutation half of the editor lives elsewhere.
-- It also pins Geometry::PrimitiveTypeName, because a combo box positioned by hand offered "Plane" for
-- the Pyramid enumerator for as long as the ISM editor has existed.

local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Editor/Source/Editor/Core/Commands/InstanceFold.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source", -- <Engine/Geometry/PrimitiveType.hpp>, header-only here
        "%{wks.location}/Editor/Source",
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

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

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

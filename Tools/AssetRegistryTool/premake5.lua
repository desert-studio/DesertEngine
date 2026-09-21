-- AssetRegistryTool — writes and checks a project's cooked asset registry with no engine and no GPU.
--
-- It exists because the registry is now the ONLY route from a content file to the engine: since T2.4
-- neither host walks the content roots at boot, so a project with no registry has no content and the
-- editor cannot write one on its way up (its own shader preload is the first reader). This tool breaks
-- that circle, and the same binary is the failing half of the cook gate in CI.
--
-- Links Common alone, like AssetClosure next door, for the same reason: the census it walks
-- (Common/Content/ContentKinds.hpp) and the format it writes (Common/Utilities/AssetRegistry.hpp) are
-- both below the engine, so this builds and runs on a machine with no Vulkan loader.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "AssetRegistryTool"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
    }

    includedirs {
        "%{wks.location}/Tools/Shared",
        "%{wks.location}/Desert/Common/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    -- CommonSpecific carries reflect-cpp, which Common/Project/ProjectFormat.hpp includes directly
    -- (<rflcpp/rfl/ExtraFields.hpp>) — reading a .deproj is what this tool starts from.
    for name, path in pairs(deps.CommonSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    links { "Common", "ReflectCpp" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
        -- Common contains Objective-C (file dialog); linking it needs AppKit + the ObjC runtime.
        links { "Cocoa.framework", "Foundation.framework" }

    filter {}

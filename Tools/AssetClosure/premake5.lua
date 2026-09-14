-- AssetClosure — prints the set of assets one scene needs, for the packaging scripts.
--
-- It compiles the EDITOR's own reference scan (AssetReferences.cpp + AssetReferencesScan.cpp) rather
-- than restating the token rule: an asset graph the packager disagrees with is a package that is
-- missing a file the editor can see. Those two translation units deliberately know nothing of the
-- engine — the ProjectContext half lives in AssetReferencesScanProject.cpp, which is NOT listed here
-- — so this links Common alone and runs on a CI machine with no Vulkan loader.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "AssetClosure"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
        "%{wks.location}/Editor/Source/Editor/Core/AssetReferences.cpp",
        "%{wks.location}/Editor/Source/Editor/Core/AssetReferencesScan.cpp",
    }

    includedirs {
        "%{wks.location}/Tools/Shared",
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Editor/Source",
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

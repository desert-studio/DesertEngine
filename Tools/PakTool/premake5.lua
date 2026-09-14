-- PakTool — standalone CLI over the ENGINE's .dpak archive format (Common::Utils::PakFile — the same
-- code the Runtime mounts and the editor packs with; the tool can never drift from the engine).
-- Create/list/extract archives from scripts and CI without booting the editor.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "PakTool"
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

    links { "Common" }

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

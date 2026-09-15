-- DomeSheet — plans a sweep of the whole celestial dome and assembles the captured tiles into one
-- labelled contact sheet. Depends on nothing but the vendored stb, exactly as ImageStat, LineJump and
-- ImageDiff do, so it runs on a machine that cannot build an engine.
project "DomeSheet"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "Source/**.cpp",
    }
    includedirs {
        "%{wks.location}/Tools/Shared",
    }


    externalincludedirs {
        "%{wks.location}/ThirdParty/stb/include",
    }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }

    filter {}

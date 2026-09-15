-- LineJump — standalone CLI that measures the largest step in mean luminance between adjacent rows and
-- between adjacent columns of a rendered frame, so a banding claim can be checked instead of asserted.
-- Built exactly like ImageStat next door: nothing but the vendored stb_image, deliberately not linked
-- against the engine, so it runs on a machine that cannot build one. It answers the question ImageStat
-- structurally cannot — a band shifts no percentile, because it is local to a line.
project "LineJump"
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

-- ImageStat — standalone CLI that measures the luminance distribution of a rendered frame, so a claim
-- about a rendering change can be checked instead of asserted. Depends on nothing but the vendored
-- stb_image; deliberately not linked against the engine, so it runs on a machine that cannot build one.
project "ImageStat"
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

-- ImageDiff — standalone CLI that compares two rendered frames over a rectangle and reports both the SIZE
-- of the difference and its SHAPE, the second being the axis that separates a ghost from speckle. Depends
-- on nothing but the vendored stb_image, exactly as ImageStat and LineJump do, so it runs on a machine
-- that cannot build an engine.
project "ImageDiff"
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

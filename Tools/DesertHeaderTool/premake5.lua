-- DesertHeaderTool — standalone codegen utility (no engine dependencies).
project "DesertHeaderTool"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    files {
        "**.cpp",
        "**.hpp",
    }

    includedirs {
        "%{wks.location}/Tools/Shared",
    }

    -- Keep the tool self-contained: it links nothing from the engine and uses only the STL.
    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

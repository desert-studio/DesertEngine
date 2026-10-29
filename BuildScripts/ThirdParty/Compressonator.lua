-- AMD Compressonator CMP_Core, BC7 ENCODER ONLY (MIT, ThirdParty/compressonator/LICENSE). The texture cook's BC7
-- encode (Engine/Core/Formats/BlockCompression.cpp): all eight BC7 modes, including the two- and three-subset
-- partitions a 4x4 block needs when it straddles two colours (a yellow petal over a green blade). Vendored
-- unmodified from github.com/GPUOpen-Tools/compressonator, commit 095a143a (2026-10-19), cmp_core/: the BC7 kernel
-- and the five headers it includes, nothing else. Built as plain C++ on every platform (no ISPC toolchain).
--
-- OPTIMISED IN EVERY CONFIGURATION, like the engine's other derived-data builders (Desert/Desert/premake5.lua):
-- at -O0 a 2k albedo's BC7 chain takes minutes instead of seconds, and the cook is what the editor waits on.

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/compressonator/cmp_core"

project "Compressonator"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/" .. "Compressonator" )

    files
    {
        root .. "/shaders/bc7_encode_kernel.cpp",
        root .. "/shaders/*.h",
        root .. "/source/*.h",
    }

    includedirs { root .. "/shaders", root .. "/source" }

    warnings "Off"
    optimize "Speed"

    filter "system:windows"
        systemversion "latest"
        runtimechecks "Off" -- Debug's /RTC1 and /O2 together are D8016

    filter "system:not windows"
        pic "On"

    filter {}

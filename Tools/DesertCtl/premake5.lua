-- DesertCtl — the control channel's client. One request per invocation, the reply on stdout, and THE
-- OUTCOME IN THE EXIT STATUS, which is the whole reason it is a program rather than a shell function:
-- a script that drives the editor has to be able to stop when a command was refused, and grepping a JSON
-- line for the word "ok" is the kind of check that passes on a payload that happens to contain it.
--
-- It links reflect-cpp for exactly that: the reply is PARSED, so the status comes from the response's own
-- `ok` field and not from a substring of it. Nothing else is linked — no engine, no editor, no window —
-- so this builds and runs on a machine that cannot build a renderer.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

project "DesertCtl"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    -- The reply is read through the JSON facade (Common::Json), compiled in here as one source rather than by
    -- linking Common: Common drags Optick and Cocoa into a tool whose whole job is one socket round-trip. The
    -- facade's ResultStr needs fmt, which comes header-only from the vendored spdlog.
    files {
        "Source/**.cpp",
        "%{wks.location}/Desert/Common/Source/Common/Json/Json.cpp",
    }
    includedirs {
        "%{wks.location}/Tools/Shared",
        "%{wks.location}/Desert/Common/Source",
    }

    defines { "FMT_HEADER_ONLY" }

    externalincludedirs {
        "%{wks.location}/ThirdParty/reflect-cpp/include",
        "%{wks.location}/ThirdParty/spdlog/include",
    }

    filter "system:not windows"
        links { "ReflectCpp" }
    filter {}

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

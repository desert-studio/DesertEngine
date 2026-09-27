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
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source/Common/Json/Json.cpp",
    }
    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Tools/Shared",
        -- For Common/Core/LocalSocket.hpp (header-only: the editor's end of this channel opens the same
        -- socket, and the platform differences are written once rather than once per end) and for the
        -- facade's Json.cpp compiled in above. No Common library is linked -- this tool builds where a
        -- renderer cannot.
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
    }

    defines { "FMT_HEADER_ONLY" }

    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/spdlog/include",
    }

    -- ON EVERY PLATFORM, which it was not before. The Windows build of this tool used to be a dozen lines
    -- that printed "no transport on Windows" and returned: it reached no parser, so it linked no parser,
    -- and the exclusion below looked harmless. Now that the channel HAS a Windows transport the Windows
    -- build does the same work as the others, and without this it fails to link with LNK2019 on
    -- rfl::Generic's constructors and yyjson_read_opts.
    links { "ReflectCpp" }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
        -- AF_UNIX over Winsock, and the DACL on the socket file (Common/Core/LocalSocket.hpp).
        links { "ws2_32", "advapi32" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }

    filter {}

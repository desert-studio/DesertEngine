-- "THE TRANSPORT NOTICES A CLIENT THAT HAS GONE, BEFORE IT JUDGES THE NEXT ONE."
--
-- The one part of the control channel a pure header cannot cover: the socket. It is here because of a
-- defect that reached a live editor and was found by driving it -- the client slot was still occupied by a
-- peer that had already closed, because a disconnected peer is only noticed by READING from it, and the
-- accept loop ran first. For the way this channel is actually used -- one request per invocation of
-- desertctl: connect, ask, read, close -- that made every second command come back "another client already
-- holds this editor's control channel".
--
-- So the suite drives the real ControlSocket over a real unix socket in a temp directory. No editor, no
-- window, no GPU: it links Common for the logger and nothing else.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Editor/Source/Editor/Core/Control/ControlSocket.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Editor/Source",
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
        -- The suite drives the real socket, so it links what the socket needs: AF_UNIX over Winsock and
        -- the DACL on the socket file (Common/Core/LocalSocket.hpp).
        links { "ws2_32", "advapi32" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    links { "Common", "Optick" }

    filter "configurations:Debug"
        for name, path in pairs(deps.TestSpecific.Libraries.Debug) do
            links { path }
        end

    filter "configurations:Release"
        for name, path in pairs(deps.TestSpecific.Libraries.Release) do
            links { path }
        end

    filter {}

print("Configured test project: " .. test_name)

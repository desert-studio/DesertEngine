-- MediaPlayback: the Media core (WebM demuxer, dav1d AV1 decoder, libopus decoder, MediaPlayer's clock
-- and transport) against a one-second clip made by ffmpeg (Desert/Tests/Data/Media/red_440hz_1s.webm:
-- 320x180 solid red AV1 at 30 fps + a 440 Hz stereo Opus tone). It compiles the Media sources directly and
-- links the two codec libraries; no GPU and no audio device are involved (the test's sink is a counter).

local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')
local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Media/WebmDemuxer.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Media/MediaCodecs.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Media/MediaPlayer.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/dav1d/include",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/opus/include",
    }

    defines { 'DESERT_MEDIA_TEST_CLIP="' .. _MAIN_SCRIPT_DIR .. '/Desert/Tests/Data/Media/red_440hz_1s.webm"',
              'DESERT_MEDIA_PATTERN_CLIP="' .. _MAIN_SCRIPT_DIR .. '/Desert/Tests/Data/Media/testsrc2_1080p_5s.webm"' }

    links { "Dav1d", "Opus" }

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end
    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter {}

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

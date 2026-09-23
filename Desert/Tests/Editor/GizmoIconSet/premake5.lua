local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- NO GRAPHICS, AND THAT IS WHY THE TABLE IS SPLIT THE WAY IT IS. The census needs the role table
    -- and the SVG parser and nothing else; GizmoIconSet.cpp (which reaches Runtime::IconService and
    -- therefore the whole render stack) is deliberately NOT compiled in, so this suite stays a
    -- filesystem + parser test that runs anywhere.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Vector/VectorImage.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",   -- <Common/Core/Constants.hpp>
        "%{wks.location}/Desert/Desert/Source",   -- <Engine/Vector/VectorImage.hpp>
        "%{wks.location}/Editor/Source",          -- <Editor/Core/GizmoIconSet.hpp>
    }

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

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

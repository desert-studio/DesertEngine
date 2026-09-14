local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Text/FontBaker.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Text/Msdf.cpp",
        "%{wks.location}/ThirdParty/stb/stb_truetype.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",       -- <Engine/Text/FontBaker.hpp>
        -- The shader root, because SdfTextReference.hpp compiles Common/SdfText.glslh AS C++: the
        -- median and the screen-space ramp under test are the text the fragment shaders run, not a
        -- second copy of them that could agree with itself while the GPU does something else.
        "%{wks.location}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/stb/include",      -- <stb_truetype/stb_truetype.h>
    }

    -- glm, for the vec2/vec3 the shader text is compiled against.
    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

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

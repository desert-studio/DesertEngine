local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- Both the graph COMPILER (editor) and the DShader PARSER (engine) are compiled straight
        -- into the test: the test builds a Document, compiles it to DShader text and parses that text
        -- back with the SAME parser the engine uses — an end-to-end check of the domain contract. Both
        -- units are dependency-light (Common + reflect-cpp headers only), so no engine link is needed.
        "%{wks.location}/Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.cpp",
        -- The FILE half, which the catalogue above is compiled against: a `.dgraph` is an asset now, so
        -- pins/nodes/links and the JSON round trip live in the engine where the asset system can name them.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/ShaderGraph.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Source/Editor/Panels/NodeGraph", -- ShaderGraph.hpp
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

    links { "Common", "Optick" } -- Commons JobSystem registers worker threads with Optick

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

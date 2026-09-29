local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- The parser is compiled directly into the test (it only depends on Core/Formats
        -- headers + Common) so the test doesn't have to link the whole engine.
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/MaterialImportContract.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/TextureChannelPack.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/Assimp/SourceMaterialAdapter.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/Assimp/SourceAlphaMode.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Import/Assimp/SourceTexturePath.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/stb_image.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source", -- DShaderParser (the template's texture Properties)
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/include",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/Editor/ThirdParty/assimp/include",
        "%{_MAIN_SCRIPT_DIR}/build/generated/assimp/include",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",
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

    links { "Common", "Optick", "Assimp" } -- Commons JobSystem registers worker threads with Optick

    -- gtest comes from Dependencies.lua (prebuilt .lib on Windows, Homebrew on macOS)
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

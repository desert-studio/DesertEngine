local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The unit under test is Editor/.../FileExplorer/NewCloudAsset.cpp — WHAT a freshly created cloud
    -- asset contains. It is a separate translation unit from FileExplorerPanel.cpp for exactly this
    -- reason: that one pulls ImGui, the renderer and the editor in with it and could never be linked here.
    --
    -- Beside it: the four formats' own encoders/decoders and the four asset wrappers whose static `Save`
    -- writes them. Listed as SOURCES rather than linked as libDesert, which drags Vulkan and the whole
    -- renderer in — the cloud formats were deliberately written GPU-free, and listing the files is what
    -- keeps checking that. CloudTypeAsset.cpp is the one that reaches further (it resolves a type's noise
    -- volume through the AssetManager), so AssetManager.cpp and the mesh asset it names come with it.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Panels/FileExplorer/NewCloudAsset.cpp",

        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudTypeData.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolume.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolumeGenerator.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",

        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudLayoutAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudNoiseVolumeAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudModellingVolumeAsset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Assets/CloudTypeAsset.cpp",
    }

    -- The SHADER ROOT is on the path because CloudNoiseVolumeGenerator.cpp compiles
    -- Editor/Resources/Shaders/Common/CloudNoise.glslh AS C++ — the same arithmetic the march runs.
    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source", -- <Editor/Panels/FileExplorer/NewCloudAsset.hpp>
        "%{_MAIN_SCRIPT_DIR}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include", -- the `.decloudtype` is rfl::json
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
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
    filter {}

    -- Common: the Result type every refusal is carried in, plus the VFS the asset wrappers read through.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

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

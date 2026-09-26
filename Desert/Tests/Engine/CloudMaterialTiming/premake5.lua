local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- THE SUBJECT IS A RELATION between two files that ship, so both of them are compiled in rather than
    -- stubbed: the DSL parser, because the claim under test is an attribute of the shader's Properties
    -- block as the engine actually reads it; and Engine/Assets/CloudProceduralVolume.cpp, because the
    -- question "does moving this value force a rebake" is answered by that file's own
    -- CloudProceduralParamsEqual and by nothing else. A second implementation of either would make this
    -- suite green about code that does not run.
    --
    -- CloudLayout.cpp and CloudModellingVolume.cpp come along for the same reason they do in
    -- CloudProceduralField: the bake reads the painted layout to decide a cell's coverage and shares the
    -- sculpting maths, so everything that compiles the bake compiles those two. Neither brings an asset
    -- layer, a GPU or a filesystem with it.
    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudProceduralVolume.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
        "%{wks.location}/Editor/Resources/Shaders",
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

    -- Common: the Result/error type every refusal in the parser and the bake is carried in.
    -- Optick: Common's JobSystem registers its worker threads with the profiler.
    links { "Common", "Optick" }

    -- CloudProceduralVolume.cpp keys its bake in the DDC, whose file store is Common's FileSystem, which
    -- carries Objective-C (MacOSFileSystem's dialogs): the ObjC runtime + AppKit link too.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
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

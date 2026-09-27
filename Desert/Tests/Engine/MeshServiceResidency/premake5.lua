local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- MESHSERVICE WITHOUT A DEVICE (AL1-5c). Pending, arrival, parse-before-build and the in-frame read
    -- count are bookkeeping over assets and the async loader; the GPU half is IMeshUploader, faked here.

    files {
        test_files,
        -- THE UNIT UNDER TEST, compiled without libDesert: its device half is behind IMeshUploader
        -- (MeshUploader.hpp) and the suite passes a fake, so MeshGpuUploader.cpp is deliberately absent.
        "%{wks.location}/Desert/Desert/Source/Engine/Runtime/Services/Mesh/MeshService.cpp",
        -- Pending/arrival IS the async loader.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/AsyncAssetLoader.cpp",
        -- MeshService discovers registry rows as Static/Skinned mesh shells and a skinned mesh's rig, so
        -- the three asset types link; their loaders bring the mesh DDC and the skeleton with them.
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/StaticMeshAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/MeshDerivedData.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/MeshSourceAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/SkinnedMeshAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Serialization/MeshBinary.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Assets/Mesh/SkeletonAsset.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Skeleton.cpp",
        "%{wks.location}/Desert/Desert/Source/Engine/Animation/Pose.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
    }
    externalincludedirs {
        "%{wks.location}/ThirdParty/entt/include/",
        "%{wks.location}/ThirdParty/reflect-cpp/include",
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

    links { "Common", "Optick" }

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

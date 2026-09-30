project "Desert"
    kind "StaticLib"
    DesertUnity.EnableForProject() -- no-op without --unity (BuildScripts/UnityBuild.lua)

    pchheader "pch.hpp"
    pchsource "Source/pch.cpp"
    forceincludes { "pch.hpp" }

    -- Reflection codegen (UHT-style): run DesertHeaderTool before compiling so
    -- Source/Engine/Generated/Reflection.gen.cpp is regenerated from REFLECT()/PROPERTY()
    -- annotations. The generated file is picked up by the Source/Engine/**.cpp glob below.
    dependson { "DesertHeaderTool" }
    -- The same run verifies every routed-event handler of the engine and Common (misspelt, non-public or
    -- outside the event tree fails the build at file:line) and emits the engine's DESERT_SUBSYSTEM list.
    prebuildcommands {
        DesertPlatform.BuiltToolPath("DesertHeaderTool")
            .. ' --templates "' .. _MAIN_SCRIPT_DIR .. '/Tools/DesertHeaderTool/Templates"'
            .. ' --reflect "' .. _MAIN_SCRIPT_DIR .. '/Desert/Desert/Source" "Engine"'
            .. ' "' .. _MAIN_SCRIPT_DIR .. '/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp"'
            .. ' --check "' .. _MAIN_SCRIPT_DIR .. '/Desert/Desert/Source"'
            .. ' --check "' .. _MAIN_SCRIPT_DIR .. '/Desert/Common/Source"'
            .. ' --subsystems Engine Desert::Engine::Application Engine/Core/Application.hpp'
            .. ' "' .. _MAIN_SCRIPT_DIR .. '/Desert/Desert/Source/Engine/Generated/EngineSubsystems.gen.cpp"'
    }

    files { 
        "Source/pch.cpp",
        "Source/pch.hpp",
        "Source/Engine/**.cpp", 
        "Source/Engine/**.hpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/VulkanAllocator/vk_mem_alloc.cpp",
        -- vk-bootstrap v1.3.290 (MIT, ThirdParty/vk-bootstrap/LICENSE.txt): instance, physical-device
        -- selection and logical-device creation. Vendored as its three source files, like VMA above.
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/vk-bootstrap/VkBootstrap.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/stb_image.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/stb_truetype.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/miniaudio/miniaudio.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/pl_mpeg/pl_mpeg.cpp",
    }

    includedirs {
        "Source/",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        -- The SHADER ROOT, for the one engine translation unit that compiles a shared `.glslh` AS C++:
        -- Graphic/SkyGroundTransmittance.cpp includes Common/SkyMedium.glslh so the sun light's colour
        -- and the transmittance LUT's texels come from one text (the arrangement the test references
        -- established). Nothing else in the engine may include a `.glslh` — the rest of the shader
        -- contract travels as payload structs with static_asserted offsets.
        "%{_MAIN_SCRIPT_DIR}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/pl_mpeg/include",
    }
    
    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end
    
    for name, path in pairs(deps.DesertSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    -- NO "ImGui" HERE, AND THAT IS THE POINT OF Desert/Tests/Engine/ImGuiBoundary. Dear ImGui is the
    -- EDITOR's interface toolkit: the integration layer lives in Editor/Source/Editor/ImGuiIntegration/
    -- and the Editor links the library itself. While this line said "ImGui", every binary that linked the
    -- engine — the packaged Runtime included — carried a toolkit it never draws a pixel with.
    links {
        "Common",
        "Jolt",
        "Lua",
        "Optick",
        "MeshOptimizer",
        "OpenSubdiv",
    }
    
    for _, define in ipairs(deps.Common.Defines) do
        defines { define }
    end

    filter "configurations:Debug"
        for name, path in pairs(deps.DesertSpecific.Libraries.Debug) do
            links { path }
        end

    -- `or Shipping`: the shipping build links the SAME third-party flavour Release does. There is no
    -- third set of prebuilt libraries and inventing one would mean pinning Vulkan and reflect-cpp twice.
    filter "configurations:Release or Shipping"
        for name, path in pairs(deps.DesertSpecific.Libraries.Release) do
            links { path }
        end

    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
        symbols "On"

    filter "configurations:Release"
        defines { "DESERT_CONFIG_RELEASE" }

    filter { "system:windows" }
        defines { "DESERT_PLATFORM_WINDOWS" }
        -- /bigobj: MSVC caps an object file at 65279 sections, and the generated reflection +
        -- component-registry translation units (one template instantiation per reflected type) blow
        -- past it. Clang has no such limit, which is why this only bites the Windows build.
        buildoptions { "/bigobj" }
        files {
            "Source/Platform/Windows/**.cpp",
            "Source/Platform/Windows/**.hpp",
        }

    filter { "system:macosx" }
        defines { "DESERT_PLATFORM_MACOS" }
        files {
            "Source/Platform/MacOS/**.cpp",
            "Source/Platform/MacOS/**.hpp",
            "Source/Platform/MacOS/**.mm",
        }

    filter {}
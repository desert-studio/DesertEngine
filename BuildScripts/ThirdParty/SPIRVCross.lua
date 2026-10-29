-- SPIRV-Cross (KhronosGroup, Apache-2.0), the SPIR-V reflection behind Engine/Graphic/API/Vulkan/VulkanShaderReflection.
-- COMPILED FROM THE VENDORED SOURCES on every platform (ThirdParty/SPIRV-Cross, VERSION.txt names the upstream commit),
-- replacing the Vulkan SDK's / Homebrew's prebuilt spirv-cross-core + spirv-cross-glsl: the SDK 1.3.296 parser has no
-- SPV_KHR_cooperative_matrix and THREW on the NN layer's coopmat kernels (NN-RUNTIME step 5). One pinned copy, one
-- version on Windows and macOS. Only core + GLSL, the two libraries the engine linked before.

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/SPIRV-Cross"

if not os.isfile( root .. "/spirv_cross.cpp" ) then
    error( "ThirdParty/SPIRV-Cross is empty: it is vendored source, committed with the engine." )
end

project "SPIRVCross"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/SPIRVCross" )

    files {
        root .. "/spirv_cross.cpp",
        root .. "/spirv_parser.cpp",
        root .. "/spirv_cross_parsed_ir.cpp",
        root .. "/spirv_cfg.cpp",
        root .. "/spirv_glsl.cpp",
    }
    includedirs { root }

    -- Third-party code: its warnings are its authors' style, not ours to read.
    warnings "Off"
    -- Reflection runs on every shader compile; at -O0 it is the slow half of a Debug boot.
    optimize "Speed"
    -- MSVC refuses /O2 together with the /RTC1 MSBuild adds to every Debug configuration (D8016).
    runtimechecks "Off"

    filter "system:windows"
        systemversion "latest"
        defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:not windows"
        pic "On"

    filter {}

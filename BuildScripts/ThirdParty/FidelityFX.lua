-- AMD FidelityFX SDK v1.1.4 (MIT), the FSR 3.1.4 upscaler on its Vulkan backend: Graphic::Fsr3Upscaler
-- (Engine/Graphic/Upscalers/FSR). A pinned submodule (ThirdParty/FidelityFX-SDK, tag v1.1.4 of
-- GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) compiled as one StaticLib: the FSR3 upscaler component, the shared
-- host sources and the Vulkan backend. Only the upscaler is built (FFX_FSR3UPSCALER): no frame interpolation,
-- no other effect, so their shader blobs are never generated.
--
-- THE SHADER PERMUTATIONS. The backend embeds every pass as SPIR-V permutation headers
-- (<pass>[_wave64][_16bit]_permutations.h) that the SDK's CMake produces with its own compiler,
-- sdk/tools/binary_store/FidelityFX_SC.exe (+ the glslangValidator.exe beside it). They are generated here, at
-- premake time, into build/generated/FidelityFX — the way Dav1d.lua writes its generated units — with exactly
-- the arguments of sdk/src/backends/vk/CMakeShadersFSR3Upscaler.txt and
-- sdk/include/FidelityFX/gpu/fsr3upscaler/CMakeCompileFSR3UpscalerShaders.txt; a header newer than every
-- shader source is kept.
--
-- WINDOWS ONLY, BECAUSE THE COMPILER IS. FidelityFX_SC.exe is a Windows binary and v1.1.4 ships no other, so
-- a macOS build has no FSR: the project is not defined there, DESERT_UPSCALER_FSR is not set, and the device
-- catalog's FSR gate (Graphic::FsrDeviceSupport) refuses it by that name.

if os.target() ~= "windows" then
    return
end

local root      = _MAIN_SCRIPT_DIR .. "/ThirdParty/FidelityFX-SDK/sdk"
local generated = _MAIN_SCRIPT_DIR .. "/build/generated/FidelityFX"
local gpu       = root .. "/include/FidelityFX/gpu"
local compiler  = root .. "/tools/binary_store/FidelityFX_SC.exe"

if not os.isfile( root .. "/include/FidelityFX/host/ffx_fsr3upscaler.h" ) then
    error( "ThirdParty/FidelityFX-SDK is empty. It is a submodule: run `git submodule update --init ThirdParty/FidelityFX-SDK`." )
end
if not os.isfile( compiler ) then
    error( "FidelityFX: " .. compiler .. " is missing (it ships in the SDK's tools/binary_store)." )
end

local vulkanSdk = os.getenv( "VULKAN_SDK" )
if not vulkanSdk or vulkanSdk == "" then
    error( "FidelityFX: VULKAN_SDK is not set; the Vulkan backend compiles against the SDK's headers." )
end

-- CMakeCompileFSR3UpscalerShaders.txt: FSR3UPSCALER_BASE_ARGS, then the Vulkan API args, then the permutations.
local baseArgs = table.concat( {
    "-reflection", "-deps=gcc", "-DFFX_GPU=1",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
    "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
    "-compiler=glslang", "-e", "CS", "--target-env", "vulkan1.2", "-S", "comp", "-Os", "-DFFX_GLSL=1",
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}",
    "-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}",
    '"-I' .. gpu .. '"', '"-I' .. gpu .. '/fsr3upscaler"',
}, " " )

-- CMakeCompileShaders.txt's four headers per pass. For GLSL the wave64 builds take no extra argument (the
-- backend picks them by name when the device's subgroup size control allows wave64).
local variants = {
    { Suffix = "",             Half = 0 },
    { Suffix = "_wave64",      Half = 0 },
    { Suffix = "_16bit",       Half = 1 },
    { Suffix = "_wave64_16bit", Half = 1 },
}

local function Mtime( path )
    local stat = os.stat( path )
    return stat and stat.mtime or 0
end

-- The newest shader source a permutation header depends on: the pass itself and every GPU header it includes.
local newestHeader = 0
for _, header in ipairs( os.matchfiles( gpu .. "/*.h" ) ) do
    newestHeader = math.max( newestHeader, Mtime( header ) )
end
for _, header in ipairs( os.matchfiles( gpu .. "/fsr3upscaler/*.h" ) ) do
    newestHeader = math.max( newestHeader, Mtime( header ) )
end

os.mkdir( generated )
local passes = os.matchfiles( root .. "/src/backends/vk/shaders/fsr3upscaler/*.glsl" )
if #passes == 0 then
    error( "FidelityFX: no FSR3 upscaler pass shaders under " .. root .. "/src/backends/vk/shaders/fsr3upscaler" )
end
for _, pass in ipairs( passes ) do
    local name   = path.getbasename( pass )
    local source = math.max( newestHeader, Mtime( pass ) )
    for _, variant in ipairs( variants ) do
        local header = generated .. "/" .. name .. variant.Suffix .. "_permutations.h"
        if Mtime( header ) <= source then
            local command = string.format( '"%s" %s -name=%s%s -DFFX_HALF=%d "-output=%s" "%s"', compiler, baseArgs,
                                           name, variant.Suffix, variant.Half, generated, pass )
            print( "[FidelityFX] generating " .. name .. variant.Suffix .. "_permutations.h" )
            -- cmd /c strips the first and last quote of a line that starts with one: wrap the whole line.
            local ok = os.execute( '"' .. command .. '"' )
            if not ok or not os.isfile( header ) then
                error( "FidelityFX: FidelityFX_SC failed on " .. pass .. " (" .. variant.Suffix .. "); command: " .. command )
            end
        end
    end
end

project "FidelityFX"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/FidelityFX" )

    files
    {
        root .. "/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp",
        root .. "/src/shared/ffx_assert.cpp",
        root .. "/src/shared/ffx_message.cpp",
        root .. "/src/shared/ffx_object_management.cpp",
        root .. "/src/shared/ffx_breadcrumbs_list.cpp",
        root .. "/src/backends/shared/ffx_shader_blobs.cpp",
        root .. "/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp",
        root .. "/src/backends/vk/ffx_vk.cpp",
    }

    includedirs
    {
        root .. "/include",
        root .. "/src/shared",
        root .. "/src/components",
        root .. "/src/backends/shared",
        generated,
    }
    externalincludedirs { vulkanSdk .. "/Include" }

    defines
    {
        "FFX_FSR3UPSCALER",
        "_CRT_SECURE_NO_WARNINGS",
        -- ffx_vk.cpp converts its debug names through <codecvt> (deprecated in C++17, still present).
        "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING",
    }

    warnings "Off"
    filter {}

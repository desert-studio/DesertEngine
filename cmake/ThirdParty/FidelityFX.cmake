# BuildScripts/ThirdParty/FidelityFX.lua: FidelityFX SDK, the FSR3 upscaler with the Vulkan backend. Windows only.
# The SDK ships no pre-built permutation headers for Vulkan, so each FSR3 upscaler pass is compiled by the SDK's own
# FidelityFX_SC.exe into build/generated/FidelityFX/<pass><variant>_permutations.h at CONFIGURE time, as premake did
# at generation time, and only when a pass or a GPU header is newer than its output.
if(NOT DESERT_IS_WINDOWS)
    return()
endif()
set(_root "${DESERT_ROOT}/ThirdParty/FidelityFX-SDK/sdk")
set(_generated "${DESERT_ROOT}/build/generated/FidelityFX")
set(_gpu "${_root}/include/FidelityFX/gpu")
set(_compiler "${_root}/tools/binary_store/FidelityFX_SC.exe")
if(NOT EXISTS "${_root}/include/FidelityFX/host/ffx_fsr3upscaler.h")
    message(FATAL_ERROR "ThirdParty/FidelityFX-SDK is empty. It is a submodule: run `git submodule update --init ThirdParty/FidelityFX-SDK`.")
endif()
if(NOT EXISTS "${_compiler}")
    message(FATAL_ERROR "FidelityFX: ${_compiler} is missing (it ships in the SDK's tools/binary_store).")
endif()
if(NOT DEFINED ENV{VULKAN_SDK} OR "$ENV{VULKAN_SDK}" STREQUAL "")
    message(FATAL_ERROR "FidelityFX: VULKAN_SDK is not set; the Vulkan backend compiles against the SDK's headers.")
endif()
file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" _vulkan_sdk)

set(_base_args
    -reflection -deps=gcc -DFFX_GPU=1
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
    -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
    -compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}"
    "-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}"
    "-I${_gpu}" "-I${_gpu}/fsr3upscaler")
# Variant suffix : FFX_HALF
set(_variants ":0" "_wave64:0" "_16bit:1" "_wave64_16bit:1")

function(_desert_mtime path out)
    if(EXISTS "${path}")
        file(TIMESTAMP "${path}" _t "%s" UTC)
    else()
        set(_t 0)
    endif()
    set(${out} ${_t} PARENT_SCOPE)
endfunction()

set(_newest_header 0)
file(GLOB _headers "${_gpu}/*.h" "${_gpu}/fsr3upscaler/*.h")
foreach(header IN LISTS _headers)
    _desert_mtime("${header}" _t)
    if(_t GREATER _newest_header)
        set(_newest_header ${_t})
    endif()
endforeach()
file(MAKE_DIRECTORY "${_generated}")
file(GLOB _passes "${_root}/src/backends/vk/shaders/fsr3upscaler/*.glsl")
list(SORT _passes)
if(NOT _passes)
    message(FATAL_ERROR "FidelityFX: no FSR3 upscaler pass shaders under ${_root}/src/backends/vk/shaders/fsr3upscaler")
endif()
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_passes} ${_headers})
foreach(pass IN LISTS _passes)
    cmake_path(GET pass STEM _name)
    _desert_mtime("${pass}" _source)
    if(_newest_header GREATER _source)
        set(_source ${_newest_header})
    endif()
    foreach(variant IN LISTS _variants)
        string(REPLACE ":" ";" _v "${variant}")
        list(GET _v 0 _suffix)
        list(GET _v 1 _half)
        set(_header "${_generated}/${_name}${_suffix}_permutations.h")
        _desert_mtime("${_header}" _have)
        if(NOT _have GREATER _source)
            message(STATUS "[FidelityFX] generating ${_name}${_suffix}_permutations.h")
            execute_process(
                COMMAND "${_compiler}" ${_base_args} "-name=${_name}${_suffix}" "-DFFX_HALF=${_half}"
                        "-output=${_generated}" "${pass}"
                RESULT_VARIABLE _rc)
            if(NOT _rc EQUAL 0 OR NOT EXISTS "${_header}")
                message(FATAL_ERROR "FidelityFX: FidelityFX_SC failed on ${pass} (${_suffix}), exit ${_rc}")
            endif()
        endif()
    endforeach()
endforeach()

set(_src
    components/fsr3upscaler/ffx_fsr3upscaler.cpp
    shared/ffx_assert.cpp shared/ffx_message.cpp shared/ffx_object_management.cpp shared/ffx_breadcrumbs_list.cpp
    backends/shared/ffx_shader_blobs.cpp
    backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
    backends/vk/ffx_vk.cpp
    backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK.cpp
    backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK_DebugPacing.cpp
    backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK_Helpers.cpp
    backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK_UiComposition.cpp)
list(TRANSFORM _src PREPEND "${_root}/src/")
desert_add_library(FidelityFX FOLDER ThirdParty SOURCES ${_src})
set_target_properties(FidelityFX PROPERTIES CXX_STANDARD 17)
target_include_directories(FidelityFX PRIVATE
    "${_root}/include" "${_root}/src/shared" "${_root}/src/components" "${_root}/src/backends/shared" "${_generated}")
target_include_directories(FidelityFX SYSTEM PRIVATE "${_vulkan_sdk}/Include")
target_compile_definitions(FidelityFX PRIVATE
    FFX_FSR3UPSCALER _CRT_SECURE_NO_WARNINGS _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)

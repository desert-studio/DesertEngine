# SPIRV-Cross (KhronosGroup, Apache-2.0), the SPIR-V reflection behind Engine/Graphic/API/Vulkan/VulkanShaderReflection.
# COMPILED FROM THE VENDORED SOURCES on every platform (ThirdParty/SPIRV-Cross, VERSION.txt names the upstream commit),
# not the Vulkan SDK's / Homebrew's prebuilt spirv-cross-core + spirv-cross-glsl: the SDK 1.3.296 parser has no
# SPV_KHR_cooperative_matrix and THREW on the NN layer's coopmat kernels (NN-RUNTIME step 5). One pinned copy, one
# version on Windows and macOS. Only core + GLSL, the two libraries the engine linked before. Its own CMakeLists.txt
# is not used: it declares one target per upstream library under other names, with options for the CLI and C API.
set(_root "${DESERT_ROOT}/ThirdParty/SPIRV-Cross")
if(NOT EXISTS "${_root}/spirv_cross.cpp")
    message(FATAL_ERROR "ThirdParty/SPIRV-Cross is empty: it is vendored source, committed with the engine.")
endif()
desert_add_library(SPIRVCross FOLDER ThirdParty SOURCES
    "${_root}/spirv_cross.cpp"
    "${_root}/spirv_parser.cpp"
    "${_root}/spirv_cross_parsed_ir.cpp"
    "${_root}/spirv_cfg.cpp"
    "${_root}/spirv_glsl.cpp")
set_target_properties(SPIRVCross PROPERTIES CXX_STANDARD 17)
target_include_directories(SPIRVCross PRIVATE "${_root}")
# Third-party code: its warnings are its authors' style, not ours to read.
desert_warnings_off(SPIRVCross)
# Reflection runs on every shader compile; at -O0 it is the slow half of a Debug boot.
desert_optimize_speed(SPIRVCross)
if(DESERT_IS_WINDOWS)
    target_compile_definitions(SPIRVCross PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    set_target_properties(SPIRVCross PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

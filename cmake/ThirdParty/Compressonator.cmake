# Compressonator: AMD CMP_Core, BC7 ENCODER ONLY (MIT, ThirdParty/compressonator/LICENSE). The texture cook's BC7
# encode (Engine/Core/Formats/BlockCompression.cpp): all eight BC7 modes, including the two- and three-subset
# partitions a 4x4 block needs when it straddles two colours. Vendored unmodified from
# github.com/GPUOpen-Tools/compressonator, commit 095a143a (2026-10-19), cmp_core/: the BC7 kernel and the five
# headers it includes, nothing else. Built as plain C++ on every platform (no ISPC toolchain); its own CMake project is
# not vendored.
#
# OPTIMISED IN EVERY CONFIGURATION, like the engine's other derived-data builders: at -O0 a 2k albedo's BC7 chain
# takes minutes instead of seconds, and the cook is what the editor waits on (desert_optimize_speed: no Debug
# runtime checks, which /O2 would conflict with).
set(_root "${DESERT_ROOT}/ThirdParty/compressonator/cmp_core")
desert_glob(_src "${_root}/shaders/bc7_encode_kernel.cpp" "${_root}/shaders/*.h" "${_root}/source/*.h")
desert_add_library(Compressonator FOLDER ThirdParty SOURCES ${_src})
desert_warnings_off(Compressonator)
desert_optimize_speed(Compressonator)
set_target_properties(Compressonator PROPERTIES CXX_STANDARD 17)
target_include_directories(Compressonator PRIVATE "${_root}/shaders" "${_root}/source")
if(NOT DESERT_IS_WINDOWS)
    set_target_properties(Compressonator PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

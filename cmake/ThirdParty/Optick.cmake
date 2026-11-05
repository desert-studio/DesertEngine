# Optick: the Optick profiler, CPU only (GPU and tracing off, USE_OPTICK and both switches at workspace scope).
# ThirdParty/optick's own CMakeLists.txt is not used: it declares OptickCore (another name) and its own options.
set(_root "${DESERT_ROOT}/ThirdParty/optick")
if(NOT EXISTS "${_root}/src/optick.h")
    message(FATAL_ERROR "ThirdParty/optick sources are missing. Run scripts/MacOS/Setup.sh (macOS) "
                        "or clone https://github.com/bombomby/optick into ThirdParty/optick.")
endif()
desert_glob(_src "${_root}/src/*.cpp" "${_root}/src/*.h")
desert_add_library(Optick FOLDER ThirdParty SOURCES ${_src})
set_target_properties(Optick PROPERTIES CXX_STANDARD 17)
target_include_directories(Optick PRIVATE "${_root}/src")
if(NOT DESERT_IS_WINDOWS)
    set_target_properties(Optick PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

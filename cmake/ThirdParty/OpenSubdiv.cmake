# OpenSubdiv: the CPU refiner only (sdc/vtr/far + version), no OSD GPU evaluators.
set(_root "${DESERT_ROOT}/ThirdParty/OpenSubdiv")
if(NOT EXISTS "${_root}/opensubdiv/far/topologyRefiner.h")
    message(FATAL_ERROR "ThirdParty/OpenSubdiv sources are missing. Run `git submodule update --init ThirdParty/OpenSubdiv`.")
endif()
file(GLOB _src CONFIGURE_DEPENDS
    "${_root}/opensubdiv/sdc/*.cpp" "${_root}/opensubdiv/sdc/*.h"
    "${_root}/opensubdiv/vtr/*.cpp" "${_root}/opensubdiv/vtr/*.h"
    "${_root}/opensubdiv/far/*.cpp" "${_root}/opensubdiv/far/*.h")
list(SORT _src)
desert_add_library(OpenSubdiv FOLDER ThirdParty SOURCES
    "${_root}/opensubdiv/version.cpp" "${_root}/opensubdiv/version.h" ${_src})
desert_warnings_off(OpenSubdiv)
set_target_properties(OpenSubdiv PROPERTIES CXX_STANDARD 17)
target_include_directories(OpenSubdiv PRIVATE "${_root}")
if(DESERT_IS_WINDOWS)
    target_compile_definitions(OpenSubdiv PRIVATE _USE_MATH_DEFINES)
else()
    set_target_properties(OpenSubdiv PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

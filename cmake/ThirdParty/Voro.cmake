# BuildScripts/ThirdParty/Voro.lua: voro++ 0.4.6, the fracture bake's Voronoi cells. v_base_wl.cc is a
# generated wall-list table #included by v_base.cc, not a translation unit of its own.
set(_root "${DESERT_ROOT}/ThirdParty/voro++")
desert_glob(_src "${_root}/src/*.cc" "${_root}/src/*.hh")
list(REMOVE_ITEM _src "${_root}/src/v_base_wl.cc")
desert_add_library(Voro FOLDER ThirdParty SOURCES ${_src})
desert_warnings_off(Voro)  # premake: warnings "Off"
set_target_properties(Voro PROPERTIES CXX_STANDARD 17)
target_include_directories(Voro PRIVATE "${_root}/src")
if(NOT DESERT_IS_WINDOWS)
    set_target_properties(Voro PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

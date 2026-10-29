# Jolt: Jolt Physics, every source under Jolt/, C++17.
set(_root "${DESERT_ROOT}/ThirdParty/JoltPhysics")
desert_glob(_src "${_root}/Jolt/*.cpp" "${_root}/Jolt/*.h")
desert_add_library(Jolt FOLDER ThirdParty SOURCES ${_src})
set_target_properties(Jolt PROPERTIES CXX_STANDARD 17)
target_include_directories(Jolt PRIVATE "${_root}")
if(NOT DESERT_IS_WINDOWS)
    set_target_properties(Jolt PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

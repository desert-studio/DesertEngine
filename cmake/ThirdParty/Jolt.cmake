# Jolt: Jolt Physics, every source under Jolt/, C++17. Its allocator hooks, factory and type registry are
# process-wide: a DLL outside Shipping (PROCESS_STATE; JPH_SHARED_LIBRARY in every target: cmake/DesertWorkspace.cmake).
set(_root "${DESERT_ROOT}/ThirdParty/JoltPhysics")
desert_glob(_src "${_root}/Jolt/*.cpp" "${_root}/Jolt/*.h")
desert_add_library(Jolt PROCESS_STATE FOLDER ThirdParty SOURCES ${_src})
if(NOT DESERT_MONOLITHIC)
    target_compile_definitions(Jolt PRIVATE JPH_BUILD_SHARED_LIBRARY)
endif()
desert_ltcg(Jolt)
set_target_properties(Jolt PROPERTIES CXX_STANDARD 17)
target_include_directories(Jolt PRIVATE "${_root}")
if(NOT DESERT_IS_WINDOWS)
    set_target_properties(Jolt PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

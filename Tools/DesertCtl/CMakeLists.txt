# Tools/DesertCtl/premake5.lua: the editor control-socket client (scenarios, MCP).
desert_glob(_sources "Source/*.cpp")
desert_add_executable(DesertCtl FOLDER Tools SOURCES ${_sources} "${DESERT_ROOT}/Desert/Common/Source/Common/Json/Json.cpp")
target_include_directories(DesertCtl PRIVATE "${DESERT_ROOT}/Tools/Shared" "${DESERT_ROOT}/Desert/Common/Source")
target_compile_definitions(DesertCtl PRIVATE FMT_HEADER_ONLY)
desert_external_includes(DesertCtl "${DESERT_ROOT}/ThirdParty/reflect-cpp/include"
    "${DESERT_ROOT}/ThirdParty/spdlog/include" "${DESERT_ROOT}/ThirdParty/glm/")
target_link_libraries(DesertCtl PRIVATE ReflectCpp)
desert_platform_defines(DesertCtl)
if(DESERT_IS_WINDOWS)
    target_link_libraries(DesertCtl PRIVATE ws2_32 advapi32)
endif()

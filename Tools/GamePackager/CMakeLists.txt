# GamePackager: cooks and packages a project over the editor libraries and the engine (desert_link_editor_tool). The
# MikkTSpace is not linked: mikktspace.cpp is compiled into GeometryCore.
desert_sources(_sources "Source" MATCHING "\\.cpp$")
desert_add_executable(GamePackager FOLDER Tools SOURCES ${_sources})
target_include_directories(GamePackager PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(GamePackager)
target_include_directories(GamePackager PRIVATE "${DESERT_ROOT}/Editor/Source")
desert_external_includes(GamePackager
    "${DESERT_ROOT}/ThirdParty/stb/include"
    "${DESERT_ROOT}/ThirdParty/openexr/src/lib/OpenEXRCore"
    "${DESERT_ROOT}/build/generated/openexr/include"
    "${DESERT_ROOT}/ThirdParty/Imath/src/Imath"
    ${DESERT_COMMON_INCLUDE_DIRS}
    ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(GamePackager)
desert_link_editor_tool(GamePackager)

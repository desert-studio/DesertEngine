# TextureCook: cooks texture sources over the editor's TextureImporter/DdsSource and the engine's texture code,
# LINKED from the editor libraries and the engine module libraries (desert_link_editor_tool), never compiled a
# second time here (B-TOOLS).
desert_sources(_sources "Source" MATCHING "\\.cpp$")
desert_add_executable(TextureCook FOLDER Tools SOURCES ${_sources})
target_include_directories(TextureCook PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(TextureCook)
target_include_directories(TextureCook PRIVATE "${DESERT_ROOT}/Editor/Source")
desert_external_includes(TextureCook
    "${DESERT_ROOT}/ThirdParty/stb/include"
    "${DESERT_ROOT}/ThirdParty/openexr/src/lib/OpenEXRCore"
    "${DESERT_ROOT}/build/generated/openexr/include"
    "${DESERT_ROOT}/ThirdParty/Imath/src/Imath"
    ${DESERT_COMMON_INCLUDE_DIRS}
    ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(TextureCook)
desert_link_editor_tool(TextureCook)

# WorldGen: generates worlds. The engine (reflection, serialization, landscape, projects) is LINKED from the engine
# module libraries (desert_link_engine_tool) and the canonical settings from DesertSceneMigration -- never compiled a
# second time here (B-TOOLS).
desert_sources(_sources "Source" MATCHING "\\.cpp$")
desert_add_executable(WorldGen FOLDER Tools SOURCES ${_sources})
target_include_directories(WorldGen PRIVATE "${DESERT_ROOT}/Editor/Resources/Shaders" "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(WorldGen)
desert_external_includes(WorldGen ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(WorldGen)
target_link_libraries(WorldGen PRIVATE DesertSceneMigration)
desert_link_engine_tool(WorldGen)

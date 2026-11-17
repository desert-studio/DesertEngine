# SceneMigrator: the migration steps over the engine's own format readers and writers, LINKED from the engine module
# libraries like WorldCook (desert_link_engine_tool) -- not compiled a second time here under other defines
# (DESERT_REFLECTION_DATA_ONLY: 98 engine sources, 5 % of a cold build, BUILD-FAST-1).
desert_glob(_sources "Source/*.cpp")
desert_add_executable(SceneMigrator FOLDER Tools SOURCES ${_sources})
target_include_directories(SceneMigrator PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(SceneMigrator)
desert_external_includes(SceneMigrator "${DESERT_ROOT}/Editor/Resources/Shaders" ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(SceneMigrator)
desert_link_engine_tool(SceneMigrator)

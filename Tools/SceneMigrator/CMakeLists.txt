# SceneMigrator: the migration steps over the engine's own format readers and writers, LINKED from the engine module
# libraries like WorldCook (desert_link_engine_tool) -- not compiled a second time under other defines (BUILD-FAST-1).
# DesertSceneMigration is every step (one set of objects); SceneMigrator.exe is main.cpp over it, and every other
# user (WorldGen's SettingsCanonical, the Tools test runner) links the library instead of compiling its files (B-TOOLS).
desert_sources(_sources "Source" MATCHING "\\.(cpp|hpp)$")
list(FILTER _sources EXCLUDE REGEX "/Source/main\\.cpp$")
desert_add_library(DesertSceneMigration FOLDER Tools SOURCES ${_sources})
target_include_directories(DesertSceneMigration PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_include_directories(DesertSceneMigration PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(DesertSceneMigration)
desert_external_includes(DesertSceneMigration "${DESERT_ROOT}/Editor/Resources/Shaders" ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(DesertSceneMigration)

desert_add_executable(SceneMigrator FOLDER Tools SOURCES "Source/main.cpp")
target_include_directories(SceneMigrator PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(SceneMigrator)
desert_external_includes(SceneMigrator ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(SceneMigrator)
target_link_libraries(SceneMigrator PRIVATE DesertSceneMigration)
desert_link_engine_tool(SceneMigrator)

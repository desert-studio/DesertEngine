# CloudLayoutBaker: bakes the cloud layout over the engine's CloudLayout code (stb_image comes with ImageCore),
# LINKED from the engine module libraries (desert_link_engine_tool), never compiled a second time here (B-TOOLS).
desert_sources(_sources "Source" MATCHING "\\.cpp$")
desert_add_executable(CloudLayoutBaker FOLDER Tools SOURCES ${_sources})
target_include_directories(CloudLayoutBaker PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(CloudLayoutBaker)
desert_external_includes(CloudLayoutBaker "${DESERT_ROOT}/ThirdParty/stb/include" ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(CloudLayoutBaker)
desert_link_engine_tool(CloudLayoutBaker)

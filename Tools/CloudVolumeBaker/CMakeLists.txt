# CloudVolumeBaker: bakes the cloud modelling volume over the engine's volume and catalogue code, LINKED from the
# engine module libraries (desert_link_engine_tool), never compiled a second time here (B-TOOLS).
desert_sources(_sources "Source" MATCHING "\\.cpp$")
desert_add_executable(CloudVolumeBaker FOLDER Tools SOURCES ${_sources})
target_include_directories(CloudVolumeBaker PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(CloudVolumeBaker)
desert_external_includes(CloudVolumeBaker ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(CloudVolumeBaker)
desert_link_engine_tool(CloudVolumeBaker)

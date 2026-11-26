# LatticePeak: measures the procedural cloud lattice over the engine's cloud volume code, LINKED from the engine
# module libraries (desert_link_engine_tool), never compiled a second time here (B-TOOLS).
desert_sources(_sources "Source" MATCHING "\\.cpp$")
desert_add_executable(LatticePeak FOLDER Tools SOURCES ${_sources})
target_include_directories(LatticePeak PRIVATE "${DESERT_ROOT}/Tools/Shared" "${DESERT_ROOT}/Editor/Resources/Shaders")
desert_engine_includes(LatticePeak)
desert_external_includes(LatticePeak "${DESERT_ROOT}/ThirdParty/stb/include" ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(LatticePeak)
desert_link_engine_tool(LatticePeak)

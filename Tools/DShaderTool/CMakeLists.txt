# DShaderTool: the Desert Shader Language checker over the engine's DShader parser and shading-model registry,
# LINKED from the engine module libraries (desert_link_engine_tool), never compiled a second time here (B-TOOLS).
desert_sources(_sources "Source" MATCHING "\\.cpp$")
desert_add_executable(DShaderTool FOLDER Tools SOURCES ${_sources})
target_include_directories(DShaderTool PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(DShaderTool)
desert_external_includes(DShaderTool ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(DShaderTool)
desert_link_engine_tool(DShaderTool)

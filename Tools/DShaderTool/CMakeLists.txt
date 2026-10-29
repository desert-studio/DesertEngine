# Tools/DShaderTool/premake5.lua: the Desert Shader Language checker; compiles the engine's DShader parser and
# shading-model registry straight in, and StartupLayout.cpp for ResolveEngineDir.
desert_glob(_sources "Source/*.cpp")
set(_engine "${DESERT_ROOT}/Desert/Desert/Source/Engine/Core/ShaderCompiler")
desert_add_executable(DShaderTool FOLDER Tools SOURCES ${_sources}
    "${_engine}/DShader/DShaderParser.cpp"
    "${_engine}/ShadingModels/ShadingModelManifest.cpp"
    "${_engine}/ShadingModels/ShadingModelRegistry.cpp"
    "${_engine}/ShadingModels/ShaderRootShadingModels.cpp"
    "${DESERT_ROOT}/Desert/Projects/Source/Projects/StartupLayout.cpp")
target_include_directories(DShaderTool PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(DShaderTool)
desert_external_includes(DShaderTool ${DESERT_COMMON_INCLUDE_DIRS})
target_link_libraries(DShaderTool PRIVATE Common ReflectCpp)
desert_platform_defines(DShaderTool)

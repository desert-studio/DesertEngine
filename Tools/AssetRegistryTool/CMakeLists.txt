# Tools/AssetRegistryTool/premake5.lua: the asset registry CLI; StartupLayout.cpp for ResolveEngineDir, links
# Common. No DESERT_PLATFORM_LINUX: premake defines Windows and macOS only.
desert_glob(_sources "Source/*.cpp")
desert_add_executable(AssetRegistryTool FOLDER Tools SOURCES ${_sources}
    "${DESERT_ROOT}/Desert/Projects/Source/Projects/StartupLayout.cpp")
target_include_directories(AssetRegistryTool PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(AssetRegistryTool)
desert_external_includes(AssetRegistryTool ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_COMMONSPECIFIC_INCLUDE_DIRS})
target_link_libraries(AssetRegistryTool PRIVATE Common ReflectCpp)
if(DESERT_IS_WINDOWS)
    target_compile_definitions(AssetRegistryTool PRIVATE DESERT_PLATFORM_WINDOWS)
elseif(DESERT_IS_MACOS)
    target_compile_definitions(AssetRegistryTool PRIVATE DESERT_PLATFORM_MACOS)
endif()

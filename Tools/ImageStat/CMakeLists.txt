# Tools/ImageStat/premake5.lua: a dependency-free image tool (stb only, vendored header).
desert_glob(_sources "Source/*.cpp")
desert_add_executable(ImageStat FOLDER Tools SOURCES ${_sources})
target_include_directories(ImageStat PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_external_includes(ImageStat "${DESERT_ROOT}/ThirdParty/stb/include")
desert_platform_defines(ImageStat)

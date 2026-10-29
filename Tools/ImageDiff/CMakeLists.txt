# ImageDiff: a dependency-free image tool (stb only, vendored header).
desert_glob(_sources "Source/*.cpp")
desert_add_executable(ImageDiff FOLDER Tools SOURCES ${_sources})
target_include_directories(ImageDiff PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_external_includes(ImageDiff "${DESERT_ROOT}/ThirdParty/stb/include")
desert_platform_defines(ImageDiff)

# Tools/LineJump/premake5.lua: a dependency-free image tool (stb only, vendored header).
desert_glob(_sources "Source/*.cpp")
desert_add_executable(LineJump FOLDER Tools SOURCES ${_sources})
target_include_directories(LineJump PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_external_includes(LineJump "${DESERT_ROOT}/ThirdParty/stb/include")
desert_platform_defines(LineJump)

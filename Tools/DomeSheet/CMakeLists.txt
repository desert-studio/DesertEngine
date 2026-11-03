# DomeSheet: a dependency-free image tool (stb only, vendored header).
desert_glob(_sources "Source/*.cpp")
desert_add_executable(DomeSheet FOLDER Tools SOURCES ${_sources})
target_include_directories(DomeSheet PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_external_includes(DomeSheet "${DESERT_ROOT}/ThirdParty/stb/include")
desert_platform_defines(DomeSheet)

# PakTool: packs and lists .pak archives.
desert_glob(_sources "Source/*.cpp")
desert_add_executable(PakTool FOLDER Tools SOURCES ${_sources})
target_include_directories(PakTool PRIVATE "${DESERT_ROOT}/Tools/Shared" "${DESERT_ROOT}/Desert/Common/Source")
desert_external_includes(PakTool ${DESERT_COMMON_INCLUDE_DIRS})
target_link_libraries(PakTool PRIVATE Common)
desert_platform_defines(PakTool)

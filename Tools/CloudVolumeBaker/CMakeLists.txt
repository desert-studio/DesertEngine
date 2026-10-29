# Tools/CloudVolumeBaker/premake5.lua: bakes the cloud modelling volume; compiles the engine's volume and
# catalogue sources straight in, links Common.
desert_glob(_sources "Source/*.cpp")
desert_add_executable(CloudVolumeBaker FOLDER Tools SOURCES ${_sources}
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/CloudModellingCatalogue.cpp")
target_include_directories(CloudVolumeBaker PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(CloudVolumeBaker)
desert_external_includes(CloudVolumeBaker ${DESERT_COMMON_INCLUDE_DIRS})
target_compile_definitions(CloudVolumeBaker PRIVATE ${DESERT_COMMON_DEFINES})
target_link_libraries(CloudVolumeBaker PRIVATE Common Optick ReflectCpp)
desert_platform_defines(CloudVolumeBaker)

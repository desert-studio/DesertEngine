# Tools/LatticePeak/premake5.lua: measures the procedural cloud lattice; compiles the engine's cloud volume
# sources straight in (CloudProceduralVolume.cpp compiles Shaders/Common/CloudNoise.glslh), links Common.
desert_glob(_sources "Source/*.cpp")
desert_add_executable(LatticePeak FOLDER Tools SOURCES ${_sources}
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/CloudProceduralVolume.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/CloudModellingVolume.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/CloudTypeData.cpp")
target_include_directories(LatticePeak PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(LatticePeak)
target_include_directories(LatticePeak PRIVATE "${DESERT_ROOT}/Editor/Resources/Shaders")
desert_external_includes(LatticePeak "${DESERT_ROOT}/ThirdParty/stb/include"
    ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_COMMONSPECIFIC_INCLUDE_DIRS})
target_compile_definitions(LatticePeak PRIVATE ${DESERT_COMMON_DEFINES})
target_link_libraries(LatticePeak PRIVATE Common Optick ReflectCpp)
desert_platform_defines(LatticePeak)

# CloudLayoutBaker: bakes the cloud layout; compiles the engine's CloudLayout.cpp and
# stb_image straight in, links Common.
desert_glob(_sources "Source/*.cpp")
desert_add_executable(CloudLayoutBaker FOLDER Tools SOURCES ${_sources}
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/CloudLayout.cpp"
    "${DESERT_ROOT}/ThirdParty/stb/stb_image.cpp")
target_include_directories(CloudLayoutBaker PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(CloudLayoutBaker)
desert_external_includes(CloudLayoutBaker "${DESERT_ROOT}/ThirdParty/stb/include" ${DESERT_COMMON_INCLUDE_DIRS})
target_compile_definitions(CloudLayoutBaker PRIVATE ${DESERT_COMMON_DEFINES})
target_link_libraries(CloudLayoutBaker PRIVATE Common Optick ReflectCpp)
desert_platform_defines(CloudLayoutBaker)

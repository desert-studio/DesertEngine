# TextureCook: cooks texture sources; compiles the editor's TextureImporter/DdsSource and the
# engine's texture sources straight in, links Common, Optick, OpenEXRCore, ReflectCpp and the Vulkan SDK libraries
# (deps.DesertSpecific.Libraries; on Windows that table is where ReflectCpp comes from, elsewhere it is named).
desert_glob(_sources "Source/*.cpp")
desert_add_executable(TextureCook FOLDER Tools SOURCES ${_sources}
    "${DESERT_ROOT}/Desert/Projects/Source/Projects/ProjectContext.cpp"
    "${DESERT_ROOT}/Desert/Projects/Source/Projects/RendererSettings.cpp" # the Texture Encoding efforts
    "${DESERT_ROOT}/Desert/Projects/Source/Projects/StartupLayout.cpp"
    "${DESERT_ROOT}/Editor/Source/Editor/Import/TextureImporter.cpp"
    "${DESERT_ROOT}/Editor/Source/Editor/Import/DdsSource.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/TextureSourceAsset.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Assets/Serialization/TextureBinary.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Core/Formats/BlockCompression.cpp"
    "${DESERT_ROOT}/ThirdParty/stb/stb_image.cpp")
target_include_directories(TextureCook PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(TextureCook)
target_include_directories(TextureCook PRIVATE "${DESERT_ROOT}/Editor/Source")
desert_external_includes(TextureCook
    "${DESERT_ROOT}/ThirdParty/stb/include"
    "${DESERT_ROOT}/ThirdParty/openexr/src/lib/OpenEXRCore"
    "${DESERT_ROOT}/build/generated/openexr/include"
    "${DESERT_ROOT}/ThirdParty/Imath/src/Imath"
    ${DESERT_COMMON_INCLUDE_DIRS}
    ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(TextureCook)
target_link_libraries(TextureCook PRIVATE Common Optick OpenEXRCore ReflectCpp ${DESERT_DESERTSPECIFIC_LIBRARIES})
# The BC7 encode in BlockCompression.cpp (cmake/ThirdParty/Compressonator.cmake).
target_link_libraries(TextureCook PRIVATE Compressonator)

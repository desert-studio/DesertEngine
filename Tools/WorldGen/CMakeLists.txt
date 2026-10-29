# WorldGen: compiles the engine sources it needs straight in, data-only reflection
# (DESERT_REFLECTION_DATA_ONLY); built after the engine codegen library, whose header tool writes Reflection*.gen.cpp.
desert_glob(_sources "Source/*.cpp")
file(GLOB _generated CONFIGURE_DEPENDS "${DESERT_ROOT}/Desert/Desert/Source/Engine/Generated/Reflection_*.gen.cpp")
list(SORT _generated)
desert_add_executable(WorldGen FOLDER Tools SOURCES ${_sources} ${_generated}
    "${DESERT_ROOT}/Desert/Projects/Source/Projects/StartupLayout.cpp"
    "${DESERT_ROOT}/Tools/SceneMigrator/Source/SettingsCanonical.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Core/Serialize/ExternalEntities.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Core/Serialize/EntityDescriptorIndex.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Reflection/ReflectionSerializer.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Reflection/FieldValue.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Reflection/FunctionInfo.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Reflection/ReflectionRegistry.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapeData.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapeLayout.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapeGenerator.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapeSculpt.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapeBrush.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapeEditCache.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapeEditLayers.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapeEditLayerStack.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapeSplines.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapePaint.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/World/Landscape/LandscapeTileFiles.cpp"
    "${DESERT_ROOT}/Desert/Desert/Source/Engine/Core/Serialize/SceneDependencies.cpp")
desert_depend_on_codegen(WorldGen)
target_compile_definitions(WorldGen PRIVATE DESERT_REFLECTION_DATA_ONLY)
target_include_directories(WorldGen PRIVATE "${DESERT_ROOT}/Editor/Resources/Shaders" "${DESERT_ROOT}/Tools/Shared" "${DESERT_ROOT}/Tools/SceneMigrator/Source")
desert_engine_includes(WorldGen)
desert_external_includes(WorldGen "${DESERT_ROOT}/ThirdParty/entt/include" "${DESERT_ROOT}/ThirdParty/reflect-cpp/include" ${DESERT_COMMON_INCLUDE_DIRS})
target_link_libraries(WorldGen PRIVATE Common Optick ReflectCpp)
desert_platform_defines(WorldGen)

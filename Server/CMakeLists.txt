# DesertServer: the dedicated headless server (Docs/contracts/NET-ARCH.md §6; UE: a Server target, UE_SERVER).
#
# A SEPARATE EXECUTABLE, NOT A FLAG OF THE CLIENT. It links the engine modules that sit BELOW RenderCore in the
# module table (cmake/DesertModuleTable.cmake: Common, CoreReflection, Projects, AssetCore, GeometryCore) and
# compiles the graphics-free World Partition sources it runs (the cooked index, the plan, the residency executor,
# the cell loader) - never RenderCore/Renderer (RHI, Vulkan, shaders), ApplicationCore (window, input, GLFW),
# Audio, ImGui or the editor. So no Vulkan loader, MoltenVK, Metal or AppKit is reachable from this binary, and
# nothing here names an Apple framework: the Linux build (NET-MESH-0 pass 2) adds a platform, not a target.
#
# WHY THE SOURCES ARE LISTED AND NOT THE Engine LIBRARY: the Engine module (Scene, ECS, services) still depends on
# RenderCore at the table level (DEPS RenderCore). Until Engine is split into a render-free core and its render
# half (REMAINDER of NET-MESH-0), the server compiles exactly the render-free sources it needs; the link proves
# the closure is render-free, because no render library is on its line to resolve a symbol from.
set(_engine "${DESERT_ROOT}/Desert/Desert/Source/Engine")
set(_server_engine_sources
    # the cooked world: index, cell files, the plan read back from the index
    "${_engine}/Core/Serialize/CookedWorld.cpp"
    "${_engine}/Core/Serialize/WorldCells.cpp"
    "${_engine}/Core/Serialize/WorldCellLoader.cpp"
    "${_engine}/Core/Serialize/WorldPartitionResidencyExecutor.cpp"
    "${_engine}/Core/Serialize/SceneDependencies.cpp"
    "${_engine}/Core/Serialize/SceneFormat.cpp"
    "${_engine}/Core/Serialize/PrefabExpansion.cpp"
    "${_engine}/Core/Serialize/EntityDescriptorIndex.cpp"
    "${_engine}/Core/Serialize/EntityReferenceRemap.cpp"
    "${_engine}/Core/Serialize/ExternalEntities.cpp"
    "${_engine}/Core/Serialize/ForeignKeys.cpp"
    # the records a cell holds: prefabs, foliage types, script schemas, landscape layout (data only)
    "${_engine}/Assets/Prefab/PackedPrefab.cpp"
    "${_engine}/Assets/Prefab/PrefabFormat.cpp"
    "${_engine}/Assets/Prefab/PrefabOverrides.cpp"
    "${_engine}/Assets/Serialization/FoliageType.cpp"
    "${_engine}/Scripting/ScriptSchema.cpp"
    # the server-realm Luau runtime (NET-ARCH §7): script components' declarations are read through the SAME loader
    # as the editor's (LuauScriptSchema.cpp), and server scripts run on it
    "${_engine}/Scripting/Luau/LuauScriptSchema.cpp"
    "${_engine}/Scripting/Luau/LuauRuntime.cpp"
    "${_engine}/Scripting/Luau/LuauBinder.cpp"
    "${_engine}/Scripting/Luau/LuauDebugger.cpp"
    "${_engine}/Scripting/Luau/LuauProperties.cpp"
    "${_engine}/World/Landscape/LandscapeBrush.cpp"
    "${_engine}/World/Landscape/LandscapeData.cpp"
    "${_engine}/World/Landscape/LandscapeEditCache.cpp"
    "${_engine}/World/Landscape/LandscapeEditLayers.cpp"
    "${_engine}/World/Landscape/LandscapeEditLayerStack.cpp"
    "${_engine}/World/Landscape/LandscapeLayout.cpp"
    "${_engine}/World/Landscape/LandscapePaint.cpp"
    "${_engine}/World/Landscape/LandscapeSplines.cpp")

desert_glob(_sources "Source/*.cpp" "Source/*.hpp")
desert_add_executable(DesertServer FOLDER Server SOURCES ${_sources} ${_server_engine_sources})
target_compile_definitions(DesertServer PRIVATE DESERT_SERVER=1)
target_precompile_headers(DesertServer PRIVATE "${DESERT_ROOT}/Desert/Desert/Source/pch.hpp")
desert_depend_on_codegen(DesertServer)

desert_engine_includes(DesertServer)
target_include_directories(DesertServer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    "${DESERT_ROOT}/Desert/Desert/Source"
    "${DESERT_ROOT}/Desert/CoreReflection/Source"
    "${DESERT_ROOT}/Desert/Common/Source"
    "${DESERT_ROOT}/Tools/Shared"
    # LandscapeData.cpp reads a layout header shared with the shaders (CODEMAP note): a header, no shader code.
    "${DESERT_ROOT}/Editor/Resources/Shaders")
desert_external_includes(DesertServer
    ${DESERT_COMMON_INCLUDE_DIRS}
    "${DESERT_ROOT}/ThirdParty"
    "${DESERT_ROOT}/ThirdParty/entt/include"
    "${DESERT_ROOT}/ThirdParty/luau"
    "${DESERT_ROOT}/ThirdParty/luau/CodeGen/include"
    "${DESERT_ROOT}/ThirdParty/luau/Common/include")
desert_config_platform_defines(DesertServer)

# The render-free modules, top of the table first (a static library before the ones it depends on).
set(_server_modules GeometryCore AssetCore Projects CoreReflection Common)
set(_server_libs "")
foreach(name IN LISTS _server_modules)
    list(APPEND _server_libs ${DESERT_MODULE_${name}_LIBRARIES})
endforeach()
target_link_libraries(DesertServer PRIVATE ${_server_libs}
    LuauCodeGen LuauCompiler LuauAst LuauBytecode LuauVM LuauCommon ${DESERT_OPTICK} ${DESERT_COMMONSPECIFIC_LIBRARIES})
desert_link_mimalloc(DesertServer)

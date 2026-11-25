# DesertServer: the dedicated headless server (Docs/contracts/NET-ARCH.md §6; UE: a Server target, UE_SERVER).
#
# A SEPARATE EXECUTABLE, NOT A FLAG OF THE CLIENT. It links the engine's render-free runtime core, the module Core
# (cmake/DesertModuleTable.cmake: the cooked world and World Partition residency, scene/prefab serialization, the
# reflected-component table, the server-realm Luau runtime of NET-ARCH §7), and the modules below it - among them
# LandscapeCore, the landscape's runtime data its cells' collision stands pawns on (ServerCellCollision reads tiles
# through LandscapeCore/Public/LandscapeTileFiles.hpp, as UE's dedicated server loads Runtime/Landscape) - never
# RenderCore/Renderer (RHI, Vulkan, shaders), ApplicationCore (window, input, GLFW), Audio, ImGui or the editor.
# Engine links the same Core library, so no engine object is compiled twice. Core is RENDER_FREE:
# configure fails if any of its files reaches a render, window, Audio or ImGui header (cmake/DesertModules.cmake),
# and the link proves the closure, because no render library is on this line to resolve a symbol from. Nothing here
# names an Apple framework: the Linux build adds a platform, not a target.

desert_glob(_sources "Source/*.cpp" "Source/*.hpp")
desert_add_executable(DesertServer FOLDER Server SOURCES ${_sources})
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

# Core and its dependency closure, top of the table first (a static library before the ones it depends on).
desert_module_closure(Core _server_modules)
set(_server_libs ${DESERT_MODULE_Core_LIBRARIES})
set(_table ${DESERT_MODULES})
list(REVERSE _table)
foreach(name IN LISTS _table)
    if(name IN_LIST _server_modules)
        list(APPEND _server_libs ${DESERT_MODULE_${name}_LIBRARIES})
    endif()
endforeach()
# DesertNet: the transport, handshake and the mesh-of-one roles (gateway, sim, store; NET-ARCH §2M.2). Device-free.
# PhysicsCore (Jolt, render-free): the pawns' collision world (Net/PhysicsPawnMoveWorld.hpp).
target_link_libraries(DesertServer PRIVATE DesertNet ${DESERT_MODULE_PhysicsCore_LIBRARIES} ${_server_libs}
    LuauCodeGen LuauCompiler LuauAst LuauBytecode LuauVM LuauCommon ${DESERT_OPTICK} ${DESERT_COMMONSPECIFIC_LIBRARIES})
desert_link_mimalloc(DesertServer)

# DesertNetProbe: proves the transport, its lanes and the token handshake on its own (NET-1 scenario
# net1b_lanes_token) -- one binary: --gen-issuer-keys and --mint-token (the issuer's Ed25519 private key lives only
# here, never in DesertServer), --server or --client, under the GNS lag/loss simulator -- and
# plays a headless player (--bot) that streams the cooked world's cells around its own pawn and reports them to
# the server as a client does (NET-3).
desert_sources(_sources "Source" MATCHING "\\.cpp$")
desert_add_executable(DesertNetProbe FOLDER Tools SOURCES ${_sources})
desert_depend_on_codegen(DesertNetProbe)
desert_engine_includes(DesertNetProbe)
target_include_directories(DesertNetProbe PRIVATE
    "${DESERT_ROOT}/Desert/Desert/Source"
    "${DESERT_ROOT}/Desert/CoreReflection/Source"
    "${DESERT_ROOT}/Desert/Common/Source"
    "${DESERT_ROOT}/Editor/Resources/Shaders")
desert_external_includes(DesertNetProbe
    ${DESERT_COMMON_INCLUDE_DIRS}
    "${DESERT_ROOT}/ThirdParty"
    "${DESERT_ROOT}/ThirdParty/entt/include"
    "${DESERT_ROOT}/ThirdParty/luau"
    "${DESERT_ROOT}/ThirdParty/luau/CodeGen/include"
    "${DESERT_ROOT}/ThirdParty/luau/Common/include")
desert_config_platform_defines(DesertNetProbe)

# The bot's cells: the cooked world's index and plan (WorldCells::ReadWorldIndex / PlanFromIndex) and the streaming
# query (QueryStreamingCells) - the module Core and its closure, as DesertServer links it (render-free).
desert_module_closure(Core _probe_modules)
set(_probe_libs ${DESERT_MODULE_Core_LIBRARIES})
set(_table ${DESERT_MODULES})
list(REVERSE _table)
foreach(name IN LISTS _table)
    if(name IN_LIST _probe_modules)
        list(APPEND _probe_libs ${DESERT_MODULE_${name}_LIBRARIES})
    endif()
endforeach()
target_link_libraries(DesertNetProbe PRIVATE DesertNet ${_probe_libs}
    LuauCodeGen LuauCompiler LuauAst LuauBytecode LuauVM LuauCommon ${DESERT_OPTICK} ${DESERT_COMMONSPECIFIC_LIBRARIES})
desert_link_mimalloc(DesertNetProbe)

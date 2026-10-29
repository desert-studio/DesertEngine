# Runtime/premake5.lua: the player executable. Links the engine module libraries, never the editor.
desert_glob(_sources "Source/*.cpp" "Source/*.hpp")
desert_add_executable(Runtime SOURCES ${_sources})
set_target_properties(Runtime PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${DESERT_ROOT}/Editor"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--project ../Projects/Desert/Desert.deproj")

desert_header_tool_roots(context _context_roots)
add_dependencies(Runtime DesertHeaderTool)
desert_prebuild(Runtime HeaderTool
    COMMAND "$<TARGET_FILE:DesertHeaderTool>"
        --templates "${DESERT_ROOT}/Tools/DesertHeaderTool/Templates"
        --check "${DESERT_ROOT}/Runtime/Source"
        ${_context_roots}
    DEPENDS DesertHeaderTool
    COMMENT "DesertHeaderTool: Runtime check")

desert_engine_includes(Runtime)
target_include_directories(Runtime PRIVATE "${DESERT_ROOT}/Runtime/Source/")
desert_external_includes(Runtime
    "${DESERT_ROOT}/ThirdParty/spdlog/include/"
    "${DESERT_ROOT}/ThirdParty/GLFW/include/"
    "${DESERT_ROOT}/ThirdParty/Glad/include/"
    "${DESERT_ROOT}/ThirdParty/entt/include/"
    "${DESERT_ROOT}/ThirdParty/glm/"
    "${DESERT_ROOT}/ThirdParty/optick/src/"
    "${DESERT_ROOT}/ThirdParty/")
desert_editorspecific_includes(Runtime)
target_compile_definitions(Runtime PRIVATE USE_OPTICK=1 OPTICK_ENABLE_GPU=0 OPTICK_ENABLE_TRACING=0)
target_link_libraries(Runtime PRIVATE GLFW Optick MeshOptimizer Voro Embree Dav1d Opus)
desert_link_engine(Runtime)
desert_config_platform_defines(Runtime)
desert_copy_streamline_runtime(Runtime)
if(DESERT_IS_MACOS)
    target_link_libraries(Runtime PRIVATE
        Common Jolt LuauCodeGen LuauCompiler LuauAst LuauBytecode LuauVM LuauCommon ReflectCpp
        "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation" "-framework CoreVideo"
        "-framework QuartzCore"
        ${DESERT_VULKAN_LIBRARIES})
endif()

# The five editor libraries (cmake/DesertEditorLibs.cmake), EditorObjC on macOS, and the
# Editor executable, which is nothing but the entry point linked against them.

# The settings every editor library, EditorObjC and the Editor executable compile with (EditorCompileSettings).
function(_desert_editor_compile_settings target)
    desert_engine_includes(${target})
    target_include_directories(${target} PRIVATE "${DESERT_ROOT}/Editor/Source/")
    desert_external_includes(${target}
        "${DESERT_ROOT}/ThirdParty/openexr/src/lib/OpenEXRCore"   # <openexr.h>, for the texture import
        "${DESERT_ROOT}/build/generated/openexr/include"           # its generated config headers
        "${DESERT_ROOT}/ThirdParty/Imath/src/Imath"
        "${DESERT_ROOT}/ThirdParty/spdlog/include/"
        "${DESERT_ROOT}/ThirdParty/GLFW/include/"
        "${DESERT_ROOT}/ThirdParty/Glad/include/"
        "${DESERT_ROOT}/ThirdParty/entt/include/"
        "${DESERT_ROOT}/ThirdParty/ImGui/"
        "${DESERT_ROOT}/ThirdParty/glm/"
        "${DESERT_ROOT}/ThirdParty/optick/src/"
        "${DESERT_ROOT}/ThirdParty/")
    desert_editorspecific_includes(${target})
    desert_external_includes(${target}
        "${DESERT_VULKAN_INCLUDE}" "${DESERT_VULKAN_INCLUDE}/shaderc" "${DESERT_ROOT}/ThirdParty/SPIRV-Cross")
    target_compile_definitions(${target} PRIVATE USE_OPTICK=1 OPTICK_ENABLE_GPU=0 OPTICK_ENABLE_TRACING=0)
    if(IS_DIRECTORY "${DESERT_ROOT}/ThirdParty/dlib")
        target_compile_definitions(${target} PRIVATE DESERT_WITH_DLIB)
        desert_external_includes(${target} "${DESERT_ROOT}/ThirdParty/dlib/")
    endif()
    desert_config_platform_defines(${target})
endfunction()

desert_header_tool_roots(context _context_roots)

set(_first_layer ON)
foreach(layer IN LISTS DESERT_EDITOR_LAYERS)
    set(_files ${DESERT_EDITOR_LAYER_${layer}_FILES})
    if(NOT DESERT_IS_WINDOWS)
        set(_foreign ${_files})
        list(FILTER _foreign INCLUDE REGEX "/Source/Editor/(Splash|Platform)/Windows/")
        desert_platform_foreign_sources(${_foreign})
        list(FILTER _files EXCLUDE REGEX "/Source/Editor/(Splash|Platform)/Windows/")
    endif()
    desert_add_library(${layer} SOURCES "Source/EditorPCH.cpp" "Source/EditorPCH.hpp" ${_files})
    if(_first_layer)
        target_sources(${layer} PRIVATE "ThirdParty/ImGuizmo/ImGuizmo.cpp")
        set(_first_layer OFF)
    endif()
    desert_enable_unity(${layer})
    target_precompile_headers(${layer} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source/EditorPCH.hpp")
    desert_depend_on_codegen(${layer})
    if(layer STREQUAL DESERT_EDITOR_CODEGEN_LIB)
        add_dependencies(${layer} DesertHeaderTool)
        desert_prebuild(${layer} HeaderTool
            COMMAND "$<TARGET_FILE:DesertHeaderTool>"
                --templates "${DESERT_ROOT}/Tools/DesertHeaderTool/Templates"
                --check "${DESERT_ROOT}/Editor/Source"
                ${_context_roots}
                --subsystems Editor Desert::Editor::EditorLayer EditorLayer.hpp
                "${DESERT_ROOT}/Editor/Source/Editor/Generated/EditorSubsystems.gen.cpp"
            DEPENDS DesertHeaderTool
            COMMENT "DesertHeaderTool: editor subsystem codegen")
    endif()
    _desert_editor_compile_settings(${layer})
endforeach()

if(DESERT_IS_MACOS)
    desert_add_library(${DESERT_EDITOR_OBJC_LIB} SOURCES ${DESERT_EDITOR_OBJC_FILES})
    desert_depend_on_codegen(${DESERT_EDITOR_OBJC_LIB})
    _desert_editor_compile_settings(${DESERT_EDITOR_OBJC_LIB})
endif()

desert_add_executable(Editor WINDOWED SOURCES "Source/Sandbox.cpp" "Source/Sandbox.hpp")
set_target_properties(Editor PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${DESERT_ROOT}/Editor"
    VS_DEBUGGER_COMMAND_ARGUMENTS "--project ../Projects/Desert/Desert.deproj")
_desert_editor_compile_settings(Editor)
desert_link_editor_libs(Editor WHOLE_ARCHIVE)
target_link_libraries(Editor PRIVATE
    GLFW Optick MeshOptimizer Voro Compressonator OpenSubdiv Embree ImGui ImGuiNodeEditor Assimp OpenEXRCore Dav1d Opus)
desert_link_engine(Editor)
if(IS_DIRECTORY "${DESERT_ROOT}/ThirdParty/dlib")
    target_link_libraries(Editor PRIVATE Dlib)
endif()
if(DESERT_IS_WINDOWS)
    # A windowed app that still enters through main() (WINDOWED + /ENTRY:mainCRTStartup).
    target_link_options(Editor PRIVATE /ENTRY:mainCRTStartup)
    target_sources(Editor PRIVATE "Resources/Branding/Editor.rc")
    target_link_libraries(Editor PRIVATE ws2_32 advapi32 comdlg32 shell32 ole32)
endif()
desert_copy_streamline_runtime(Editor)
# The OIDN runtime (PathTracing.Denoiser OIDN), cmake/DesertDependencies.cmake.
desert_copy_oidn_runtime(Editor)
if(DESERT_IS_MACOS)
    target_link_libraries(Editor PRIVATE
        Common Jolt LuauCodeGen LuauCompiler LuauAst LuauBytecode LuauVM LuauCommon ReflectCpp
        "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation" "-framework CoreVideo"
        "-framework CoreMedia" "-framework AVFoundation" "-framework QuartzCore"
        ${DESERT_DESERTSPECIFIC_LIBRARIES})
endif()

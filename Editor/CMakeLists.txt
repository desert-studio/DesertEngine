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
    if(IS_DIRECTORY "${DESERT_ROOT}/ThirdParty/dlib")
        target_compile_definitions(${target} PRIVATE DESERT_WITH_DLIB)
        desert_external_includes(${target} "${DESERT_ROOT}/ThirdParty/dlib/")
    endif()
    desert_config_platform_defines(${target})
endfunction()

desert_header_tool_roots(context _context_roots)

# What every editor layer's code calls besides the layers below it and the engine: as DLLs (Development/DebugGame)
# each layer resolves its own imports; in the Shipping monolith these travel to the executable with the archives.
set(_editor_thirdparty
    GLFW ${DESERT_OPTICK} MeshOptimizer Voro Compressonator OpenSubdiv Embree ImGui ImGuiNodeEditor Assimp OpenEXRCore
    Dav1d Opus)
if(IS_DIRECTORY "${DESERT_ROOT}/ThirdParty/dlib")
    list(APPEND _editor_thirdparty Dlib)
endif()

desert_unity_exclude(REASON "anonymous-namespace SlicePreviewHelper also in Panels/Clouds/CloudModellingVolumePanel.cpp"
    "Source/Editor/Panels/Clouds/CloudNoiseVolumePanel.cpp")
desert_unity_exclude(REASON "defines IMGUI_DEFINE_MATH_OPERATORS / NOMINMAX before its includes"
    "Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp" "Source/EditorLayer.cpp")
desert_unity_exclude(REASON "defines DE_POSIX_PROC before its includes"
    "Source/Editor/Panels/Photogrammetry/PhotogrammetryPanel.cpp")
desert_unity_exclude(REASON "anonymous-namespace AddCategory / kAddCategories also in the component widget registry"
    "Source/Editor/Panels/SceneHierarchy/SceneHierarchyPanel.cpp")
desert_unity_exclude(REASON "anonymous-namespace Target / edges also in the modeling operations"
    "Source/Editor/Panels/ViewportPanel/Tools/ElementSelectTool.cpp")
desert_unity_exclude(REASON "anonymous-namespace kTraceDistanceCm / TraceLandscape() also in LandscapePaintTool.cpp"
    "Source/Editor/Panels/ViewportPanel/Tools/LandscapeSculptTool.cpp")
desert_unity_exclude(REASON "anonymous-namespace kDome* camera constants also in AssetThumbnailRenderer.cpp"
    "Source/Editor/Widgets/PreviewViewport.cpp")

set(_lower_layers "")
set(_first_layer ON)
foreach(layer IN LISTS DESERT_EDITOR_LAYERS)
    set(_files ${DESERT_EDITOR_LAYER_${layer}_FILES})
    if(NOT DESERT_IS_WINDOWS)
        set(_foreign ${_files})
        list(FILTER _foreign INCLUDE REGEX "/Source/Editor/(Splash|Platform)/Windows/")
        desert_platform_foreign_sources(${_foreign})
        list(FILTER _files EXCLUDE REGEX "/Source/Editor/(Splash|Platform)/Windows/")
    endif()
    desert_module_library(${layer} SOURCES "Source/EditorPCH.cpp" "Source/EditorPCH.hpp" ${_files})
    if(_first_layer)
        target_sources(${layer} PRIVATE "ThirdParty/ImGuizmo/ImGuizmo.cpp")
        # ImGuizmo marks its own functions IMGUI_API, which is dllimport outside the ImGui DLL; its
        # definitions live in this layer, so they compile with IMGUI_API empty.
        set_source_files_properties("ThirdParty/ImGuizmo/ImGuizmo.cpp" TARGET_DIRECTORY ${layer}
            PROPERTIES COMPILE_DEFINITIONS "IMGUI_API=" SKIP_UNITY_BUILD_INCLUSION ON)
        set(_first_layer OFF)
    endif()
    desert_enable_unity(${layer})
    # Every layer compiles with the same flags: one EditorPCH serves them all (desert_shared_pch).
    desert_shared_pch(${layer} Editor "${CMAKE_CURRENT_SOURCE_DIR}/Source/EditorPCH.hpp")
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
    # A layer calls the layers below it, never above (DESERT_EDITOR_LAYERS is bottom first).
    target_link_libraries(${layer} PRIVATE ${_lower_layers} ${_editor_thirdparty})
    desert_link_engine(${layer})
    list(APPEND _lower_layers ${layer})
endforeach()

# The Windows system libraries EditorCore's own sources call, on EditorCore and not on one executable: the
# control channel's socket (Editor/Core/Control/ControlSocket.cpp, AF_UNIX over Winsock, and the DACL that keeps
# the socket file to its owner) and the system file dialogs (Editor/Platform/Windows/DesktopPlatformWindows.cpp).
# Every binary that links EditorCore gets them -- the Editor, and GamePackager, whose unity object holding the
# packer's members also holds ControlSocket. Not the engine: the shipped game has no control channel.
if(DESERT_IS_WINDOWS)
    target_link_libraries(EditorCore PUBLIC ws2_32 advapi32 comdlg32 shell32 ole32)
endif()

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
desert_link_mimalloc(Editor)
desert_link_editor_libs(Editor)
target_link_libraries(Editor PRIVATE ${_editor_thirdparty})
desert_link_engine(Editor)
if(DESERT_IS_WINDOWS)
    # A windowed app that still enters through main() (WINDOWED + /ENTRY:mainCRTStartup).
    target_link_options(Editor PRIVATE /ENTRY:mainCRTStartup)
    target_sources(Editor PRIVATE "Resources/Branding/Editor.rc")
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

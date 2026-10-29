# The CMake port of BuildScripts/EditorLibs.lua: the editor's five layered static libraries, which source
# belongs to which, and the one way to link them.
#
# Each source under Editor/Source/Editor/ belongs to the layer whose directory is the LONGEST prefix of its path;
# a source in no layer, or a file directly under Editor/Source/ that no layer claims, fails configure.

include_guard(GLOBAL)

set(DESERT_EDITOR_SOURCE_ROOT "${DESERT_ROOT}/Editor/Source")
set(DESERT_EDITOR_LAYERS EditorCore EditorImport EditorWidgets EditorPanels EditorLevel)
set(DESERT_EDITOR_LAYER_EditorCore_DIRS    Core Platform RenderSystems)
set(DESERT_EDITOR_LAYER_EditorImport_DIRS  Import Packaging)
set(DESERT_EDITOR_LAYER_EditorWidgets_DIRS Widgets Splash ImGuiIntegration)
set(DESERT_EDITOR_LAYER_EditorPanels_DIRS  Panels)
set(DESERT_EDITOR_LAYER_EditorLevel_DIRS   LevelEditor Generated Panels/SceneProperties Panels/SceneHierarchy Panels/Modeling)
set(DESERT_EDITOR_LAYER_EditorLevel_ROOT_FILES EditorLayer.cpp EditorLayer.hpp)
set(DESERT_EDITOR_OBJC_LIB EditorObjC)
set(DESERT_EDITOR_ENTRY_POINT Sandbox.cpp Sandbox.hpp)
set(DESERT_EDITOR_CODEGEN_LIB EditorLevel)

foreach(layer IN LISTS DESERT_EDITOR_LAYERS)
    set(DESERT_EDITOR_LAYER_${layer}_FILES "")
endforeach()

file(GLOB_RECURSE _editor_files LIST_DIRECTORIES false CONFIGURE_DEPENDS RELATIVE "${DESERT_EDITOR_SOURCE_ROOT}/Editor"
     "${DESERT_EDITOR_SOURCE_ROOT}/Editor/*.cpp" "${DESERT_EDITOR_SOURCE_ROOT}/Editor/*.hpp"
     "${DESERT_EDITOR_SOURCE_ROOT}/Editor/*.h" "${DESERT_EDITOR_SOURCE_ROOT}/Editor/*.inl")
list(SORT _editor_files)
foreach(rel IN LISTS _editor_files)
    set(_best "")
    set(_best_len -1)
    foreach(layer IN LISTS DESERT_EDITOR_LAYERS)
        foreach(dir IN LISTS DESERT_EDITOR_LAYER_${layer}_DIRS)
            string(LENGTH "${dir}" _len)
            string(FIND "${rel}" "${dir}/" _at)
            if(_at EQUAL 0 AND _len GREATER _best_len)
                set(_best "${layer}")
                set(_best_len ${_len})
            endif()
        endforeach()
    endforeach()
    if(NOT _best)
        message(FATAL_ERROR "cmake/DesertEditorLibs.cmake: Editor/Source/Editor/${rel} is in no editor library - "
                            "give its directory a row in DESERT_EDITOR_LAYERS")
    endif()
    list(APPEND DESERT_EDITOR_LAYER_${_best}_FILES "${DESERT_EDITOR_SOURCE_ROOT}/Editor/${rel}")
endforeach()
foreach(layer IN LISTS DESERT_EDITOR_LAYERS)
    foreach(file IN LISTS DESERT_EDITOR_LAYER_${layer}_ROOT_FILES)
        list(APPEND DESERT_EDITOR_LAYER_${layer}_FILES "${DESERT_EDITOR_SOURCE_ROOT}/${file}")
    endforeach()
    list(SORT DESERT_EDITOR_LAYER_${layer}_FILES)
endforeach()

# Every file directly under Editor/Source/ must be claimed.
file(GLOB _editor_root_files LIST_DIRECTORIES false RELATIVE "${DESERT_EDITOR_SOURCE_ROOT}" "${DESERT_EDITOR_SOURCE_ROOT}/*")
foreach(file IN LISTS _editor_root_files)
    set(_claimed OFF)
    if(file IN_LIST DESERT_EDITOR_ENTRY_POINT OR file STREQUAL "EditorPCH.hpp" OR file STREQUAL "EditorPCH.cpp")
        set(_claimed ON)
    endif()
    foreach(layer IN LISTS DESERT_EDITOR_LAYERS)
        if(file IN_LIST DESERT_EDITOR_LAYER_${layer}_ROOT_FILES)
            set(_claimed ON)
        endif()
    endforeach()
    if(NOT _claimed)
        message(FATAL_ERROR "cmake/DesertEditorLibs.cmake: Editor/Source/${file} is in no editor library")
    endif()
endforeach()

file(GLOB_RECURSE DESERT_EDITOR_OBJC_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${DESERT_EDITOR_SOURCE_ROOT}/Editor/*.mm")
list(SORT DESERT_EDITOR_OBJC_FILES)

# DesertEditorLibs.Link(wholeArchive): the five layers top first (EditorObjC ahead of them on macOS); with
# WHOLE_ARCHIVE every object reaches the binary (component editors register themselves from static initialisers).
function(desert_link_editor_libs target)
    cmake_parse_arguments(ARG "WHOLE_ARCHIVE" "" "" ${ARGN})
    set(_names ${DESERT_EDITOR_LAYERS})
    list(REVERSE _names)
    if(DESERT_IS_MACOS)
        list(PREPEND _names ${DESERT_EDITOR_OBJC_LIB})
    endif()
    if(ARG_WHOLE_ARCHIVE)
        desert_link_whole_archive(${target} ${_names})
    else()
        target_link_libraries(${target} PRIVATE ${_names})
    endif()
endfunction()

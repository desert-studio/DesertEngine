# The engine's module table (cmake/DesertModuleTable.cmake) turned into the build: one static library per module,
# the engine include paths, and the one way to link the engine.
#
# ONE SOURCE OF TRUTH. The rows live in cmake/DesertModuleTable.cmake, written as desert_module() /
# desert_placement() calls. This file collects them, validates the layering, places every legacy file, and writes
# <build>/DesertModules.json (DESERT_MODULE_TABLE_JSON) for the two C++ readers, DesertHeaderTool (`--modules`) and
# the ModuleBoundary suite: the rows as written plus the module CMake placed each legacy file into, so the C++ side
# can check its std::regex placement against the build's instead of keeping a second table.
#
# Placement patterns are the table's regex subset (literal chars, `.`, `\w`, `\d`, escapes, groups, `|`,
# `*`/`+`/`?`, `^`/`$`, unanchored search). CMake's regex has the same operators except the classes, so
# `\w` and `\d` are rewritten to bracket expressions and everything else is used as written.

include_guard(GLOBAL)

set(DESERT_MODULE_TABLE "${DESERT_ROOT}/cmake/DesertModuleTable.cmake")
set(DESERT_MODULE_TABLE_JSON "${CMAKE_BINARY_DIR}/DesertModules.json")
set(DESERT_LEGACY_ROOT "Desert/Desert/Source")
set(DESERT_LEGACY_REFLECT_DIR "Engine")

# ── THE ROWS ───────────────────────────────────────────────────────────────────────────────────────
set(DESERT_MODULES "")
set(DESERT_PLACEMENT_MODULES "")
set(DESERT_PLACEMENT_PATTERNS "")
set(DESERT_PLACEMENT_SOURCE_PATTERNS "")

# desert_module(<Name> [DEPS <module>...] UE "<Unreal module>" [FOLDER <repo-relative dir>] [REFLECTED])
macro(desert_module name)
    cmake_parse_arguments(_m "REFLECTED" "UE;FOLDER" "DEPS" ${ARGN})
    if(_m_UNPARSED_ARGUMENTS OR NOT _m_UE)
        message(FATAL_ERROR "DesertModuleTable.cmake: module '${name}': unexpected '${_m_UNPARSED_ARGUMENTS}' or no UE")
    endif()
    if(NOT "${name}" MATCHES "^[A-Za-z0-9]+$")
        message(FATAL_ERROR "DesertModuleTable.cmake: module name '${name}' is not alphanumeric")
    endif()
    if("${name}" IN_LIST DESERT_MODULES)
        message(FATAL_ERROR "DesertModuleTable.cmake: module '${name}' is listed twice")
    endif()
    foreach(_dep IN LISTS _m_DEPS)
        if(NOT _dep IN_LIST DESERT_MODULES)
            message(FATAL_ERROR "DesertModuleTable.cmake: '${name}' depends on '${_dep}', which is not listed before it")
        endif()
    endforeach()
    set(_folder "Desert/${name}/Source/${name}")
    if(_m_FOLDER)
        set(_folder "${_m_FOLDER}")
    endif()
    if(NOT _folder MATCHES "^(.+)/([^/]+)$")
        message(FATAL_ERROR "DesertModuleTable.cmake: module '${name}' has FOLDER '${_folder}', which is not <source root>/<dir>")
    endif()
    set(DESERT_MODULE_${name}_SOURCE_ROOT "${CMAKE_MATCH_1}")
    set(DESERT_MODULE_${name}_SCAN_DIR "${CMAKE_MATCH_2}")
    list(APPEND DESERT_MODULES "${name}")
    set(DESERT_MODULE_${name}_DEPS "${_m_DEPS}")
    set(DESERT_MODULE_${name}_FOLDER "${_folder}")
    set(DESERT_MODULE_${name}_REFLECTED ${_m_REFLECTED})
    # Generated reflection of a module lives in the legacy root and belongs to it: these rows come before the
    # table's own placement rows (collected separately and appended after the table is read).
    list(APPEND _desert_generated_modules "${name}")
endmacro()

# desert_placement(<Module> [[<ECMAScript pattern>]])
# A function, not a macro: a macro would re-read the pattern's backslashes as escape sequences.
function(desert_placement module pattern)
    if(NOT "${module}" IN_LIST DESERT_MODULES)
        message(FATAL_ERROR "DesertModuleTable.cmake: placement row names '${module}', which is not a module listed above it")
    endif()
    if(pattern MATCHES ";")
        message(FATAL_ERROR "DesertModuleTable.cmake: placement row of '${module}' has a ';', which a CMake list cannot carry")
    endif()
    list(APPEND _desert_table_placement_modules "${module}")
    list(APPEND _desert_table_placement_patterns "${pattern}")
    set(_desert_table_placement_modules "${_desert_table_placement_modules}" PARENT_SCOPE)
    set(_desert_table_placement_patterns "${_desert_table_placement_patterns}" PARENT_SCOPE)
endfunction()

set(_desert_generated_modules "")
set(_desert_table_placement_modules "")
set(_desert_table_placement_patterns "")
include("${DESERT_MODULE_TABLE}")
if(NOT DESERT_MODULES OR NOT _desert_table_placement_modules)
    message(FATAL_ERROR "DesertModuleTable.cmake declares no module or no placement row")
endif()

# Placement rows: the generated-reflection rows first (in module order), then the table's rows in order.
foreach(name IN LISTS _desert_generated_modules)
    list(APPEND DESERT_PLACEMENT_MODULES "${name}")
    list(APPEND DESERT_PLACEMENT_SOURCE_PATTERNS "Engine/Generated/Reflection_${name}\\.gen\\.cpp$")
endforeach()
list(APPEND DESERT_PLACEMENT_MODULES ${_desert_table_placement_modules})
list(APPEND DESERT_PLACEMENT_SOURCE_PATTERNS ${_desert_table_placement_patterns})
foreach(_pattern IN LISTS DESERT_PLACEMENT_SOURCE_PATTERNS)
    string(REPLACE "\\w" "[A-Za-z0-9_]" _pattern "${_pattern}")
    string(REPLACE "\\d" "[0-9]" _pattern "${_pattern}")
    list(APPEND DESERT_PLACEMENT_PATTERNS "${_pattern}")
endforeach()
list(LENGTH DESERT_PLACEMENT_PATTERNS _nplace)
math(EXPR DESERT_PLACEMENT_LAST "${_nplace} - 1")

# ── LEGACY FILES: every file under Desert/Desert/Source, placed by the first matching row ───────────
set(_legacy_root "${DESERT_ROOT}/${DESERT_LEGACY_ROOT}")
file(GLOB_RECURSE _legacy LIST_DIRECTORIES false CONFIGURE_DEPENDS RELATIVE "${_legacy_root}"
     "${_legacy_root}/*.cpp" "${_legacy_root}/*.hpp" "${_legacy_root}/*.h" "${_legacy_root}/*.inl" "${_legacy_root}/*.mm")
list(SORT _legacy)
list(REMOVE_ITEM _legacy "pch.cpp" "pch.hpp")
set(DESERT_HAS_LEGACY_FILES OFF)
foreach(name IN LISTS DESERT_MODULES)
    set(DESERT_MODULE_${name}_LEGACY "")
endforeach()
foreach(rel IN LISTS _legacy)
    set(DESERT_HAS_LEGACY_FILES ON)
    set(_placed "")
    foreach(i RANGE ${DESERT_PLACEMENT_LAST})
        list(GET DESERT_PLACEMENT_PATTERNS ${i} _pattern)
        if(rel MATCHES "${_pattern}")
            list(GET DESERT_PLACEMENT_MODULES ${i} _placed)
            break()
        endif()
    endforeach()
    if(NOT _placed)
        message(FATAL_ERROR "DesertModules.cmake: no placement row matches '${rel}'")
    endif()
    list(APPEND DESERT_MODULE_${_placed}_LEGACY "${rel}")
endforeach()

# ── PER-MODULE FACTS ───────────────────────────────────────────────────────────────────────────────
# HasOwnProject: the module's directory carries its own build script (Desert/Common, Desert/UI,
# Desert/Render2DCore), which declares the library under the module's own name.
foreach(name IN LISTS DESERT_MODULES)
    set(_folder "${DESERT_MODULE_${name}_FOLDER}")
    cmake_path(GET _folder PARENT_PATH _p1)
    cmake_path(GET _p1 PARENT_PATH _p2)
    set(DESERT_MODULE_${name}_OWN_PROJECT OFF)
    if(EXISTS "${DESERT_ROOT}/${_p2}/CMakeLists.txt")
        set(DESERT_MODULE_${name}_OWN_PROJECT ON)
    endif()
    set(DESERT_MODULE_${name}_HAS_FOLDER OFF)
    if(IS_DIRECTORY "${DESERT_ROOT}/${_folder}")
        set(DESERT_MODULE_${name}_HAS_FOLDER ON)
    endif()
    # FolderFiles: a module folder without its own project is compiled into the module's table library,
    # but only when it holds a translation unit.
    set(_files "")
    if(DESERT_MODULE_${name}_HAS_FOLDER AND NOT DESERT_MODULE_${name}_OWN_PROJECT)
        set(_root "${DESERT_ROOT}/${_folder}")
        file(GLOB_RECURSE _files LIST_DIRECTORIES false CONFIGURE_DEPENDS RELATIVE "${DESERT_ROOT}"
             "${_root}/*.cpp" "${_root}/*.hpp" "${_root}/*.h" "${_root}/*.inl" "${_root}/*.mm")
        list(FILTER _files INCLUDE REGEX "\\.(cpp|mm)$")
        if(_files)
            file(GLOB_RECURSE _files LIST_DIRECTORIES false CONFIGURE_DEPENDS RELATIVE "${DESERT_ROOT}"
                 "${_root}/*.cpp" "${_root}/*.hpp" "${_root}/*.h" "${_root}/*.inl" "${_root}/*.mm")
            list(SORT _files)
        endif()
    endif()
    set(DESERT_MODULE_${name}_FOLDER_FILES "${_files}")
    set(DESERT_MODULE_${name}_TABLE_LIBRARY OFF)
    if(DESERT_MODULE_${name}_LEGACY OR _files)
        set(DESERT_MODULE_${name}_TABLE_LIBRARY ON)
    endif()
    # LibrariesOf: the own project's library, then the table library Desert<Name>.
    set(_libs "")
    if(DESERT_MODULE_${name}_OWN_PROJECT)
        list(APPEND _libs "${name}")
    endif()
    if(DESERT_MODULE_${name}_TABLE_LIBRARY)
        list(APPEND _libs "Desert${name}")
    endif()
    set(DESERT_MODULE_${name}_LIBRARIES "${_libs}")
endforeach()

# ── THE TABLE FOR THE C++ READERS ──────────────────────────────────────────────────────────────────
# <build>/DesertModules.json: the rows as written (placement patterns before the \w / \d rewrite), the module each
# legacy file was placed into here, and the table library of each module with legacy files. Rewritten only when
# its text changes, so a reconfigure does not rerun the header tool.
function(_desert_json_string out text)
    string(REPLACE "\\" "\\\\" text "${text}")
    string(REPLACE "\"" "\\\"" text "${text}")
    set(${out} "\"${text}\"" PARENT_SCOPE)
endfunction()
function(_desert_write_module_json)
    set(_modules "")
    foreach(name IN LISTS DESERT_MODULES)
        set(_deps "")
        foreach(dep IN LISTS DESERT_MODULE_${name}_DEPS)
            list(APPEND _deps "\"${dep}\"")
        endforeach()
        list(JOIN _deps ", " _deps)
        list(APPEND _modules "    { \"Name\": \"${name}\", \"Folder\": \"${DESERT_MODULE_${name}_FOLDER}\", \"Deps\": [${_deps}] }")
    endforeach()
    list(JOIN _modules ",\n" _modules)
    set(_placement "")
    foreach(i RANGE ${DESERT_PLACEMENT_LAST})
        list(GET DESERT_PLACEMENT_MODULES ${i} _module)
        list(GET DESERT_PLACEMENT_SOURCE_PATTERNS ${i} _pattern)
        _desert_json_string(_pattern "${_pattern}")
        list(APPEND _placement "    { \"Module\": \"${_module}\", \"Pattern\": ${_pattern} }")
    endforeach()
    list(JOIN _placement ",\n" _placement)
    set(_files "")
    set(_libraries "")
    foreach(name IN LISTS DESERT_MODULES)
        foreach(rel IN LISTS DESERT_MODULE_${name}_LEGACY)
            _desert_json_string(_rel "${rel}")
            list(APPEND _files "    ${_rel}: \"${name}\"")
        endforeach()
        if(DESERT_MODULE_${name}_LEGACY)
            list(APPEND _libraries "    \"${name}\": \"Desert${name}\"")
        endif()
    endforeach()
    list(SORT _files)
    list(JOIN _files ",\n" _files)
    list(JOIN _libraries ",\n" _libraries)
    set(_text "{\n  \"RepoRoot\": \"${DESERT_ROOT}\",\n  \"LegacyRoot\": \"${DESERT_LEGACY_ROOT}\",\n  \"Modules\": [\n${_modules}\n  ],\n")
    string(APPEND _text "  \"Placement\": [\n${_placement}\n  ],\n  \"LegacyFiles\": {\n${_files}\n  },\n")
    string(APPEND _text "  \"LegacyLibraries\": {\n${_libraries}\n  }\n}\n")
    desert_write_generated("${DESERT_MODULE_TABLE_JSON}" "${_text}")
endfunction()
_desert_write_module_json()

# CodegenLib: the first module (in table order) that builds a table library; the header tool runs before it.
set(DESERT_CODEGEN_LIB "")
foreach(name IN LISTS DESERT_MODULES)
    if(DESERT_MODULE_${name}_TABLE_LIBRARY)
        set(DESERT_CODEGEN_LIB "Desert${name}")
        break()
    endif()
endforeach()
if(NOT DESERT_CODEGEN_LIB)
    message(FATAL_ERROR "DesertModules.cmake: no module has a file under ${DESERT_LEGACY_ROOT} or a Folder to build")
endif()

# SourceRoots (IncludeDirs): the legacy root, then every module's source root in REVERSE table order.
function(_desert_source_roots out)
    set(_closure "${ARGN}")
    set(_roots "")
    if(NOT _closure AND DESERT_HAS_LEGACY_FILES)
        list(APPEND _roots "${DESERT_ROOT}/${DESERT_LEGACY_ROOT}")
    endif()
    set(_reversed ${DESERT_MODULES})
    list(REVERSE _reversed)
    foreach(name IN LISTS _reversed)
        if((NOT _closure OR name IN_LIST _closure) AND DESERT_MODULE_${name}_HAS_FOLDER)
            list(APPEND _roots "${DESERT_ROOT}/${DESERT_MODULE_${name}_SOURCE_ROOT}")
        endif()
    endforeach()
    set(${out} ${_roots} PARENT_SCOPE)
endfunction()
_desert_source_roots(DESERT_ENGINE_INCLUDE_DIRS)

# Closure: every module a module depends on, transitively (not the module itself).
function(desert_module_closure name out)
    set(_seen "")
    set(_queue ${DESERT_MODULE_${name}_DEPS})
    while(_queue)
        list(POP_FRONT _queue _dep)
        if(NOT _dep IN_LIST _seen)
            list(APPEND _seen "${_dep}")
            list(APPEND _queue ${DESERT_MODULE_${_dep}_DEPS})
        endif()
    endwhile()
    set(${out} ${_seen} PARENT_SCOPE)
endfunction()

# DesertModuleLibs.IncludeDirs(): every engine source root.
function(desert_engine_includes target)
    target_include_directories(${target} PRIVATE ${DESERT_ENGINE_INCLUDE_DIRS})
endfunction()

# DesertModuleLibs.ClosureIncludeDirs(name): the source roots of the module's dependency closure only.
function(desert_closure_includes target module)
    desert_module_closure(${module} _closure)
    if(_closure)
        _desert_source_roots(_roots ${_closure})
        target_include_directories(${target} PRIVATE ${_roots})
    endif()
endfunction()

# The `--check`/`--context` arguments of the header tool: one per engine source root.
function(desert_header_tool_roots flag out)
    set(_args "")
    foreach(root IN LISTS DESERT_ENGINE_INCLUDE_DIRS)
        list(APPEND _args "--${flag}" "${root}")
    endforeach()
    set(${out} ${_args} PARENT_SCOPE)
endfunction()

# DesertModuleLibs.DependOnCodegen(): built after the library whose prebuild step runs the header tool.
function(desert_depend_on_codegen target)
    add_dependencies(${target} ${DESERT_CODEGEN_LIB})
endfunction()

# DesertModuleLibs.Link(): every module library, top of the table first, then SPIRV-Cross and the Vulkan SDK libraries.
function(desert_link_engine target)
    set(_names "")
    set(_reversed ${DESERT_MODULES})
    list(REVERSE _reversed)
    foreach(name IN LISTS _reversed)
        list(APPEND _names ${DESERT_MODULE_${name}_LIBRARIES})
    endforeach()
    target_link_libraries(${target} PRIVATE ${_names} ${DESERT_DESERTSPECIFIC_LIBRARIES})
endfunction()

# The header tool invocation the engine's codegen library runs before it compiles (Desert/Desert/CMakeLists.txt).
function(desert_engine_header_tool_command out)
    set(_cmd "$<TARGET_FILE:DesertHeaderTool>"
        --templates "${DESERT_ROOT}/Tools/DesertHeaderTool/Templates"
        --modules "${DESERT_MODULE_TABLE_JSON}")
    set(_first ON)
    set(_reflect_roots "")
    if(DESERT_HAS_LEGACY_FILES)
        list(APPEND _reflect_roots "${DESERT_ROOT}/${DESERT_LEGACY_ROOT}|${DESERT_LEGACY_REFLECT_DIR}")
    endif()
    foreach(name IN LISTS DESERT_MODULES)
        if(DESERT_MODULE_${name}_REFLECTED AND DESERT_MODULE_${name}_HAS_FOLDER)
            list(APPEND _reflect_roots "${DESERT_ROOT}/${DESERT_MODULE_${name}_SOURCE_ROOT}|${DESERT_MODULE_${name}_SCAN_DIR}")
        endif()
    endforeach()
    foreach(entry IN LISTS _reflect_roots)
        string(REPLACE "|" ";" _pair "${entry}")
        list(GET _pair 0 _root)
        list(GET _pair 1 _dir)
        if(_first)
            list(APPEND _cmd --reflect "${_root}" "${_dir}"
                 "${DESERT_ROOT}/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp")
            set(_first OFF)
        else()
            list(APPEND _cmd --reflect-root "${_root}" "${_dir}")
        endif()
    endforeach()
    desert_header_tool_roots(check _check)
    list(APPEND _cmd
        --reflect-enums "${DESERT_ROOT}/Desert/Common/Source/Common/Settings"
        --reflect-enums "${DESERT_ROOT}/Desert/PhysicsCore/Source/PhysicsCore"
        --reflect-components "${DESERT_ROOT}/Desert/Desert/Source/Engine/Generated/ReflectedComponentBlocks.gen.hpp"
        ${_check}
        --subsystems Engine Desert::Engine::Application Engine/Core/Application.hpp
        "${DESERT_ROOT}/Desert/Desert/Source/Engine/Generated/EngineSubsystems.gen.cpp"
        --subsystems World Desert::Core::Scene Engine/Core/Scene.hpp
        "${DESERT_ROOT}/Desert/Desert/Source/Engine/Generated/WorldSubsystems.gen.cpp")
    set(${out} ${_cmd} PARENT_SCOPE)
endfunction()

# The CMake port of BuildScripts/DesertModuleLibs.lua: the engine's module table and the one way to link the
# engine (one static library per module).
#
# ONE SOURCE OF TRUTH. The module table and the placement rows stay in BuildScripts/DesertModules.lua, which
# DesertHeaderTool also reads (`--modules`). This file PARSES that table instead of restating it, so adding a
# module or a placement row is still one edit, seen by premake, by the header tool and by CMake alike. The
# parser accepts exactly the shapes the table is written in -- `M( "Name", { deps }, "ue" [, { Folder = "...",
# Reflected = true }] )` and `P( "Module", [[pattern]] )` -- and fails configure on anything else it meets
# inside the two tables, never skipping a row silently.
#
# Placement patterns are the table's regex subset (literal chars, `.`, `\w`, `\d`, escapes, groups, `|`,
# `*`/`+`/`?`, `^`/`$`, unanchored search). CMake's regex has the same operators except the classes, so
# `\w` and `\d` are rewritten to bracket expressions and everything else is used as written.

include_guard(GLOBAL)

set(DESERT_MODULE_TABLE "${DESERT_ROOT}/BuildScripts/DesertModules.lua")
set(DESERT_LEGACY_ROOT "Desert/Desert/Source")
set(DESERT_LEGACY_REFLECT_DIR "Engine")
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DESERT_MODULE_TABLE}")

# ── PARSE THE TABLE ────────────────────────────────────────────────────────────────────────────────
file(READ "${DESERT_MODULE_TABLE}" _table)
# Drop Lua line comments; the tables' strings never contain "--".
string(REGEX REPLACE "--[^\n]*" "" _table "${_table}")
# CMake lists split on ';' -- the table has none in code; guard anyway.
if(_table MATCHES ";")
    message(FATAL_ERROR "DesertModules.cmake: ${DESERT_MODULE_TABLE} has a ';' outside a comment, which this parser cannot carry")
endif()

string(FIND "${_table}" "DesertModules.Modules = {" _mstart)
string(FIND "${_table}" "DesertModules.Placement = {" _pstart)
if(_mstart EQUAL -1 OR _pstart EQUAL -1)
    message(FATAL_ERROR "DesertModules.cmake: ${DESERT_MODULE_TABLE} has no `DesertModules.Modules = {` or `DesertModules.Placement = {`")
endif()
math(EXPR _mlen "${_pstart} - ${_mstart}")
string(SUBSTRING "${_table}" ${_mstart} ${_mlen} _modules_block)
string(SUBSTRING "${_table}" ${_pstart} -1 _placement_block)
string(FIND "${_placement_block}" "\n}" _pend)
string(SUBSTRING "${_placement_block}" 0 ${_pend} _placement_block)

# Each M( ... ) row: the only ')' inside one is in the Unreal-name string ("CoreUObject (reflection)"), so a
# parenthesised part of a quoted string is unwrapped first (the Unreal name is not used here).
string(REGEX REPLACE "\"([^\"()]*)\\(([^\"()]*)\\)([^\"()]*)\"" "\"\\1\\2\\3\"" _modules_block "${_modules_block}")
string(REGEX MATCHALL "M\\([^)]*\\)" _rows "${_modules_block}")
string(REGEX MATCHALL "[^A-Za-z_]M\\(" _row_starts "${_modules_block}")
list(LENGTH _rows _nrows)
list(LENGTH _row_starts _nstarts)
if(NOT _nrows EQUAL _nstarts OR _nrows EQUAL 0)
    message(FATAL_ERROR "DesertModules.cmake: parsed ${_nrows} module rows out of ${_nstarts} `M(` calls in ${DESERT_MODULE_TABLE}")
endif()

set(DESERT_MODULES "")
foreach(row IN LISTS _rows)
    if(NOT row MATCHES "^M\\([ \t\r\n]*\"([A-Za-z0-9]+)\"[ \t\r\n]*,[ \t\r\n]*{([^}]*)}[ \t\r\n]*,[ \t\r\n]*\"[^\"]*\"[ \t\r\n]*(,[ \t\r\n]*{([^}]*)})?[ \t\r\n]*\\)$")
        message(FATAL_ERROR "DesertModules.cmake: cannot parse module row: ${row}")
    endif()
    set(_name "${CMAKE_MATCH_1}")
    set(_deps_text "${CMAKE_MATCH_2}")
    set(_options "${CMAKE_MATCH_4}")
    string(REGEX MATCHALL "\"[A-Za-z0-9]+\"" _deps "${_deps_text}")
    string(REPLACE "\"" "" _deps "${_deps}")
    set(_folder "Desert/${_name}/Source/${_name}")
    if(_options MATCHES "Folder[ \t]*=[ \t]*\"([^\"]+)\"")
        set(_folder "${CMAKE_MATCH_1}")
    endif()
    set(_reflected OFF)
    if(_options MATCHES "Reflected[ \t]*=[ \t]*true")
        set(_reflected ON)
    endif()
    if(NOT _folder MATCHES "^(.+)/([^/]+)$")
        message(FATAL_ERROR "DesertModules.cmake: module '${_name}' has Folder '${_folder}', which is not <source root>/<dir>")
    endif()
    set(DESERT_MODULE_${_name}_SOURCE_ROOT "${CMAKE_MATCH_1}")
    set(DESERT_MODULE_${_name}_SCAN_DIR "${CMAKE_MATCH_2}")
    foreach(dep IN LISTS _deps)
        if(NOT dep IN_LIST DESERT_MODULES)
            message(FATAL_ERROR "DesertModules.cmake: '${_name}' depends on '${dep}', which is not listed before it")
        endif()
    endforeach()
    if(_name IN_LIST DESERT_MODULES)
        message(FATAL_ERROR "DesertModules.cmake: module '${_name}' is listed twice")
    endif()
    list(APPEND DESERT_MODULES "${_name}")
    set(DESERT_MODULE_${_name}_DEPS "${_deps}")
    set(DESERT_MODULE_${_name}_FOLDER "${_folder}")
    set(DESERT_MODULE_${_name}_REFLECTED ${_reflected})
endforeach()

# Placement rows, the generated-reflection rows first (in module order), as DesertModules.lua inserts them.
set(DESERT_PLACEMENT_MODULES "")
set(DESERT_PLACEMENT_PATTERNS "")
foreach(name IN LISTS DESERT_MODULES)
    list(APPEND DESERT_PLACEMENT_MODULES "${name}")
    list(APPEND DESERT_PLACEMENT_PATTERNS "Engine/Generated/Reflection_${name}\\.gen\\.cpp$")
endforeach()
string(REGEX MATCHALL "P\\([ \t]*\"[A-Za-z0-9]+\"[ \t]*,[ \t]*\\[\\[[^]]*\\]\\][ \t]*\\)" _prows "${_placement_block}")
string(REGEX MATCHALL "[^A-Za-z_]P\\(" _prow_starts "${_placement_block}")
list(LENGTH _prows _np)
list(LENGTH _prow_starts _nps)
if(NOT _np EQUAL _nps OR _np EQUAL 0)
    message(FATAL_ERROR "DesertModules.cmake: parsed ${_np} placement rows out of ${_nps} `P(` calls in ${DESERT_MODULE_TABLE}")
endif()
foreach(row IN LISTS _prows)
    string(REGEX MATCH "^P\\([ \t]*\"([A-Za-z0-9]+)\"[ \t]*,[ \t]*\\[\\[([^]]*)\\]\\]" _ "${row}")
    set(_module "${CMAKE_MATCH_1}")
    set(_pattern "${CMAKE_MATCH_2}")
    if(NOT _module IN_LIST DESERT_MODULES)
        message(FATAL_ERROR "DesertModules.cmake: placement row names '${_module}', which is not a module")
    endif()
    string(REPLACE "\\w" "[A-Za-z0-9_]" _pattern "${_pattern}")
    string(REPLACE "\\d" "[0-9]" _pattern "${_pattern}")
    list(APPEND DESERT_PLACEMENT_MODULES "${_module}")
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

# DesertModuleLibs.Link(): every module library, top of the table first, then the Vulkan SDK libraries.
function(desert_link_engine target)
    set(_names "")
    set(_reversed ${DESERT_MODULES})
    list(REVERSE _reversed)
    foreach(name IN LISTS _reversed)
        list(APPEND _names ${DESERT_MODULE_${name}_LIBRARIES})
    endforeach()
    target_link_libraries(${target} PRIVATE ${_names} ${DESERT_VULKAN_LIBRARIES})
endfunction()

# The header tool invocation the engine's codegen library runs before it compiles (Desert/Desert/premake5.lua).
function(desert_engine_header_tool_command out)
    set(_cmd "$<TARGET_FILE:DesertHeaderTool>"
        --templates "${DESERT_ROOT}/Tools/DesertHeaderTool/Templates"
        --modules "${DESERT_MODULE_TABLE}")
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

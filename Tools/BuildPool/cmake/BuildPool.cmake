# BuildPool.cmake -- the one entry of a CMake project into the machine's build pool (README.md).
#
#   include(<path>/Tools/BuildPool/cmake/BuildPool.cmake)      # after project()
#   buildpool_enable([HOME <dir>] [CACHE ccache] [CACHE_WRAPPER <command...>] [CACHE_ENV NAME=VALUE...]
#                    [LANGUAGES <lang>...] [SOURCE_ROOT <dir>] [BUILD_ROOT <dir>])
#
# What it does, at configure:
#   home      HOME, else the environment's BUILDPOOL_HOME, else the home this build tree was configured with before
#             (cache BUILDPOOL_HOME), else a PRIVATE pool in ${CMAKE_BINARY_DIR}/buildpool -- said in the configure
#             log, never silently: a private pool shares no slot with any other build of the machine.
#   binary    <home>/bin/<rev>/buildpool, built once per source revision (BuildPoolBootstrap.cmake, <rev> =
#             BuildPoolRevision.cmake) with the project's own C++ compiler and make program.
#   launchers CMAKE_<LANG>_COMPILER_LAUNCHER = `buildpool cc ... --`, CMAKE_<LANG>_LINKER_LAUNCHER = `buildpool link
#             ... --`, and the archive rules (lib.exe, ar, libtool) prefixed with the same `buildpool link`, for every
#             language of LANGUAGES (default C CXX OBJC OBJCXX) the project has enabled. Every target created after
#             the call inherits them.
#   cache     CACHE names the compile cache (only ccache: the pool runs it as an opaque prefix with itself standing in
#             for the compiler, which needs the compiler type and identity settings below). CACHE_WRAPPER goes in
#             front of the cache program (a script that takes `<cache> <compiler> <args...>`). The module owns the
#             settings that the pool's two stages require and that make the cache one per home:
#               CCACHE_DIR, CCACHE_TEMPDIR, CCACHE_CONFIGPATH  under <home>/ccache (no machine ccache.conf decides)
#               CCACHE_COMPILERTYPE   the real compiler's type (ccache sees `buildpool` as the compiler)
#               CCACHE_COMPILERCHECK  string:{compiler_id} (the client expands it: Invocation.hpp CompilerId)
#             The project's own cache choices come as CACHE_ENV; naming one of the module's settings is an error.
#             No CACHE, or the program not found: compiles run through the pool uncached and without dedup between
#             trees, and the configure log says so.
# Sets in the caller's scope: BUILDPOOL_EXECUTABLE, BUILDPOOL_HOME, BUILDPOOL_CACHE (the cache command; empty = none).
# BUILDPOOL_HOME and BUILDPOOL_EXECUTABLE are also cached, for the scripts that drive a configured tree.
include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/BuildPoolRevision.cmake")
get_filename_component(_BUILDPOOL_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(buildpool_enable)
    cmake_parse_arguments(PARSE_ARGV 0 BP "" "HOME;CACHE;SOURCE_ROOT;BUILD_ROOT" "CACHE_WRAPPER;CACHE_ENV;LANGUAGES")
    if(BP_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "buildpool_enable: unknown arguments: ${BP_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT CMAKE_CXX_COMPILER AND NOT CMAKE_C_COMPILER)
        message(FATAL_ERROR "buildpool_enable: call it after project(): the archive rules and the compiler type come from the enabled languages")
    endif()
    if(NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        message(FATAL_ERROR "buildpool_enable: the generator '${CMAKE_GENERATOR}' runs no compiler or linker launcher (CMake supports them under Ninja and Makefiles)")
    endif()
    if(NOT BP_LANGUAGES)
        set(BP_LANGUAGES C CXX OBJC OBJCXX)
    endif()
    if(NOT BP_SOURCE_ROOT)
        set(BP_SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
    endif()
    if(NOT BP_BUILD_ROOT)
        set(BP_BUILD_ROOT "${CMAKE_BINARY_DIR}")
    endif()
    file(TO_CMAKE_PATH "${BP_SOURCE_ROOT}" _source_root)
    file(TO_CMAKE_PATH "${BP_BUILD_ROOT}" _build_root)

    # ---- the home ----
    if(BP_HOME)
        set(_home "${BP_HOME}")
        set(_why "HOME argument")
    elseif(NOT "$ENV{BUILDPOOL_HOME}" STREQUAL "")
        set(_home "$ENV{BUILDPOOL_HOME}")
        set(_why "environment BUILDPOOL_HOME")
    elseif(BUILDPOOL_HOME)
        set(_home "${BUILDPOOL_HOME}")
        set(_why "this tree's earlier configure (cache BUILDPOOL_HOME)")
    else()
        set(_home "${CMAKE_BINARY_DIR}/buildpool")
        set(_why "BUILDPOOL_HOME is not set: a PRIVATE pool of this build tree, sharing no slot with other builds")
    endif()
    file(MAKE_DIRECTORY "${_home}")
    file(REAL_PATH "${_home}" _home)
    file(TO_CMAKE_PATH "${_home}" _home)
    set(BUILDPOOL_HOME "${_home}" CACHE PATH "The build pool home (pool.ini, ledger, cache, binaries); BuildPool.cmake" FORCE)
    message(STATUS "BuildPool: home ${_home} (${_why})")

    # ---- the binary ----
    buildpool_revision("${_BUILDPOOL_PROJECT_DIR}" _rev)
    foreach(_file IN LISTS _rev_FILES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_BUILDPOOL_PROJECT_DIR}/${_file}")
    endforeach()
    if(CMAKE_HOST_WIN32)
        set(_exe "${_home}/bin/${_rev}/buildpool.exe")
    else()
        set(_exe "${_home}/bin/${_rev}/buildpool")
    endif()
    if(NOT EXISTS "${_exe}")
        if(CMAKE_GENERATOR MATCHES "Ninja")
            set(_generator "Ninja")
        else()
            set(_generator "${CMAKE_GENERATOR}")
        endif()
        set(_bootstrap "${CMAKE_COMMAND}" "-DBUILDPOOL_HOME=${_home}" "-DBUILDPOOL_REV=${_rev}"
            "-DBUILDPOOL_GENERATOR=${_generator}" "-DBUILDPOOL_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
        if(CMAKE_CXX_COMPILER)
            list(APPEND _bootstrap "-DBUILDPOOL_CXX=${CMAKE_CXX_COMPILER}")
        endif()
        message(STATUS "BuildPool: building the pool binary of revision ${_rev} (once per machine and revision)")
        execute_process(COMMAND ${_bootstrap} -P "${_BUILDPOOL_PROJECT_DIR}/cmake/BuildPoolBootstrap.cmake"
            RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
        if(NOT _rc EQUAL 0 OR NOT EXISTS "${_exe}")
            message(FATAL_ERROR "BuildPool: the pool binary ${_exe} could not be built (${_rc}):\n${_out}")
        endif()
    endif()
    set(BUILDPOOL_EXECUTABLE "${_exe}" CACHE INTERNAL "The buildpool binary this tree's launchers run; BuildPool.cmake")

    # ---- the cache ----
    set(_cache "")
    set(_cache_env "")
    if(BP_CACHE)
        get_filename_component(_cache_name "${BP_CACHE}" NAME_WE)
        if(NOT _cache_name STREQUAL "ccache")
            message(FATAL_ERROR "buildpool_enable: CACHE '${BP_CACHE}' -- only ccache can run as the pool's cache prefix")
        endif()
        find_program(BUILDPOOL_CACHE_PROGRAM NAMES "${BP_CACHE}" DOC "The compile cache the build pool runs (ccache)")
        if(BUILDPOOL_CACHE_PROGRAM)
            set(_cache ${BP_CACHE_WRAPPER} "${BUILDPOOL_CACHE_PROGRAM}")
        else()
            message(STATUS "BuildPool: no ${BP_CACHE} found (set BUILDPOOL_CACHE_PROGRAM): compiles run uncached, no dedup between trees")
        endif()
    else()
        message(STATUS "BuildPool: no CACHE given: compiles run uncached, no dedup between trees")
    endif()
    if(_cache)
        set(_id "${CMAKE_CXX_COMPILER_ID}")
        set(_variant "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}")
        if(NOT _id)
            set(_id "${CMAKE_C_COMPILER_ID}")
            set(_variant "${CMAKE_C_COMPILER_FRONTEND_VARIANT}")
        endif()
        if(_id STREQUAL "MSVC")
            set(_type msvc)
        elseif(_id MATCHES "Clang" AND _variant STREQUAL "MSVC")
            set(_type clang-cl)
        elseif(_id MATCHES "Clang")
            set(_type clang)
        elseif(_id STREQUAL "GNU")
            set(_type gcc)
        else()
            message(FATAL_ERROR "buildpool_enable: ccache has no compiler type for '${_id}' (msvc, clang-cl, clang, gcc)")
        endif()
        set(_owned CCACHE_DIR CCACHE_TEMPDIR CCACHE_CONFIGPATH CCACHE_COMPILERTYPE CCACHE_COMPILERCHECK)
        foreach(_entry IN LISTS BP_CACHE_ENV)
            if(NOT _entry MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=")
                message(FATAL_ERROR "buildpool_enable: CACHE_ENV '${_entry}' is not NAME=VALUE")
            endif()
            if(CMAKE_MATCH_1 IN_LIST _owned)
                message(FATAL_ERROR "buildpool_enable: CACHE_ENV '${CMAKE_MATCH_1}' is the module's own setting (${_owned})")
            endif()
        endforeach()
        file(MAKE_DIRECTORY "${_home}/ccache/tmp")
        set(_cache_env
            "CCACHE_DIR=${_home}/ccache"
            "CCACHE_TEMPDIR=${_home}/ccache/tmp"
            "CCACHE_CONFIGPATH=${_home}/ccache/ccache.conf"
            "CCACHE_COMPILERTYPE=${_type}"
            "CCACHE_COMPILERCHECK=string:{compiler_id}"
            ${BP_CACHE_ENV})
    endif()

    # ---- the launchers ----
    set(_roots --home "${_home}" --source-root "${_source_root}" --build-root "${_build_root}")
    set(_cc "${_exe}" cc ${_roots})
    if(_cache)
        list(APPEND _cc --cache ${_cache})
        foreach(_entry IN LISTS _cache_env)
            list(APPEND _cc --cache-env "${_entry}")
        endforeach()
    endif()
    list(APPEND _cc --)
    set(_link "${_exe}" link ${_roots} --)

    # The archive rules are command strings, not lists: the same `buildpool link` spelled for the shell.
    if(CMAKE_HOST_WIN32)
        file(TO_NATIVE_PATH "${_exe}" _exe_shell)
    else()
        set(_exe_shell "${_exe}")
    endif()
    set(_link_shell "\"${_exe_shell}\" link --home \"${_home}\" --source-root \"${_source_root}\" --build-root \"${_build_root}\" -- ")

    get_property(_enabled GLOBAL PROPERTY ENABLED_LANGUAGES)
    set(_wired "")
    foreach(_lang IN LISTS BP_LANGUAGES)
        if(NOT _lang IN_LIST _enabled)
            continue()
        endif()
        list(APPEND _wired ${_lang})
        set(CMAKE_${_lang}_COMPILER_LAUNCHER ${_cc} PARENT_SCOPE)
        set(CMAKE_${_lang}_LINKER_LAUNCHER ${_link} PARENT_SCOPE)
        foreach(_rule CREATE_STATIC_LIBRARY CREATE_STATIC_LIBRARY_IPO ARCHIVE_CREATE ARCHIVE_APPEND
                      ARCHIVE_CREATE_IPO ARCHIVE_APPEND_IPO)
            set(_var CMAKE_${_lang}_${_rule})
            if(NOT "${${_var}}" STREQUAL "")
                string(FIND "${${_var}}" "${_exe_shell}" _at)
                if(_at EQUAL -1)
                    set(${_var} "${_link_shell}${${_var}}" PARENT_SCOPE)
                endif()
            endif()
        endforeach()
    endforeach()
    if(NOT _wired)
        message(FATAL_ERROR "buildpool_enable: none of LANGUAGES (${BP_LANGUAGES}) is enabled in this project")
    endif()

    set(BUILDPOOL_EXECUTABLE "${_exe}" PARENT_SCOPE)
    set(BUILDPOOL_HOME "${_home}" PARENT_SCOPE)
    set(BUILDPOOL_CACHE "${_cache}" PARENT_SCOPE)
    if(_cache)
        list(JOIN _cache " " _cache_text)
    else()
        set(_cache_text "none")
    endif()
    message(STATUS "BuildPool: ${_exe} launches the compiles and links of ${_wired}; cache: ${_cache_text}")
endfunction()

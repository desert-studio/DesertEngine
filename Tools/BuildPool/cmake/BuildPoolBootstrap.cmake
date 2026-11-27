# BuildPoolBootstrap.cmake -- builds and installs the buildpool binary of one source revision into a pool home.
#
#   cmake -DBUILDPOOL_HOME=<home> [-DBUILDPOOL_REV=<rev>] [-DBUILDPOOL_CXX=<compiler>]
#         [-DBUILDPOOL_GENERATOR=<generator>] [-DBUILDPOOL_MAKE_PROGRAM=<ninja|make>]
#         -P Tools/BuildPool/cmake/BuildPoolBootstrap.cmake
#
# Builds in <home>/build/<rev>, installs to <home>/bin/<rev>/buildpool[.exe] and prints the binary's path as the last
# line ("buildpool: <path>"). A binary already installed for <rev> is used as it is; trees configuring at once wait on
# one lock per revision, so the binary is built once per machine and revision. <rev> defaults to the hash of the
# project's sources (cmake/BuildPoolRevision.cmake), the same value buildpool_enable computes.
# Runs with the caller's environment: on Windows the compiler of the x64 developer prompt (vcvars64).
cmake_minimum_required(VERSION 3.21)

if(NOT BUILDPOOL_HOME)
    message(FATAL_ERROR "BuildPoolBootstrap: BUILDPOOL_HOME is not given (-DBUILDPOOL_HOME=<pool home>)")
endif()
get_filename_component(_bp_source "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/BuildPoolRevision.cmake")
if(NOT BUILDPOOL_REV)
    buildpool_revision("${_bp_source}" BUILDPOOL_REV)
endif()
if(NOT BUILDPOOL_GENERATOR)
    set(BUILDPOOL_GENERATOR "Ninja")
endif()

file(TO_CMAKE_PATH "${BUILDPOOL_HOME}" _bp_home)
set(_bp_build "${_bp_home}/build/${BUILDPOOL_REV}")
set(_bp_bin "${_bp_home}/bin/${BUILDPOOL_REV}")
if(CMAKE_HOST_WIN32)
    set(_bp_exe "${_bp_bin}/buildpool.exe")
else()
    set(_bp_exe "${_bp_bin}/buildpool")
endif()

file(MAKE_DIRECTORY "${_bp_home}/build")
file(LOCK "${_bp_home}/build/${BUILDPOOL_REV}.lock" GUARD FILE TIMEOUT 900 RESULT_VARIABLE _bp_lock)
if(NOT _bp_lock EQUAL 0)
    message(FATAL_ERROR "BuildPoolBootstrap: no lock on ${_bp_home}/build/${BUILDPOOL_REV}.lock after 900 s: ${_bp_lock}")
endif()

if(NOT EXISTS "${_bp_exe}")
    set(_bp_configure "${CMAKE_COMMAND}" -S "${_bp_source}" -B "${_bp_build}" -G "${BUILDPOOL_GENERATOR}"
        -DCMAKE_BUILD_TYPE=Release
        # The pool is not its own client: whatever launcher the environment names stays out of this build.
        -DCMAKE_CXX_COMPILER_LAUNCHER= -DCMAKE_CXX_LINKER_LAUNCHER=)
    if(BUILDPOOL_CXX)
        list(APPEND _bp_configure "-DCMAKE_CXX_COMPILER=${BUILDPOOL_CXX}")
    endif()
    if(BUILDPOOL_MAKE_PROGRAM)
        list(APPEND _bp_configure "-DCMAKE_MAKE_PROGRAM=${BUILDPOOL_MAKE_PROGRAM}")
    endif()
    set(_bp_log "${_bp_home}/build/${BUILDPOOL_REV}.log")
    foreach(_bp_step configure build install)
        if(_bp_step STREQUAL "configure")
            set(_bp_cmd ${_bp_configure})
        elseif(_bp_step STREQUAL "build")
            set(_bp_cmd "${CMAKE_COMMAND}" --build "${_bp_build}" --config Release)
        else()
            set(_bp_cmd "${CMAKE_COMMAND}" --install "${_bp_build}" --config Release --prefix "${_bp_bin}")
        endif()
        execute_process(COMMAND ${_bp_cmd} RESULT_VARIABLE _bp_rc OUTPUT_VARIABLE _bp_out ERROR_VARIABLE _bp_out)
        file(APPEND "${_bp_log}" "---- ${_bp_step}\n${_bp_out}\n")
        if(NOT _bp_rc EQUAL 0)
            string(LENGTH "${_bp_out}" _bp_len)
            if(_bp_len GREATER 3000)
                math(EXPR _bp_from "${_bp_len} - 3000")
                string(SUBSTRING "${_bp_out}" ${_bp_from} -1 _bp_out)
            endif()
            message(FATAL_ERROR "BuildPoolBootstrap: ${_bp_step} of ${_bp_source} failed (${_bp_rc}); log ${_bp_log}\n${_bp_out}")
        endif()
    endforeach()
    if(NOT EXISTS "${_bp_exe}")
        message(FATAL_ERROR "BuildPoolBootstrap: the install step wrote no ${_bp_exe}; log ${_bp_log}")
    endif()
endif()

file(LOCK "${_bp_home}/build/${BUILDPOOL_REV}.lock" RELEASE)
message("buildpool: ${_bp_exe}")

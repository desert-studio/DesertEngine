# DesertEngine -- CMake entry point: every target, one output directory (build/Bin/<Config>), the shared
# settings in cmake/*.cmake. The build of record on both platforms (scripts/Windows/BuildWindows.bat,
# scripts/MacOS/BuildMacOS.sh).
#
#   cmake --preset windows-msvc-ninja        (from an x64 developer prompt: vcvars64)
#   cmake --build --preset windows-msvc-ninja-debug
#   cmake --preset macos-clang-ninja && cmake --build --preset macos-clang-ninja-release
cmake_minimum_required(VERSION 3.28)

# A compiler launcher (sccache first, then ccache) when one is installed: chosen before project() so the
# workspace settings below can switch the MSVC debug format to /Z7, which a cache can share.
if(NOT DEFINED DESERT_COMPILER_LAUNCHER)
    find_program(_desert_launcher NAMES sccache ccache)
    set(DESERT_COMPILER_LAUNCHER "${_desert_launcher}" CACHE FILEPATH "Compiler launcher (sccache/ccache); empty for none")
    unset(_desert_launcher CACHE)
endif()
if(DESERT_COMPILER_LAUNCHER)
    set(_desert_launcher "${DESERT_COMPILER_LAUNCHER}")
    # ccache outside Windows goes through scripts/Dev/ccache_launcher.sh: the cache settings that share it between
    # trees (base_dir, a tree-independent digest of each PCH), whoever starts the build.
    if(NOT WIN32 AND DESERT_COMPILER_LAUNCHER MATCHES "ccache$")
        set(_desert_launcher "${CMAKE_SOURCE_DIR}/scripts/Dev/ccache_launcher.sh" "${DESERT_COMPILER_LAUNCHER}")
    endif()
    # ccache with MSVC: the cache settings travel on its command line (ccache KEY=VALUE compiler ...), so they are the
    # build's own, not the machine's ccache.conf (BUILD-FAST-WIN, measured with CCACHE_DEBUG on one TU, 11-18):
    #   depend_mode=false  depend mode takes a source's headers from /showIncludes lines that start with
    #                      msvc_dep_prefix ("Note: including file:"); a localized cl (the Russian-only BuildTools
    #                      here print the prefix in CP866; no 1033 pack, VSLANG ignored) matches none, so the manifest held no header and
    #                      an edited header outside the PCH could be served stale (11-04). Preprocessor mode reads
    #                      the #line output instead, whatever the language.
    #   max_size=20G       an MSVC PCH result is ~1 GB raw / ~180 MB compressed; at 3 GB the cache evicted itself (9095 cleanups).
    if(WIN32 AND DESERT_COMPILER_LAUNCHER MATCHES "ccache(\\.exe)?$")
        set(_desert_launcher "${DESERT_COMPILER_LAUNCHER}" depend_mode=false max_size=20G)
    endif()
    set(CMAKE_C_COMPILER_LAUNCHER   ${_desert_launcher})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${_desert_launcher})
    set(CMAKE_OBJCXX_COMPILER_LAUNCHER ${_desert_launcher})
endif()

project(Desert LANGUAGES C CXX)
if(APPLE)
    # Objective-C++ is compiled by the C++ compiler project() just resolved, BY ITS FULL PATH. A bare name
    # ("clang++", once in the preset) is not looked up for OBJCXX the way it is for CXX: enable_language failed
    # with "is not a full path and was not found in the PATH" on the macOS runners (run 40953089350).
    if(NOT CMAKE_OBJCXX_COMPILER)
        set(CMAKE_OBJCXX_COMPILER "${CMAKE_CXX_COMPILER}")
    endif()
    enable_language(OBJCXX)
endif()
# clang writes the mtime of every header into a PCH and rejects the PCH when one differs; the mtimes differ per tree,
# so a PCH (and the cache entries of the sources behind it) would only ever serve the tree that built it. Ninja's
# depfiles already rebuild a PCH whose header changed. ccache requires it for PCH caching (ccache(1) PRECOMPILED HEADERS).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:SHELL:-Xclang -fno-pch-timestamp>")
endif()

# Build-cost measurement (BUILD-MOD-4): clang writes <object>.json next to every object, which
# scripts/Dev/build_cost.sh feeds to ClangBuildAnalyzer (header parse seconds by full path). OFF by default:
# it changes every compile line, so turning it on rebuilds the tree once.
option(DESERT_TIME_TRACE "Compile with clang -ftime-trace for scripts/Dev/build_cost.sh" OFF)
if(DESERT_TIME_TRACE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC)
        message(FATAL_ERROR "DESERT_TIME_TRACE needs clang (not clang-cl/MSVC); compiler is ${CMAKE_CXX_COMPILER_ID}")
    endif()
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-ftime-trace>")
endif()

# The presets turn it ON (CMakePresets.json `base`); the build scripts pass it on every configure, so a
# -DDESERT_WITH_TESTS=OFF run (CI's no-tests legs) is honoured and never sticks to the next configure.
option(DESERT_WITH_TESTS "Configure the test suites" OFF)

include(cmake/DesertWorkspace.cmake)
include(cmake/DesertDependencies.cmake)
# The engine's module table and the one way to link the engine: before any target.
include(cmake/DesertModules.cmake)
# The editor's five layered libraries and the one way to link them: before the tests and tools that link them.
include(cmake/DesertEditorLibs.cmake)

add_subdirectory(ThirdParty)

# Tools that need nothing of the engine.
add_subdirectory(Tools/DesertHeaderTool)
add_subdirectory(Tools/FbxMeshSplitter)
add_subdirectory(Tools/DShaderTool)
add_subdirectory(Tools/PakTool)
add_subdirectory(Tools/WorldGen)
add_subdirectory(Tools/ImageStat)
add_subdirectory(Tools/LineJump)
add_subdirectory(Tools/ImageDiff)
add_subdirectory(Tools/DesertCtl)
add_subdirectory(Tools/DomeSheet)
add_subdirectory(Tools/CrashReporter)
add_subdirectory(Tools/UIMockup)

add_subdirectory(Desert)

# Tools that compile or link engine code.
add_subdirectory(Tools/CloudVolumeBaker)
add_subdirectory(Tools/CloudLayoutBaker)
add_subdirectory(Tools/LatticePeak)
add_subdirectory(Tools/AssetClosure)
add_subdirectory(Tools/AssetRegistryTool)
add_subdirectory(Tools/GamePackager)
add_subdirectory(Tools/WorldCook)
add_subdirectory(Tools/SceneMigrator)
add_subdirectory(Tools/TextureCook)
add_subdirectory(Tools/DesertNetProbe)

add_subdirectory(Editor)
add_subdirectory(Runtime)

# Per-source rules (vendored warnings off, Debug runtime checks, unity opt-outs) once every target exists.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL desert_finalize_targets)

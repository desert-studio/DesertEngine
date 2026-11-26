# DesertEngine -- CMake entry point: every target, one output directory (build/Bin/<Config>), the shared
# settings in cmake/*.cmake. The build of record on both platforms (scripts/Windows/BuildWindows.bat,
# scripts/MacOS/BuildMacOS.sh).
#
#   cmake --preset win-dev && cmake --build --preset win-dev      (from an x64 developer prompt: vcvars64)
#   cmake --preset mac-dev && cmake --build --preset mac-dev
# One configuration per tree: win-dev / win-debug / win-ship (Development, DebugGame, Shipping; cmake/DesertWorkspace.cmake).
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
    #   max_size=100G      an MSVC PCH result is ~1 GB raw / ~180 MB compressed; at 3 GB the cache evicted itself (9095
    #                      cleanups), at 20 GB still 352 cleanups and 7 % hits on a batch build (11-24).
    #   base_dir, hash_dir=false  paths under the repository root are hashed relative to it, and the working directory
    #                      not at all: two worktrees (S:/wt/<task>, F:/DesertEngine) share one result (STUDIO-AUDIT B-CACHE).
    #   compiler_check=content  the compiler is identified by its contents, not its mtime: a Visual Studio update that
    #                      rewrites cl.exe without changing it keeps the cache.
    #   sloppiness         a header's ctime/mtime (a checkout touches every file), the PCH's defines, __TIME__/__DATE__
    #                      and system headers do not decide a hit; the contents do.
    if(WIN32 AND DESERT_COMPILER_LAUNCHER MATCHES "ccache(\\.exe)?$")
        set(_desert_launcher "${DESERT_COMPILER_LAUNCHER}" depend_mode=false max_size=100G
            "base_dir=${CMAKE_SOURCE_DIR}" hash_dir=false compiler_check=content
            "sloppiness=include_file_ctime,include_file_mtime,pch_defines,time_macros,system_headers")
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

# Build-cost measurement (STUDIO-AUDIT B-GATES): scripts/Dev/build_cost.py reads what the compiler records. clang
# (clang-cl too) writes <object>.json next to every object for ClangBuildAnalyzer; MSVC prints per-source front/back-end
# seconds (/Bt+) and the slow functions of code generation (/d2cgsummary) into the build output. OFF by default: it
# changes every compile line, so turning it on rebuilds the tree once.
option(DESERT_TIME_TRACE "Record compile timings for scripts/Dev/build_cost.py" OFF)
if(DESERT_TIME_TRACE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND MSVC)
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/clang:-ftime-trace>")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-ftime-trace>")
    elseif(MSVC)
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/Bt+>" "$<$<COMPILE_LANGUAGE:C,CXX>:/d2cgsummary>")
    else()
        message(FATAL_ERROR "DESERT_TIME_TRACE knows clang and MSVC; compiler is ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

# The presets turn it ON (CMakePresets.json `base`); the build scripts pass it on every configure, so a
# -DDESERT_WITH_TESTS=OFF run (CI's no-tests legs) is honoured and never sticks to the next configure.
option(DESERT_WITH_TESTS "Configure the test suites" OFF)

# The dedicated-server build (NET-ARCH §6, §9 NET-MESH-0; UE: a Server target built alone, -server). It configures
# the engine module table, the vendored libraries and the header tool, then only the server: no editor, no player,
# no engine tools and no test suites, so a Linux server image needs no Vulkan SDK, window system or GPU driver.
# Server/Dockerfile and the CI job server-linux configure with it (preset linux-server-clang).
option(DESERT_SERVER_ONLY "Configure only the dedicated server DesertServer and what it links" OFF)
if(DESERT_SERVER_ONLY AND DESERT_WITH_TESTS)
    message(FATAL_ERROR "DESERT_SERVER_ONLY configures no test suites; pass -DDESERT_WITH_TESTS=OFF")
endif()

include(cmake/DesertWorkspace.cmake)
include(cmake/DesertDependencies.cmake)
# The engine's module table and the one way to link the engine: before any target.
include(cmake/DesertModules.cmake)
# The editor's five layered libraries and the one way to link them: before the tests and tools that link them.
include(cmake/DesertEditorLibs.cmake)

add_subdirectory(ThirdParty)

# Tools that need nothing of the engine. The header tool is the one the server build needs: it runs before the
# engine's codegen library compiles.
add_subdirectory(Tools/DesertHeaderTool)
# Writes the generated shader includes before the ShaderCompiler module compiles; the server build compiles
# that module too.
add_subdirectory(Tools/ShaderProtocolGen)
if(NOT DESERT_SERVER_ONLY)
    add_subdirectory(Tools/FbxMeshSplitter)
    add_subdirectory(Tools/PakTool)
    add_subdirectory(Tools/ImageStat)
    add_subdirectory(Tools/LineJump)
    add_subdirectory(Tools/ImageDiff)
    add_subdirectory(Tools/DesertCtl)
    add_subdirectory(Tools/DomeSheet)
    add_subdirectory(Tools/CrashReporter)
    add_subdirectory(Tools/UIMockup)
endif()

add_subdirectory(Desert)

if(NOT DESERT_SERVER_ONLY)
    # Tools that link engine code (desert_link_engine_tool / desert_link_editor_tool, never its sources).
    add_subdirectory(Tools/CloudVolumeBaker)
    add_subdirectory(Tools/CloudLayoutBaker)
    add_subdirectory(Tools/LatticePeak)
    add_subdirectory(Tools/AssetClosure)
    add_subdirectory(Tools/AssetRegistryTool)
    add_subdirectory(Tools/GamePackager)
    add_subdirectory(Tools/WorldCook)
    add_subdirectory(Tools/SceneMigrator)
    add_subdirectory(Tools/WorldGen)
    add_subdirectory(Tools/DShaderTool)
    add_subdirectory(Tools/TextureCook)
    add_subdirectory(Tools/DesertNetProbe)

    add_subdirectory(Editor)
    add_subdirectory(Runtime)
endif()
# The dedicated headless server (NET-ARCH §6): render-free, no Apple framework, built for Linux by Server/Dockerfile.
add_subdirectory(Server)

# Per-source rules (vendored warnings off, DebugGame runtime checks, unity exclusions) once every target exists.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL desert_finalize_targets)

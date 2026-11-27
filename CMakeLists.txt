# DesertEngine -- CMake entry point: every target, one output directory (build/Bin/<Config>), the shared
# settings in cmake/*.cmake. The build of record on both platforms (scripts/Windows/BuildWindows.bat,
# scripts/MacOS/BuildMacOS.sh).
#
#   cmake --preset win-dev && cmake --build --preset win-dev      (from an x64 developer prompt: vcvars64)
#   cmake --preset mac-dev && cmake --build --preset mac-dev
# One configuration per tree: win-dev / win-debug / win-ship (Development, DebugGame, Shipping; cmake/DesertWorkspace.cmake).
cmake_minimum_required(VERSION 3.28)

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
# Every compile and link of every target goes through the machine's build pool (Tools/BuildPool/README.md): one owner
# of the compiler and linker slots of the machine, admitting by measured memory, with ccache behind it. The home comes
# from BUILDPOOL_HOME (unset: a private pool of this tree, said in the configure log). Before the workspace settings:
# with a cache the MSVC debug information is embedded (/Z7, cmake/DesertWorkspace.cmake), which a cache can share.
# The cache choices that are this project's (the identity settings the pool needs are the module's):
#   CCACHE_NODEPEND    depend mode takes a source's headers from /showIncludes lines that start with msvc_dep_prefix
#                      ("Note: including file:"); a localized cl (the Russian-only BuildTools here print the prefix in
#                      CP866; no 1033 pack, VSLANG ignored) matches none, so the manifest held no header and an edited
#                      header outside the PCH could be served stale (11-04). Preprocessor mode reads the #line output.
#   CCACHE_MAXSIZE     an MSVC PCH result is ~1 GB raw / ~180 MB compressed; at 3 GB the cache evicted itself (9095
#                      cleanups), at 20 GB still 352 cleanups and 7 % hits on a batch build (11-24).
#   CCACHE_NOHASHDIR   the working directory is not hashed. No CCACHE_BASEDIR: with base_dir ccache 4.14.1 rewrites
#                      the /showIncludes lines, and the CP866 lines of the Russian cl abort it (std::filesystem
#                      "Illegal byte sequence", code 3) on every cache miss (build #35, 11-27).
#   CCACHE_SLOPPINESS  a header's ctime/mtime (a checkout touches every file), the PCH's defines, __TIME__/__DATE__ and
#                      system headers do not decide a hit; the contents do.
# Outside Windows the cache runs behind scripts/Dev/ccache_launcher.sh, which owns base_dir, sloppiness and the
# tree-independent PCH digest there.
include(Tools/BuildPool/cmake/BuildPool.cmake)
if(WIN32)
    buildpool_enable(CACHE ccache CACHE_ENV
        CCACHE_NODEPEND=1 CCACHE_MAXSIZE=100G CCACHE_NOHASHDIR=1
        "CCACHE_SLOPPINESS=include_file_ctime,include_file_mtime,pch_defines,time_macros,system_headers")
else()
    buildpool_enable(CACHE ccache CACHE_WRAPPER "${CMAKE_SOURCE_DIR}/scripts/Dev/ccache_launcher.sh"
        CACHE_ENV CCACHE_MAXSIZE=100G)
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

# Workspace-wide settings shared by every target: configurations, compile flags, output directories,
# platform settings, the unity switch and the target helpers every CMakeLists.txt declares through.
#
# Everything set here at directory scope reaches every target below the root, first-party and vendored
# alike. The warning level, optimisation and runtime checks are set
# per TARGET, and the per-file rules (warnings off for vendored files, the Debug-optimised
# builders) become per-target settings of split OBJECT libraries; both are applied once,
# after every target exists, by desert_finalize_targets() (called from the root CMakeLists.txt through
# cmake_language(DEFER)).

include_guard(GLOBAL)

if(NOT DESERT_ROOT)
  set(DESERT_ROOT "${CMAKE_SOURCE_DIR}")
endif()

# ── PLATFORM ───────────────────────────────────────────────────────────
set(DESERT_IS_WINDOWS OFF)
set(DESERT_IS_MACOS OFF)
set(DESERT_IS_LINUX OFF)
if(WIN32)
    set(DESERT_IS_WINDOWS ON)
elseif(APPLE)
    set(DESERT_IS_MACOS ON)
else()
    set(DESERT_IS_LINUX ON)
endif()

# Homebrew prefix on macOS (arm64 -> /opt/homebrew, intel -> /usr/local); empty when absent.
set(DESERT_HOMEBREW_PREFIX "")
if(DESERT_IS_MACOS)
    foreach(prefix "$ENV{HOMEBREW_PREFIX}" "/opt/homebrew" "/usr/local")
        if(prefix AND IS_DIRECTORY "${prefix}/include")
            set(DESERT_HOMEBREW_PREFIX "${prefix}")
            break()
        endif()
    endforeach()
endif()

# ── CONFIGURATIONS ───────────────────────────────────────────────
# Development, DebugGame and Shipping -- the studio meaning of the three names (STUDIO-AUDIT B1):
#   Development  the daily editor, every dev script and every scenario (scripts/Dev/dev_config.txt): optimised
#                (MSVC /Ox /Oi /Gy /GF per target, clang -O2), NDEBUG, the development instruments on, debug
#                information embedded (/Z7), an incremental link with /DEBUG:FASTLINK (the .pdb references the
#                objects instead of copying their CodeView), no /LTCG, no /OPT:REF,ICF. Defines DESERT_CONFIG_RELEASE.
#   DebugGame    a debugger session: no optimisation (MSVC /Od /RTC1 per target, clang -O0 -g), debug CRT, the
#                instruments on, an incremental link. Defines DESERT_CONFIG_DEBUG.
#   Shipping     the packaged game only: full optimisation, /Gw /Ob3 /GL on the runtime libraries (desert_ltcg),
#                /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO, a full .pdb (/Zi /DEBUG:FULL), instruments off
#                (DESERT_CONFIG_SHIPPING, which Common/Core/DevInstruments.hpp turns into DESERT_DEV_INSTRUMENTS 0).
# One configuration per build tree (CMakePresets.json: win-dev, win-debug, win-ship, mac-dev, ...): the objects of a
# debugger session never sit beside the daily build's. Flags are spelled out instead of inheriting CMake's
# per-configuration defaults, so a CMake upgrade cannot change how a configuration compiles.
set(DESERT_CONFIGURATIONS Development DebugGame Shipping)
get_property(_desert_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_desert_multi_config)
    message(FATAL_ERROR "${CMAKE_GENERATOR} holds several configurations in one tree; DesertEngine builds one per tree "
                        "(the module libraries are DLLs in Development/DebugGame and static in Shipping): "
                        "configure through a CMakePresets.json preset (win-dev, win-ship, mac-dev, ...)")
endif()
if(NOT CMAKE_BUILD_TYPE IN_LIST DESERT_CONFIGURATIONS)
    message(FATAL_ERROR "CMAKE_BUILD_TYPE is '${CMAKE_BUILD_TYPE}', not one of ${DESERT_CONFIGURATIONS}: "
                        "configure through a CMakePresets.json preset (win-dev, win-ship, mac-dev, ...)")
endif()
# The module libraries (cmake/DesertModuleTable.cmake rows, the editor layers): one DLL / dylib per module in Development
# and DebugGame, so an edit relinks that module alone; static libraries linked into one monolithic executable in
# Shipping, where /LTCG sees the whole program (desert_module_library, cmake/DesertModules.cmake).
if(CMAKE_BUILD_TYPE STREQUAL "Shipping")
    set(DESERT_MONOLITHIC ON)
else()
    set(DESERT_MONOLITHIC OFF)
    # Every static library that ends up inside a shared module (the vendored ones) is position-independent.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # A module DLL sits beside the executable (build/Bin/<Config>): macOS and Linux find it through the rpath.
    if(APPLE)
        set(CMAKE_BUILD_RPATH "@executable_path")
    elseif(UNIX)
        set(CMAKE_BUILD_RPATH "$ORIGIN")
    endif()
endif()

foreach(lang C CXX)
    if(MSVC)
        # The optimisation level and /RTC1 are per TARGET (desert_finalize_targets, _desert_target_flags): a target
        # optimised for speed must not also get /Od or /Ox from here (cl D9025). No /DWIN32 /D_WINDOWS:
        # no source tests them.
        set(CMAKE_${lang}_FLAGS_DEBUGGAME   "" CACHE STRING "" FORCE)
        set(CMAKE_${lang}_FLAGS_DEVELOPMENT "/DNDEBUG" CACHE STRING "" FORCE)
        set(CMAKE_${lang}_FLAGS_SHIPPING    "/DNDEBUG" CACHE STRING "" FORCE)
    else()
        set(CMAKE_${lang}_FLAGS_DEBUGGAME   "-O0 -g" CACHE STRING "" FORCE)
        set(CMAKE_${lang}_FLAGS_DEVELOPMENT "-O2 -DNDEBUG" CACHE STRING "" FORCE)
        set(CMAKE_${lang}_FLAGS_SHIPPING    "-O3 -DNDEBUG" CACHE STRING "" FORCE)
    endif()
endforeach()
if(APPLE)
    set(CMAKE_OBJCXX_FLAGS_DEBUGGAME   "${CMAKE_CXX_FLAGS_DEBUGGAME}" CACHE STRING "" FORCE)
    set(CMAKE_OBJCXX_FLAGS_DEVELOPMENT "${CMAKE_CXX_FLAGS_DEVELOPMENT}" CACHE STRING "" FORCE)
    set(CMAKE_OBJCXX_FLAGS_SHIPPING    "${CMAKE_CXX_FLAGS_SHIPPING}" CACHE STRING "" FORCE)
endif()
# ── THE PROCESS ALLOCATOR AND THE LINK POOLS ──
# UE: FMallocMimalloc / FMallocBinned as the process allocator. MSVC only.
#   DESERT_MIMALLOC  mimalloc.dll + mimalloc-redirect.dll as the process allocator of the executables marked
#                    desert_link_mimalloc (Editor, Runtime, DesertCtl) in Development and Shipping. DebugGame keeps
#                    the debug CRT heap: the redirect patches ucrtbase.dll, which a /MDd process does not allocate from.
# Whole-program optimisation is not a switch: it is what Shipping is (/GL on the desert_ltcg runtime libraries, /LTCG on
# the link); no other configuration pays for it.
# Link pools (Ninja): an executable links alone (desert_link_exe=1: Editor.exe holds every object of the editor, and two
# such links beside the compile ran out of memory, LNK1327, ledger 11-24); module DLLs and libraries link three at a
# time (desert_link_dll=3). Compiles keep running beside both. desert_finalize_targets() puts every executable in
# desert_link_exe.
if(MSVC)
    option(DESERT_MIMALLOC "MSVC: mimalloc + mimalloc-redirect as the process allocator (Development, Shipping)" ON)
endif()
if(CMAKE_GENERATOR MATCHES "Ninja")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS desert_link_exe=1 desert_link_dll=3)
    set(CMAKE_JOB_POOL_LINK desert_link_dll)
endif()

# ── THE INSTRUCTION SET (CPU-SIMD) ─────────────────────────────────────────────
# UE: one vector layer (VectorRegister: SSE/AVX behind one header) over the baseline ISA the binary is built for.
#   DESERT_AVX2      x86-64: ONE minimum CPU for every compiler (UE MinCpuArchX64 = AVX2), so a Windows client
#                    and a Linux server run the same code paths. MSVC takes /arch:AVX2; clang and gcc take
#                    -march=x86-64-v3, the set /arch:AVX2 enables (AVX, AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE),
#                    with -ffp-contract=off: clang and gcc would otherwise fuse a*b+c into an FMA, which MSVC's
#                    /fp:precise does not, and a client and its server would round the same expression apart.
#                    macOS is arm64 (CMAKE_SYSTEM_PROCESSOR there names the host, not CMAKE_OSX_ARCHITECTURES).
#                    Every target in EVERY configuration, so DebugGame compiles the same code paths as Development and
#                    Shipping (Common/Core/Math/Simd.hpp picks them at compile time from _M_X64 / __x86_64__ /
#                    __AVX__) and a DebugGame test exercises what ships. The compiler autovectorises
#                    with AVX2; the hot loops it cannot vectorise (frustum culling) are written by hand through
#                    Simd.hpp. VS2022's /fp:precise does not contract a*b+c into an FMA (that is /fp:contract),
#                    so a scalar and a SIMD path of the same arithmetic round alike. A vendored library with its
#                    OWN instruction-set choice (desert_own_isa_dispatch: Embree's SSE2 ISA build, dav1d's and
#                    meshoptimizer's cpuid dispatch, Compressonator's, mimalloc, which runs before main) keeps
#                    its own flags. glm does not follow /arch: its SIMD types switch on only with
#                    GLM_FORCE_INTRINSICS / GLM_FORCE_*, which nothing defines, so __AVX2__ changes no glm
#                    layout. clang (macOS ARM64) takes the scalar path.
if((MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64") OR
   (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$"))
    option(DESERT_AVX2 "x86-64: AVX2 (MSVC /arch:AVX2, clang/gcc -march=x86-64-v3) on every target without its own ISA dispatch" ON)
endif()

if(MSVC)
    # MSBuild's ClCompile defaults that differ from cl's own: ExceptionHandling Sync (/EHsc, passed to .c too),
    # RemoveUnreferencedCodeData (/Zc:inline), UseFullPaths (/FC). SupportJustMyCode (/JMC) under UseDebugLibraries
    # is added per configuration below.
    set(CMAKE_C_FLAGS "/EHsc /Zc:inline /FC" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS "/EHsc /Zc:inline /FC" CACHE STRING "" FORCE)
    # Every configuration writes a .pdb (DesertCrashReporter symbolises with it). Development and DebugGame link
    # incrementally with /DEBUG:FASTLINK: an edit relinks what changed, and the .pdb points into the objects (/Z7)
    # instead of merging 12 GB of CodeView (Editor.exe linked 952 s with /INCREMENTAL:NO /OPT:REF,ICF, 11-26).
    # Shipping alone folds and drops (/OPT:REF,ICF) and generates code across the program (/LTCG, UE bAllowLTCG):
    # a full .pdb from /Zi objects. /GL IL lives only in Shipping objects, so no Development archive carries it
    # (the 4 GB archive limit: lib.exe hangs at 4294967295 bytes, 11-07).
    foreach(kind EXE SHARED MODULE)
        set(CMAKE_${kind}_LINKER_FLAGS_DEVELOPMENT "/DEBUG:FASTLINK /INCREMENTAL" CACHE STRING "" FORCE)
        set(CMAKE_${kind}_LINKER_FLAGS_DEBUGGAME   "/DEBUG:FASTLINK /INCREMENTAL" CACHE STRING "" FORCE)
        set(CMAKE_${kind}_LINKER_FLAGS_SHIPPING    "/DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF /LTCG" CACHE STRING "" FORCE)
    endforeach()
    set(CMAKE_STATIC_LINKER_FLAGS_DEVELOPMENT "" CACHE STRING "" FORCE)
    set(CMAKE_STATIC_LINKER_FLAGS_DEBUGGAME   "" CACHE STRING "" FORCE)
    set(CMAKE_STATIC_LINKER_FLAGS_SHIPPING    "" CACHE STRING "" FORCE)
    # Shipping's /Zi objects of one target write one .pdb in parallel: /FS serialises the writes through mspdbsrv.
    add_compile_options("$<$<AND:$<CONFIG:Shipping>,$<COMPILE_LANGUAGE:C,CXX>>:/FS>")
else()
    foreach(kind EXE SHARED MODULE STATIC)
        set(CMAKE_${kind}_LINKER_FLAGS_DEVELOPMENT "" CACHE STRING "" FORCE)
        set(CMAKE_${kind}_LINKER_FLAGS_DEBUGGAME   "" CACHE STRING "" FORCE)
        set(CMAKE_${kind}_LINKER_FLAGS_SHIPPING    "" CACHE STRING "" FORCE)
    endforeach()
endif()

# MSBuild's SupportJustMyCode default under UseDebugLibraries (DebugGame): /JMC.
if(MSVC)
    add_compile_options("$<$<AND:$<CONFIG:DebugGame>,$<COMPILE_LANGUAGE:C,CXX>>:/JMC>")
endif()
# MSBuild never scans for C++20 module dependencies (ScanSourceForModuleDependencies false); a scanned source
# also never joins a unity batch.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# /MDd for DebugGame, /MD for Development and Shipping (dynamic CRT, one runtime across every target).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:DebugGame>:Debug>DLL")

# Debug information, never /ZI (edit-and-continue defeats a cache, BUILD1 P3). Development and DebugGame embed it in
# every object (/Z7): /DEBUG:FASTLINK reads it from there, and a compiler cache can share an object but not the one
# vc143.pdb every compile of a target writes into. Shipping writes a .pdb per target (/Zi) for the full link .pdb.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<CONFIG:Shipping>,ProgramDatabase,Embedded>")

# /Yl-: no __@@_PchSym_ reference symbol in the precompiled-header objects. Every target links its PCH holder's
# object itself, so the reference has nothing to pull in, and WINDOWS_EXPORT_ALL_SYMBOLS cut the symbol at its '@'
# into a `__ DATA` export the linker matched only fuzzily: ambiguous, LNK2001 `__`, once shaderc_combined's own
# PchSym symbols joined DesertShaderCompiler's link (B-INT build #12).
add_compile_options($<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Yl->)

add_compile_definitions(
    $<$<CONFIG:Shipping>:DESERT_CONFIG_SHIPPING>
    # EnTT's own counters (entt::family) atomic in every target or none; its type index is process-wide
    # (Common/Core/Entt.hpp).
    ENTT_USE_ATOMIC
    # spdlog is a compiled library (cmake/ThirdParty/Spdlog.cmake), so every target or none: spdlog catches nothing,
    # its own failure aborts with its message, and a sink's throw ends the process at the log call (Logger.hpp).
    SPDLOG_COMPILED_LIB
    SPDLOG_NO_EXCEPTIONS
    # Optick (the CPU profiler) in every target or none, with ONE value: Profiler.hpp's class layout depends on it,
    # and optick.config.h turns an undefined USE_OPTICK on. Off in Shipping, where no target links the library
    # either (DESERT_OPTICK), as Profiler.hpp promises. GPU and tracing off everywhere.
    $<IF:$<CONFIG:Shipping>,USE_OPTICK=0,USE_OPTICK=1>
    OPTICK_ENABLE_GPU=0
    OPTICK_ENABLE_TRACING=0
)
# The Optick library as a link item: every configuration but Shipping.
set(DESERT_OPTICK "$<$<NOT:$<CONFIG:Shipping>>:Optick>")

# ── THIRD-PARTY PROCESS STATE ─────────────────────────────────────────────────────────────────────
# A vendored library that keeps process-wide state - the ImGui context, the node editor's current editor, the spdlog
# registry, Jolt's allocator hooks and factory, the Optick profiler, GLFW's library state - is ONE shared library
# outside Shipping (desert_add_library PROCESS_STATE), so every module DLL reads the same instance; the Shipping
# monolith links it statically. Each library is built with its own documented shared-build switches: the library
# compiles the export side, EVERY other target the import side, so no target's definitions depend on what it links
# (the members of a shared PCH group must agree, C2855). Optick has no import side (OPTICK_EXPORTS, Optick.cmake).
# EnTT is header-only; its type index is made process-wide by Common/Core/Entt.hpp.
# Luau is one PROCESS_STATE library too (cmake/ThirdParty/Luau.cmake: the VM state two modules run). The other
# vendored libraries several modules link stay STATIC, a copy per DLL, because they keep no process state the
# engine relies on: MeshOptimizer's only global is the allocator pair meshopt_setAllocator sets, which nothing calls;
# Voro, Compressonator, OpenSubdiv, Embree (state per RTCDevice handle), Dav1d and Opus (per decoder/encoder
# context) hand their state to the caller as an object.
if(NOT DESERT_MONOLITHIC)
    set(_desert_self "$<TARGET_PROPERTY:NAME>")
    add_compile_definitions(
        JPH_SHARED_LIBRARY                                   # Jolt.cmake adds JPH_BUILD_SHARED_LIBRARY
        SPDLOG_SHARED_LIB                                    # Spdlog.cmake: spdlog_EXPORTS
        FMT_SHARED                                           # spdlog's bundled fmt; Spdlog.cmake: FMT_LIB_EXPORT
        # GLFW itself compiles _GLFW_BUILD_DLL (GLFW.cmake); glfw3.h refuses both at once.
        "$<$<NOT:$<STREQUAL:${_desert_self},GLFW>>:GLFW_DLL>")
    if(DESERT_IS_WINDOWS)
        # ImGui and the node editor leave their API macro to the build; elsewhere default visibility exports.
        add_compile_definitions(
            "$<IF:$<STREQUAL:${_desert_self},ImGui>,IMGUI_API=__declspec(dllexport),IMGUI_API=__declspec(dllimport)>"
            "$<IF:$<STREQUAL:${_desert_self},ImGuiNodeEditor>,IMGUI_NODE_EDITOR_API=__declspec(dllexport),IMGUI_NODE_EDITOR_API=__declspec(dllimport)>"
            # The GLFW and Vulkan backends are not in the ImGui DLL: the editor compiles them (ImGuiBuild.cpp).
            "IMGUI_IMPL_API=")
    endif()
endif()

# ── LANGUAGE ───────────────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

# ── OUTPUT DIRECTORIES ───────────────────────────────────────────────────
# build/Bin/<Config> for executables AND libraries: DesertPlatform.BuiltToolPath and
# every script that starts a built tool read this path. Objects stay in the CMake binary directory
# (build/cmake/<preset>).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${DESERT_ROOT}/build/Bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${DESERT_ROOT}/build/Bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${DESERT_ROOT}/build/Bin/$<CONFIG>")
set(CMAKE_PDB_OUTPUT_DIRECTORY     "${DESERT_ROOT}/build/Bin/$<CONFIG>")

# ── FIRST-PARTY CODE UNDER ThirdParty/ ─────────────────────────────────────────────
# desert-shared is our own submodule: an ordinary include directory, never a SYSTEM one.
include_directories("${DESERT_ROOT}/ThirdParty/desert-shared/Include")

# ── WINDOWS ───────────────────────────────────────────────────
if(DESERT_IS_WINDOWS)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "DesertEngine builds x64 only on Windows; "
                            "configure from an x64 developer environment (vcvars64)")
    endif()
    # <windows.h> min/max macros break std::max and reflect-cpp.
    add_compile_definitions(NOMINMAX)
    # Unicode character set, i.e. /DUNICODE /D_UNICODE in every target,
    # except one marked desert_ascii_characterset (Embree): added per target by desert_finalize_targets().
    # /utf-8: source AND execution charset UTF-8. /bigobj: every target, C1128 is per object, not per project.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8;/bigobj>")
    # The running process is UTF-8 as well: every executable embeds this manifest (desert_add_executable).
    set(DESERT_WINDOWS_MANIFEST "${DESERT_ROOT}/BuildScripts/Windows/Utf8CodePage.manifest")
endif()

# ── macOS ──────────────────────────────────────────────────────────────────────
if(DESERT_IS_MACOS)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
    endif()
    # 14.0: Homebrew bottles (shaderc, MoltenVK) target it; libc++ gates float to_chars behind 13.3.
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "" FORCE)
    endif()
    if(DESERT_HOMEBREW_PREFIX)
        link_directories("${DESERT_HOMEBREW_PREFIX}/lib")
        # The development machine's MoltenVK driver manifest, read by VulkanContext.cpp SelectDriverManifest.
        set(_icd "${DESERT_HOMEBREW_PREFIX}/etc/vulkan/icd.d/MoltenVK_icd.json")
        if(EXISTS "${_icd}")
            add_compile_definitions("DESERT_MOLTENVK_ICD=${_icd}")
        endif()
    endif()
    if(DEFINED ENV{VULKAN_SDK})
        link_directories("$ENV{VULKAN_SDK}/lib")
    endif()
    # An archive is exactly its current object list: CMake's Ninja and Makefile rules delete the archive
    # before `ar qc`, so a deleted source cannot leave a stale object behind.
endif()

# ── WARNINGS ───────────────────────────────────────────────────────────────────────
# First-party sources: /W3 on MSVC (a measured choice: /W4 buried the real warnings), -Wall -Wextra on clang/gcc.
# Vendored sources (any path containing /ThirdParty/): warnings off. A vendored .cpp compiled INTO a first-party
# target (vk_mem_alloc.cpp, stb_image.cpp, miniaudio.cpp, ImGuizmo.cpp) moves to the OBJECT library
# <target>-ThirdParty so both levels stay target-wide. Applied by desert_finalize_targets(); no warning level is
# set at directory scope (CMP0092: CMake adds none).
if(MSVC)
    set(DESERT_WARNINGS_ON  /W3)
    set(DESERT_WARNINGS_OFF /W0)
else()
    set(DESERT_WARNINGS_ON  -Wall -Wextra)
    set(DESERT_WARNINGS_OFF -w)
endif()

# ── UNITY BUILD ───────────────────────────────────────────────────────────────────
# ON in Development and Shipping (STUDIO-AUDIT B3): a first-party library compiles as unity files of 16 sources, so
# the headers behind the sources (p90 477 per object) are parsed once per batch instead of once per source. DebugGame
# compiles each source alone (a debugger session steps through the code as written). A source that cannot share a
# batch says so where it is owned: SKIP_UNITY_BUILD_INCLUSION, with the reason, in the CMakeLists.txt of its target
# (desert_unity_exclude). A vendored source (a path under /ThirdParty/) never joins a batch. The sources this branch
# changed are compiled alone too (adaptive unity): scripts/Dev/adaptive_unity.py writes <build>/adaptive_unity.cmake
# from `git diff --name-only dev...HEAD` before the build, so an edit recompiles one object, not its batch.
if(CMAKE_BUILD_TYPE STREQUAL "DebugGame")
    set(_desert_unity_default OFF)
else()
    set(_desert_unity_default ON)
endif()
option(DESERT_UNITY "Compile Common, the engine module libraries and the editor libraries as unity files" ${_desert_unity_default})
set(DESERT_UNITY_BATCH_SIZE 16)
set(DESERT_ADAPTIVE_UNITY_FILE "${CMAKE_BINARY_DIR}/adaptive_unity.cmake")
set(DESERT_ADAPTIVE_UNITY_SKIP "")
if(EXISTS "${DESERT_ADAPTIVE_UNITY_FILE}")
    include("${DESERT_ADAPTIVE_UNITY_FILE}")
endif()
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DESERT_ADAPTIVE_UNITY_FILE}")

# Called from inside a target's script. Does nothing without DESERT_UNITY.
function(desert_enable_unity target)
    if(DESERT_UNITY)
        set_target_properties(${target} PROPERTIES
            UNITY_BUILD ON UNITY_BUILD_MODE BATCH UNITY_BUILD_BATCH_SIZE ${DESERT_UNITY_BATCH_SIZE})
        set_property(TARGET ${target} PROPERTY DESERT_UNITY_TARGET ON)
    endif()
endfunction()

# desert_unity_exclude(REASON "<why>" <source>...): the sources compile alone, never in a unity batch of their target.
# Called by the CMakeLists.txt that owns the sources; the reason is the macro or name that would leak into the batch.
function(desert_unity_exclude)
    cmake_parse_arguments(ARG "" "REASON" "" ${ARGN})
    if(NOT ARG_REASON OR NOT ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "desert_unity_exclude: REASON \"<why>\" and at least one source")
    endif()
    foreach(src IN LISTS ARG_UNPARSED_ARGUMENTS)
        cmake_path(ABSOLUTE_PATH src BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE _abs)
        if(NOT EXISTS "${_abs}")
            message(FATAL_ERROR "desert_unity_exclude: ${_abs} does not exist (${ARG_REASON})")
        endif()
        set_property(GLOBAL APPEND PROPERTY DESERT_UNITY_EXCLUDED "${_abs}")
    endforeach()
endfunction()

# ── TARGET HELPERS ─────────────────────────────────────────────────────────────────────────────────

# desert_sources(<out> <dir> [MATCHING <regex>] [RELATIVE]): the files <dir>/Sources.cmake lists, sorted. That list
# is written by scripts/Dev/sources_sync.py from git and committed; nothing in the build globs a first-party
# directory, so a build re-configures only when a Sources.cmake (or another CMake file) changed. <dir> is relative
# to the calling CMakeLists.txt unless absolute; MATCHING keeps the paths (relative to <dir>) that match; RELATIVE
# returns them relative to <dir> instead of absolute.
function(desert_sources out dir)
    cmake_parse_arguments(_s "RELATIVE" "MATCHING" "" ${ARGN})
    get_filename_component(_dir "${dir}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${_dir}/Sources.cmake")
        message(FATAL_ERROR "${_dir}/Sources.cmake is missing: run python scripts/Dev/sources_sync.py")
    endif()
    set(DESERT_SOURCES "")
    include("${_dir}/Sources.cmake")
    set(_files "")
    foreach(rel IN LISTS DESERT_SOURCES)
        if(_s_MATCHING AND NOT rel MATCHES "${_s_MATCHING}")
            continue()
        endif()
        if(_s_RELATIVE)
            list(APPEND _files "${rel}")
        else()
            list(APPEND _files "${_dir}/${rel}")
        endif()
    endforeach()
    set(${out} ${_files} PARENT_SCOPE)
endfunction()

# _desert_thirdparty_glob(<out> [RECURSE] <pattern>...): the one glob of the build, for VENDORED code under
# ThirdParty/ or Editor/ThirdParty/ only (first-party sources come from the committed Sources.cmake lists, desert_sources). No
# CONFIGURE_DEPENDS: a vendored tree changes only with a submodule bump, which re-runs configure anyway, so a no-op
# build re-checks nothing. RECURSE descends into subdirectories; without it only the named directory is listed.
function(_desert_thirdparty_glob out)
    cmake_parse_arguments(_g "RECURSE" "" "" ${ARGN})
    foreach(pattern IN LISTS _g_UNPARSED_ARGUMENTS)
        if(NOT pattern MATCHES "^${DESERT_ROOT}/([^/]+/)?ThirdParty/")
            message(FATAL_ERROR "_desert_thirdparty_glob: ${pattern} is not under ThirdParty/ (list first-party sources in Sources.cmake)")
        endif()
    endforeach()
    if(_g_RECURSE)
        file(GLOB_RECURSE _files LIST_DIRECTORIES false ${_g_UNPARSED_ARGUMENTS})
    else()
        file(GLOB _files LIST_DIRECTORIES false ${_g_UNPARSED_ARGUMENTS})
    endif()
    list(SORT _files)
    set(${out} ${_files} PARENT_SCOPE)
endfunction()

# The target-level settings five or more targets need, in one place:
#   DESERT_CONFIG_DEBUG (DebugGame) / DESERT_CONFIG_RELEASE (Development), never Shipping; DESERT_PLATFORM_<OS>.
function(desert_config_platform_defines target)
    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:DebugGame>:DESERT_CONFIG_DEBUG>
        $<$<CONFIG:Development>:DESERT_CONFIG_RELEASE>)
    if(DESERT_IS_WINDOWS)
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_WINDOWS)
    elseif(DESERT_IS_MACOS)
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_MACOS)
    else()
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_LINUX)
    endif()
endfunction()

# add_library for a first-party or vendored library: STATIC, or with PROCESS_STATE (the library keeps process-wide
# state, THIRD-PARTY PROCESS STATE above) SHARED outside Shipping and STATIC in the Shipping monolith. FOLDER groups
# it in an IDE.
function(desert_add_library name)
    cmake_parse_arguments(ARG "PROCESS_STATE" "FOLDER" "SOURCES" ${ARGN})
    if(ARG_PROCESS_STATE AND NOT DESERT_MONOLITHIC)
        add_library(${name} SHARED ${ARG_SOURCES})
    else()
        add_library(${name} STATIC ${ARG_SOURCES})
    endif()
    if(ARG_FOLDER)
        set_target_properties(${name} PROPERTIES FOLDER "${ARG_FOLDER}")
    endif()
endfunction()

# add_executable for a ConsoleApp / WindowedApp. WINDOWED selects the Windows
# subsystem; the entry stays mainCRTStartup where the target's CMakeLists.txt says so, via LINK_OPTIONS).
# Every executable gets the UTF-8 code-page manifest on Windows (BuildScripts/Windows/Utf8CodePage.manifest).
function(desert_add_executable name)
    cmake_parse_arguments(ARG "WINDOWED" "FOLDER" "SOURCES" ${ARGN})
    set(_win "")
    if(ARG_WINDOWED AND DESERT_IS_WINDOWS)
        set(_win WIN32)
    endif()
    add_executable(${name} ${_win} ${ARG_SOURCES})
    if(DESERT_IS_WINDOWS)
        target_sources(${name} PRIVATE "${DESERT_WINDOWS_MANIFEST}")
    endif()
    if(ARG_FOLDER)
        set_target_properties(${name} PROPERTIES FOLDER "${ARG_FOLDER}")
    endif()
endfunction()

# SYSTEM include directories (MSVC /external:I + /external:W0).
# The directories are also recorded in DESERT_SYSTEM_INCLUDES: CMake keeps a target's SYSTEM set internally (the
# SYSTEM_INCLUDE_DIRECTORIES property reads back empty), and desert_finalize_targets() needs it to order the list and
# to give a split OBJECT library the same SYSTEM directories.
function(desert_external_includes target)
    target_include_directories(${target} SYSTEM PRIVATE ${ARGN})
    foreach(dir IN LISTS ARGN)
        if(NOT dir MATCHES "^\\$<")
            cmake_path(ABSOLUTE_PATH dir BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE dir)
            string(REGEX REPLACE "/+$" "" dir "${dir}")
        endif()
        set_property(TARGET ${target} APPEND PROPERTY DESERT_SYSTEM_INCLUDES "${dir}")
    endforeach()
endfunction()

# A command that runs before the target's sources compile (the header tool, GenVersion), and only when one of its
# DEPENDS changed: the step touches a stamp after the command succeeds, so a build that changed nothing runs
# nothing (`ninja -d explain` is empty). DEPENDS must name every file the command reads (the header tool's inputs
# come from the Sources.cmake lists); BYPRODUCTS names the files it writes (rewritten only when their text changes,
# so ninja's restat keeps their readers up to date).
function(desert_prebuild target name)
    cmake_parse_arguments(ARG "" "COMMENT" "COMMAND;DEPENDS;BYPRODUCTS" ${ARGN})
    if(NOT ARG_DEPENDS)
        message(FATAL_ERROR "desert_prebuild(${target} ${name}): DEPENDS names no input, the step would never re-run")
    endif()
    set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}.${name}.prebuild")
    add_custom_command(
        OUTPUT "${_stamp}"
        BYPRODUCTS ${ARG_BYPRODUCTS}
        COMMAND ${ARG_COMMAND}
        COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
        DEPENDS ${ARG_DEPENDS}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "${ARG_COMMENT}"
        VERBATIM)
    # The step is its own target (<target>-<name>) that <target> depends on, so a tool that needs the generated
    # sources and nothing else can build them: DesertPrebuild (desert_finalize_targets) depends on every step.
    # One target owns the output; <target> only orders after it, so the command never runs twice in parallel.
    add_custom_target(${target}-${name} DEPENDS "${_stamp}")
    add_dependencies(${target} ${target}-${name})
    set_property(GLOBAL APPEND PROPERTY DESERT_PREBUILD_TARGETS ${target}-${name})
endfunction()

# Collect every buildsystem target below a directory.
function(_desert_collect_targets dir out)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS _subdirs)
        _desert_collect_targets("${sub}" _sub)
        list(APPEND _targets ${_sub})
    endforeach()
    set(${out} ${_targets} PARENT_SCOPE)
endfunction()

# Compile flags applied ONCE PER TARGET so the
# target's PCH (cmake_pch.cxx) and its unity files compile exactly like its sources, and no source carries a
# per-file flag that conflicts with a target flag: cl answers /Od+/O2 and /W3+/W0 with D9025 and /RTC1+/O2 with
# the error D8016, and CMake never puts a source with its own COMPILE_OPTIONS into a unity file. Three choices:
#   warnings   ON: /W3 (-Wall -Wextra). OFF: /W0 (-w), for a target marked
#              (desert_warnings_off) and for a target whose every source is vendored (a path under /ThirdParty/).
#   speed      desert_optimize_speed: /O2 /Oi /Gy /GF in EVERY configuration and
#              no DebugGame runtime checks (clang -O3).
#   debug_opt  speed with no runtime checks, DebugGame only, on single files
#              (DESERT_DEBUG_OPTIMIZE, the mesh card/distance field builders): DebugGame /O2 (clang -O2).
# Otherwise: DebugGame /Od /RTC1 (no optimisation, fast runtime checks),
# Development and Shipping /Ox /Oi /Gy /GF (full optimisation, intrinsics,
# function-level linking, string pooling). clang's per-configuration level stays in CMAKE_<LANG>_FLAGS_<CONFIG>.
function(_desert_target_flags target warnings speed debug_opt)
    set(_cc "$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>")
    if(warnings)
        target_compile_options(${target} PRIVATE "$<${_cc}:${DESERT_WARNINGS_ON}>")
    else()
        target_compile_options(${target} PRIVATE "$<${_cc}:${DESERT_WARNINGS_OFF}>")
    endif()
    if(MSVC)
        if(speed)
            target_compile_options(${target} PRIVATE "$<${_cc}:/O2;/Oi;/Gy;/GF>")
        else()
            if(debug_opt)
                set(_debug /O2)
            else()
                set(_debug /Od /RTC1)
            endif()
            target_compile_options(${target} PRIVATE
                "$<$<AND:$<CONFIG:DebugGame>,${_cc}>:${_debug}>"
                "$<$<AND:$<NOT:$<CONFIG:DebugGame>>,${_cc}>:/Ox;/Oi;/Gy;/GF>")
        endif()
    elseif(speed)
        target_compile_options(${target} PRIVATE "$<${_cc}:-O3>")
    elseif(debug_opt)
        target_compile_options(${target} PRIVATE "$<$<AND:$<CONFIG:DebugGame>,${_cc}>:-O2>")
    endif()
    # Shipping, a desert_ltcg target: whole-program code generation (/GL), global data in COMDATs the linker can
    # fold and drop (/Gw), the aggressive inliner (/Ob3, which pays across modules only with /GL). Its archive is
    # written by lib /LTCG, which otherwise restarts itself on the first /GL object. No other configuration pays for it.
    # DESERT_AVX2: AVX2 (/arch:AVX2, -march=x86-64-v3 -ffp-contract=off), every configuration, but not on a target
    # with its own ISA dispatch.
    get_target_property(_own_isa ${target} DESERT_OWN_ISA_DISPATCH)
    if(DESERT_AVX2 AND NOT _own_isa)
        if(MSVC)
            target_compile_options(${target} PRIVATE "$<${_cc}:/arch:AVX2>")
        else()
            target_compile_options(${target} PRIVATE "$<${_cc}:-march=x86-64-v3;-ffp-contract=off>")
        endif()
    endif()
    get_target_property(_whole ${target} DESERT_WHOLE_PROGRAM)
    if(MSVC AND _whole AND CMAKE_BUILD_TYPE STREQUAL "Shipping")
        target_compile_options(${target} PRIVATE "$<${_cc}:/Gw;/Ob3;/GL>")
        get_target_property(_type ${target} TYPE)
        if(_type STREQUAL "STATIC_LIBRARY")
            set_property(TARGET ${target} APPEND PROPERTY STATIC_LIBRARY_OPTIONS /LTCG)
        endif()
    endif()
endfunction()

# A shared precompiled header (UE's shared PCHs): every target of <group> compiles with the same flags, include
# directories and definitions, so <header> is precompiled once. The first member declared is the donor: it holds the
# PCH in <donor>-Pch and every other member reuses it (desert_finalize_targets). A target whose flags differ must not
# join: MSVC refuses an inconsistent option (C2855) and clang a macro that differs from the PCH's.
function(desert_shared_pch target group header)
    get_property(_hdr GLOBAL PROPERTY DESERT_PCH_GROUP_${group}_HEADER)
    if(_hdr AND NOT _hdr STREQUAL header)
        message(FATAL_ERROR "${target}: PCH group '${group}' precompiles ${_hdr}, not ${header}")
    endif()
    set_property(GLOBAL PROPERTY DESERT_PCH_GROUP_${group}_HEADER "${header}")
    set_property(GLOBAL APPEND PROPERTY DESERT_PCH_GROUP_${group} ${target})
    set_property(TARGET ${target} PROPERTY DESERT_PCH_GROUP ${group})
    target_precompile_headers(${target} PRIVATE "${header}")
endfunction()

# Moves <sources> (absolute paths) of <owner> into the OBJECT library <owner>-<suffix>: compiled with the owner's
# include directories (in declaration order), definitions, options and usage requirements (the same link
# libraries), and archived/linked into the owner through $<TARGET_OBJECTS>, so the owner's binary holds the same
# objects as if nothing were split. The split target gets its OWN target-level flags (_desert_target_flags): this is
# how a per-file setting (a vendored file's warnings off, a Debug-optimised builder) is expressed without
# a conflicting per-source flag. <pch_holder> (may be empty): the target whose PCH the split target reuses.
function(_desert_split_objects owner suffix pch_holder out)
    set(_obj "${owner}-${suffix}")
    get_target_property(_dir ${owner} SOURCE_DIR)
    get_target_property(_srcs ${owner} SOURCES)
    set(_keep "")
    foreach(src IN LISTS _srcs)
        if(NOT src MATCHES "^\\$<")
            cmake_path(ABSOLUTE_PATH src BASE_DIRECTORY "${_dir}" OUTPUT_VARIABLE src)
            if(src IN_LIST ARGN)
                continue()
            endif()
        endif()
        list(APPEND _keep "${src}")
    endforeach()
    set_property(TARGET ${owner} PROPERTY SOURCES ${_keep})

    add_library(${_obj} OBJECT ${ARGN})
    # The owner's whole-program mark (desert_ltcg): its split parts, the PCH holder included, compile like it.
    get_target_property(_whole ${owner} DESERT_WHOLE_PROGRAM)
    if(_whole)
        set_property(TARGET ${_obj} PROPERTY DESERT_WHOLE_PROGRAM ON)
    endif()
    # The owner's instruction set (desert_own_isa_dispatch): its split parts keep the same flags.
    get_target_property(_own_isa ${owner} DESERT_OWN_ISA_DISPATCH)
    if(_own_isa)
        set_property(TARGET ${_obj} PROPERTY DESERT_OWN_ISA_DISPATCH ON)
    endif()
    # SYSTEM directories through target_include_directories(SYSTEM), from the owner's DESERT_SYSTEM_INCLUDES
    # (desert_external_includes). The owner's list is already ordinary-then-SYSTEM, so the order is kept.
    get_target_property(_incs ${owner} INCLUDE_DIRECTORIES)
    get_target_property(_sys ${owner} DESERT_SYSTEM_INCLUDES)
    if(NOT _incs)
        set(_incs "")
    endif()
    set(_plain "")
    set(_system "")
    foreach(dir IN LISTS _incs)
        if(_sys AND dir IN_LIST _sys)
            list(APPEND _system "${dir}")
        else()
            list(APPEND _plain "${dir}")
        endif()
    endforeach()
    set_property(TARGET ${_obj} PROPERTY INCLUDE_DIRECTORIES ${_plain})
    if(_system)
        target_include_directories(${_obj} SYSTEM PRIVATE ${_system})
    endif()
    foreach(prop COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES
                 C_STANDARD C_STANDARD_REQUIRED C_EXTENSIONS CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS
                 OBJCXX_STANDARD OBJCXX_STANDARD_REQUIRED OBJCXX_EXTENSIONS MSVC_RUNTIME_LIBRARY
                 MSVC_DEBUG_INFORMATION_FORMAT POSITION_INDEPENDENT_CODE C_COMPILER_LAUNCHER CXX_COMPILER_LAUNCHER
                 OBJCXX_COMPILER_LAUNCHER FOLDER)
        get_property(_v TARGET ${owner} PROPERTY ${prop})
        set_property(TARGET ${_obj} PROPERTY ${prop} "${_v}")
    endforeach()
    # Usage requirements: the owner's link libraries, by name (an object library links nothing; the directory
    # markers `::@` of a cross-directory target_link_libraries() and a $<LINK_LIBRARY:...> wrapper are link-only).
    get_target_property(_libs ${owner} LINK_LIBRARIES)
    if(_libs)
        set(_names "")
        foreach(lib IN LISTS _libs)
            if(lib MATCHES "^::@")
                continue()
            endif()
            string(REGEX REPLACE "^\\$<LINK_LIBRARY:[A-Z_]+,(.+)>$" "\\1" lib "${lib}")
            list(APPEND _names "${lib}")
        endforeach()
        target_link_libraries(${_obj} PRIVATE ${_names})
    endif()
    # Ordering the owner's compile waits for (the header tool's codegen).
    get_target_property(_deps ${owner} MANUALLY_ADDED_DEPENDENCIES)
    if(_deps)
        add_dependencies(${_obj} ${_deps})
    endif()
    if(pch_holder)
        set_property(TARGET ${_obj} PROPERTY PRECOMPILE_HEADERS_REUSE_FROM ${pch_holder})
    endif()
    target_sources(${owner} PRIVATE "$<TARGET_OBJECTS:${_obj}>")
    set(${out} ${_obj} PARENT_SCOPE)
endfunction()

# Runs once, after every target is declared: the per-target rules plus the
# per-file rules (vendored files, unity exclusions, DebugGame-optimised builders).
function(desert_finalize_targets)
    _desert_collect_targets("${CMAKE_SOURCE_DIR}" _targets)
    get_property(_unity_excluded GLOBAL PROPERTY DESERT_UNITY_EXCLUDED)
    # The adaptive list is relative to the repository root (git diff --name-only).
    set(_adaptive "")
    foreach(rel IN LISTS DESERT_ADAPTIVE_UNITY_SKIP)
        list(APPEND _adaptive "${CMAKE_SOURCE_DIR}/${rel}")
    endforeach()
    foreach(target IN LISTS _targets)
        get_target_property(_type ${target} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
            continue()
        endif()
        # An executable links alone (desert_link_exe, see THE LINK POOLS).
        if(MSVC AND _type STREQUAL "EXECUTABLE")
            set_property(TARGET ${target} PROPERTY JOB_POOL_LINK desert_link_exe)
        endif()
        get_target_property(_ascii ${target} DESERT_ASCII)
        if(DESERT_IS_WINDOWS AND NOT _ascii)
            target_compile_definitions(${target} PRIVATE UNICODE _UNICODE)
        endif()
        # A repeated directory keeps its LAST position (CMake keeps the first), which the include order relies on: rebuild the
        # include list that way, ordinary directories (/I) before SYSTEM ones (/external:I), as MSBuild orders them.
        get_target_property(_incs ${target} INCLUDE_DIRECTORIES)
        get_target_property(_sys ${target} DESERT_SYSTEM_INCLUDES)
        if(_incs)
            if(NOT _sys)
                set(_sys "")
            endif()
            list(TRANSFORM _sys REPLACE "/+$" "")
            set(_plain "")
            set(_system "")
            list(REVERSE _incs)
            foreach(dir IN LISTS _incs)
                # "Source/" and "Source" are one directory: compare without the trailing slash.
                if(NOT dir MATCHES "^\\$<")
                    string(REGEX REPLACE "/+$" "" dir "${dir}")
                endif()
                if(dir IN_LIST _sys)
                    if(NOT dir IN_LIST _system)
                        list(PREPEND _system "${dir}")
                    endif()
                elseif(NOT dir IN_LIST _plain)
                    list(PREPEND _plain "${dir}")
                endif()
            endforeach()
            set_property(TARGET ${target} PROPERTY INCLUDE_DIRECTORIES ${_plain} ${_system})
        endif()

        get_target_property(_sources ${target} SOURCES)
        get_target_property(_srcdir ${target} SOURCE_DIR)
        get_target_property(_unity ${target} DESERT_UNITY_TARGET)
        get_target_property(_off ${target} DESERT_WARNINGS_OFF)
        get_target_property(_speed ${target} DESERT_OPTIMIZE_SPEED)
        set(_ours "")
        set(_vendored "")
        set(_debug_opt "")
        set(_no_unity "")
        foreach(src IN LISTS _sources)
            if(NOT src MATCHES "\\.(c|cc|cpp|cxx|mm|m)$")
                continue()
            endif()
            # Absolute: a relative SOURCE path given to set_property() here resolves against the root directory
            # (this call is deferred there), not the target's, and silently names a different file.
            cmake_path(ABSOLUTE_PATH src BASE_DIRECTORY "${_srcdir}" OUTPUT_VARIABLE _abs)
            get_property(_dopt SOURCE "${_abs}" TARGET_DIRECTORY ${target} PROPERTY DESERT_DEBUG_OPTIMIZE)
            if(_dopt)
                list(APPEND _debug_opt "${_abs}")
            elseif(_abs MATCHES "/ThirdParty/" AND NOT _off)
                list(APPEND _vendored "${_abs}")
            else()
                list(APPEND _ours "${_abs}")
                # Vendored sources compiled with ours never share a batch; desert_unity_exclude marks the rest.
                if(_unity AND (_abs MATCHES "/ThirdParty/" OR _abs IN_LIST _unity_excluded OR _abs IN_LIST _adaptive))
                    list(APPEND _no_unity "${_abs}")
                endif()
            endif()
        endforeach()
        if(_speed AND _debug_opt)
            message(FATAL_ERROR "${target}: DESERT_DEBUG_OPTIMIZE sources in a desert_optimize_speed target")
        endif()
        # Warnings off for the whole target: desert_warnings_off, or nothing of ours in it (a vendored library).
        set(_warnings ON)
        if(_off OR NOT _ours)
            set(_warnings OFF)
            list(APPEND _ours ${_vendored})
            set(_vendored "")
        endif()

        # A target that splits sources off and has a PCH: the PCH moves to <target>-Pch with the target's pchsource
        # (the pchsource, the source named like the header), and the target and its split parts all reuse
        # it -- the split parts cannot reuse the target's own PCH, the target links their objects (a cycle).
        set(_holder "")
        set(_pch_abs "")
        set(_pch_src "")
        get_target_property(_pch ${target} PRECOMPILE_HEADERS)
        if(_pch)
            foreach(hdr IN LISTS _pch)
                if(NOT hdr MATCHES "^(\\$<|<)")
                    cmake_path(ABSOLUTE_PATH hdr BASE_DIRECTORY "${_srcdir}" OUTPUT_VARIABLE hdr)
                    cmake_path(REMOVE_EXTENSION hdr LAST_ONLY OUTPUT_VARIABLE _stem)
                    foreach(ext cpp cc cxx c)
                        if("${_stem}.${ext}" IN_LIST _ours)
                            list(APPEND _pch_src "${_stem}.${ext}")
                        endif()
                    endforeach()
                endif()
                list(APPEND _pch_abs "${hdr}")
            endforeach()
            # The pchsource stays its own translation unit in every target (compiled with /Yc; in a target that
            # splits sources off it moves to <target>-Pch, which is never a unity target).
            if(_unity AND _pch_src)
                list(APPEND _no_unity ${_pch_src})
            endif()
        endif()
        # A shared PCH group (desert_shared_pch): the donor (the group's first member) always holds the PCH in
        # <donor>-Pch; every other member drops its own pchsource and reuses that holder, its split parts too.
        set(_shared_donor OFF)
        get_target_property(_group ${target} DESERT_PCH_GROUP)
        if(_pch AND _group)
            get_property(_members GLOBAL PROPERTY DESERT_PCH_GROUP_${_group})
            list(GET _members 0 _donor)
            if(target STREQUAL _donor)
                set(_shared_donor ON)
            else()
                if(_pch_src)
                    list(REMOVE_ITEM _ours ${_pch_src})
                    list(REMOVE_ITEM _no_unity ${_pch_src})
                    set(_keep "")
                    foreach(src IN LISTS _sources)
                        if(NOT src MATCHES "^\\$<")
                            cmake_path(ABSOLUTE_PATH src BASE_DIRECTORY "${_srcdir}" OUTPUT_VARIABLE src)
                            if(src IN_LIST _pch_src)
                                continue()
                            endif()
                        endif()
                        list(APPEND _keep "${src}")
                    endforeach()
                    set_property(TARGET ${target} PROPERTY SOURCES ${_keep})
                endif()
                set(_holder "${_donor}-Pch")
                set_property(TARGET ${target} PROPERTY PRECOMPILE_HEADERS)
                set_property(TARGET ${target} PROPERTY PRECOMPILE_HEADERS_REUSE_FROM ${_holder})
                set(_pch "")
                # A member that links its own binary (a module DLL, an executable over DLLs) carries the PCH object:
                # every object compiled /Yu references its symbol (LNK2011 without it). In the static monolith
                # the donor's archive brings it into the one binary.
                if(NOT DESERT_MONOLITHIC AND NOT _type STREQUAL "STATIC_LIBRARY" AND NOT _type STREQUAL "OBJECT_LIBRARY")
                    target_sources(${target} PRIVATE "$<TARGET_OBJECTS:${_holder}>")
                endif()
            endif()
        endif()
        if(_pch AND (_vendored OR _debug_opt OR _shared_donor))
            list(LENGTH _pch_src _n)
            if(NOT _n EQUAL 1)
                message(FATAL_ERROR "${target}: moves its PCH to a holder but has ${_n} PCH sources named like its "
                                    "precompiled header (${_pch}); the pchsource must be one of its sources")
            endif()
            list(REMOVE_ITEM _ours ${_pch_src})
            _desert_split_objects(${target} Pch "" _holder ${_pch_src})
            set_property(TARGET ${_holder} PROPERTY PRECOMPILE_HEADERS ${_pch_abs})
            # REMOVED, not set to "": an empty value still counts as set, and CMake refuses PRECOMPILE_HEADERS_REUSE_FROM
            # on a target whose PRECOMPILE_HEADERS is set (run 40953089350, every split engine/editor library).
            set_property(TARGET ${target} PROPERTY PRECOMPILE_HEADERS)
            set_property(TARGET ${target} PROPERTY PRECOMPILE_HEADERS_REUSE_FROM ${_holder})
            _desert_target_flags(${_holder} ${_warnings} "${_speed}" OFF)
        endif()
        # Vendored single files compiled into a first-party target (stb_image, vk_mem_alloc, ImGuizmo, ...):
        # warnings off for them only.
        if(_vendored)
            _desert_split_objects(${target} ThirdParty "${_holder}" _split ${_vendored})
            _desert_target_flags(${_split} OFF "${_speed}" OFF)
        endif()
        # Sources optimised in DebugGame. MSVC: DebugGame compiles them without the PCH but with pch.hpp still force-included, i.e. the
        # reused PCH ignored (/Y-) with cmake_pch.hxx still force-included; Development/Shipping use the PCH. clang has
        # no per-configuration PCH switch: the header is force-included in every configuration instead.
        if(_debug_opt)
            if(MSVC)
                _desert_split_objects(${target} DebugOptimized "${_holder}" _split ${_debug_opt})
                target_compile_options(${_split} PRIVATE "$<$<AND:$<CONFIG:DebugGame>,$<COMPILE_LANGUAGE:C,CXX>>:/Y->")
            else()
                _desert_split_objects(${target} DebugOptimized "" _split ${_debug_opt})
                foreach(hdr IN LISTS _pch_abs)
                    target_compile_options(${_split} PRIVATE "SHELL:-include \"${hdr}\"")
                endforeach()
            endif()
            _desert_target_flags(${_split} ON OFF ON)
        endif()
        _desert_target_flags(${target} ${_warnings} "${_speed}" OFF)
        if(_no_unity)
            set_property(SOURCE ${_no_unity} TARGET_DIRECTORY ${target} PROPERTY SKIP_UNITY_BUILD_INCLUSION ON)
        endif()
    endforeach()
    # The sources this host leaves out of every target (desert_platform_foreign_sources), one absolute path per line.
    # Tools that walk a change against compile_commands.json (scripts/CI/CheckTidy.sh) read this list to tell
    # "compiled only on another platform" from "compiled nowhere" without re-deriving the platform split.
    # DesertPrebuild: every generated source the compile lines expect (version header, reflection codegen),
    # without compiling anything else. scripts/CI/CheckTidy.sh builds it before analysing a configured tree.
    get_property(_prebuild GLOBAL PROPERTY DESERT_PREBUILD_TARGETS)
    add_custom_target(DesertPrebuild)
    if(_prebuild)
        add_dependencies(DesertPrebuild ${_prebuild})
    endif()

    get_property(_foreign GLOBAL PROPERTY DESERT_PLATFORM_FOREIGN_SOURCES)
    list(REMOVE_DUPLICATES _foreign)
    list(SORT _foreign)
    list(JOIN _foreign "\n" _foreign_text)
    file(WRITE "${CMAKE_BINARY_DIR}/PlatformForeignSources.txt" "${_foreign_text}\n")
endfunction()

# Records sources that belong to a platform other than this host's and are therefore in no target here.
# desert_finalize_targets() writes the list to <build>/PlatformForeignSources.txt.
function(desert_platform_foreign_sources)
    foreach(src IN LISTS ARGN)
        get_filename_component(_abs "${src}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        set_property(GLOBAL APPEND PROPERTY DESERT_PLATFORM_FOREIGN_SOURCES "${_abs}")
    endforeach()
endfunction()

# ASCII character set: defines neither UNICODE/_UNICODE
# nor _MBCS (only MultiByte would add _MBCS).
function(desert_ascii_characterset target)
    set_property(TARGET ${target} PROPERTY DESERT_ASCII ON)
endfunction()

# A vendored target optimised in EVERY configuration (MSVC /O2 /Oi /Gy /GF, clang -O3)
# and without the DebugGame runtime checks /O2 cannot be combined with (no /RTC1). Applied per target by
# desert_finalize_targets().
function(desert_optimize_speed target)
    set_property(TARGET ${target} PROPERTY DESERT_OPTIMIZE_SPEED ON)
endfunction()

# A runtime library compiled for whole-program optimisation in Shipping (/GL /Gw /Ob3, archived by lib /LTCG).
# Applied per target, with its split parts, by desert_finalize_targets().
function(desert_ltcg target)
    set_property(TARGET ${target} PROPERTY DESERT_WHOLE_PROGRAM ON)
endfunction()

# A vendored library that picks its instruction set itself (a cpuid dispatch, an ISA-specific build): no
# /arch:AVX2 or -march=x86-64-v3 under DESERT_AVX2. Applied per target, with its split parts, by
# desert_finalize_targets().
function(desert_own_isa_dispatch target)
    set_property(TARGET ${target} PROPERTY DESERT_OWN_ISA_DISPATCH ON)
endfunction()

# DESERT_MIMALLOC: mimalloc.dll as the process allocator of the executable <target>, Development and Shipping
# (cmake/ThirdParty/Mimalloc.cmake). It must be the executable's FIRST import, so the redirect is in place before
# any other module allocates: its import library goes into LINK_OPTIONS, which the MSVC link rule writes before
# every link library, and /INCLUDE:mi_version keeps the import although no source calls mimalloc. Nothing includes
# mimalloc.h: Common::Utils::ReadProcessAllocator asks the running process which heap it has.
function(desert_link_mimalloc target)
    if(NOT DESERT_MIMALLOC)
        return()
    endif()
    set(_on "$<NOT:$<CONFIG:DebugGame>>")
    target_link_options(${target} PRIVATE "$<${_on}:$<TARGET_LINKER_FILE:Mimalloc>;/INCLUDE:mi_version>")
    # The build order and the relink when mimalloc.lib changes; the same import library again in the link list.
    target_link_libraries(${target} PRIVATE "$<${_on}:Mimalloc>")
endfunction()

# Warnings off for a whole target: /W0 (-w) for every source of the target, including the ones outside
# ThirdParty/ (Dav1d's generated bitdepth instances). Applied per target by desert_finalize_targets().
function(desert_warnings_off target)
    set_property(TARGET ${target} PROPERTY DESERT_WARNINGS_OFF ON)
endfunction()

# DesertPlatform.WriteGenerated: writes a file the compile needs (a third-party config.h, a template
# instantiation) at configure time, and ONLY when its text changed, so regenerating does not restamp it and
# recompile everything that includes it.
function(desert_write_generated path text)
    if(EXISTS "${path}")
        file(READ "${path}" _old)
        if(_old STREQUAL text)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${text}")
endfunction()

# Only DESERT_PLATFORM_<OS>, for the tools that need the platform and no configuration.
function(desert_platform_defines target)
    if(DESERT_IS_WINDOWS)
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_WINDOWS)
    elseif(DESERT_IS_MACOS)
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_MACOS)
    else()
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_LINUX)
    endif()
endfunction()

# The target is absent from that configuration's build tree (EXCLUDE_FROM_ALL in the tree whose CMAKE_BUILD_TYPE is
# <config>); the target stays declared, so a build that names it explicitly still builds it.
function(desert_remove_configuration target config)
    set_property(TARGET ${target} PROPERTY EXCLUDE_FROM_ALL "$<CONFIG:${config}>")
endfunction()

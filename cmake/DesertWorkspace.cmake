# Workspace-wide settings shared by every target: configurations, compile flags, output directories,
# platform settings, the unity switch and the target helpers every CMakeLists.txt declares through.
#
# Everything set here at directory scope reaches every target below the root, first-party and vendored
# alike. The warning level, optimisation and runtime checks are set
# per TARGET, and the per-file rules (warnings off for vendored files, the Debug-optimised
# builders, the unity opt-out list) become per-target settings of split OBJECT libraries; both are applied once,
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
# Debug, Release and Shipping. Shipping is the player's artifact: DESERT_CONFIG_SHIPPING is what the
# sources test to leave the development instruments out, so it is a configuration and not a Release flag.
# Flags are spelled out explicitly instead of inheriting CMake's per-config defaults, so a CMake upgrade
# cannot change how a configuration compiles:
#   Debug     runtime Debug,  symbols On (/Zi or /Z7, never /ZI), no optimisation (MSVC /Od /RTC1 per target)
#   Release   runtime Release, optimize On  (MSVC /Ox /Oi /Gy /GF per target, clang -O2), NDEBUG
#   Shipping  runtime Release, optimize Full (MSVC /Ox, clang -O3), symbols Off, NDEBUG + DESERT_CONFIG_SHIPPING
set(DESERT_CONFIGURATIONS Debug Release Shipping)
get_property(_desert_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_desert_multi_config)
    set(CMAKE_CONFIGURATION_TYPES ${DESERT_CONFIGURATIONS} CACHE STRING "" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

foreach(lang C CXX)
    if(MSVC)
        # The optimisation level and /RTC1 are per TARGET (desert_finalize_targets, _desert_target_flags): a target
        # optimised for speed must not also get /Od or /Ox from here (cl D9025). No /DWIN32 /D_WINDOWS:
        # no source tests them.
        set(CMAKE_${lang}_FLAGS_DEBUG    "" CACHE STRING "" FORCE)
        set(CMAKE_${lang}_FLAGS_RELEASE  "/DNDEBUG" CACHE STRING "" FORCE)
        set(CMAKE_${lang}_FLAGS_SHIPPING "/DNDEBUG" CACHE STRING "" FORCE)
    else()
        set(CMAKE_${lang}_FLAGS_DEBUG    "-O0 -g" CACHE STRING "" FORCE)
        set(CMAKE_${lang}_FLAGS_RELEASE  "-O2 -DNDEBUG" CACHE STRING "" FORCE)
        set(CMAKE_${lang}_FLAGS_SHIPPING "-O3 -DNDEBUG" CACHE STRING "" FORCE)
    endif()
endforeach()
if(APPLE)
    set(CMAKE_OBJCXX_FLAGS_DEBUG    "${CMAKE_CXX_FLAGS_DEBUG}" CACHE STRING "" FORCE)
    set(CMAKE_OBJCXX_FLAGS_RELEASE  "${CMAKE_CXX_FLAGS_RELEASE}" CACHE STRING "" FORCE)
    set(CMAKE_OBJCXX_FLAGS_SHIPPING "${CMAKE_CXX_FLAGS_SHIPPING}" CACHE STRING "" FORCE)
endif()
foreach(kind EXE SHARED MODULE STATIC)
    set(CMAKE_${kind}_LINKER_FLAGS_SHIPPING "${CMAKE_${kind}_LINKER_FLAGS_RELEASE}" CACHE STRING "" FORCE)
endforeach()
if(MSVC)
    # MSBuild's ClCompile defaults that differ from cl's own: ExceptionHandling Sync (/EHsc, passed to .c too),
    # RemoveUnreferencedCodeData (/Zc:inline), UseFullPaths (/FC). SupportJustMyCode (/JMC) under UseDebugLibraries
    # is added per configuration below.
    set(CMAKE_C_FLAGS "/EHsc /Zc:inline /FC" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS "/EHsc /Zc:inline /FC" CACHE STRING "" FORCE)
    # Debug and Release write a .pdb (DesertCrashReporter
    # symbolises with it); Shipping writes none. Optimised links fold and drop (/OPT:REF,ICF).
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG    "/DEBUG /INCREMENTAL" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE  "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS_SHIPPING "/INCREMENTAL:NO /OPT:REF /OPT:ICF" CACHE STRING "" FORCE)
endif()

# MSBuild's SupportJustMyCode default under UseDebugLibraries (Debug): /JMC.
if(MSVC)
    add_compile_options("$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C,CXX>>:/JMC>")
endif()
# MSBuild never scans for C++20 module dependencies (ScanSourceForModuleDependencies false); a scanned source
# also never joins a unity batch.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# /MDd for Debug, /MD for Release and Shipping (dynamic CRT, one runtime across every target).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Debug information: Debug and Release never Shipping. /Zi and not /ZI (edit-and-continue defeats a cache, BUILD1 P3). With a compiler
# launcher (sccache/ccache) the information is embedded (/Z7):
# a cache cannot share the one vc143.pdb every compile of a project writes into.
if(DESERT_COMPILER_LAUNCHER)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<NOT:$<CONFIG:Shipping>>:Embedded>")
else()
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<NOT:$<CONFIG:Shipping>>:ProgramDatabase>")
endif()

add_compile_definitions(
    $<$<CONFIG:Shipping>:DESERT_CONFIG_SHIPPING>
    # EnTT's type-index counter must be atomic in every target or none.
    ENTT_USE_ATOMIC
)

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
# Off by default: a developer build must compile the code as written.
# CMake's unity build works with every generator, so the switch is not
# refused for Ninja.
option(DESERT_UNITY "Compile Common, the engine module libraries and the editor libraries as unity files (CI speed)" OFF)
set(DESERT_UNITY_BATCH_SIZE 12)
set(DESERT_UNITY_OPT_OUT
    "ThirdParty/"
    "Source/Common/Utilities/ContentManifest.cpp"
    "Source/Engine/Animation/Rig/ControlShape.cpp"
    "Source/Engine/Assets/CloudNoiseVolume.cpp"
    "Source/Engine/Assets/CloudProceduralVolume.cpp"
    "Source/Engine/Assets/CloudNoiseVolumeSheet.cpp"
    "Source/Engine/Assets/Serialization/TextureBinary.cpp"
    "Source/Engine/Graphic/Systems/Scene/Fog/HeightFogRenderer.cpp"
    "Source/Engine/Graphic/Systems/Scene/PostProcessing/AutoExposureRenderer.cpp"
    "Source/Engine/Graphic/Systems/Scene/PostProcessing/BloomRenderer.cpp"
    "Source/Engine/Graphic/Systems/Scene/PostProcessing/LensFlareRenderer.cpp"
    "Source/Engine/Graphic/Systems/Scene/PostProcessing/LightShaftRenderer.cpp"
    "Source/UI/UICanvasRenderer2D.cpp"
    "Source/Engine/World/Landscape/LandscapeEditCache.cpp"
    "Source/Editor/Panels/Clouds/CloudLayoutPanel.cpp"
    "Source/Editor/Panels/Clouds/CloudNoiseVolumePanel.cpp"
    "Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp"
    "Source/Editor/Panels/Photogrammetry/PhotogrammetryPanel.cpp"
    "Source/Editor/Panels/SceneHierarchy/SceneHierarchyPanel.cpp"
    "Source/Editor/Panels/SceneProperties/ComponentEditor.cpp"
    "Source/Editor/Panels/SceneSettings/SceneSettingsPanel.cpp"
    "Source/Editor/Panels/ViewportPanel/Tools/ElementSelectTool.cpp"
    "Source/Editor/Panels/ViewportPanel/Tools/LandscapeSculptTool.cpp"
    "Source/Editor/Panels/WorldPartition/WorldPartitionMap.cpp"
    "Source/Editor/RenderSystems/Passes/EditorUIPass.cpp"
    "Source/Editor/Widgets/PreviewViewport.cpp"
    "Source/Editor/Widgets/ThumbnailCache.cpp"
    "Source/Editor/Widgets/ThumbnailSubject.cpp"
    "Source/EditorLayer.cpp"
    "Source/Sandbox.cpp"
)

# Called from inside a target's script, as DesertUnity.EnableForProject() was. Does nothing without DESERT_UNITY.
function(desert_enable_unity target)
    if(DESERT_UNITY)
        set_target_properties(${target} PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE ${DESERT_UNITY_BATCH_SIZE})
        set_property(TARGET ${target} PROPERTY DESERT_UNITY_TARGET ON)
    endif()
endfunction()

# ── TARGET HELPERS ─────────────────────────────────────────────────────────────────────────────────

# Recursive glob, re-run on every build so a new file is picked up without a manual re-configure.
# Patterns are relative to the calling CMakeLists.txt's directory unless absolute.
function(desert_glob out)
    file(GLOB_RECURSE _files LIST_DIRECTORIES false CONFIGURE_DEPENDS ${ARGN})
    list(SORT _files)
    set(${out} ${_files} PARENT_SCOPE)
endfunction()

# The target-level settings five or more targets need, in one place:
#   DESERT_CONFIG_DEBUG / DESERT_CONFIG_RELEASE (never Shipping), DESERT_PLATFORM_<OS>.
function(desert_config_platform_defines target)
    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:Debug>:DESERT_CONFIG_DEBUG>
        $<$<CONFIG:Release>:DESERT_CONFIG_RELEASE>)
    if(DESERT_IS_WINDOWS)
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_WINDOWS)
    elseif(DESERT_IS_MACOS)
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_MACOS)
    else()
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_LINUX)
    endif()
endfunction()

# add_library(STATIC) for a first-party or vendored static library. FOLDER groups it in an IDE.
function(desert_add_library name)
    cmake_parse_arguments(ARG "" "FOLDER" "SOURCES" ${ARGN})
    add_library(${name} STATIC ${ARG_SOURCES})
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

# DesertPlatform.LinkWholeArchive: every object of the static library reaches the binary, so a file whose
# one job is a static initialiser (a reflected component's self-registration) is not dropped by the linker.
# CMake spells -force_load / --whole-archive / /WHOLEARCHIVE as one generator expression.
function(desert_link_whole_archive target)
    foreach(lib IN LISTS ARGN)
        target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${lib}>")
    endforeach()
endfunction()

# Commands that must run on EVERY build before the target's sources compile (the header
# tool, GenVersion). A custom command whose output is a SYMBOLIC file is never up to date, so it runs every
# build, and as a source of the target it orders before every object of that target -- and only of that
# target, which adds no target to the build (a custom target would).
function(desert_prebuild target name)
    cmake_parse_arguments(ARG "" "COMMENT" "COMMAND;DEPENDS" ${ARGN})
    set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}.${name}.prebuild")
    add_custom_command(
        OUTPUT "${_stamp}"
        COMMAND ${ARG_COMMAND}
        DEPENDS ${ARG_DEPENDS}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "${ARG_COMMENT}"
        VERBATIM)
    set_source_files_properties("${_stamp}" PROPERTIES SYMBOLIC TRUE)
    target_sources(${target} PRIVATE "${_stamp}")
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
#              no Debug runtime checks (clang -O3).
#   debug_opt  speed with no runtime checks, Debug only, on single files
#              (DESERT_DEBUG_OPTIMIZE, the mesh card/distance field builders): Debug /O2 (clang -O2).
# Otherwise: Debug /Od /RTC1 (no optimisation, fast runtime checks),
# Release and Shipping /Ox /Oi /Gy /GF (full optimisation, intrinsics,
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
                "$<$<AND:$<CONFIG:Debug>,${_cc}>:${_debug}>"
                "$<$<AND:$<NOT:$<CONFIG:Debug>>,${_cc}>:/Ox;/Oi;/Gy;/GF>")
        endif()
    elseif(speed)
        target_compile_options(${target} PRIVATE "$<${_cc}:-O3>")
    elseif(debug_opt)
        target_compile_options(${target} PRIVATE "$<$<AND:$<CONFIG:Debug>,${_cc}>:-O2>")
    endif()
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
    # markers `::@` of a cross-directory target_link_libraries() and the WHOLE_ARCHIVE wrapper are link-only).
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
# per-file rules (vendored files, unity opt-outs, Debug-optimised builders).
function(desert_finalize_targets)
    _desert_collect_targets("${CMAKE_SOURCE_DIR}" _targets)
    foreach(target IN LISTS _targets)
        get_target_property(_type ${target} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
            continue()
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
                if(_unity)
                    foreach(pattern IN LISTS DESERT_UNITY_OPT_OUT)
                        string(FIND "${_abs}" "${pattern}" _at)
                        if(NOT _at EQUAL -1)
                            list(APPEND _no_unity "${_abs}")
                            break()
                        endif()
                    endforeach()
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
        if(_pch AND (_vendored OR _debug_opt))
            list(LENGTH _pch_src _n)
            if(NOT _n EQUAL 1)
                message(FATAL_ERROR "${target}: splits sources off but has ${_n} PCH sources named like its "
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
        # Sources optimised in Debug. MSVC: Debug compiles them without the PCH but with pch.hpp still force-included, i.e. the
        # reused PCH ignored (/Y-) with cmake_pch.hxx still force-included; Release/Shipping use the PCH. clang has
        # no per-configuration PCH switch: the header is force-included in every configuration instead.
        if(_debug_opt)
            if(MSVC)
                _desert_split_objects(${target} DebugOptimized "${_holder}" _split ${_debug_opt})
                target_compile_options(${_split} PRIVATE "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C,CXX>>:/Y->")
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
endfunction()

# ASCII character set: defines neither UNICODE/_UNICODE
# nor _MBCS (only MultiByte would add _MBCS).
function(desert_ascii_characterset target)
    set_property(TARGET ${target} PROPERTY DESERT_ASCII ON)
endfunction()

# A vendored target optimised in EVERY configuration (MSVC /O2 /Oi /Gy /GF, clang -O3)
# and without the Debug runtime checks /O2 cannot be combined with (no /RTC1). Applied per target by
# desert_finalize_targets().
function(desert_optimize_speed target)
    set_property(TARGET ${target} PROPERTY DESERT_OPTIMIZE_SPEED ON)
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

# The target is absent from that configuration's build. Ninja
# Multi-Config (the presets' generator) honours a per-configuration EXCLUDE_FROM_ALL; the target stays declared,
# so a build that names it explicitly still builds it.
function(desert_remove_configuration target config)
    set_property(TARGET ${target} PROPERTY EXCLUDE_FROM_ALL "$<CONFIG:${config}>")
endfunction()

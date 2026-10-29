# Workspace-wide settings shared by every target: the CMake port of BuildScripts/Platform.lua,
# Workspace.lua, Configurations.lua, PlatformWindows.lua, PlatformMacOS.lua and UnityBuild.lua.
#
# Everything set here at directory scope reaches every target below the root, first-party and vendored
# alike, exactly as premake's workspace scope did. The per-source rules premake expressed as
# `filter "files:..."` (warnings off for vendored files, the unity opt-out list) cannot be expressed at
# directory scope in CMake, so they are applied once, after every target exists, by desert_finalize_targets()
# (called from the root CMakeLists.txt through cmake_language(DEFER)).

include_guard(GLOBAL)

if(NOT DESERT_ROOT)
  set(DESERT_ROOT "${CMAKE_SOURCE_DIR}")
endif()

# ── PLATFORM (BuildScripts/Platform.lua) ───────────────────────────────────────────────────────────
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

# ── CONFIGURATIONS (BuildScripts/Configurations.lua) ───────────────────────────────────────────────
# Debug, Release and Shipping. Shipping is the player's artifact: DESERT_CONFIG_SHIPPING is what the
# sources test to leave the development instruments out (see Configurations.lua for the full reasoning).
# Flags spell premake's choices out explicitly instead of inheriting CMake's per-config defaults, so the
# two build systems compile the same way:
#   Debug     runtime Debug,  symbols On (/Zi or /Z7, never /ZI), no optimisation
#   Release   runtime Release, optimize On  (MSVC /Ox, clang -O2), NDEBUG
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
        set(CMAKE_${lang}_FLAGS_DEBUG    "/Od" CACHE STRING "" FORCE)  # /RTC1 per source, see desert_finalize_targets
        # premake `optimize "On"/"Full"` on MSBuild: Optimization Full (/Ox) + IntrinsicFunctions (/Oi) +
        # FunctionLevelLinking (/Gy) + StringPooling (/GF). No /DWIN32 /D_WINDOWS: MSBuild projects never had them.
        set(CMAKE_${lang}_FLAGS_RELEASE  "/Ox /Oi /Gy /GF /DNDEBUG" CACHE STRING "" FORCE)
        set(CMAKE_${lang}_FLAGS_SHIPPING "/Ox /Oi /Gy /GF /DNDEBUG" CACHE STRING "" FORCE)
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
    set(CMAKE_C_FLAGS "" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS "/EHsc" CACHE STRING "" FORCE)
    # Debug and Release write a .pdb (MSBuild's defaults under premake's `symbols` Default/On: DesertCrashReporter
    # symbolises with it); Shipping writes none (`symbols "Off"`). Optimised links fold and drop (/OPT:REF,ICF).
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG    "/DEBUG /INCREMENTAL" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE  "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS_SHIPPING "/INCREMENTAL:NO /OPT:REF /OPT:ICF" CACHE STRING "" FORCE)
endif()

# /MDd for Debug, /MD for Release and Shipping (premake `runtime "Debug"/"Release"`, dynamic CRT).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Debug information: Debug and Release (MSBuild's default ProgramDatabase where premake says nothing), never Shipping. /Zi and not /ZI (Configurations.lua, BUILD1 P3). With a compiler
# launcher (sccache/ccache) the information is embedded (/Z7), which is what BuildScripts/MSBuild/Ccache.targets
# did for CI: a cache cannot share the one vc143.pdb every compile of a project writes into.
if(DESERT_COMPILER_LAUNCHER)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<NOT:$<CONFIG:Shipping>>:Embedded>")
else()
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<NOT:$<CONFIG:Shipping>>:ProgramDatabase>")
endif()

add_compile_definitions(
    $<$<CONFIG:Shipping>:DESERT_CONFIG_SHIPPING>
    # EnTT's type-index counter must be atomic in every target or none (Workspace.lua).
    ENTT_USE_ATOMIC
)

# ── LANGUAGE ───────────────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

# ── OUTPUT DIRECTORIES (Workspace.lua targetdir) ───────────────────────────────────────────────────
# build/Bin/<Config> for executables AND libraries, as premake wrote them: DesertPlatform.BuiltToolPath and
# every script that starts a built tool read this path. Objects stay in the CMake binary directory
# (premake's build/Intermediates/<Config> is MSBuild/gmake's own layout).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${DESERT_ROOT}/build/Bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${DESERT_ROOT}/build/Bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${DESERT_ROOT}/build/Bin/$<CONFIG>")
set(CMAKE_PDB_OUTPUT_DIRECTORY     "${DESERT_ROOT}/build/Bin/$<CONFIG>")

# ── FIRST-PARTY CODE UNDER ThirdParty/ (Workspace.lua) ─────────────────────────────────────────────
# desert-shared is our own submodule: an ordinary include directory, never a SYSTEM one.
include_directories("${DESERT_ROOT}/ThirdParty/desert-shared/Include")

# ── WINDOWS (PlatformWindows.lua, Workspace.lua) ───────────────────────────────────────────────────
if(DESERT_IS_WINDOWS)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "DesertEngine builds x64 only on Windows (PlatformWindows.lua `architecture \"x64\"`); "
                            "configure from an x64 developer environment (vcvars64)")
    endif()
    # <windows.h> min/max macros break std::max and reflect-cpp (PlatformWindows.lua).
    add_compile_definitions(NOMINMAX)
    # premake's default `characterset` on Visual Studio is Unicode, i.e. /DUNICODE /D_UNICODE in every project,
    # except one that says `characterset "ASCII"` (Embree): added per target by desert_finalize_targets().
    # /utf-8: source AND execution charset UTF-8. /bigobj: every target, C1128 is per object, not per project.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8;/bigobj>")
    # The running process is UTF-8 as well: every executable embeds this manifest (desert_add_executable).
    set(DESERT_WINDOWS_MANIFEST "${DESERT_ROOT}/BuildScripts/Windows/Utf8CodePage.manifest")
endif()

# ── macOS (PlatformMacOS.lua) ──────────────────────────────────────────────────────────────────────
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
    # before `ar qc`, which is what PlatformMacOS.lua's `rm -f` prelink step did for gmake.
endif()

# ── WARNINGS (Workspace.lua) ───────────────────────────────────────────────────────────────────────
# First-party sources: /W3 on MSVC (a measured choice, see Workspace.lua), -Wall -Wextra on clang/gcc.
# Vendored sources (any path containing /ThirdParty/): warnings off, per file, so a vendored .cpp compiled
# INTO a first-party target (vk_mem_alloc.cpp, stb_image.cpp, miniaudio.cpp, ImGuizmo.cpp) is covered too.
# Applied by desert_finalize_targets(); no warning level is set at directory scope (CMP0092: CMake adds none).
if(MSVC)
    set(DESERT_WARNINGS_ON  /W3)
    set(DESERT_WARNINGS_OFF /W0)
else()
    set(DESERT_WARNINGS_ON  -Wall -Wextra)
    set(DESERT_WARNINGS_OFF -w)
endif()

# ── UNITY BUILD (UnityBuild.lua) ───────────────────────────────────────────────────────────────────
# Off by default for the reason UnityBuild.lua gives: a developer build must compile the code as written.
# Unlike premake (Visual Studio only), CMake's unity build works with every generator, so the switch is not
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

# Recursive glob, re-run on every build so a new file is picked up as premake's regeneration picked it up.
# Patterns are relative to the calling CMakeLists.txt's directory unless absolute.
function(desert_glob out)
    file(GLOB_RECURSE _files LIST_DIRECTORIES false CONFIGURE_DEPENDS ${ARGN})
    list(SORT _files)
    set(${out} ${_files} PARENT_SCOPE)
endfunction()

# The project-level settings premake spelled out in five or more project scripts, in one place:
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

# add_library(STATIC) for a first-party or vendored static library. FOLDER mirrors premake's `group`.
function(desert_add_library name)
    cmake_parse_arguments(ARG "" "FOLDER" "SOURCES" ${ARGN})
    add_library(${name} STATIC ${ARG_SOURCES})
    if(ARG_FOLDER)
        set_target_properties(${name} PROPERTIES FOLDER "${ARG_FOLDER}")
    endif()
endfunction()

# add_executable for a ConsoleApp / WindowedApp. WINDOWED is premake's `kind "WindowedApp"` (Windows
# subsystem; the entry stays mainCRTStartup where the premake script said so, via LINK_OPTIONS).
# Every executable gets the UTF-8 code-page manifest on Windows (PlatformWindows.lua).
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

# premake `externalincludedirs`: SYSTEM include directories (MSVC /external:I + /external:W0).
function(desert_external_includes target)
    target_include_directories(${target} SYSTEM PRIVATE ${ARGN})
endfunction()

# DesertPlatform.LinkWholeArchive: every object of the static library reaches the binary, so a file whose
# one job is a static initialiser (a reflected component's self-registration) is not dropped by the linker.
# CMake spells -force_load / --whole-archive / /WHOLEARCHIVE as one generator expression.
function(desert_link_whole_archive target)
    foreach(lib IN LISTS ARGN)
        target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${lib}>")
    endforeach()
endfunction()

# premake `prebuildcommands` that must run on EVERY build before the target's sources compile (the header
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

# The per-source rules premake expressed as workspace `filter "files:..."` (Workspace.lua, UnityBuild.lua):
# warnings per file, and the unity opt-out list. Runs once, after every target is declared.
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
        get_target_property(_sources ${target} SOURCES)
        get_target_property(_srcdir ${target} SOURCE_DIR)
        get_target_property(_unity ${target} DESERT_UNITY_TARGET)
        set(_ours "")
        set(_vendored "")
        set(_no_unity "")
        foreach(src IN LISTS _sources)
            if(NOT src MATCHES "\\.(c|cc|cpp|cxx|mm|m)$")
                continue()
            endif()
            cmake_path(ABSOLUTE_PATH src BASE_DIRECTORY "${_srcdir}" OUTPUT_VARIABLE _abs)
            if(_abs MATCHES "/ThirdParty/")
                list(APPEND _vendored "${src}")
            else()
                list(APPEND _ours "${src}")
            endif()
            if(_unity)
                foreach(pattern IN LISTS DESERT_UNITY_OPT_OUT)
                    string(FIND "${_abs}" "${pattern}" _at)
                    if(NOT _at EQUAL -1)
                        list(APPEND _no_unity "${src}")
                        break()
                    endif()
                endforeach()
            endif()
        endforeach()
        # MSVC Debug runtime checks (/RTC1, MSBuild's EnableFastChecks under UseDebugLibraries) on every source
        # except one marked DESERT_NO_RUNTIME_CHECKS (premake `runtimechecks "Off"`, needed where Debug optimises a file).
        if(MSVC)
            set(_rtc "")
            foreach(src IN LISTS _ours _vendored)
                get_property(_off SOURCE "${src}" TARGET_DIRECTORY ${target} PROPERTY DESERT_NO_RUNTIME_CHECKS)
                if(NOT _off)
                    list(APPEND _rtc "${src}")
                endif()
            endforeach()
            if(_rtc)
                set_property(SOURCE ${_rtc} TARGET_DIRECTORY ${target} APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Debug>:/RTC1>")
            endif()
        endif()
        if(_ours)
            set_property(SOURCE ${_ours} TARGET_DIRECTORY ${target} APPEND PROPERTY COMPILE_OPTIONS ${DESERT_WARNINGS_ON})
        endif()
        if(_vendored)
            set_property(SOURCE ${_vendored} TARGET_DIRECTORY ${target} APPEND PROPERTY COMPILE_OPTIONS ${DESERT_WARNINGS_OFF})
        endif()
        if(_no_unity)
            set_property(SOURCE ${_no_unity} TARGET_DIRECTORY ${target} PROPERTY SKIP_UNITY_BUILD_INCLUSION ON)
        endif()
    endforeach()
endfunction()

# premake `characterset "ASCII"` (MultiByte): the project compiles with _MBCS instead of UNICODE/_UNICODE.
function(desert_ascii_characterset target)
    set_property(TARGET ${target} PROPERTY DESERT_ASCII ON)
    if(DESERT_IS_WINDOWS)
        target_compile_definitions(${target} PRIVATE _MBCS)
    endif()
endfunction()

# premake `optimize "Speed"` on a vendored project: optimised in EVERY configuration (MSVC /O2, clang -O3), and on
# MSVC without the Debug runtime checks /O2 cannot be combined with (`runtimechecks "Off"`).
function(desert_optimize_speed target)
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<CONFIG:Debug>:/O2>")
        get_target_property(_sources ${target} SOURCES)
        set_property(SOURCE ${_sources} TARGET_DIRECTORY ${target} PROPERTY DESERT_NO_RUNTIME_CHECKS ON)
    else()
        target_compile_options(${target} PRIVATE -O3)
    endif()
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

# Only DESERT_PLATFORM_<OS>, for the tools whose premake script defines the platform and no configuration.
function(desert_platform_defines target)
    if(DESERT_IS_WINDOWS)
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_WINDOWS)
    elseif(DESERT_IS_MACOS)
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_MACOS)
    else()
        target_compile_definitions(${target} PRIVATE DESERT_PLATFORM_LINUX)
    endif()
endfunction()

# premake `removeconfigurations { "<Config>" }`: the target is absent from that configuration's build. Ninja
# Multi-Config (the presets' generator) honours a per-configuration EXCLUDE_FROM_ALL; the target stays declared,
# so a build that names it explicitly still builds it, which premake's missing project could not.
function(desert_remove_configuration target config)
    set_property(TARGET ${target} PROPERTY EXCLUDE_FROM_ALL "$<CONFIG:${config}>")
endfunction()

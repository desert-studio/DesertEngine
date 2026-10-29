# The CMake port of Desert/Dependencies.lua and Editor/Dependencies.lua: the include directories, defines and
# prebuilt libraries every engine-facing target shares, the Vulkan SDK lookup, and the Streamline runtime copy.
#
# The four premake tables become lists with the same names, so a target script reads like its premake one:
#   deps.Common.IncludeDir / .Defines          DESERT_COMMON_INCLUDE_DIRS / DESERT_COMMON_DEFINES
#   deps.CommonSpecific.IncludeDir / .Libraries DESERT_COMMONSPECIFIC_INCLUDE_DIRS / DESERT_COMMONSPECIFIC_LIBRARIES
#   deps.DesertSpecific.IncludeDir             DESERT_DESERTSPECIFIC_INCLUDE_DIRS
#   deps.DesertSpecific.Libraries.Debug/Release DESERT_VULKAN_LIBRARIES (one list, per-config generator expressions)
#   deps.TestSpecific.*                        DESERT_TEST_INCLUDE_DIRS / DESERT_TEST_LIBRARIES / DESERT_TEST_DEFINES
#   Editor deps.EditorSpecific.IncludeDir      DESERT_EDITORSPECIFIC_INCLUDE_DIRS (relative entries made absolute
#                                              against the target's own directory by the caller, as premake did)
# Lists are in premake's DesertSortedPairs order (keys sorted), so include order matches.

include_guard(GLOBAL)

set(_tp "${DESERT_ROOT}/ThirdParty")

# ── VULKAN SDK ─────────────────────────────────────────────────────────────────────────────────────
# Same search order as findVulkanSDK(): $VULKAN_SDK, then (Windows) the newest Program Files\VulkanSDK\*,
# (macOS) the newest ~/VulkanSDK/*/macOS then Homebrew, (Linux) /usr/local/vulkan. A missing SDK is an error:
# the engine links shaderc and spirv-cross out of the SDK's Lib directory.
set(DESERT_VULKAN_INCLUDE "")
set(DESERT_VULKAN_LIB "")
if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" _sdk)
    if(DESERT_IS_WINDOWS)
        set(DESERT_VULKAN_INCLUDE "${_sdk}/Include")
        set(DESERT_VULKAN_LIB "${_sdk}/Lib")
    else()
        set(DESERT_VULKAN_INCLUDE "${_sdk}/include")
        set(DESERT_VULKAN_LIB "${_sdk}/lib")
    endif()
elseif(DESERT_IS_WINDOWS)
    file(TO_CMAKE_PATH "$ENV{PROGRAMFILES}/VulkanSDK" _root)
    file(GLOB _versions LIST_DIRECTORIES true "${_root}/*")
    list(SORT _versions ORDER DESCENDING)
    if(_versions)
        list(GET _versions 0 _sdk)
        set(DESERT_VULKAN_INCLUDE "${_sdk}/Include")
        set(DESERT_VULKAN_LIB "${_sdk}/Lib")
    endif()
elseif(DESERT_IS_MACOS)
    file(GLOB _versions LIST_DIRECTORIES true "$ENV{HOME}/VulkanSDK/*")
    list(SORT _versions ORDER DESCENDING)
    if(_versions)
        list(GET _versions 0 _sdk)
    endif()
    if(_versions AND IS_DIRECTORY "${_sdk}/macOS")
        set(DESERT_VULKAN_INCLUDE "${_sdk}/macOS/include")
        set(DESERT_VULKAN_LIB "${_sdk}/macOS/lib")
    elseif(DESERT_HOMEBREW_PREFIX AND EXISTS "${DESERT_HOMEBREW_PREFIX}/include/vulkan/vulkan.h")
        set(DESERT_VULKAN_INCLUDE "${DESERT_HOMEBREW_PREFIX}/include")
        set(DESERT_VULKAN_LIB "${DESERT_HOMEBREW_PREFIX}/lib")
    endif()
elseif(IS_DIRECTORY "/usr/local/vulkan")
    set(DESERT_VULKAN_INCLUDE "/usr/local/vulkan/include")
    set(DESERT_VULKAN_LIB "/usr/local/vulkan/lib")
endif()
if(NOT DESERT_VULKAN_INCLUDE)
    message(FATAL_ERROR
        "Vulkan SDK not found.\n"
        "  macOS:   run scripts/MacOS/Setup.sh (Homebrew vulkan-headers + vulkan-loader + molten-vk +\n"
        "           shaderc + spirv-cross), or install the LunarG SDK into ~/VulkanSDK.\n"
        "  Windows: run scripts\\Windows\\Setup.bat, which installs the pinned LunarG SDK and sets\n"
        "           VULKAN_SDK. A headers-only install is not enough: the engine links shaderc*.lib\n"
        "           and spirv-cross-*.lib out of the SDK's Lib directory.")
endif()
message(STATUS "[Vulkan] SDK include: ${DESERT_VULKAN_INCLUDE}")

# deps.DesertSpecific.Libraries.{Debug,Release}: the Vulkan loader, shaderc and spirv-cross. Debug links the
# SDK's `d` archives on Windows; Release AND Shipping link the plain ones (premake `Release or Shipping`).
if(DESERT_IS_WINDOWS)
    set(DESERT_VULKAN_LIBRARIES "${DESERT_VULKAN_LIB}/vulkan-1.lib")
    foreach(lib shaderc shaderc_combined shaderc_util spirv-cross-core spirv-cross-glsl)
        list(APPEND DESERT_VULKAN_LIBRARIES "${DESERT_VULKAN_LIB}/${lib}$<$<CONFIG:Debug>:d>.lib")
    endforeach()
else()
    set(DESERT_VULKAN_LIBRARIES vulkan shaderc_combined spirv-cross-glsl spirv-cross-core)
endif()

# ── THE TABLES ─────────────────────────────────────────────────────────────────────────────────────
# deps.Common.IncludeDir, sorted by key: GLFW, glm, optick, spdlog.
set(DESERT_COMMON_INCLUDE_DIRS
    "${_tp}/GLFW/include"
    "${_tp}/glm"
    "${_tp}/optick/src"
    "${_tp}/spdlog/include")
set(DESERT_COMMON_DEFINES USE_OPTICK=1 OPTICK_ENABLE_GPU=0 OPTICK_ENABLE_TRACING=0)

set(DESERT_COMMONSPECIFIC_INCLUDE_DIRS "${_tp}/reflect-cpp/include")
set(DESERT_COMMONSPECIFIC_LIBRARIES ReflectCpp)

# deps.DesertSpecific.IncludeDir, sorted by key (premake sorts with Lua's `<`, upper case before lower case).
set(DESERT_DESERTSPECIFIC_INCLUDE_DIRS
    "${_tp}/JoltPhysics"                                        # jolt
    "${DESERT_VULKAN_INCLUDE}"                                  # Vulkan
    "${_tp}"                                                    # base
    "${_tp}/embree/include"                                     # embree
    "${DESERT_ROOT}/BuildScripts/ThirdParty/EmbreeConfig/include/embree4" # embree_config
    "${_tp}/entt/include"                                       # entt
    "${_tp}/lua"                                                # lua
    "${_tp}/meshoptimizer/src"                                  # meshoptimizer
    "${_tp}/OpenSubdiv"                                         # opensubdiv
    "${_tp}/reflect-cpp/include"                                # reflect_cpp
    "${DESERT_VULKAN_INCLUDE}/shaderc"                          # shaderc
    "${DESERT_VULKAN_INCLUDE}/spirv_cross"                      # spirv_cross
    "${_tp}/stb/include"                                        # stb
    "${_tp}/VulkanAllocator")                                   # vkallocator

# deps.TestSpecific. GoogleTest is the vendored project on Windows and Homebrew's gtest elsewhere.
if(DESERT_IS_WINDOWS)
    set(_gtest_include "${_tp}/google-test/include")
    set(_gtest_lib GoogleTest)
elseif(DESERT_HOMEBREW_PREFIX)
    set(_gtest_include "${DESERT_HOMEBREW_PREFIX}/include")
    set(_gtest_lib gtest)
else()
    set(_gtest_include "/usr/local/include")
    set(_gtest_lib gtest)
endif()
set(DESERT_TEST_INCLUDE_DIRS
    "${DESERT_ROOT}/Editor/Resources/Shaders"   # desert_shaders
    "${_gtest_include}"                         # gtest
    "${_tp}/reflect-cpp/include")               # reflect_cpp
set(DESERT_TEST_LIBRARIES ${_gtest_lib} ReflectCpp)
set(DESERT_TEST_DEFINES
    GTEST
    TESTING
    "DESERT_TEST_ENGINE_DIR=\"${DESERT_ROOT}/Editor\""
    "DESERT_TEST_REPO_ROOT=\"${DESERT_ROOT}\""
    "DESERT_TEST_DATA_DIR=\"${DESERT_ROOT}/Desert/Tests/Data\"")
# deps.TestSpecific.EnvVars: the environment a suite runs in (premake wrote it into the debugger settings).
set(DESERT_TEST_REPORTS_DIR "${DESERT_ROOT}/build/TestReports")

# Editor/Dependencies.lua EditorSpecific.IncludeDir, sorted: assimp, assimpGen, imGuizmo, reflect_cpp, stb.
# imGuizmo is RELATIVE ("ThirdParty/ImGuizmo"): premake resolved it against the including project's own
# directory, so for Editor/ it is Editor/ThirdParty/ImGuizmo and for any other project a path that does not
# exist. desert_editorspecific_includes() keeps that meaning.
set(DESERT_EDITORSPECIFIC_INCLUDE_DIRS
    "${DESERT_ROOT}/Editor/ThirdParty/assimp/include"
    "${DESERT_ROOT}/build/generated/assimp/include"
    "ThirdParty/ImGuizmo"
    "${_tp}/reflect-cpp/include"
    "${_tp}/stb/include")

function(desert_editorspecific_includes target)
    get_target_property(_dir ${target} SOURCE_DIR)
    set(_dirs "")
    foreach(d IN LISTS DESERT_EDITORSPECIFIC_INCLUDE_DIRS)
        cmake_path(ABSOLUTE_PATH d BASE_DIRECTORY "${_dir}" OUTPUT_VARIABLE _abs)
        list(APPEND _dirs "${_abs}")
    endforeach()
    target_include_directories(${target} SYSTEM PRIVATE ${_dirs})
endfunction()

# ── STREAMLINE RUNTIME (DesertCopyStreamlineRuntime) ───────────────────────────────────────────────
# ThirdParty/Streamline/manifest.json pins the DLSS runtime; each listed archive is copied next to the
# executable after the link, and a missing one is reported, not fatal (DLSS is then unavailable).
set(DESERT_STREAMLINE_MANIFEST "${DESERT_ROOT}/ThirdParty/Streamline/manifest.json")
set(DESERT_STREAMLINE_RUNTIME_DIR "${DESERT_ROOT}/ThirdParty/Streamline/bin/x64")

function(desert_streamline_runtime_files out)
    if(NOT EXISTS "${DESERT_STREAMLINE_MANIFEST}")
        message(FATAL_ERROR "ThirdParty/Streamline/manifest.json is missing: it pins the DLSS runtime the build copies")
    endif()
    file(READ "${DESERT_STREAMLINE_MANIFEST}" _json)
    string(JSON _count LENGTH "${_json}" Files)
    set(_names "")
    if(_count GREATER 0)
        math(EXPR _last "${_count} - 1")
        foreach(i RANGE ${_last})
            string(JSON _archive GET "${_json}" Files ${i} Archive)
            cmake_path(GET _archive FILENAME _name)
            list(APPEND _names "${_name}")
        endforeach()
    endif()
    set(${out} ${_names} PARENT_SCOPE)
endfunction()

function(desert_copy_streamline_runtime target)
    if(NOT DESERT_IS_WINDOWS)
        return()
    endif()
    desert_streamline_runtime_files(_names)
    foreach(name IN LISTS _names)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                -D "SOURCE=${DESERT_STREAMLINE_RUNTIME_DIR}/${name}"
                -D "DEST=$<TARGET_FILE_DIR:${target}>"
                -D "MISSING=ThirdParty/Streamline/bin/x64/${name} is not fetched, DLSS will be unavailable: run python scripts/Windows/FetchVendorBinaries.py"
                -P "${DESERT_ROOT}/cmake/CopyIfExists.cmake"
            VERBATIM)
    endforeach()
endfunction()

unset(_tp)

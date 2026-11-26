# One library per engine module (a DLL in Development/DebugGame, static in Shipping: desert_module_library), built from
# the module table (cmake/DesertModuleTable.cmake).
# The libraries are named Desert<Module> (DesertModules.LegacyLibName); which files each one compiles is decided
# by cmake/DesertModuleTable.cmake's placement rows (cmake/DesertModules.cmake).

# Vendored single-file implementations compiled INTO a module library, each into the module that uses it.
set(_vendored_RenderCore
    "${DESERT_ROOT}/ThirdParty/VulkanAllocator/vk_mem_alloc.cpp"
    "${DESERT_ROOT}/ThirdParty/vk-bootstrap/VkBootstrap.cpp")
set(_vendored_ImageCore    "${DESERT_ROOT}/ThirdParty/stb/stb_image.cpp")
set(_vendored_TextCore     "${DESERT_ROOT}/ThirdParty/stb/stb_truetype.cpp")
set(_vendored_Audio        "${DESERT_ROOT}/ThirdParty/miniaudio/miniaudio.cpp")
set(_vendored_GeometryCore "${DESERT_ROOT}/ThirdParty/mikktspace/mikktspace.cpp")
foreach(name RenderCore ImageCore TextCore Audio GeometryCore)
    if(NOT DESERT_MODULE_${name}_TABLE_LIBRARY)
        message(FATAL_ERROR "Desert/Desert/CMakeLists.txt: vendored sources assigned to '${name}', which builds no library")
    endif()
endforeach()

# The vendored libraries a module library links (besides Optick, which every module's profiling scopes use). A static
# library's links travel to whoever links it, so this list is what the dedicated server's link (Server/CMakeLists.txt:
# the closure of Core) builds and links: the modules of that closure name exactly the libraries their code calls
# (measured with nm -u on each archive), and the server build compiles no physics, ray tracing, Voronoi fracture,
# texture compression or media decoding. Modules not named here link the full set.
set(_luau Luau)
set(_thirdparty_default Jolt ${_luau} MeshOptimizer Voro Compressonator OpenSubdiv Embree Dav1d Opus)
set(_thirdparty_CoreReflection "")
set(_thirdparty_Projects "")
set(_thirdparty_AssetCore "")
set(_thirdparty_GeometryCore OpenSubdiv MeshOptimizer)
set(_thirdparty_Core ${_luau})

desert_engine_header_tool_command(_header_tool_command _header_tool_inputs)
set_source_files_properties(${DESERT_ENGINE_GENERATED_FILES} PROPERTIES GENERATED TRUE)

# DesertSchemaLock: the one explicit command that rewrites Desert/Schema/ComponentSchema.lock to the code's
# COMPONENT blocks (scripts/Dev/schema_lock.sh); the build only compares and fails on a difference.
add_custom_target(DesertSchemaLock
    COMMAND ${_header_tool_command} --write-schema-lock
    DEPENDS DesertHeaderTool
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "DesertHeaderTool: rewrite Desert/Schema/ComponentSchema.lock"
    VERBATIM)

set(_source "${CMAKE_CURRENT_SOURCE_DIR}/Source")
set(_postprocessing "${_source}/Engine/Graphic/Systems/Scene/PostProcessing")

# Sources that never share a unity batch (cmake/DesertWorkspace.cmake desert_unity_exclude), each with the name it
# would clash on.
desert_sources(_d3d12 "${_source}" MATCHING "^Engine/Graphic/API/D3D12/[^/]+\\.cpp$")
desert_unity_exclude(REASON "windows.h / d3d12.h macros (min, max, near, far, CreateWindow) leak into the batch"
    ${_d3d12} "${_source}/Engine/Core/Device.cpp")
desert_unity_exclude(REASON "anonymous-namespace Finite() also in Animation/Rig/ControlHierarchy.cpp"
    "${_source}/Engine/Animation/Rig/ControlShape.cpp")
desert_unity_exclude(REASON "anonymous-namespace kBytesPerVoxel / kFormatRgba8 / kMinResolution also in the other cloud volume and bake sources"
    "${_source}/Engine/Assets/CloudNoiseVolume.cpp"
    "${_source}/Engine/Assets/CloudNoiseVolumeSheet.cpp"
    "${_source}/Engine/Assets/CloudProceduralVolume.cpp")
desert_unity_exclude(REASON "anonymous-namespace FileHeader / kByteOrderTag also in Assets/Serialization/MeshBinary.cpp"
    "${_source}/Engine/Assets/Serialization/TextureBinary.cpp")
desert_unity_exclude(REASON "anonymous-namespace kGroupSize / GroupCount() also in the other compute-pass renderers"
    "${_source}/Engine/Graphic/Systems/Scene/Fog/HeightFogRenderer.cpp"
    "${_postprocessing}/AutoExposureRenderer.cpp"
    "${_postprocessing}/BloomRenderer.cpp"
    "${_postprocessing}/LensFlareRenderer.cpp"
    "${_postprocessing}/LightShaftRenderer.cpp")
desert_unity_exclude(REASON "anonymous-namespace FloorDiv() also in Landscape/Private/LandscapeBrush.cpp"
    "${DESERT_ROOT}/Desert/Landscape/Source/Landscape/Private/LandscapeEditCache.cpp")

function(_desert_engine_module_library name)
    set(target "Desert${name}")
    set(_common "${_source}/pch.cpp" "${_source}/pch.hpp")
    set(_windows "")
    set(_mac "")
    # Platform split by path: a Windows/ or MacOS/ directory compiles on that platform only.
    macro(_desert_add rel abs)
        if("${rel}" MATCHES "^Platform/Windows/" OR "${rel}" MATCHES "^Engine/Graphic/Upscalers/"
           OR "${rel}" MATCHES "/Streamline/" OR "${rel}" MATCHES "/FidelityFX/" OR "${rel}" MATCHES "/D3D12/")
            list(APPEND _windows "${abs}")
        elseif("${rel}" MATCHES "^Platform/MacOS/" OR "${rel}" MATCHES "\\.mm$")
            list(APPEND _mac "${abs}")
        else()
            list(APPEND _common "${abs}")
        endif()
    endmacro()
    foreach(rel IN LISTS DESERT_MODULE_${name}_LEGACY)
        _desert_add("${rel}" "${_source}/${rel}")
    endforeach()
    # The module's generated registration (<build>/Generated, written by the codegen library's HeaderTool step).
    list(APPEND _common ${DESERT_MODULE_${name}_GENERATED})
    string(LENGTH "${DESERT_MODULE_${name}_FOLDER}/" _prefix)
    foreach(file IN LISTS DESERT_MODULE_${name}_FOLDER_FILES)
        string(SUBSTRING "${file}" ${_prefix} -1 _rel)
        _desert_add("${_rel}" "${DESERT_ROOT}/${file}")
    endforeach()

    desert_module_library(${target} SOURCES ${_common} ${_vendored_${name}})
    desert_enable_unity(${target})
    # Runtime code: whole-program optimised in Shipping (every module library, one shared PCH).
    desert_ltcg(${target})

    # pch.hpp, force-included into every source (pchheader/pchsource/forceincludes); every module library
    # compiles with the same flags, so one PCH serves them all (desert_shared_pch).
    desert_shared_pch(${target} Engine "${_source}/pch.hpp")
    # Objective-C++ sources build without it: the PCH is C++, not Objective-C++.
    set(_no_pch "")
    foreach(src IN LISTS _mac)
        if(src MATCHES "\\.mm$")
            list(APPEND _no_pch "${src}")
        endif()
    endforeach()
    if(_no_pch)
        set_source_files_properties(${_no_pch} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
    # The distance-field and card builders are optimised even in DebugGame (a DebugGame bake is otherwise unusable):
    # desert_finalize_targets() compiles them as <target>-DebugOptimized with DebugGame /O2, no runtime checks and the
    # PCH off in DebugGame only.
    foreach(src IN LISTS _common)
        if(src MATCHES "/Engine/Assets/(MeshDistanceFieldBuild|MeshCardBuild)\\.cpp$")
            set_property(SOURCE "${src}" PROPERTY DESERT_DEBUG_OPTIMIZE ON)
        endif()
    endforeach()

    if(target STREQUAL DESERT_CODEGEN_LIB)
        add_dependencies(${target} DesertHeaderTool)
        desert_prebuild(${target} HeaderTool
            COMMAND ${_header_tool_command}
            DEPENDS ${_header_tool_inputs}
            BYPRODUCTS ${DESERT_ENGINE_GENERATED_FILES}
            COMMENT "DesertHeaderTool: engine reflection and subsystem codegen")
    else()
        desert_depend_on_codegen(${target})
    endif()

    # Closure-only (UBT): the module's own FOLDER and its dependency closure's, so an include of a module it does
    # not depend on does not compile (cmake/DesertModules.cmake, MODULE BOUNDARY, refuses it at configure first).
    desert_module_includes(${target} ${name})
    target_include_directories(${target} PRIVATE
        "${DESERT_ROOT}/ThirdParty/mikktspace"
        "${DESERT_ROOT}/Editor/Resources/Shaders")
    desert_external_includes(${target}
        "${DESERT_ROOT}/ThirdParty/voro++/src"
        "${DESERT_ROOT}/ThirdParty/dav1d/include"
        "${DESERT_ROOT}/ThirdParty/opus/include"
        "${DESERT_ROOT}/ThirdParty/luau"
        "${DESERT_ROOT}/ThirdParty/luau/CodeGen/include"
        "${DESERT_ROOT}/ThirdParty/luau/Common/include"
        # Intel Open Image Denoise 2.x, headers only: the library is LOADED at run time from beside the executable
        # (Engine/Graphic/Denoise/OidnDenoiser.cpp), nothing links it (desert_copy_oidn_runtime).
        "${DESERT_ROOT}/ThirdParty/OIDN/include"
        ${DESERT_COMMON_INCLUDE_DIRS}
        ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})

    # The dependency CLOSURE, not only the named deps: a module compiles against its closure's headers
    # (desert_module_includes), so it calls its closure's code, and a module DLL resolves every symbol it uses at its
    # own link -- a PRIVATE dep of a dep is not on its link line (TextCore -> ImageCore -> Common left TextCore's Common
    # calls unresolved, build #8 of B-INT).
    desert_module_closure(${name} _link_closure)
    foreach(dep IN LISTS _link_closure)
        target_link_libraries(${target} PRIVATE ${DESERT_MODULE_${dep}_LIBRARIES})
    endforeach()
    if(DEFINED _thirdparty_${name})
        target_link_libraries(${target} PRIVATE ${DESERT_OPTICK} ${_thirdparty_${name}})
    else()
        target_link_libraries(${target} PRIVATE ${DESERT_OPTICK} ${_thirdparty_default})
    endif()
    # GLFW (the window, the surface, input) is called by the window and render modules and the modules above them;
    # the server closure has none of these (cmake/DesertModules.cmake RENDER-FREE MODULES), so it never links GLFW.
    desert_module_closure(${name} _closure)
    list(APPEND _closure ${name})
    if("ApplicationCore" IN_LIST _closure OR "RenderCore" IN_LIST _closure OR "Renderer" IN_LIST _closure)
        target_link_libraries(${target} PRIVATE GLFW)
    endif()

    # Engine's client net driver (Engine/Net/ClientNetDriver: Play as a network client) runs Desert/Net's session;
    # Core's Gameplay (Engine/Gameplay: the pawn's movement step) is the Net::IPawnMover the session calls.
    if(name STREQUAL "Engine" OR name STREQUAL "Core")
        target_link_libraries(${target} PRIVATE DesertNet)
    endif()
    desert_config_platform_defines(${target})
    if(DESERT_IS_WINDOWS)
        target_sources(${target} PRIVATE ${_windows})
        target_compile_definitions(${target} PRIVATE DESERT_UPSCALER_FSR DESERT_UPSCALER_DLSS)
        desert_external_includes(${target}
            "${DESERT_ROOT}/ThirdParty/FidelityFX-SDK/sdk/include"
            "${DESERT_ROOT}/ThirdParty/Streamline/include")
        target_link_libraries(${target} PRIVATE FidelityFX)
    elseif(DESERT_IS_MACOS)
        target_sources(${target} PRIVATE ${_mac})
    endif()
    if(NOT DESERT_IS_WINDOWS)
        desert_platform_foreign_sources(${_windows})
    endif()
    if(NOT DESERT_IS_MACOS)
        desert_platform_foreign_sources(${_mac})
    endif()
endfunction()

foreach(name IN LISTS DESERT_MODULES)
    if(DESERT_MODULE_${name}_TABLE_LIBRARY)
        _desert_engine_module_library(${name})
    endif()
endforeach()

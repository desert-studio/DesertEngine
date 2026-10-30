# One static library per engine module, built from the module table (cmake/DesertModuleTable.cmake).
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

desert_engine_header_tool_command(_header_tool_command)

set(_source "${CMAKE_CURRENT_SOURCE_DIR}/Source")

function(_desert_engine_module_library name)
    set(target "Desert${name}")
    set(_common "${_source}/pch.cpp" "${_source}/pch.hpp")
    set(_windows "")
    set(_mac "")
    # Platform split by path: a Windows/ or MacOS/ directory compiles on that platform only.
    macro(_desert_add rel abs)
        if("${rel}" MATCHES "^Platform/Windows/" OR "${rel}" MATCHES "^Engine/Graphic/Upscalers/"
           OR "${rel}" MATCHES "/Streamline/" OR "${rel}" MATCHES "/FidelityFX/")
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
    string(LENGTH "${DESERT_MODULE_${name}_FOLDER}/" _prefix)
    foreach(file IN LISTS DESERT_MODULE_${name}_FOLDER_FILES)
        string(SUBSTRING "${file}" ${_prefix} -1 _rel)
        _desert_add("${_rel}" "${DESERT_ROOT}/${file}")
    endforeach()

    desert_add_library(${target} SOURCES ${_common} ${_vendored_${name}})
    desert_enable_unity(${target})

    # pch.hpp, force-included into every source (pchheader/pchsource/forceincludes).
    target_precompile_headers(${target} PRIVATE "${_source}/pch.hpp")
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
    # The distance-field and card builders are optimised even in Debug (a Debug bake is otherwise unusable):
    # desert_finalize_targets() compiles them as <target>-DebugOptimized with Debug /O2, no runtime checks and the
    # PCH off in Debug only.
    foreach(src IN LISTS _common)
        if(src MATCHES "/Engine/Assets/(MeshDistanceFieldBuild|MeshCardBuild)\\.cpp$")
            set_property(SOURCE "${src}" PROPERTY DESERT_DEBUG_OPTIMIZE ON)
        endif()
    endforeach()

    if(target STREQUAL DESERT_CODEGEN_LIB)
        add_dependencies(${target} DesertHeaderTool)
        desert_prebuild(${target} HeaderTool
            COMMAND ${_header_tool_command}
            DEPENDS DesertHeaderTool
            COMMENT "DesertHeaderTool: engine reflection and subsystem codegen")
    else()
        desert_depend_on_codegen(${target})
    endif()

    desert_engine_includes(${target})
    target_include_directories(${target} PRIVATE
        "${_source}/"
        "${DESERT_ROOT}/ThirdParty/mikktspace"
        "${DESERT_ROOT}/Desert/UI/Source"
        "${DESERT_ROOT}/Desert/Render2DCore/Source"
        "${DESERT_ROOT}/Desert/CoreReflection/Source"
        "${DESERT_ROOT}/Desert/Common/Source"
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

    foreach(dep IN LISTS DESERT_MODULE_${name}_DEPS)
        target_link_libraries(${target} PRIVATE ${DESERT_MODULE_${dep}_LIBRARIES})
    endforeach()
    target_link_libraries(${target} PRIVATE
        Jolt LuauCodeGen LuauCompiler LuauAst LuauBytecode LuauVM LuauCommon
        Optick MeshOptimizer Voro Compressonator OpenSubdiv Embree Dav1d Opus)

    target_compile_definitions(${target} PRIVATE ${DESERT_COMMON_DEFINES})
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
endfunction()

foreach(name IN LISTS DESERT_MODULES)
    if(DESERT_MODULE_${name}_TABLE_LIBRARY)
        _desert_engine_module_library(${name})
    endif()
endforeach()

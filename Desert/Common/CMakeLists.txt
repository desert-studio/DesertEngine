# Common: the Common module library (a DLL in Development/DebugGame, static in Shipping: desert_module_library).
desert_sources(_sources "Source/Common" MATCHING "\\.(cpp|hpp)$")
desert_module_library(Common SOURCES
    ${_sources}
    "${DESERT_ROOT}/ThirdParty/desert-shared/Source/ProjectFormat.cpp"
    "${DESERT_ROOT}/ThirdParty/desert-shared/Source/EngineRegistry.cpp")
desert_enable_unity(Common)
desert_ltcg(Common)
# The first PCH of the chain (Common -> engine pch.hpp -> EditorPCH.hpp): Common is the group's donor, DesertUI and
# Render2DCore reuse it (Desert/CMakeLists.txt declares Common first).
desert_shared_pch(Common Common "${DESERT_ROOT}/Desert/Common/Source/Common/CommonPCH.hpp")
desert_unity_exclude(REASON "anonymous-namespace kMagic / kFormatVersion also in Utilities/PakFile.cpp"
    "Source/Common/Utilities/ContentManifest.cpp")

# Core/Version.gen.hpp is written (only when its text changed) whenever what it is made of changes: the VERSION
# file, a commit or checkout of this worktree (<git-dir>/logs/HEAD) or a change of the index (<git-dir>/index, the
# dirty flag as of the last stage). A tree that is not a git checkout has no hash to write: VERSION alone.
set(_version_inputs "${DESERT_ROOT}/VERSION")
execute_process(COMMAND git -C "${DESERT_ROOT}" rev-parse --absolute-git-dir
    OUTPUT_VARIABLE _git_dir OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _git_rc ERROR_QUIET)
if(_git_rc EQUAL 0)
    foreach(_f logs/HEAD index)
        if(EXISTS "${_git_dir}/${_f}")
            list(APPEND _version_inputs "${_git_dir}/${_f}")
        endif()
    endforeach()
endif()
if(DESERT_IS_WINDOWS)
    desert_prebuild(Common GenVersion
        COMMAND cmd /c call "${DESERT_ROOT}/scripts/Windows/GenVersion.bat"
        DEPENDS ${_version_inputs} "${DESERT_ROOT}/scripts/Windows/GenVersion.bat"
        COMMENT "GenVersion: Common/Core/Version.gen.hpp")
else()
    desert_prebuild(Common GenVersion
        COMMAND bash "${DESERT_ROOT}/scripts/GenVersion.sh"
        DEPENDS ${_version_inputs} "${DESERT_ROOT}/scripts/GenVersion.sh"
        COMMENT "GenVersion: Common/Core/Version.gen.hpp")
endif()

target_include_directories(Common PRIVATE "Source/" "Source/Common")
desert_external_includes(Common ${DESERT_COMMON_INCLUDE_DIRS})
target_link_libraries(Common PRIVATE ${DESERT_OPTICK} ${DESERT_COMMONSPECIFIC_LIBRARIES})
# Logger.hpp is in CommonPCH and calls spdlog inline: every target that links Common links the one spdlog.
target_link_libraries(Common PUBLIC spdlog)
desert_config_platform_defines(Common)
if(DESERT_IS_WINDOWS)
    target_link_libraries(Common PRIVATE dbghelp)
elseif(DESERT_IS_MACOS)
    desert_sources(_mac "Source/Common" MATCHING "^Platform/MacOS/.*\\.mm$")
    target_sources(Common PRIVATE ${_mac})
endif()

# Desert/Common/premake5.lua: the Common static library (module "Common", own project).
desert_glob(_sources "Source/Common/*.cpp" "Source/Common/*.hpp")
desert_add_library(Common SOURCES
    ${_sources}
    "${DESERT_ROOT}/ThirdParty/desert-shared/Source/ProjectFormat.cpp"
    "${DESERT_ROOT}/ThirdParty/desert-shared/Source/EngineRegistry.cpp")
desert_enable_unity(Common)

# Core/Version.gen.hpp is written before every build (only when its text changed).
if(DESERT_IS_WINDOWS)
    desert_prebuild(Common GenVersion
        COMMAND cmd /c call "${DESERT_ROOT}/scripts/Windows/GenVersion.bat"
        COMMENT "GenVersion: Common/Core/Version.gen.hpp")
else()
    desert_prebuild(Common GenVersion
        COMMAND bash "${DESERT_ROOT}/scripts/GenVersion.sh"
        COMMENT "GenVersion: Common/Core/Version.gen.hpp")
endif()

target_include_directories(Common PRIVATE "Source/" "Source/Common")
desert_external_includes(Common ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_COMMONSPECIFIC_INCLUDE_DIRS})
target_link_libraries(Common PRIVATE Optick ${DESERT_COMMONSPECIFIC_LIBRARIES})
target_compile_definitions(Common PRIVATE ${DESERT_COMMON_DEFINES})
desert_config_platform_defines(Common)
if(DESERT_IS_WINDOWS)
    target_link_libraries(Common PRIVATE dbghelp)
elseif(DESERT_IS_MACOS)
    desert_glob(_mac "Source/Common/Platform/MacOS/*.mm")
    target_sources(Common PRIVATE ${_mac})
endif()

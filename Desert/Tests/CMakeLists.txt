# ONE TEST RUNNER PER LAYER (BUILD1 P1, BuildScripts/BUILD1-CONTRACT.md §4).
#
# A suite is a directory Desert/Tests/<Layer>/<Suite>/ whose *.cpp (that directory, not below it) are compiled into
# the layer's runner <Layer>Tests, with TestSupport/RunnerMain.cpp as the only main. What a suite needs beyond that
# goes into its runner's settings below, once, with the reason beside it.
# The runners and the two aggregates are absent from Shipping (desert_remove_configuration): the player's
# configuration ships no test code, and a Shipping build that compiled the suites would hide that it links none.
# Generated at configure time: build/TestManifest.txt
# ("<Executable> <Suite>" per suite) and build/TestNeedsVulkanDevice.txt. RunAllTests writes run_tests.bat.

set(_tests_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(_layers Common Engine Editor Runtime Tools)
set(_runner_layers Common Runtime Tools Engine Editor) # the layers kRunners configures (all of them)
file(MAKE_DIRECTORY "${DESERT_TEST_REPORTS_DIR}")

# -DDESERT_DEV_SUITES=A,B: DevTests, one runner of only these suites (agent builds; CI never sets it).
set(DESERT_DEV_SUITES "" CACHE STRING "Also generate DevTests: one runner of only these test suites (comma list)")

# Every runner is absent from Shipping and writes to build/Bin/Tests/<Config>.
function(_desert_tests_common_settings target)
    set_target_properties(${target} PROPERTIES FOLDER Tests
        RUNTIME_OUTPUT_DIRECTORY "${DESERT_ROOT}/build/Bin/Tests/$<CONFIG>")
    desert_remove_configuration(${target} Shipping)
    target_include_directories(${target} PRIVATE "${DESERT_ROOT}/Desert/Tests")
    desert_external_includes(${target} ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_TEST_INCLUDE_DIRS})
    target_compile_definitions(${target} PRIVATE ${DESERT_TEST_DEFINES}
        "DESERT_MODULE_TABLE_JSON=\"${DESERT_MODULE_TABLE_JSON}\"") # the ModuleBoundary suite reads the table
    desert_platform_defines(${target})
    if(DESERT_IS_MACOS)
        target_link_libraries(${target} PRIVATE "-framework Cocoa" "-framework Foundation")
    endif()
    target_link_libraries(${target} PRIVATE ${DESERT_TEST_LIBRARIES})
endfunction()

# DesertRunnerSettings: the engine and its link closure, for a runner that LINKS the engine (Tools, Engine, Editor).
function(_desert_runner_settings target)
    target_sources(${target} PRIVATE "${DESERT_ROOT}/Desert/Tests/TestSupport/EngineReflectionLink.cpp")
    desert_engine_includes(${target})
    target_include_directories(${target} PRIVATE "${DESERT_ROOT}/Editor/Source")
    desert_external_includes(${target}
        "${DESERT_ROOT}/ThirdParty/GLFW/include/" "${DESERT_ROOT}/ThirdParty/" "${DESERT_ROOT}/ThirdParty/luau"
        ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
    target_compile_definitions(${target} PRIVATE USE_OPTICK=1 OPTICK_ENABLE_GPU=0 OPTICK_ENABLE_TRACING=0
        $<$<CONFIG:Debug>:DESERT_CONFIG_DEBUG> $<$<CONFIG:Release>:DESERT_CONFIG_RELEASE>)
    desert_link_engine(${target})
    target_link_libraries(${target} PRIVATE
        GLFW Optick MeshOptimizer OpenSubdiv Embree ImGui Assimp OpenEXRCore Dav1d Opus Voro Compressonator)
    if(DESERT_IS_MACOS)
        target_link_libraries(${target} PRIVATE
            Common Jolt LuauCodeGen LuauCompiler LuauAst LuauBytecode LuauVM LuauCommon ReflectCpp
            "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation" "-framework CoreVideo"
            "-framework CoreMedia" "-framework AVFoundation" "-framework QuartzCore" "-framework Foundation")
    endif()
    # deps.DesertSpecific.Libraries: ReflectCpp and the Vulkan SDK libraries (the latter already in desert_link_engine).
    target_link_libraries(${target} PRIVATE ReflectCpp)
endfunction()

# The header tool run before a runner compiles (desert_prebuild, after DesertHeaderTool is built).
function(_desert_tests_header_tool target name)
    add_dependencies(${target} DesertHeaderTool)
    desert_prebuild(${target} ${name}
        COMMAND "$<TARGET_FILE:DesertHeaderTool>" --templates "${DESERT_ROOT}/Tools/DesertHeaderTool/Templates" ${ARGN}
        DEPENDS DesertHeaderTool
        COMMENT "DesertHeaderTool: ${target} ${name}")
endfunction()

function(_desert_runner_Common target)
    desert_engine_includes(${target})
    target_include_directories(${target} PRIVATE "${DESERT_ROOT}/Editor/Source" "${DESERT_ROOT}/Runtime/Source")
    target_link_libraries(${target} PRIVATE Common ReflectCpp Optick)
    set(_subsystems "${DESERT_ROOT}/Desert/Tests/Common/Subsystems")
    _desert_tests_header_tool(${target} Subsystems
        --check "${_subsystems}/Owner"
        --context "${DESERT_ROOT}/Desert/Common/Source/Common/Core/Events"
        --subsystems Sample SubsystemSamples::SampleOwner SampleOwner.hpp "${_subsystems}/Generated/SampleSubsystems.gen.cpp"
        --subsystems SampleWorld SubsystemSamples::SampleWorld SampleOwner.hpp "${_subsystems}/Generated/SampleWorldSubsystems.gen.cpp")
    file(GLOB _files CONFIGURE_DEPENDS "${_subsystems}/Owner/*.hpp" "${_subsystems}/Generated/*.gen.cpp")
    list(SORT _files)
    target_sources(${target} PRIVATE ${_files})
    target_include_directories(${target} PRIVATE "${_subsystems}/Owner")
endfunction()

function(_desert_runner_Runtime target)
    target_sources(${target} PRIVATE "${DESERT_ROOT}/Runtime/Source/PackagedContent.cpp")
    target_include_directories(${target} PRIVATE "${DESERT_ROOT}/Desert/Common/Source" "${DESERT_ROOT}/Runtime/Source")
    target_link_libraries(${target} PRIVATE Common Optick)
endfunction()

function(_desert_runner_Tools target)
    _desert_runner_settings(${target})
    target_link_libraries(${target} PRIVATE Lua)
    set(_t "${DESERT_ROOT}/Tools")
    target_sources(${target} PRIVATE
        "${_t}/SceneMigrator/Source/MigratorMain.cpp"
        "${_t}/SceneMigrator/Source/SceneMigration.cpp"
        "${_t}/SceneMigrator/Source/SettingsCanonical.cpp"
        "${_t}/SceneMigrator/Source/UILift.cpp"
        "${_t}/SceneMigrator/Source/MaterialAOParam.cpp"
        "${_t}/SceneMigrator/Source/ClipInterpShift.cpp"
        "${_t}/SceneMigrator/Source/ClipMigration.cpp"
        "${_t}/SceneMigrator/Source/ClipGeneration3.cpp"
        "${_t}/SceneMigrator/Source/ClipLift.cpp"
        "${_t}/SceneMigrator/Source/ImportRecordSourceHash.cpp"
        "${_t}/SceneMigrator/Source/ShaderLocatorFollow.cpp"
        "${_t}/WorldGen/Source/WorldBuild.cpp"
        "${_t}/WorldGen/Source/WorldGenMain.cpp"
        "${_t}/CrashReporter/Source/CrashReport.cpp"
        "${_t}/WorldCook/Source/WorldCookMain.cpp"
        "${_t}/DesertHeaderTool/Source/HeaderScan.cpp"
        "${_t}/DesertHeaderTool/Source/AnnotationText.cpp"
        "${_t}/DesertHeaderTool/Source/ComponentBlocks.cpp"
        "${_t}/DesertHeaderTool/Source/ModuleTable.cpp"
        "${DESERT_ROOT}/Editor/Source/Editor/Core/AssetReferences.cpp"
        "${DESERT_ROOT}/Editor/Source/Editor/Core/AssetReferencesScan.cpp")
    set(_reflected_functions "${DESERT_ROOT}/Desert/Tests/Tools/ReflectedFunctions")
    set(_luau_runtime "${DESERT_ROOT}/Desert/Tests/Tools/LuauRuntime")
    target_include_directories(${target} PRIVATE
        "${_t}/SceneMigrator/Source"
        "${_t}/WorldGen/Source"
        "${_t}/CrashReporter/Source"
        "${_t}/WorldCook/Source"
        "${_t}/Shared"
        "${_t}/DesertHeaderTool/Source"
        "${_reflected_functions}"
        "${_luau_runtime}")
    _desert_tests_header_tool(${target} ReflectedFunctions
        --reflect "${_reflected_functions}" "Fixture" "${_reflected_functions}/Generated/FunctionFixture.gen.cpp"
        --reflect-anchor ForceLinkReflectedFunctionFixture)
    _desert_tests_header_tool(${target} LuauRuntime
        --reflect "${_luau_runtime}" "Fixture" "${_luau_runtime}/Generated/LuauFixture.gen.cpp"
        --reflect-anchor ForceLinkLuauRuntimeFixture)
    file(GLOB _files CONFIGURE_DEPENDS
        "${_reflected_functions}/Fixture/*.hpp" "${_reflected_functions}/Generated/*.gen.cpp"
        "${_luau_runtime}/Fixture/*.hpp" "${_luau_runtime}/Generated/*.gen.cpp")
    list(SORT _files)
    target_sources(${target} PRIVATE ${_files})
endfunction()

function(_desert_runner_Engine target)
    _desert_runner_settings(${target})
    target_include_directories(${target} PRIVATE
        "${DESERT_ROOT}/Desert/Tests/Engine/SettingConsumers"
        "${DESERT_ROOT}/Tools/ImageDiff/Source"
        "${DESERT_ROOT}/Tools/LatticePeak/Source")
    desert_external_includes(${target} "${DESERT_ROOT}/ThirdParty/voro++/src")
    target_compile_definitions(${target} PRIVATE
        "DESERT_MEDIA_TEST_CLIP=\"${DESERT_ROOT}/Desert/Tests/Data/Media/red_440hz_1s.webm\""
        "DESERT_MEDIA_PATTERN_CLIP=\"${DESERT_ROOT}/Desert/Tests/Data/Media/testsrc2_1080p_5s.webm\"")
endfunction()

function(_desert_runner_Editor target)
    # Whole, as the Editor links them, and ahead of the engine on the link line.
    desert_link_editor_libs(${target} WHOLE_ARCHIVE)
    _desert_runner_settings(${target})
    target_sources(${target} PRIVATE "${DESERT_ROOT}/Runtime/Source/PackagedContent.cpp")
    target_include_directories(${target} PRIVATE
        "${DESERT_ROOT}/Editor/Source/Editor/Import"
        "${DESERT_ROOT}/Editor/Source/Editor/Panels/NodeGraph"
        "${DESERT_ROOT}/Runtime/Source"
        "${DESERT_ROOT}/ThirdParty/ImGui")
    desert_external_includes(${target}
        "${DESERT_ROOT}/Editor/ThirdParty/assimp/include"
        "${DESERT_ROOT}/build/generated/assimp/include"
        "${DESERT_ROOT}/ThirdParty/openexr/src/lib/OpenEXRCore"
        "${DESERT_ROOT}/build/generated/openexr/include"
        "${DESERT_ROOT}/ThirdParty/Imath/src/Imath")
    target_link_libraries(${target} PRIVATE ImGuiNodeEditor)
    if(IS_DIRECTORY "${DESERT_ROOT}/ThirdParty/dlib")
        target_link_libraries(${target} PRIVATE Dlib)
    endif()
    if(DESERT_IS_WINDOWS)
        target_link_libraries(${target} PRIVATE ws2_32 advapi32 comdlg32 shell32 ole32)
    endif()
endfunction()

# kAdoptedSources: test sources outside Desert/Tests that a suite adopts; compiled wherever that suite is.
set(_adopted_suites ProjectFormat)
set(_adopted_ProjectFormat "${DESERT_ROOT}/ThirdParty/desert-shared/Tests/project_format_test.cpp")

# A suite's sources: its directory's *.cpp (not below it) and what it adopts.
function(_desert_suite_sources dir out)
    file(GLOB _files LIST_DIRECTORIES false CONFIGURE_DEPENDS "${dir}/*.cpp")
    list(SORT _files)
    get_filename_component(_suite "${dir}" NAME)
    if(_suite IN_LIST _adopted_suites)
        list(APPEND _files ${_adopted_${_suite}})
    endif()
    set(${out} ${_files} PARENT_SCOPE)
endfunction()

set(_manifest_lines "")
set(_test_projects "")
set(_known_suites "")
foreach(layer IN LISTS _layers)
    file(GLOB _suite_dirs LIST_DIRECTORIES true "${_tests_dir}/${layer}/*")
    set(_dirs "")
    foreach(d IN LISTS _suite_dirs)
        if(IS_DIRECTORY "${d}")
            list(APPEND _dirs "${d}")
        endif()
    endforeach()
    list(SORT _dirs)
    set(_runner "${layer}Tests")
    set(_sources "")
    foreach(dir IN LISTS _dirs)
        get_filename_component(_suite "${dir}" NAME)
        if(EXISTS "${dir}/CMakeLists.txt")
            message(FATAL_ERROR "Desert/Tests/${layer}/${_suite} has its own CMakeLists.txt: a suite is compiled into "
                "${_runner}. Delete the script, delete the suite's main (set-up it did goes into a SuiteEnvironment, a "
                "child mode into a ChildEntry: TestSupport/runner.hpp), and move any source or include directory it "
                "added into Desert/Tests/CMakeLists.txt's _desert_runner_${layer} (BuildScripts/BUILD1-CONTRACT.md §4)")
        endif()
        _desert_suite_sources("${dir}" _suite_files)
        list(APPEND _sources ${_suite_files})
        list(APPEND _manifest_lines "${_runner} ${_suite}")
        list(APPEND _known_suites "${_suite}")
        set(_suite_layer_${_suite} "${layer}")
        set(_suite_dir_${_suite} "${dir}")
    endforeach()
    if(layer IN_LIST _runner_layers AND _dirs)
        desert_add_executable(${_runner} SOURCES "${_tests_dir}/TestSupport/RunnerMain.cpp" ${_sources})
        _desert_tests_common_settings(${_runner})
        cmake_language(CALL _desert_runner_${layer} ${_runner})
        list(APPEND _test_projects ${_runner})
    endif()
endforeach()
foreach(suite IN LISTS _adopted_suites)
    if(NOT suite IN_LIST _known_suites)
        message(FATAL_ERROR "kAdoptedSources names ${suite}, which is no suite directory Desert/Tests/<Layer>/${suite}")
    endif()
endforeach()

# DEVTESTS (BLD-DEVTESTS): RunnerMain, the named suites' sources and the runner settings of every layer they come from.
if(DESERT_DEV_SUITES)
    string(REPLACE "," ";" _dev_list "${DESERT_DEV_SUITES}")
    set(_dev_sources "")
    set(_dev_layers "")
    foreach(suite IN LISTS _dev_list)
        string(STRIP "${suite}" suite)
        if(suite STREQUAL "")
            continue()
        endif()
        if(NOT suite IN_LIST _known_suites)
            message(FATAL_ERROR "DESERT_DEV_SUITES: ${suite} is no suite directory Desert/Tests/<Layer>/${suite}")
        endif()
        if(NOT _suite_layer_${suite} IN_LIST _runner_layers)
            message(FATAL_ERROR "DESERT_DEV_SUITES: ${suite} is in ${_suite_layer_${suite}}, a layer with no runner")
        endif()
        _desert_suite_sources("${_suite_dir_${suite}}" _suite_files)
        list(APPEND _dev_sources ${_suite_files})
        list(APPEND _dev_layers ${_suite_layer_${suite}})
    endforeach()
    if(NOT _dev_sources)
        message(FATAL_ERROR "DESERT_DEV_SUITES names no suite")
    endif()
    desert_add_executable(DevTests SOURCES "${_tests_dir}/TestSupport/RunnerMain.cpp" ${_dev_sources})
    _desert_tests_common_settings(DevTests)
    foreach(layer IN LISTS _layers)
        if(layer IN_LIST _dev_layers)
            cmake_language(CALL _desert_runner_${layer} DevTests)
        endif()
    endforeach()
endif()

# A suite that needs a Vulkan device is named once, here (CI reads build/TestNeedsVulkanDevice.txt).
set(_vulkan_device_suites EngineHost RenderGraphVulkan)
foreach(suite IN LISTS _vulkan_device_suites)
    file(GLOB _homes LIST_DIRECTORIES true "${_tests_dir}/*/${suite}")
    list(LENGTH _homes _count)
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "vulkan_device_suites names ${suite}, which is not exactly one suite directory")
    endif()
endforeach()
list(SORT _vulkan_device_suites)

string(REPLACE ";" "\n" _manifest_text "${_manifest_lines}")
desert_write_generated("${DESERT_ROOT}/build/TestManifest.txt" "${_manifest_text}\n")
set(_vulkan_text "")
if(_vulkan_device_suites)
    string(REPLACE ";" "\n" _vulkan_text "${_vulkan_device_suites}")
    string(APPEND _vulkan_text "\n")
endif()
desert_write_generated("${DESERT_ROOT}/build/TestNeedsVulkanDevice.txt" "${_vulkan_text}")

# Aggregate targets: present in Debug and Release, absent from Shipping. BuildAllTests carries no edges
# (no dependencies of its own).
add_custom_target(BuildAllTests ALL)
# On Windows RunAllTests writes run_tests.bat, the shim that hands the built configuration to
# scripts/Windows/RunTests.ps1 (cmake/WriteRunTestsBat.cmake; the text is data, no line passes through cmd's echo).
# Elsewhere it does nothing: ci.yml runs scripts/MacOS/RunTests.sh itself, so a post-build run here would run the tests twice.
set(_write_runner "")
if(DESERT_IS_WINDOWS)
    set(_write_runner COMMAND "${CMAKE_COMMAND}" -D "OUTPUT=${DESERT_ROOT}/run_tests.bat" -D "CONFIG=$<CONFIG>"
        -P "${DESERT_ROOT}/cmake/WriteRunTestsBat.cmake")
endif()
add_custom_target(RunAllTests ALL ${_write_runner} VERBATIM)
foreach(t BuildAllTests RunAllTests)
    set_target_properties(${t} PROPERTIES FOLDER Tests)
    desert_remove_configuration(${t} Shipping)
endforeach()
# RunAllTests means "every test suite is built" (it depends on every runner), so it runs after them.
if(_test_projects)
    add_dependencies(RunAllTests ${_test_projects})
endif()

list(LENGTH _manifest_lines _suite_count)
list(LENGTH _test_projects _project_count)
message(STATUS "=== Test Configuration ===")
message(STATUS "${_suite_count} suites in ${_project_count} test projects (build/TestManifest.txt)")
message(STATUS "Test reports will be saved to: ${DESERT_TEST_REPORTS_DIR}")

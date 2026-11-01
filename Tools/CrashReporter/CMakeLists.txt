# CrashReporter: DesertCrashReporter links NOTHING of the engine (it must survive the engine's
# crash): GLFW + Dear ImGui compiled straight in with the GLFW/OpenGL3 backends. Fonts copied next to it.
desert_glob(_sources "Source/*.hpp" "Source/*.cpp")
set(_imgui "${DESERT_ROOT}/ThirdParty/ImGui")
desert_add_executable(DesertCrashReporter WINDOWED FOLDER Tools SOURCES ${_sources}
    "${_imgui}/imgui.cpp" "${_imgui}/imgui_draw.cpp" "${_imgui}/imgui_tables.cpp" "${_imgui}/imgui_widgets.cpp"
    "${_imgui}/backends/imgui_impl_glfw.cpp" "${_imgui}/backends/imgui_impl_opengl3.cpp")
desert_external_includes(DesertCrashReporter "${_imgui}" "${_imgui}/backends" "${DESERT_ROOT}/ThirdParty/GLFW/include")
target_link_libraries(DesertCrashReporter PRIVATE GLFW)
desert_platform_defines(DesertCrashReporter)
foreach(font Roboto-Regular.ttf Roboto-Bold.ttf RobotoMono-Regular.ttf fontawesome-webfont.ttf)
    add_custom_command(TARGET DesertCrashReporter POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:DesertCrashReporter>/Resources/Fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DESERT_ROOT}/Editor/Resources/Fonts/${font}"
                "$<TARGET_FILE_DIR:DesertCrashReporter>/Resources/Fonts/"
        VERBATIM)
endforeach()
if(DESERT_IS_WINDOWS)
    target_link_options(DesertCrashReporter PRIVATE /ENTRY:mainCRTStartup)
    target_link_libraries(DesertCrashReporter PRIVATE opengl32 Shell32 Dwmapi User32)
    file(GLOB _rc "Resources/*.rc")
    target_sources(DesertCrashReporter PRIVATE ${_rc})
    set_property(TARGET DesertCrashReporter APPEND PROPERTY VS_RESOURCE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    target_include_directories(DesertCrashReporter PRIVATE "$<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}/Resources>")
elseif(DESERT_IS_MACOS)
    target_link_libraries(DesertCrashReporter PRIVATE "-framework OpenGL" "-framework Cocoa" "-framework IOKit"
        "-framework CoreFoundation" "-framework CoreVideo" "-framework QuartzCore" "-framework Carbon")
else()
    target_link_libraries(DesertCrashReporter PRIVATE GL)
endif()

# UIMockup: renders the editor theme to a PNG with no engine linked; ThemeManager.cpp and Dear
# ImGui (GLFW + OpenGL3 backends) compiled straight in, fonts copied next to it. Absent from Shipping
# (desert_remove_configuration).
desert_glob(_sources "Source/*.hpp" "Source/*.cpp")
set(_imgui "${DESERT_ROOT}/ThirdParty/ImGui")
desert_add_executable(UIMockup FOLDER Tools SOURCES ${_sources}
    "${DESERT_ROOT}/Editor/Source/Editor/Core/ThemeManager.cpp"
    "${_imgui}/imgui.cpp" "${_imgui}/imgui_draw.cpp" "${_imgui}/imgui_tables.cpp" "${_imgui}/imgui_widgets.cpp"
    "${_imgui}/backends/imgui_impl_glfw.cpp" "${_imgui}/backends/imgui_impl_opengl3.cpp")
desert_remove_configuration(UIMockup Shipping)
target_include_directories(UIMockup PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source" "${DESERT_ROOT}/Editor/Source")
desert_external_includes(UIMockup
    "${DESERT_ROOT}/ThirdParty"
    "${_imgui}"
    "${_imgui}/backends"
    "${DESERT_ROOT}/ThirdParty/GLFW/include"
    "${DESERT_ROOT}/ThirdParty/stb/include")
target_link_libraries(UIMockup PRIVATE GLFW)
desert_platform_defines(UIMockup)
foreach(font Roboto-Regular.ttf Roboto-Bold.ttf materialdesignicons-webfont.ttf)
    add_custom_command(TARGET UIMockup POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:UIMockup>/Resources/Fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DESERT_ROOT}/Editor/Resources/Fonts/${font}"
                "$<TARGET_FILE_DIR:UIMockup>/Resources/Fonts/"
        VERBATIM)
endforeach()
if(DESERT_IS_WINDOWS)
    target_link_libraries(UIMockup PRIVATE opengl32)
elseif(DESERT_IS_MACOS)
    target_link_libraries(UIMockup PRIVATE "-framework OpenGL" "-framework Cocoa" "-framework IOKit"
        "-framework CoreFoundation" "-framework CoreVideo" "-framework QuartzCore" "-framework Carbon")
else()
    target_link_libraries(UIMockup PRIVATE GL)
endif()

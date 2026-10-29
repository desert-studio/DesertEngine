# BuildScripts/ThirdParty/ImGuiNodeEditor.lua: imgui-node-editor, C++17, against our ImGui.
set(_root "${DESERT_ROOT}/ThirdParty/imgui-node-editor")
set(_src crude_json.cpp crude_json.h imgui_bezier_math.h imgui_canvas.cpp imgui_canvas.h imgui_extra_math.h
         imgui_node_editor.cpp imgui_node_editor.h imgui_node_editor_api.cpp imgui_node_editor_internal.h)
list(TRANSFORM _src PREPEND "${_root}/")
desert_add_library(ImGuiNodeEditor FOLDER ThirdParty SOURCES ${_src})
set_target_properties(ImGuiNodeEditor PROPERTIES CXX_STANDARD 17)
target_include_directories(ImGuiNodeEditor PRIVATE "${DESERT_ROOT}/ThirdParty/ImGui")
target_link_libraries(ImGuiNodeEditor PRIVATE ImGui)
if(NOT DESERT_IS_WINDOWS)
    # Static CRT on Linux/macOS is meaningless; only PIC carries over.
    set_target_properties(ImGuiNodeEditor PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

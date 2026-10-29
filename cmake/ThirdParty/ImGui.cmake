# BuildScripts/ThirdParty/ImGui.lua: Dear ImGui core + demo, C++17.
# `staticruntime "On"` is carried over as written: the library compiles against the STATIC CRT (/MT, /MTd)
# while everything else uses the DLL CRT. It links only because imgui.cpp includes no STL header, so no
# RuntimeLibrary mismatch record reaches the linker.
set(_root "${DESERT_ROOT}/ThirdParty/ImGui")
set(_src imconfig.h imgui.h imgui.cpp imgui_draw.cpp imgui_internal.h imgui_tables.cpp imgui_widgets.cpp
         imstb_rectpack.h imstb_textedit.h imstb_truetype.h imgui_demo.cpp)
list(TRANSFORM _src PREPEND "${_root}/")
desert_add_library(ImGui FOLDER ThirdParty SOURCES ${_src})
set_target_properties(ImGui PROPERTIES CXX_STANDARD 17
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT DESERT_IS_WINDOWS)
    set_target_properties(ImGui PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

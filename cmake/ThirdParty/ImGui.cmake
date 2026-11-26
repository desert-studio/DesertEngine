# ImGui: Dear ImGui core + demo, C++17. One context per process: a DLL outside Shipping (PROCESS_STATE; IMGUI_API
# export/import: cmake/DesertWorkspace.cmake), static in the Shipping monolith. The DLL CRT like every target.
set(_root "${DESERT_ROOT}/ThirdParty/ImGui")
set(_src imconfig.h imgui.h imgui.cpp imgui_draw.cpp imgui_internal.h imgui_tables.cpp imgui_widgets.cpp
         imstb_rectpack.h imstb_textedit.h imstb_truetype.h imgui_demo.cpp)
list(TRANSFORM _src PREPEND "${_root}/")
desert_add_library(ImGui PROCESS_STATE FOLDER ThirdParty SOURCES ${_src})
set_target_properties(ImGui PROPERTIES CXX_STANDARD 17)
if(NOT DESERT_IS_WINDOWS)
    set_target_properties(ImGui PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

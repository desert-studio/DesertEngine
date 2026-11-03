# Dlib.
# compile only under DESERT_WITH_DLIB, which the editor sets by the same condition).
set(_root "${DESERT_ROOT}/ThirdParty/dlib")
if(EXISTS "${_root}/dlib/all/source.cpp")
    desert_add_library(Dlib FOLDER ThirdParty SOURCES "${_root}/dlib/all/source.cpp")
    set_target_properties(Dlib PROPERTIES CXX_STANDARD 17)
    target_include_directories(Dlib PRIVATE "${_root}")
    target_compile_definitions(Dlib PRIVATE DLIB_NO_GUI_SUPPORT)
    if(NOT DESERT_IS_WINDOWS)
        set_target_properties(Dlib PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

# BuildScripts/ThirdParty/ReflectCpp.lua: reflect-cpp v0.19.0, core + JSON backend + yyjson (the engine only uses rfl::json).
set(_root "${DESERT_ROOT}/ThirdParty/reflect-cpp")
if(NOT EXISTS "${_root}/src/reflectcpp.cpp")
    message(FATAL_ERROR "ThirdParty/reflect-cpp/src is missing. Run scripts/MacOS/Setup.sh (or scripts\\Windows\\Setup.bat) "
                        "to fetch the reflect-cpp v0.19.0 sources.")
endif()
desert_add_library(ReflectCpp FOLDER ThirdParty SOURCES
    "${_root}/src/reflectcpp.cpp" "${_root}/src/reflectcpp_json.cpp" "${_root}/src/yyjson.c")
target_include_directories(ReflectCpp PRIVATE "${_root}/include/rflcpp" "${_root}/include/rflcpp/rfl/thirdparty")
set_target_properties(ReflectCpp PROPERTIES POSITION_INDEPENDENT_CODE ON)

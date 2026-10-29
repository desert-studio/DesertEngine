# DesertHeaderTool: the reflection / subsystem code generator every codegen library runs first.
desert_glob(_sources "*.cpp" "*.hpp")
desert_add_executable(DesertHeaderTool FOLDER Tools SOURCES ${_sources})
target_include_directories(DesertHeaderTool PRIVATE "${DESERT_ROOT}/Tools/Shared" "${DESERT_ROOT}/Desert/Common/Source")
desert_external_includes(DesertHeaderTool ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_COMMONSPECIFIC_INCLUDE_DIRS})
target_link_libraries(DesertHeaderTool PRIVATE Common ReflectCpp)
desert_platform_defines(DesertHeaderTool)

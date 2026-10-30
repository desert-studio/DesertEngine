# FbxMeshSplitter: splits an FBX into per-mesh OBJ files through Assimp.
desert_glob(_sources "*.cpp" "*.hpp")
desert_add_executable(FbxMeshSplitter FOLDER Tools SOURCES ${_sources}
    "${DESERT_ROOT}/Desert/Common/Source/Common/Json/Json.cpp")
target_include_directories(FbxMeshSplitter PRIVATE "${DESERT_ROOT}/Editor/Source" "${DESERT_ROOT}/Desert/Common/Source")
target_compile_definitions(FbxMeshSplitter PRIVATE FMT_HEADER_ONLY)
desert_external_includes(FbxMeshSplitter
    "${DESERT_ROOT}/Editor/ThirdParty/assimp/include" "${DESERT_ROOT}/build/generated/assimp/include"
    ${DESERT_COMMON_INCLUDE_DIRS}) # Common's headers and the roots they include (glm, spdlog/fmt, reflect-cpp): one table
target_link_libraries(FbxMeshSplitter PRIVATE Assimp ReflectCpp)
desert_platform_defines(FbxMeshSplitter)

# FbxMeshSplitter: splits an FBX into per-mesh OBJ files through Assimp.
desert_glob(_sources "*.cpp" "*.hpp")
desert_add_executable(FbxMeshSplitter FOLDER Tools SOURCES ${_sources}
    "${DESERT_ROOT}/Desert/Common/Source/Common/Json/Json.cpp")
target_include_directories(FbxMeshSplitter PRIVATE "${DESERT_ROOT}/Editor/Source" "${DESERT_ROOT}/Desert/Common/Source")
target_compile_definitions(FbxMeshSplitter PRIVATE FMT_HEADER_ONLY)
desert_external_includes(FbxMeshSplitter
    "${DESERT_ROOT}/Editor/ThirdParty/assimp/include" "${DESERT_ROOT}/build/generated/assimp/include"
    "${DESERT_ROOT}/ThirdParty/reflect-cpp/include" "${DESERT_ROOT}/ThirdParty/spdlog/include")
target_link_libraries(FbxMeshSplitter PRIVATE Assimp ReflectCpp)
desert_platform_defines(FbxMeshSplitter)

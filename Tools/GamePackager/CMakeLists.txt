# GamePackager: cooks and packages a project. Links the editor libraries (not whole: nothing
# here needs their self-registering editors), the third-party libraries they use and the engine. The
# MikkTSpace is not linked: mikktspace.cpp is compiled into GeometryCore.
desert_glob(_sources "Source/*.cpp")
desert_add_executable(GamePackager FOLDER Tools SOURCES ${_sources})
target_include_directories(GamePackager PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_engine_includes(GamePackager)
target_include_directories(GamePackager PRIVATE "${DESERT_ROOT}/Editor/Source")
desert_external_includes(GamePackager
    "${DESERT_ROOT}/ThirdParty/stb/include"
    "${DESERT_ROOT}/ThirdParty/openexr/src/lib/OpenEXRCore"
    "${DESERT_ROOT}/build/generated/openexr/include"
    "${DESERT_ROOT}/ThirdParty/Imath/src/Imath"
    ${DESERT_COMMON_INCLUDE_DIRS}
    ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(GamePackager)
desert_link_editor_libs(GamePackager)
target_link_libraries(GamePackager PRIVATE
    Assimp ImGui ImGuiNodeEditor GLFW Optick MeshOptimizer Voro OpenSubdiv Embree OpenEXRCore Dav1d Opus)
desert_link_engine(GamePackager)
if(IS_DIRECTORY "${DESERT_ROOT}/ThirdParty/dlib")
    target_link_libraries(GamePackager PRIVATE Dlib)
endif()
if(DESERT_IS_MACOS)
    target_link_libraries(GamePackager PRIVATE
        Common Jolt LuauCodeGen LuauCompiler LuauAst LuauBytecode LuauVM LuauCommon ReflectCpp
        "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation" "-framework CoreVideo"
        "-framework CoreMedia" "-framework AVFoundation" "-framework QuartzCore" "-framework Foundation")
endif()

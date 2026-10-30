# WorldCook: cooks a world's streaming cells; links the third-party libraries and the engine.
# MikkTSpace is not linked: mikktspace.cpp is compiled into GeometryCore.
desert_glob(_sources "Source/*.cpp")
desert_add_executable(WorldCook FOLDER Tools SOURCES ${_sources})
# Include order: the engine roots, then Tools/Shared.
desert_engine_includes(WorldCook)
target_include_directories(WorldCook PRIVATE "${DESERT_ROOT}/Tools/Shared")
desert_external_includes(WorldCook ${DESERT_COMMON_INCLUDE_DIRS} ${DESERT_DESERTSPECIFIC_INCLUDE_DIRS})
desert_config_platform_defines(WorldCook)
target_link_libraries(WorldCook PRIVATE GLFW Optick MeshOptimizer Voro Compressonator OpenSubdiv Embree OpenEXRCore Dav1d Opus)
desert_link_engine(WorldCook)
if(DESERT_IS_MACOS)
    target_link_libraries(WorldCook PRIVATE
        Common Jolt LuauCodeGen LuauCompiler LuauAst LuauBytecode LuauVM LuauCommon ReflectCpp
        "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation" "-framework CoreVideo"
        "-framework CoreMedia" "-framework AVFoundation" "-framework QuartzCore" "-framework Foundation")
endif()

# ThirdParty/premake5.lua: the vendored libraries, one cmake/ThirdParty/<Name>.cmake per
# BuildScripts/ThirdParty/<Name>.lua, in the same order. Upstream CMakeLists.txt files are not added: each one
# declares targets under other names and with other source sets than the ones the engine links (see each file).
foreach(name GLFW ImGui ImGuiNodeEditor Jolt Lua Luau Optick MeshOptimizer Voro Compressonator OpenSubdiv Embree Dlib Assimp
             OpenEXR Dav1d Opus FidelityFX ReflectCpp SPIRVCross)
    include("${DESERT_ROOT}/cmake/ThirdParty/${name}.cmake")
endforeach()
if(DESERT_IS_WINDOWS)
    include("${DESERT_ROOT}/cmake/ThirdParty/GoogleTest.cmake")
endif()

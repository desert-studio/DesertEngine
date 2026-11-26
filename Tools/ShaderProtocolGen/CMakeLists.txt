# ShaderProtocolGen: writes the generated shader includes from the C++ layout declarations (ShaderLayout.hpp,
# ShaderProtocol.hpp) before the ShaderCompiler module compiles (Desert/Desert/CMakeLists.txt). Links only the CRT:
# its include paths are the source roots and glm, nothing else, so a layout header that includes anything heavier
# (Common, RDG, Vulkan) does not compile here - the tool is the check that layout headers stay light.
desert_sources(_sources "Source" MATCHING "\\.cpp$")
desert_add_executable(ShaderProtocolGen FOLDER Tools SOURCES ${_sources})
target_include_directories(ShaderProtocolGen PRIVATE
    "${DESERT_ROOT}/Desert/Desert/Source"
    "${DESERT_ROOT}/Desert/Landscape/Source"
    "${DESERT_ROOT}/Desert/Water/Source"
    "${DESERT_ROOT}/Desert/VFX/Source")
desert_external_includes(ShaderProtocolGen "${DESERT_ROOT}/ThirdParty/glm")
desert_platform_defines(ShaderProtocolGen)

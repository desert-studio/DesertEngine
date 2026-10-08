#pragma once

// Compiles Editor/Resources/Shaders/Common/ReconstructPosition.glslh (and the Common/ScreenUV.glslh it includes)
// AS C++ — the same text every deferred reader of a world position compiles as GLSL (GBUF1). The arrangement is
// ViewRayReference.hpp's: glm supplies the GLSL types, the include sits in an anonymous namespace.

#include <glm/glm.hpp>

#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::ReconstructPositionRef
{
    namespace
    {
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;
        using vec4 = glm::vec4;
        using mat4 = glm::mat4;

        DESERT_GLSL_AS_CPP_BEGIN
#include <Common/ReconstructPosition.glslh>
        DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::ReconstructPositionRef

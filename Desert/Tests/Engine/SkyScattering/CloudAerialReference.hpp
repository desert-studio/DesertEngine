#pragma once

// Compiles Engine/Content/Shaders/Common/CloudAerial.glslh AS C++ — the cloud half of the aerial
// perspective, the same text Programs/Clouds/CloudRaymarch.shader and Programs/Compute/
// BakeProceduralSky.shader compile as GLSL.
//
// It lives in the SkyScattering suite and not in a cloud one on purpose. What the composition has to be
// right about is a RELATION to the atmosphere — the cloud must end up in the same air the sky beside it
// is in — and this is the only suite that has both sides: SkyScatteringReference.hpp's froxel walk and
// its distant-sky integrator. Tested on its own the composition is three lines of algebra that cannot
// fail; tested against the sky it is the statement the whole term exists to make.
//
// No LUT callbacks and no medium: the header takes radiance in and gives radiance back.

#include <glm/glm.hpp>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::CloudAerialRef
{
    namespace
    {
        using vec3 = glm::vec3;

        using glm::clamp;
        using glm::max;
        using glm::mix;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/CloudAerial.glslh>
             DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::CloudAerialRef

#pragma once

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // The velocity a geometry pass writes for one surface point, the CPU twin of DesertVelocity in
    // Editor/Resources/Shaders/Common/ObjectMotion.glslh (same expression, so a test can pin what the shader
    // writes). Both clip positions are UNJITTERED (ViewFrame rule: velocity never carries the jitter):
    //   clip     = ViewProjection     * World     * p
    //   prevClip = PrevViewProjection * PrevWorld * p
    // The value is current minus previous NDC.xy, so a reprojection reads history at uv - velocity * (0.5, -0.5)
    // under the engine's negative-height viewport (Common/FullscreenTriangle.glslh). A still camera over a still
    // object gives exactly 0, which is also the target's clear value and its RDG fault default (Black).
    [[nodiscard]] inline glm::vec2 VelocityNdc( const glm::vec4& clip, const glm::vec4& prevClip )
    {
        return glm::vec2( clip ) / clip.w - glm::vec2( prevClip ) / prevClip.w;
    }
} // namespace Desert::Graphic

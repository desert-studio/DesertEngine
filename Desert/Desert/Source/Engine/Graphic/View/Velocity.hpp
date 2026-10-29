#pragma once

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // The velocity a geometry pass writes for one surface point, the CPU twin of DesertVelocity in
    // Editor/Resources/Shaders/Common/Velocity.glslh (same expression, so a test can pin what the shader
    // writes). Both clip positions are UNJITTERED (ViewFrame rule: velocity never carries the jitter):
    //   clip     = ViewProjection     * World     * p
    //   prevClip = PrevViewProjection * PrevWorld * p
    // The value is current minus previous NDC.xyz, so a reprojection reads history at uv - velocity.xy * (0.5,
    // -0.5) under the engine's negative-height viewport (Common/FullscreenTriangle.glslh), and finds the surface's
    // device depth of last frame at depth - velocity.z (UE's velocity with VELOCITY_ENCODE_DEPTH: the screen probe
    // gather's history test, Common/ScreenProbeTemporal.glslh). A still camera over a still object gives exactly
    // 0, which is also the target's clear value and its RDG fault default (Black).
    [[nodiscard]] inline glm::vec3 VelocityNdc( const glm::vec4& clip, const glm::vec4& prevClip )
    {
        return glm::vec3( clip ) / clip.w - glm::vec3( prevClip ) / prevClip.w;
    }
} // namespace Desert::Graphic

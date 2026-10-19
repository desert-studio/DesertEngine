#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>

namespace Desert::Graphic
{
    /**
     * THE VIEW'S RAIN OCCLUSION: what stands between a surface and the sky, so a floor under a roof stays dry
     * while the yard beside it wets (Mesh/Surface/SurfaceWeather.glslh DesertSurfaceWetness).
     *
     * UE has no engine rain occlusion: CitySample paints it into masks per asset. The shipped-engine pattern taken
     * here instead is a top-down orthographic depth of the casters (terrain and meshes, the sun cascades' caster
     * cells, drawn by MeshRenderer::RainOcclusionPass) in a square box around the camera, its centre snapped to
     * whole texels so a still world never shimmers, and redrawn only when the camera has left the middle of the
     * box (RefreshFraction of its extent) or its casters changed. A surface is covered when the map holds
     * something above it; it is sampled a texel out along the surface's normal (so a wall facing the open sky is
     * not hidden by its own top) and compared softly over a 2x2 gather.
     *
     * The receivers' projection is STANDARD-Z and orthographic, like the cascades'
     * (MeshRenderer::SetupShadowPass): depth is linear in height, 0 at the top of the box, 1 at its bottom; the
     * map clears to 1 ("nothing overhead").
     */

    // The budget, from the scalability catalogue (Shadows.RainOcclusion*, read in SceneRenderer::BeginScene).
    // MapSize 0 = no map: every surface is open to the sky.
    struct RainOcclusionQuality
    {
        uint32_t MapSize         = 0;    // texels per side
        float    Extent          = 0.0f; // world cm per side, and the depth of the box
        float    RefreshFraction = 0.0f; // the camera moves this fraction of Extent from the centre -> recentre

        [[nodiscard]] bool operator==( const RainOcclusionQuality& ) const = default;
    };

    // std140 mirror of RainOcclusionUB (Mesh/Surface/SurfaceWeather.glslh).
    struct RainOcclusionUniforms
    {
        glm::mat4 WorldToMap = glm::mat4( 1.0f ); // world -> the map's standard-Z clip (uv = ndc.xy * 0.5 + 0.5)
        glm::vec4 Params =
             glm::vec4( 0.0f ); // x = depth range (cm), y = valid (>0.5), z = bias (cm), w = texel (cm)
    };
    static_assert( sizeof( RainOcclusionUniforms ) == 80, "RainOcclusionUB is a mat4 and a vec4" );

    // One fitted box: where it stands and the matrix both the casters and the receivers use.
    struct RainOcclusionBox
    {
        glm::vec3 Centre{ 0.0f };
        glm::mat4 ViewProj = glm::mat4( 1.0f );
        float     Texel    = 0.0f;
    };

    // The box around @p eye: centred on it to the nearest texel, its top half the extent above the centre, its
    // view straight down (-Y; world up is +Y), north (-Z) up the map.
    [[nodiscard]] inline RainOcclusionBox FitRainOcclusionBox( const glm::vec3&            eye,
                                                               const RainOcclusionQuality& quality )
    {
        RainOcclusionBox box;
        box.Texel         = quality.Extent / static_cast<float>( quality.MapSize );
        const auto snap   = [texel = box.Texel]( const float v ) { return std::floor( v / texel ) * texel; };
        box.Centre        = glm::vec3( snap( eye.x ), snap( eye.y ), snap( eye.z ) );
        const float     h = 0.5f * quality.Extent;
        const glm::mat4 view =
             glm::lookAtRH( box.Centre + glm::vec3( 0.0f, h, 0.0f ), box.Centre, glm::vec3( 0.0f, 0.0f, -1.0f ) );
        const glm::mat4 proj = glm::orthoRH_ZO( -h, h, -h, h, 0.0f, quality.Extent );
        box.ViewProj         = proj * view;
        return box;
    }

    // Whether @p eye has left the middle of @p box: further than RefreshFraction * Extent from its centre on any
    // axis.
    [[nodiscard]] inline bool RainOcclusionNeedsRecentre( const RainOcclusionBox& box, const glm::vec3& eye,
                                                          const RainOcclusionQuality& quality )
    {
        const glm::vec3 d     = glm::abs( eye - box.Centre );
        const float     limit = quality.RefreshFraction * quality.Extent;
        return d.x > limit || d.y > limit || d.z > limit;
    }

    // The receivers' block for @p box. The bias is a texel and a half of height: a surface is never hidden by its
    // own depth, a roof a storey up always hides what is under it.
    [[nodiscard]] inline RainOcclusionUniforms PackRainOcclusion( const RainOcclusionBox&     box,
                                                                  const RainOcclusionQuality& quality )
    {
        RainOcclusionUniforms u;
        u.WorldToMap = box.ViewProj;
        u.Params     = glm::vec4( quality.Extent, 1.0f, 1.5f * box.Texel, box.Texel );
        return u;
    }
} // namespace Desert::Graphic

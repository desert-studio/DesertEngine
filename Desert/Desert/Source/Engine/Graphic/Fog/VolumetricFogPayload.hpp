#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
    // The volumetric fog grid's parameter block (UE FVolumetricFogGlobalData), byte for byte the block in
    // Editor/Resources/Shaders/Common/VolumetricFog.glslh. Centimetres and per-centimetre coefficients. Packed per
    // frame per view by VolumetricFogRenderer from ExponentialHeightFogData, the view's ViewFrame (this and the
    // previous frame's camera), the sun and its cascades.
    struct VolumetricFogGpuPayload
    {
        glm::mat4  InvViewProjection{ 1.0f };
        glm::mat4  ViewProjection{ 1.0f };
        glm::mat4  PrevViewProjection{ 1.0f };
        glm::mat4  LightViewProj[4]{ glm::mat4( 1.0f ), glm::mat4( 1.0f ), glm::mat4( 1.0f ), glm::mat4( 1.0f ) };
        glm::uvec4 GridSize{ 1u };      // xyz froxels, w frame index
        glm::vec4  GridZParams{ 0.0f }; // B, O, S, grid far (cm)
        glm::vec4  GridPixel{ 0.0f };   // froxel edge px, render width, height, history valid
        glm::vec4  Jitter{ 0.0f };      // in-froxel offset, w = history weight
        glm::vec4  Camera{ 0.0f };      // position cm, w = phase g
        glm::vec4  CameraForward{ 0.0f };
        glm::vec4  PrevCamera{ 0.0f };
        glm::vec4  PrevCameraForward{ 0.0f };
        glm::vec4  Albedo{ 0.0f };   // rgb, w = extinction scale
        glm::vec4  Emissive{ 0.0f }; // rgb, w = start distance cm
        glm::vec4  Fade{ 0.0f };     // x = near fade-in distance cm
        glm::vec4  Layer0{ 0.0f };   // density /cm, falloff /cm, height cm
        glm::vec4  Layer1{ 0.0f };
        glm::vec4  SunDirection{ 0.0f };    // toward the sun, w = sun present
        glm::vec4  SunRadiance{ 0.0f };     // rgb, w = cascade count
        glm::vec4  CascadeBias{ 0.0f };     // per cascade, map depth units
        glm::vec4  SkyAmbient{ 0.0f };      // rgb CPU-folded sky, w = sample the distant sky light texel
        glm::vec4  SkyAmbientScale{ 0.0f }; // rgb scale on the texel
    };
    static_assert( offsetof( VolumetricFogGpuPayload, LightViewProj ) == 192 );
    static_assert( offsetof( VolumetricFogGpuPayload, GridSize ) == 448 );
    static_assert( offsetof( VolumetricFogGpuPayload, Layer0 ) == 448 + 16 * 11 );
    static_assert( offsetof( VolumetricFogGpuPayload, SkyAmbientScale ) == 448 + 16 * 17 );
    static_assert( sizeof( VolumetricFogGpuPayload ) == 448 + 16 * 18 );

    // Must equal VOLUMETRIC_FOG_PARAMS_BINDING in Common/VolumetricFog.glslh.
    inline constexpr uint32_t kVolumetricFogParamsBinding = 1;

    // UE r.VolumetricFog.DepthDistributionScale: how strongly the slices crowd toward the camera.
    inline constexpr float kVolumetricFogDepthDistributionScale = 32.0f;
    // UE r.VolumetricFog.HistoryWeight.
    inline constexpr float kVolumetricFogHistoryWeight = 0.9f;

    // The grid of one view (Scalability Effects.VolumetricFogGridPixelSize / GridSizeZ).
    struct VolumetricFogQuality
    {
        bool     Enabled    = false; // Effects.VolumetricFog: the machine's ceiling
        uint32_t GridPixels = 16;
        uint32_t GridSizeZ  = 64;
    };

    // PURE (UE GetVolumetricFogGridZParams). B, O, S of slice = log2(depth * B + O) * S such that depth = near
    // lands on slice 0 and depth = far on slice gridSizeZ; w = far.
    [[nodiscard]] inline glm::vec4 MakeVolumetricFogGridZParams( float nearPlane, float farPlane,
                                                                 uint32_t gridSizeZ )
    {
        const float zNear = std::max( nearPlane, 1.0f );
        const float zFar  = std::max( farPlane, zNear * 2.0f );
        const float s     = kVolumetricFogDepthDistributionScale;
        const float o     = ( zFar - zNear * std::exp2( static_cast<float>( gridSizeZ ) / s ) ) / ( zFar - zNear );
        const float b     = ( 1.0f - o ) / zNear;
        return glm::vec4( b, o, s, zFar );
    }

    // Froxel counts of a render target of @p width x @p height pixels.
    [[nodiscard]] inline glm::uvec3 MakeVolumetricFogGridSize( uint32_t width, uint32_t height,
                                                               const VolumetricFogQuality& quality )
    {
        const uint32_t edge = std::max( quality.GridPixels, 1u );
        return glm::uvec3( ( std::max( width, 1u ) + edge - 1u ) / edge,
                           ( std::max( height, 1u ) + edge - 1u ) / edge, std::max( quality.GridSizeZ, 1u ) );
    }
} // namespace Desert::Graphic

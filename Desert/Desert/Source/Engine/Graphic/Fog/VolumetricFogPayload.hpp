#pragma once

#include <Engine/ECS/LocalFogVolumeComponent.hpp>

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
        // The cloud layer's shadow on the sun term (UE VolumetricCloud "Shadow On Atmosphere"): the very
        // CloudShadowUniforms every surface receiver reads (CloudShadowPackUniforms; Params.y = 0 when no layer
        // casts, and the froxels then never fetch the map).
        glm::mat4 CloudShadowWorldToMap{ 1.0f };
        glm::vec4 CloudShadowParams{ 0.0f }; // far depth km, live, border fade uv, strength
        // The view's rain occlusion (Graphic/RainOcclusion.hpp PackRainOcclusion): what covers a froxel from the
        // sky, so a barn's interior gets no sky in-scatter. Params.y = 0 when the view has no map.
        glm::mat4 RainOcclusionWorldToMap{ 1.0f };
        glm::vec4 RainOcclusionParams{ 0.0f }; // depth range cm, valid, bias cm, texel cm
        // x = local fog volumes in the LocalFogVolumesBuffer this frame; yzw unused.
        glm::uvec4 LocalFog{ 0u };
    };
    static_assert( offsetof( VolumetricFogGpuPayload, LightViewProj ) == 192 );
    static_assert( offsetof( VolumetricFogGpuPayload, GridSize ) == 448 );
    static_assert( offsetof( VolumetricFogGpuPayload, Layer0 ) == 448 + 16 * 11 );
    static_assert( offsetof( VolumetricFogGpuPayload, SkyAmbientScale ) == 448 + 16 * 17 );
    static_assert( offsetof( VolumetricFogGpuPayload, CloudShadowWorldToMap ) == 736 );
    static_assert( offsetof( VolumetricFogGpuPayload, CloudShadowParams ) == 800 );
    static_assert( offsetof( VolumetricFogGpuPayload, RainOcclusionWorldToMap ) == 816 );
    static_assert( offsetof( VolumetricFogGpuPayload, RainOcclusionParams ) == 880 );
    static_assert( offsetof( VolumetricFogGpuPayload, LocalFog ) == 896 );
    static_assert( sizeof( VolumetricFogGpuPayload ) == 912 );

    // Must equal VOLUMETRIC_FOG_PARAMS_BINDING in Common/VolumetricFog.glslh.
    inline constexpr uint32_t kVolumetricFogParamsBinding = 1;

    // ONE LOCAL FOG VOLUME as the froxels voxelise it (UE 5.3 FLocalFogVolumeGPUInstanceData), byte for byte
    // the LocalFogVolumeGpu struct of Common/VolumetricFog.glslh. Packed per frame by PackLocalFogVolume from
    // ECS::LocalFogVolumeData and the entity's world transform.
    struct LocalFogVolumeGpu
    {
        // World cm -> the volume's unit space: the unit sphere (Sphere) or the [-1, 1] cube (Box).
        glm::mat4 WorldToVolume{ 1.0f };
        // x = radial extinction at the centre (/cm), y = height-fog extinction at the offset (/cm),
        // z = height falloff (per unit of the volume's half height), w = height offset (unit space, -1..1)
        glm::vec4 Density{ 0.0f };
        glm::vec4 Albedo{ 0.0f };   // rgb albedo, w = shape (0 sphere, 1 box)
        glm::vec4 Emissive{ 0.0f }; // rgb emitted radiance per unit extinction, w = edge fade width (unit space)
    };
    static_assert( offsetof( LocalFogVolumeGpu, Density ) == 64 );
    static_assert( offsetof( LocalFogVolumeGpu, Emissive ) == 96 );
    static_assert( sizeof( LocalFogVolumeGpu ) == 112 );

    // PURE. One authored volume as the froxels read it: @p world is the entity's world transform (cm), the
    // shape's half size Extent is in its local space. Extinctions are authored per metre (UE's units) and
    // carried per centimetre; the height falloff and offset are already unit-space quantities.
    [[nodiscard]] inline LocalFogVolumeGpu PackLocalFogVolume( const ECS::LocalFogVolumeData& data,
                                                               const glm::mat4&               world )
    {
        constexpr float kPerMetreToPerCm = 1.0f / 100.0f;
        const glm::vec3 extent           = glm::max( data.Extent, glm::vec3( 1.0f ) );
        glm::mat4       localToUnit( 1.0f );
        localToUnit[0][0] = 1.0f / extent.x;
        localToUnit[1][1] = 1.0f / extent.y;
        localToUnit[2][2] = 1.0f / extent.z;

        LocalFogVolumeGpu gpu{};
        gpu.WorldToVolume = localToUnit * glm::inverse( world );
        gpu.Density       = glm::vec4( std::max( data.RadialFogExtinction, 0.0f ) * kPerMetreToPerCm,
                                       std::max( data.HeightFogExtinction, 0.0f ) * kPerMetreToPerCm,
                                       std::max( data.HeightFogFalloff, 0.0f ),
                                       std::clamp( data.HeightFogOffset, -1.0f, 1.0f ) );
        const float shape = data.Shape == ECS::LocalFogVolumeShape::Box ? 1.0f : 0.0f;
        gpu.Albedo   = glm::vec4( glm::clamp( data.Albedo, glm::vec3( 0.0f ), glm::vec3( 1.0f ) ), shape );
        gpu.Emissive = glm::vec4( glm::max( data.Emissive, glm::vec3( 0.0f ) ),
                                  std::clamp( data.EdgeFade, 1e-3f, 1.0f ) );
        return gpu;
    }

    // The froxel pass reads at most this many volumes a frame (the buffer's reserve; UE caps its local fog
    // volume instances per view the same way, r.LocalFogVolume.MaxInstanceCount); the gatherer says the excess.
    inline constexpr uint32_t kVolumetricFogMaxLocalVolumes = 64;

    // Must equal VOLUMETRIC_FOG_LOCAL_VOLUMES_BINDING in Programs/Fog/VolumetricFogLightScattering.shader: the
    // pipeline's own buffer (SetStorageBuffer), like the parameter block.
    inline constexpr uint32_t kVolumetricFogLocalVolumesBinding = 14;

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

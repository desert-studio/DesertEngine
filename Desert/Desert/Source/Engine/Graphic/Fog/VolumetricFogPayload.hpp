#pragma once

#include <Engine/ECS/LocalFogVolumeComponent.hpp>
#include <Engine/Graphic/Fog/FogLayout.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
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

#pragma once

#include <Engine/ECS/HeterogeneousVolumeComponent.hpp>

#include <Common/Content/AssetGuid.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
    // ONE HETEROGENEOUS VOLUME as the lighting cache, the march and the froxels' shadow read it, byte for byte the
    // HeterogeneousVolumeGpu struct of Common/HeterogeneousVolume.glslh (std430). Packed per frame by
    // PackHeterogeneousVolume from ECS::HeterogeneousVolumeData and the entity's world transform.
    struct HeterogeneousVolumeGpu
    {
        glm::mat4 LocalToWorld{ 1.0f }; // the unit box [-1, 1]^3 -> world (Extent and the entity's transform)
        glm::mat4 WorldToLocal{ 1.0f }; // world -> the unit box
        glm::vec4 Medium{ 0.0f };       // x = extinction per cm, y = phase g, z = coverage, w = edge fade
        glm::vec4 Albedo{ 0.0f };       // rgb = albedo, w = 1 / NoiseScale (per cm)
        glm::vec4 HalfSize{ 0.0f };     // xyz = world half size of the box along its axes (cm), w = unused
        glm::vec4 Wind{ 0.0f };         // xyz = local advection velocity (cm/s), w = the noise clock (s)
        glm::vec4 Emissive{ 0.0f };     // rgb = emission per unit extinction, w = peak temperature (K)
        glm::vec4 Blackbody{ 0.0f };    // x = blackbody intensity, y = lighting-cache slab (set by the renderer)
        // The baked source (DensitySource BakedSparse), set by the renderer's residency once the asset's bricks
        // are in the shared atlas: x = the asset's first entry in the page-table buffer, yzw = pages per axis.
        // yzw = 0 = the procedural source (also a baked volume whose asset is not resident yet).
        glm::uvec4 Sparse{ 0u };
        glm::vec4  SparseScale{ 0.0f }; // xyz = the asset's voxels per axis, w = its peak temperature (K)
    };
    static_assert( offsetof( HeterogeneousVolumeGpu, Medium ) == 128 );
    static_assert( offsetof( HeterogeneousVolumeGpu, Blackbody ) == 208 );
    static_assert( offsetof( HeterogeneousVolumeGpu, Sparse ) == 224 );
    static_assert( sizeof( HeterogeneousVolumeGpu ) == 256 );

    // One gathered volume as the ECS hands it to the renderer: the packed GPU block plus the baked asset it
    // samples (null guid = procedural). The renderer resolves the guid against its brick residency per frame
    // and writes Sparse / SparseScale; the GPU never sees the guid.
    struct HeterogeneousVolumeInstance
    {
        HeterogeneousVolumeGpu     Gpu;
        Common::Content::AssetGuid SparseVolume;
    };

    // Volumes per view (UE draws every heterogeneous volume of the view; here the lighting-cache atlas is sized
    // for this many slabs, allocated the first frame a volume is on screen). The gatherer says the excess by name.
    inline constexpr uint32_t kHeterogeneousVolumeMaxVolumes = 8;
    // The lighting cache's voxels per axis per volume (UE r.HeterogeneousVolumes.LightingCache, its default
    // downsample of a 128-voxel volume is 32): RGBA16F, 32^3 x 8 slabs = 2 MiB.
    inline constexpr uint32_t kHeterogeneousVolumeCacheResolution = 32;
    // The pipelines' own buffers (SetStorageBuffer takes the number): the volumes, read by the cache, the march
    // and - for the froxels' sun shadow - VolumetricFogLightScattering.shader; and the march's parameter block.
    inline constexpr uint32_t kHeterogeneousVolumesBinding      = 15;
    inline constexpr uint32_t kHeterogeneousVolumeParamsBinding = 17;
    // The resident baked volumes' page tables (HETEROGENEOUS_VOLUME_PAGES_BINDING of
    // Common/HeterogeneousVolume.glslh), the cache's and the march's own buffer like the two above.
    inline constexpr uint32_t kSparseVolumePagesBinding = 18;

    // The march's parameter block (HvParams of Common/HeterogeneousVolume.glslh).
    struct HeterogeneousVolumeMarchGpu
    {
        glm::uvec4 Counts{ 0u };    // x = volumes, y = frame index (jitter), z = history valid, w = max steps
        glm::vec4  Target{ 0.0f };  // xy = half-res size, zw = full render size (pixels)
        glm::vec4  History{ 0.0f }; // x = history weight, yzw unused
        // The volumes' screen rectangle (uv min xy, max zw, CPU-projected boxes): outside it the march and the
        // upsample write "no medium" without marching, so the cost follows the volumes' screen size.
        glm::vec4 Rect{ 0.0f };
    };
    static_assert( sizeof( HeterogeneousVolumeMarchGpu ) == 64 );

    // A noise clock wraps here (s) so the advection offset never loses float precision over a long session; the
    // wrap is one jump every ~17 minutes per volume.
    inline constexpr float kHeterogeneousVolumeClockWrap = 1024.0f;

    [[nodiscard]] inline HeterogeneousVolumeGpu PackHeterogeneousVolume( const ECS::HeterogeneousVolumeData& data,
                                                                         const glm::mat4& world, float clock )
    {
        const glm::vec3        extent = glm::max( data.Extent, glm::vec3( 1.0f ) );
        HeterogeneousVolumeGpu gpu{};
        // The unit box scaled to the authored half size, then placed by the entity (its scale folded in).
        gpu.LocalToWorld =
             world * glm::mat4( glm::vec4( extent.x, 0.0f, 0.0f, 0.0f ), glm::vec4( 0.0f, extent.y, 0.0f, 0.0f ),
                                glm::vec4( 0.0f, 0.0f, extent.z, 0.0f ), glm::vec4( 0.0f, 0.0f, 0.0f, 1.0f ) );
        gpu.WorldToLocal = glm::inverse( gpu.LocalToWorld );
        // UE's authored extinction is per metre; one world unit is a centimetre.
        gpu.Medium =
             glm::vec4( std::max( data.Extinction, 0.0f ) * 0.01f, glm::clamp( data.PhaseG, -0.9f, 0.9f ),
                        glm::clamp( data.Coverage, 0.0f, 1.0f ), glm::clamp( data.EdgeFade, 0.001f, 1.0f ) );
        gpu.Albedo   = glm::vec4( glm::clamp( data.Albedo, glm::vec3( 0.0f ), glm::vec3( 1.0f ) ),
                                  1.0f / std::max( data.NoiseScale, 1.0f ) );
        gpu.HalfSize = glm::vec4( glm::length( glm::vec3( gpu.LocalToWorld[0] ) ),
                                  glm::length( glm::vec3( gpu.LocalToWorld[1] ) ),
                                  glm::length( glm::vec3( gpu.LocalToWorld[2] ) ), 0.0f );
        gpu.Wind     = glm::vec4( data.Wind, clock );
        gpu.Emissive =
             glm::vec4( glm::max( data.Emissive, glm::vec3( 0.0f ) ), std::max( data.Temperature, 0.0f ) );
        gpu.Blackbody = glm::vec4( std::max( data.BlackbodyIntensity, 0.0f ), 0.0f, 0.0f, 0.0f );
        return gpu;
    }
} // namespace Desert::Graphic

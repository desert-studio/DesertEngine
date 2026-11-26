#pragma once

#include <Engine/ECS/HeterogeneousVolumeComponent.hpp>
#include <Engine/Graphic/Fog/FogLayout.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
    // One gathered volume as the ECS hands it to the renderer: the packed GPU block plus the baked asset it
    // samples (the `.dsvol`'s AssetHandle value, 0 = procedural). The renderer resolves the handle against its
    // brick residency per view frame and writes Sparse / SparseScale; the GPU never sees the handle.
    struct HeterogeneousVolumeInstance
    {
        HeterogeneousVolumeGpu Gpu;
        uint64_t               SparseVolume = 0;
    };

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

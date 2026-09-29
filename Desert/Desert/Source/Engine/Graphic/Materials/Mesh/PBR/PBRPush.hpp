#pragma once

#include <glm/glm.hpp>

#include <Engine/Assets/Mesh/PBRSurfaceParams.hpp>

namespace Desert::Graphic
{
    // One per-object PBR material entry as it lives in the GPU `Materials[]` storage buffer (std430, binding 2
    // in PBR.glsl.frag), indexed per draw by the push-constant `MaterialIndex`. NOTE: this is NOT a push
    // constant itself (the push constant only carries Transform + MaterialIndex) — the name is historical.
    // Layout must match `struct GpuMaterial` in PBR.glsl.frag.
    struct PBRGpuMaterial
    {
        glm::vec4 AlbedoAO;           // rgb = albedo, a = ambient occlusion
        glm::vec4 MetalRoughEmission; // x = metallic, y = roughness, z = emission strength, w = alpha cutoff
        glm::vec4 EmissionColor;      // rgb = emission color, a = mask channel (0 = u_OpacityTexture.r, 1 = .a)
        glm::vec4 ExtraParams;        // xy = UV tiling, z = IOR, w = 1 -> mesh does NOT receive sun shadows
        glm::vec4 GlassTint;          // rgb = glass tint, a = transmission (0 = opaque)
    };

    // Packs the reflected material data into the GPU entry — the single mapping data -> shader.
    inline PBRGpuMaterial BuildPBRGpuMaterial( const Assets::PBRSurfaceParams& d )
    {
        const glm::vec2 tiling = d.UVTiling.value_or( glm::vec2( 1.0f ) ); // absent -> no tiling
        return { glm::vec4( glm::vec3( d.AlbedoColor ), d.AOStrength ),
                 glm::vec4( d.MetallicFactor, d.RoughnessFactor, d.EmissiveIntensity, d.AlphaCutoff ),
                 glm::vec4( glm::vec3( d.EmissiveColor ), d.MaskFromAlbedoAlpha() ? 1.0f : 0.0f ),
                 glm::vec4( tiling.x, tiling.y, d.IOR, 0.0f ),
                 glm::vec4( glm::vec3( d.GlassTint ), d.Transmission ) };
    }
} // namespace Desert::Graphic

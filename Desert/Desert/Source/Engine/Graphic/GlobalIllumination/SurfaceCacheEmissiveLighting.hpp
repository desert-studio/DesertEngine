#pragma once

// THE CONTRACT OF THE SURFACE CACHE'S EMISSIVE LIGHTING PASS (GI-TEMPORAL). Written by the architect; no package
// edits it. Shader twin: Editor/Resources/Shaders/Common/EmissiveCardLighting.glslh (the layer, the blend, the push
// block and the resource names are stated there in full).

#include <cstdint>
#include <string_view>

namespace Desert::Graphic::SurfaceCache::EmissiveLighting
{
    // The programs (Editor/Resources/Shaders/Programs/SurfaceCache/), one variant per GI tracer.
    inline constexpr std::string_view kShaderName      = "SurfaceCacheEmissiveLighting";
    inline constexpr std::string_view kAsyncShaderName = "SurfaceCacheEmissiveLightingAsync";
    // The graph names of the pass and of the layer (RGBA16F, the atlas' size, owned by SurfaceCacheRenderer).
    inline constexpr std::string_view kPassName  = "SurfaceCache.EmissiveLighting";
    inline constexpr std::string_view kLayerName = "SurfaceCache.EmissiveLighting";
    // The names the programs bind the layer by: the direct lighting reads, the async pass writes.
    inline constexpr std::string_view kSampledParameter = "AtlasEmissiveLightingTexture";
    inline constexpr std::string_view kStorageParameter = "AtlasEmissiveLightingImage";

    // SurfaceEmissiveLightingPush.EmissiveWeights.x = 1 / relights accumulated: a scene whose emissive list has no
    // dynamic triangle settles over kStaticRelightsAccumulated relights of a page; one with dynamic triangles
    // follows them within kDynamicRelightsAccumulated (UE r.LumenScene.Radiosity.Temporal.MaxFramesAccumulated).
    inline constexpr float kStaticRelightsAccumulated  = 32.0f;
    inline constexpr float kDynamicRelightsAccumulated = 4.0f;

    [[nodiscard]] constexpr float SampleWeight( const std::uint32_t dynamicTriangles )
    {
        return 1.0f / ( dynamicTriangles == 0u ? kStaticRelightsAccumulated : kDynamicRelightsAccumulated );
    }
} // namespace Desert::Graphic::SurfaceCache::EmissiveLighting

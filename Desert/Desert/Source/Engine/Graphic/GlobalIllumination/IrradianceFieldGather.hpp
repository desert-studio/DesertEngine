#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GlobalIllumination/WorldRadianceCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <memory>

// THE IRRADIANCE FIELD GATHER (GI-PROBE-SW) — the final gather of GIMethod::ProbeVolume, ported from UE Lumen's
// LumenIrradianceFieldGather.cpp / .usf (r.Lumen.IrradianceFieldGather: Lumen's cheap final gather, the radiance
// cache configured as an irradiance field and read per pixel). One compute pass a frame
// (Programs/GlobalIllumination/IrradianceFieldGather.shader), after the world radiance cache's update: each G-buffer
// pixel interpolates the cache's irradiance probes at its position — diffuse at its normal, a rough specular lobe
// about its reflection direction — into the two targets the deferred composite reads in place of the sky's.
//
// WHY THE SAME CACHE, NOT A DDGI VOLUME OF ITS OWN (lead decision, 10-15). The world radiance cache already IS a
// probe grid on the global distance field: clipmaps around the camera, sparse, relocated out of geometry,
// visibility-weighted by its distance moments, lit by every light and the sky through the one hit contract. A
// separate probe volume would trace the same rays into a second grid. ProbeVolume is therefore the traced pipeline
// with the screen probes switched off (GlobalIllumination::PipelineFor) and the cache re-traced less often
// (RadianceCache::MakeRadianceCacheInputs) — the cost of SoftwareTraced minus its per-frame screen traces.
//
// WHERE THIS DIFFERS FROM UE (for the owner):
//   * UE's irradiance field is a SECOND radiance cache with its own shape (64³ cells, 5000 cm clipmap 0) beside
//     the screen probes' one; here both methods read ONE cache, so switching method keeps every traced probe.
//   * The read is visibility-weighted (DDGI's Chebyshev test on the probes' distance moments); UE's irradiance
//     field interpolates trilinearly and leaks through walls a cell thick.
//   * Rough specular is the cache's cosine lobe about the reflection direction; UE copies the diffuse.
//   * No temporal pass: the cache is already accumulated per probe, and the read is deterministic per pixel.
namespace Desert::Graphic
{
    class Image2D;
    class Shader;
    class ComputePipeline;
} // namespace Desert::Graphic

namespace Desert::Graphic::GlobalIllumination
{
    // This frame's graph resources the gather reads.
    struct IrradianceFieldGatherInputs
    {
        RDG::TextureRef    GBufferDepth;
        RDG::TextureRef    GBufferB; // normal
        RDG::BufferRef     GIFrame;  // GIFramePayload, "GIFrameUB"
        RadianceCacheFrame RadianceCache;
        glm::uvec2         Extent{ 0u };
    };

    // What the deferred composite reads (ViewTargetFormats::kGIFinalGather, the G-buffer's extent).
    struct IrradianceFieldGatherFrame
    {
        RDG::TextureRef Diffuse;  // rgb = irradiance / pi — "u_GIDiffuse"
        RDG::TextureRef Specular; // rgb = the cosine lobe about the reflection direction — "u_GISpecular"
    };

    class IrradianceFieldGather
    {
    public:
        IrradianceFieldGather();
        ~IrradianceFieldGather();
        IrradianceFieldGather( const IrradianceFieldGather& )            = delete;
        IrradianceFieldGather& operator=( const IrradianceFieldGather& ) = delete;

        // The shader and its pipeline. An error names what is missing.
        [[nodiscard]] Common::BoolResultStr Initialize();

        // The gather of one frame, its pass added to @p graph after the radiance cache's update.
        [[nodiscard]] Common::ResultStr<IrradianceFieldGatherFrame>
        AddPasses( RDG::Builder& graph, const WorldRadianceCache::ImportImage& import,
                   const IrradianceFieldGatherInputs& inputs );

    private:
        [[nodiscard]] Common::BoolResultStr EnsureImages( glm::uvec2 extent );

        std::shared_ptr<Shader>          m_Shader;
        std::shared_ptr<ComputePipeline> m_Pipeline;
        mutable ShaderBindingLayoutCache m_Layout;
        std::shared_ptr<Image2D>         m_Diffuse;
        std::shared_ptr<Image2D>         m_Specular;
        glm::uvec2                       m_Extent{ 0u };
    };
} // namespace Desert::Graphic::GlobalIllumination

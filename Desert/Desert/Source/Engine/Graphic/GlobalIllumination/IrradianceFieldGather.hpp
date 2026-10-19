#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GlobalIllumination/WorldRadianceCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <memory>

// THE IRRADIANCE FIELD GATHER (GI-PROBE-SW) — the final gather of GIMethod::ProbeVolume, ported from UE Lumen's
// LumenIrradianceFieldGather.cpp / .usf (r.Lumen.IrradianceFieldGather: Lumen's cheap final gather, the radiance
// cache configured as an irradiance field and read per pixel). Two compute passes a frame, after the world
// radiance cache's update (Programs/GlobalIllumination/IrradianceField*.shader):
//   Gather    at HALF resolution (the G-buffer pixel at each even corner): the cache's irradiance probes
//             interpolated at the sample's position — diffuse at its normal, a rough specular lobe about its
//             reflection direction, one probe lookup for both;
//   Upsample  to the G-buffer's extent, each pixel from the four samples around it weighted by distance and
//             normal (UE's screen probe upsample weights), into the two targets the deferred composite reads in
//             place of the sky's.
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
//   * The read runs at half resolution and is upsampled (UE reads the irradiance field per pixel): the probes are
//     a cell apart (~1 m), so a 2-pixel grid loses nothing the field holds, and the full-resolution read was the
//     method's largest cost (2.2-2.3 ms of a 1440x860 frame on an M1 Pro, against the screen probes' 0.65 ms
//     integrate).
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
        RDG::TextureRef Backface; // rgb = irradiance / pi from -N — "u_GIBackface"
    };

    class IrradianceFieldGather
    {
    public:
        IrradianceFieldGather();
        ~IrradianceFieldGather();
        IrradianceFieldGather( const IrradianceFieldGather& )            = delete;
        IrradianceFieldGather& operator=( const IrradianceFieldGather& ) = delete;

        // The two shaders and their pipelines. An error names what is missing.
        [[nodiscard]] Common::BoolResultStr Initialize();

        // The gather of one frame, its two passes added to @p graph after the radiance cache's update.
        [[nodiscard]] Common::ResultStr<IrradianceFieldGatherFrame>
        AddPasses( RDG::Builder& graph, const WorldRadianceCache::ImportImage& import,
                   const IrradianceFieldGatherInputs& inputs );

    private:
        [[nodiscard]] Common::BoolResultStr EnsureImages( glm::uvec2 extent );

        enum Program : uint32_t
        {
            Gather = 0,
            Upsample,
            ProgramCount
        };

        std::array<std::shared_ptr<Shader>, ProgramCount>          m_Shaders;
        std::array<std::shared_ptr<ComputePipeline>, ProgramCount> m_Pipelines;
        mutable std::array<ShaderBindingLayoutCache, ProgramCount> m_Layouts;
        std::shared_ptr<Image2D>                                   m_HalfDiffuse; // .a = distance to the camera
        std::shared_ptr<Image2D>                                   m_HalfSpecular;
        std::shared_ptr<Image2D>                                   m_HalfBackface;
        std::shared_ptr<Image2D>                                   m_Diffuse;
        std::shared_ptr<Image2D>                                   m_Specular;
        std::shared_ptr<Image2D>                                   m_Backface;
        glm::uvec2                                                 m_Extent{ 0u };
    };
} // namespace Desert::Graphic::GlobalIllumination

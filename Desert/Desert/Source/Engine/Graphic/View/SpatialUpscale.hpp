#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Graphic::RDG
{
    class Builder;
}

// SCAL-SPATIAL1 — THE SPATIAL UPSCALE AND THE POST SHARPEN (UE: the secondary / spatial upscale and
// r.Tonemapper.Sharpen; ours are ports of AMD FidelityFX FSR1 EASU and RCAS, MIT, notice in the shaders).
//
// WHERE THEY RUN. At the one point where the frame's RenderExtent scene colour becomes the post chain's input
// (SceneRenderer::AddFrameTemporal, View/TemporalUpscaler.hpp WHERE IT RUNS): a frame below 100 % without a
// temporal method (Upscaler::Spatial, TemporalMethod::None) is brought to OutputExtent there by SpatialUpscale,
// exactly where TAAU would have, so every pass after it (overlays, auto exposure, bloom, tonemap, FXAA / SMAA)
// runs at OutputExtent whatever upscaled the frame — the ViewTargetSet::Output rule keeps one source of truth
// for the post extent. UE upscales spatially after tonemap instead; that would make the post chain's extent
// depend on which upscaler ran (Render for spatial, Output for temporal), two answers to one question. Both
// passes work on linear HDR through a reversible tonemap (c / (1 + max(c)), inverted after), the space FSR1
// asks for, so a bright highlight cannot dominate the edge direction or ring.
//
// The SHARPEN follows the resolve (TAA, TAAU or the spatial upscale: SharpenRuns), on the resolved colour at
// OutputExtent, outside any history (TAA's history stays unsharpened), and reads Resolution.Sharpness (0-100):
// 0 adds no node, 100 is RCAS at its full strength.
namespace Desert::Graphic
{
    // True when this frame's resolve is the spatial upscale: below 100 % with no temporal method
    // (SelectTemporalMethod gives TemporalMethod::None at an Upscale split only under Upscaler::Spatial).
    [[nodiscard]] bool IsSpatialUpscale( const ViewFrame& frame );

    // True when the post sharpen runs this frame: @p sharpnessPercent above 0 and a resolve to sharpen (a temporal
    // method, or the spatial upscale). Native / SSAA frames without a temporal method have none.
    [[nodiscard]] bool SharpenRuns( const ViewFrame& frame, int sharpnessPercent );

    // The binding layouts of SpatialUpscale.shader and Sharpen.shader as the graph sees them; the ShaderCacheKey
    // suite pins both against the compiled shaders' reflection.
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& SpatialUpscaleLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& SharpenLayout();

    // SpatialUpscale.shader's push constants.
    struct SpatialUpscalePush
    {
        int32_t SourceWidth       = 0;
        int32_t SourceHeight      = 0;
        int32_t DestinationWidth  = 0;
        int32_t DestinationHeight = 0;
    };
    static_assert( sizeof( SpatialUpscalePush ) == 16 );

    // Sharpen.shader's push constants. Amount = Resolution.Sharpness / 100, the RCAS lobe scale in [0, 1].
    struct SharpenPush
    {
        int32_t Width  = 0;
        int32_t Height = 0;
        float   Amount = 0.0f;
        int32_t Pad    = 0;
    };
    static_assert( sizeof( SharpenPush ) == 16 );

    // EASU: RenderExtent -> OutputExtent in one compute node "SpatialUpscale" writing the transient
    // "SpatialUpscale.Output" (RGBA16F, OutputExtent, the returned ref). Owns its compute pipeline (made on first
    // record), one per SceneRenderer like SupersampleResolve. Error (named) when the frame is not a spatial
    // upscale (IsSpatialUpscale) or @p sceneColor is invalid.
    class SpatialUpscale
    {
    public:
        SpatialUpscale();
        ~SpatialUpscale();
        SpatialUpscale( const SpatialUpscale& )            = delete;
        SpatialUpscale& operator=( const SpatialUpscale& ) = delete;

        [[nodiscard]] Common::ResultStr<RDG::TextureRef> AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                                                    RDG::TextureRef sceneColor ) const;

    private:
        struct PipelineHolder;
        std::unique_ptr<PipelineHolder> m_Pipeline;
    };

    // RCAS: one compute node "Sharpen" writing the transient "Sharpen.Output" (RGBA16F, the extent of @p color,
    // the returned ref). Error (named) when SharpenRuns is false for (@p frame, @p sharpnessPercent) or @p color
    // is invalid.
    class Sharpen
    {
    public:
        Sharpen();
        ~Sharpen();
        Sharpen( const Sharpen& )            = delete;
        Sharpen& operator=( const Sharpen& ) = delete;

        [[nodiscard]] Common::ResultStr<RDG::TextureRef> AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                                                    RDG::TextureRef color,
                                                                    int             sharpnessPercent ) const;

    private:
        struct PipelineHolder;
        std::unique_ptr<PipelineHolder> m_Pipeline;
    };
} // namespace Desert::Graphic

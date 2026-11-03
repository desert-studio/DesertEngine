#pragma once

#include <Engine/Graphic/View/TemporalUpscaler.hpp>
#include <Engine/Graphic/View/UpscalerExposure.hpp>

// AMD FIDELITYFX FSR 3.1 — a vendor ITemporalUpscaler on the Vulkan backend (UE: the FSR3 plugin's
// FFXFSR3TemporalUpscaler). Windows builds only: the FidelityFX SDK (ThirdParty/FidelityFX-SDK, v1.1.4 = FSR
// 3.1.4) is compiled by cmake/ThirdParty/FidelityFX.cmake, whose shader compiler is a Windows binary; the
// build compiles this directory there only, and kFsrBuilt (API/UpscalerBuild.hpp) tells everyone else.
//
// WHAT THE ENGINE HANDS FSR, AND IN WHICH CONVENTION (FidelityFX docs/techniques/super-resolution-upscaler.md):
//   * colour — the jittered linear HDR scene colour at RenderExtent (FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE);
//   * depth — reversed-Z DEPTH32F (FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED: cameraNear is the FAR plane);
//   * motion vectors — the engine's velocity, current minus previous NDC, unjittered (Velocity.hpp). FSR wants
//     previous minus current in render pixels, y down: motionVectorScale = (-Width / 2, +Height / 2) (the engine's
//     NDC is y-up under its negative-height viewport, Common/FullscreenTriangle.glslh);
//   * exposure — UpscalerExposure's 1x1 multiplier (1 / adapted luminance, the engine TAA's own definition);
//   * reactive — TemporalUpscalerInputs::ResponsiveAA, the responsive-translucency coverage (R8, 0..1);
//   * jitter — FSR's OWN sequence (ffxFsr3UpscalerGetJitterPhaseCount / GetJitterOffset) through
//     JitterSequenceLength / JitterPixels, which SceneViewState writes into ViewFrame. FSR's offset (x, y) is
//     applied to the projection as NDC (2x / W, -2y / H); in the engine's y-up NDC that is JitterPixels (x, -y),
//     and the dispatch's jitterOffset is (x, y) again.
//
// THE CONTEXT IS PER VIEW: the ITemporalUpscalerHistory this object returns (Fsr3History) owns the FidelityFX
// backend interface, the FSR context and its three "shared" resources. It is created on the view's first FSR
// frame and recreated when the render extent, the output extent (the quality mode is the ratio of the two) or the
// depth convention changes; the old one is destroyed through VulkanAllocator::RT_DestroyLater, after the frames
// that recorded with it. FSR's history is internal: HistoryDescs is empty and ViewFrame::HistoryReset is the
// dispatch's `reset`.
namespace Desert::Graphic
{
    class Fsr3Upscaler final : public ITemporalUpscaler
    {
    public:
        Fsr3Upscaler();
        ~Fsr3Upscaler() override;

        [[nodiscard]] std::string_view DebugName() const override
        {
            return "FSR3";
        }
        [[nodiscard]] TemporalMethod Method() const override
        {
            return TemporalMethod::FSR;
        }

        // Native ("FSR Native AA") and Upscale; never Supersample — FSR reconstructs at or above its input size.
        [[nodiscard]] bool Supports( const ResolutionSplit& split ) const override;
        // Empty: FSR keeps its history inside its context.
        [[nodiscard]] std::vector<HistoryTextureDesc>
        HistoryDescs( const ResolutionSplit&                      split,
                      const Common::Scalability::ResolvedQuality& quality ) const override;

        // ffxFsr3UpscalerGetJitterPhaseCount( Render.Width, Output.Width ): ceil( 8 * ratio^2 ).
        [[nodiscard]] uint32_t JitterSequenceLength( const ResolutionSplit& split ) const override;
        // ffxFsr3UpscalerGetJitterOffset, y negated into the engine's NDC orientation (header comment).
        [[nodiscard]] glm::vec2 JitterPixels( uint32_t index, uint32_t length ) const override;
        // AMD's rule for FSR (super-resolution-upscaler.md "Mipmap biasing", applied by UE's FSR3 plugin):
        // log2( Render.Width / Output.Width ) - 1, at Native AA too (-1 there).
        [[nodiscard]] float MaterialMipBias( const ResolutionSplit& split ) const override;

        // Two compute nodes: "UpscalerExposure" and "FSR3.Upscale" (the FidelityFX dispatch, writing the returned
        // OutputExtent RGBA16F transient "FSR3.Output"). Errors: a missing input, a supersample split, the FSR
        // context failing to create (the FfxErrorCode named), a device that is not Vulkan.
        [[nodiscard]] Common::ResultStr<TemporalUpscalerOutputs>
        AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                   const TemporalUpscalerInputs& inputs ) const override;

    private:
        UpscalerExposure m_Exposure;
    };
} // namespace Desert::Graphic

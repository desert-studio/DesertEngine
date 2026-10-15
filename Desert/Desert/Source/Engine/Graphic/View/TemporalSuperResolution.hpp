#pragma once

#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>

// Ported from UE 5.8 Engine/Source/Runtime/Renderer/Private/PostProcess/TemporalSuperResolution.cpp (pass chain
// :2420-3470, history sample-count parameters :3218-3227, BaseScalability.ini [AntiAliasingQuality@N] r.TSR.*
// rows), adapted: our RDG and ITemporalUpscaler instead of FRDGBuilder / ITemporalUpscaler plugin, the r.TSR.*
// console variables read from Scalability rows (Parameter::Tsr*), GLSL compute in
// Programs/TemporalSuperResolution/ instead of the TSR*.usf set, no wave intrinsics / 16-bit types / texture
// atomics (MoltenVK: the parallax scatter is a storage-buffer atomicMax), no thin-geometry detection, no history
// resurrection, no reprojection field, no lens distortion (REMAINDER-TSR.md lists each with its UE file).
namespace Desert::Graphic
{
    class ComputePipeline;

    // TSR — THE ENGINE'S TEMPORAL SUPER RESOLUTION (UE: TemporalSuperResolution.cpp / TSR*.usf), the
    // vendor-neutral temporal upscaler (TemporalMethod::TSR, Upscaler::TSR): Mac (MoltenVK), non-RTX, non-AMD.
    // Resolves Upscale splits only (at 100 % the native method is TAA). Pure compute, the same pass chain as UE:
    //
    //   TSR.ClearPrevTextures   Render   the parallax scatter buffer -> 0 (UE TSRClearPrevTextures.usf)
    //   TSR.DilateVelocity      Render   3x3 closest depth -> dilated velocity + closest depth; scatters each
    //                                    pixel's closest depth to where it was last frame (UE
    //                                    TSRDilateVelocity.usf, TSRClosestOccluder.ush ScatterClosestOccluder)
    //   TSR.DecimateHistory     Render   reprojects the guide and moire histories, parallax disocclusion against
    //                                    the scatter (UE TSRDecimateHistory.usf, ProcessPrevAtomicTexturesSamples)
    //   TSR.RejectShading       Render   shading rejection of the reprojected guide against the input in UE's
    //                                    guide colour space (UE TSRRejectShading.usf, TSRShadingAnalysis.ush
    //                                    MeasureRejection), flicker-tolerant when the quality row says so; writes
    //                                    the new guide and moire histories
    //   TSR.SpatialAntiAliasing Render   edge-directed sub-pixel offset for rejected pixels (UE
    //                                    TSRSpatialAntiAliasing.usf); not added at RejectionAntiAliasing 0
    //   TSR.UpdateHistory       History  reprojects the high-frequency history, clamps it to the input box,
    //                                    accumulates with UE's validity / sample-count weights (UE
    //                                    TSRUpdateHistory.usf); writes the output too when History == Output
    //   TSR.ResolveHistory      Output   history -> output filter when History > Output (UE TSRResolveHistory.usf)
    //
    // THREE HISTORIES (HistoryDescs): the colour history at the HISTORY extent (Output x
    // TsrHistoryScreenPercentage), and the guide and moire histories at the RENDER extent. All RGBA16F: the only
    // storage formats Vulkan guarantees without shaderStorageImageExtendedFormats are used.
    //
    // QUALITY (UE sg.AntiAliasingQuality r.TSR.* rows): TsrHistoryScreenPercentage changes the history extent
    // (a new desc: the history is recreated and reset); TsrHistoryUpdateQuality, TsrShadingRejectionFlickering
    // and TsrRejectionAntiAliasing select shader variants (one pipeline each, made on first use) and keep it.
    class TemporalSuperResolution final : public ITemporalUpscaler
    {
    public:
        TemporalSuperResolution();
        ~TemporalSuperResolution() override;

        [[nodiscard]] std::string_view DebugName() const override
        {
            return "TSR";
        }
        [[nodiscard]] TemporalMethod Method() const override
        {
            return TemporalMethod::TSR;
        }

        [[nodiscard]] bool Supports( const ResolutionSplit& split ) const override;
        [[nodiscard]] std::vector<HistoryTextureDesc>
        HistoryDescs( const ResolutionSplit&                      split,
                      const Common::Scalability::ResolvedQuality& quality ) const override;
        [[nodiscard]] Common::ResultStr<TemporalUpscalerOutputs>
        AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                   const TemporalUpscalerInputs& inputs ) const override;

    private:
        // The pipeline of @p shaderName with @p defines ("" for the default program), made on first use (record
        // time). Null with @p error set when it does not compile or the pipeline is refused.
        [[nodiscard]] std::shared_ptr<ComputePipeline>
        PipelineFor( const char* shaderName, const std::string& defines, std::string& error ) const;

        mutable std::mutex                                              m_PipelineMutex;
        mutable std::map<std::string, std::shared_ptr<ComputePipeline>> m_Pipelines;
    };

    // The TSR quality of a ResolvedQuality (UE r.TSR.* of the level's sg.AntiAliasingQuality).
    struct TsrQuality
    {
        int  HistoryScreenPercentage    = 100; // 100..200
        int  HistoryUpdateQuality       = 2;   // 0..3
        bool ShadingRejectionFlickering = true;
        int  RejectionAntiAliasing      = 1; // 0 off, 1..2
    };
    [[nodiscard]] TsrQuality ReadTsrQuality( const Common::Scalability::ResolvedQuality& quality );

    // The colour history's extent at @p split: Output x HistoryScreenPercentage / 100, at least 1 per side.
    [[nodiscard]] RDG::Extent3D TsrHistoryExtent( const ResolutionSplit& split, const TsrQuality& quality );

    // The pass chain's shared parameters, the C++ side of `TSRParamsBuffer` in
    // Programs/TemporalSuperResolution/TSRCommon.glslh (std430). One buffer per frame, read by every pass.
    struct TsrParams
    {
        glm::mat4 InvViewProjection;  // this frame, unjittered: uv (+ device depth) -> world
        glm::mat4 PrevViewProjection; // previous frame, unjittered: world -> previous clip
        glm::mat4 PrevInvProjection;  // previous frame, unjittered: previous device depth -> view depth
        glm::vec2 RenderSize;         // pixels
        glm::vec2 HistorySize;        // pixels
        glm::vec2 OutputSize;         // pixels
        glm::vec2 JitterUv;           // this frame's jitter in uv: JitterNdc * (0.5, -0.5)
        float     HistoryValid = 0.0f;
        // UE TemporalSuperResolution.cpp :3220-3226, from r.TSR.History.SampleCount (16),
        // r.TSR.ShadingRejection.SampleCount (2), r.TSR.Velocity.WeightClampingSampleCount (4) and
        // r.TSR.Velocity.WeightClampingPixelSpeed (1) at this history / output ratio.
        float HistorySampleCount                = 16.0f;
        float HistoryHisteresis                 = 1.0f / 16.0f;
        float WeightClampingRejection           = 0.0f;
        float WeightClampingPixelSpeedAmplitude = 0.0f;
        float InvWeightClampingPixelSpeed       = 1.0f;
        float InputToHistoryFactor              = 1.0f; // History.Width / Render.Width
        float FlickeringFramePeriod             = 2.0f; // UE r.TSR.ShadingRejection.Flickering.Period
        // World-space radius of one render pixel at view depth d: d * DepthPixelRadiusScale + DepthPixelRadiusBias
        // (perspective: scale 1 / (H * |P11|), ortho: bias 1 / (H * |P11|); UE
        // GetDepthPixelRadiusForProjectionType).
        float DepthPixelRadiusScale      = 0.0f;
        float DepthPixelRadiusBias       = 0.0f;
        float EnableFlickeringHeuristic  = 1.0f; // TsrQuality::ShadingRejectionFlickering (UE: a uniform)
        float RejectionAntiAliasingSteps = 4.0f; // TsrQuality::RejectionAntiAliasing 0 / 1 / 2 -> 0 / 4 / 8
    };
    static_assert( offsetof( TsrParams, PrevInvProjection ) == 128 );
    static_assert( offsetof( TsrParams, RenderSize ) == 192 );
    static_assert( offsetof( TsrParams, JitterUv ) == 216 );
    static_assert( offsetof( TsrParams, HistoryValid ) == 224 );
    static_assert( offsetof( TsrParams, FlickeringFramePeriod ) == 252 );
    static_assert( offsetof( TsrParams, DepthPixelRadiusScale ) == 256 );
    static_assert( sizeof( TsrParams ) == 272 );

    // The parameters of this frame's chain (pure).
    [[nodiscard]] TsrParams MakeTsrParams( const ViewFrame& frame, RDG::Extent3D history,
                                           const TsrQuality& quality );

    // The binding layouts of the TSR shaders as the graph sees them — the C++ mirror of each shader's
    // declarations; the backend re-validates them against the compiled reflection at record time.
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrClearPrevTexturesLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrDilateVelocityLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrDecimateHistoryLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrRejectShadingLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrSpatialAntiAliasingLayout();
    // @p writesOutput: the TSR_WRITE_OUTPUT=1 variant (History == Output) also binds u_Output.
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>&
    TsrUpdateHistoryLayout( bool writesOutput );
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrResolveHistoryLayout();
} // namespace Desert::Graphic

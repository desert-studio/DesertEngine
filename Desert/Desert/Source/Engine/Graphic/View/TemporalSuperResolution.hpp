#pragma once

#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/View/TemporalUpscaler.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Ported from UE 5.8 Engine/Source/Runtime/Renderer/Private/PostProcess/TemporalSuperResolution.cpp (pass chain
// :2420-3470, history sample-count parameters :3218-3227, BaseScalability.ini [AntiAliasingQuality@N] r.TSR.*
// rows), adapted: our RDG and ITemporalUpscaler instead of FRDGBuilder / ITemporalUpscaler plugin, the r.TSR.*
// console variables read from Scalability rows (Parameter::Tsr*), GLSL compute in
// Programs/TemporalSuperResolution/ instead of the TSR*.usf set, no wave intrinsics / 16-bit types / texture
// atomics (MoltenVK: the parallax scatter is a storage-buffer atomicMax), the persistent resurrection frames as
// HistoryLifetime::Persistent textures filled by a copy pass instead of UE's rolling Texture2DArray slices
// (:1561-1665, :2270-2400), no thin-geometry detection, no lens distortion (REMAINDER-TSR.md lists each with its UE
// file).
namespace Desert::Graphic
{
    class ComputePipeline;

    // TSR — THE ENGINE'S TEMPORAL SUPER RESOLUTION (UE: TemporalSuperResolution.cpp / TSR*.usf), the
    // vendor-neutral temporal upscaler (TemporalMethod::TSR, Upscaler::TSR): Mac (MoltenVK), non-RTX, non-AMD.
    // Resolves Upscale and Native splits (AntiAliasingMethod::TSR at any percent <= 100). Pure compute, the same
    // pass chain as UE:
    //
    //   TSR.MeasureFlickeringLuma Render (AddOpaquePasses, before the translucency) with thin geometry's
    //                                    intensity lines: the opaque scene's luma (UE
    //                                    TSRMeasureFlickeringLuma.usf)
    //   TSR.ClearPrevTextures   Render   the parallax scatter buffer -> 0 (UE TSRClearPrevTextures.usf)
    //   TSR.DilateVelocity      Render   3x3 closest depth -> dilated velocity + closest depth; scatters each
    //                                    pixel's closest depth to where it was last frame (UE
    //                                    TSRDilateVelocity.usf, TSRClosestOccluder.ush ScatterClosestOccluder);
    //                                    with r.TSR.ReprojectionField the reprojection jacobian and the
    //                                    depth-anti-aliased reprojection boundary (UE
    //                                    TSRDepthVelocityAnalysis.ush)
    //   TSR.DecimateHistory     Render   reprojects the guide and moire histories, parallax disocclusion against
    //                                    the scatter (UE TSRDecimateHistory.usf,
    //                                    ProcessPrevAtomicTexturesSamples); with r.TSR.Resurrection the persistent
    //                                    frame's guide too
    //   TSR.DetectThinGeometry  Render   with the project's r.TSR.ThinGeometryDetection
    //   (Project::RendererSettings):
    //                                    sub-pixel geometry from the base passes' coverage, the depth's thin edges
    //                                    and the coverage history (UE TSRDetectThinGeometry.usf) -> the
    //                                    thin-geometry relaxation weight and this frame's coverage history
    //   TSR.RejectShading       Render   shading rejection of the reprojected guide against the input in UE's
    //                                    guide colour space (UE TSRRejectShading.usf, TSRShadingAnalysis.ush
    //                                    MeasureRejection), flicker-tolerant when the quality row says so; history
    //                                    resurrection where the persistent frame matches better; with thin
    //                                    geometry its clamp box relaxed by TSR.ThinGeometry (UE
    //                                    DIM_THIN_GEOMETRY_DETECTION); writes the new guide and moire histories
    //                                    and the spatial anti-aliaser's luma and mask
    //   TSR.SpatialAntiAliasing Render   UE's edge search on the LDR luma -> the sub-pixel offset and the noise
    //                                    filtering of masked pixels (UE TSRSpatialAntiAliasing.usf / .ush)
    //   TSR.UpdateHistory       History  reprojects the high-frequency history (through the reprojection field,
    //                                    or from the persistent frame where resurrected), clamps it to the input
    //                                    box, accumulates with UE's validity / sample-count weights (UE
    //                                    TSRUpdateHistory.usf); writes the output too when History == Output
    //   TSR.ResolveHistory      Output   history -> output filter when History > Output (UE TSRResolveHistory.usf)
    //   TSR.RecordPersistentFrame        on every kTsrPersistentFrameInterval-th frame since the history reset
    //   (r.TSR.
    //                                    Resurrection): this frame's colour and guide -> the oldest persistent
    //                                    frame
    //
    // THREE HISTORIES (HistoryDescs): the colour history at the HISTORY extent (Output x
    // TsrHistoryScreenPercentage), and the guide and moire histories at the RENDER extent; with r.TSR.Resurrection
    // also kTsrPersistentFrameCount persistent colour frames and as many persistent (linear) guide frames
    // (HistoryLifetime::Persistent); with the project's r.TSR.ThinGeometryDetection last the coverage history at
    // the RENDER extent (UE History.CoverageArray). All RGBA16F / R32F / R32_UINT: the only storage formats Vulkan
    // guarantees without shaderStorageImageExtendedFormats are used.
    //
    // QUALITY (UE sg.AntiAliasingQuality r.TSR.* rows): TsrHistoryScreenPercentage and TsrResurrection change the
    // history descs (recreated and reset); TsrHistoryUpdateQuality and TsrRejectionAntiAliasing select shader
    // variants (one pipeline each, made on first use); TsrShadingRejectionFlickering and TsrReprojectionField are
    // uniforms, as in UE.
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
        // TSR.MeasureFlickeringLuma (UE TSRMeasureFlickeringLuma.usf, AddTSRMeasureFlickeringLuma in the main
        // renderer): with thin-geometry detection's intensity lines (TsrThinGeometryIntensityLines), the luma of
        // the opaque scene in UE's guide space, RenderExtent R32F; no pass otherwise.
        [[nodiscard]] Common::ResultStr<RDG::TextureRef>
        AddOpaquePasses( RDG::Builder& graph, const ViewFrame& frame,
                         const TemporalUpscalerOpaqueInputs& inputs ) const override;

        // The permutation of a program's defines ("NAME=VALUE;NAME=VALUE"), cooked: ShippedByCook - the one
        // statement PipelineFor acquires and ProgramVariants lists.
        [[nodiscard]] static Core::ShaderVariant DefinesVariant( const std::string& defines );
        // Every varying TSR program's permutation any quality can dispatch, cooked by the package (PackageCook
        // CookShaderVariants).
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants();

    private:
        // The pipeline of @p shaderName with @p defines ("" for the default program), made on first use (record
        // time). Null with @p error set when it does not compile or the pipeline is refused.
        [[nodiscard]] std::shared_ptr<ComputePipeline>
        PipelineFor( const char* shaderName, const std::string& defines, std::string& error ) const;

        mutable std::mutex                                              m_PipelineMutex;
        mutable std::map<std::string, std::shared_ptr<ComputePipeline>> m_Pipelines;
    };

    // UE r.TSR.Resurrection.PersistentFrameCount (2) and r.TSR.Resurrection.PersistentFrameInterval (31), both
    // project constants in UE (not scalability rows).
    inline constexpr uint32_t kTsrPersistentFrameCount    = 2;
    inline constexpr uint64_t kTsrPersistentFrameInterval = 31;

    // The TSR quality of a ResolvedQuality (UE r.TSR.* of the level's sg.AntiAliasingQuality).
    struct TsrQuality
    {
        int  HistoryScreenPercentage    = 100; // 100..200
        int  HistoryUpdateQuality       = 2;   // 0..3
        bool ShadingRejectionFlickering = true;
        int  RejectionAntiAliasing      = 1; // 0 off, 1..2
        bool ReprojectionField          = true;
        bool Resurrection               = true;
        // Not a quality row: the PROJECT's r.TSR.ThinGeometryDetection (Project::RendererSettings, UE
        // DefaultEngine.ini), read with the rows so every TSR decision comes from one struct.
        bool ThinGeometryDetection = false;
    };
    [[nodiscard]] TsrQuality ReadTsrQuality( const Common::Scalability::ResolvedQuality& quality );

    // The colour history's extent at @p split: Output x HistoryScreenPercentage / 100, at least 1 per side.
    [[nodiscard]] RDG::Extent3D TsrHistoryExtent( const ResolutionSplit& split, const TsrQuality& quality );

    // UE TSRSpatialAntiAliasing.usf CONFIG_ITERATIONS of r.TSR.RejectionAntiAliasingQuality (DIM_QUALITY_PRESET
    // 1 -> 3, 2 -> 8); 0 at quality 0 (the pass writes the zero offset).
    [[nodiscard]] int TsrSpatialAntiAliasingIterations( const TsrQuality& quality );

    // One recorded persistent frame (UE FTSRHistory::ViewMatrices / SceneColorPreExposures per slice).
    struct TsrPersistentFrame
    {
        bool      Recorded = false;
        uint64_t  Frame    = 0;             // the frame count since the history reset it was recorded on
        glm::mat4 ViewProjection{ 1.0f };   // that frame's unjittered ViewProjection
        float     PreExposure = 1.0f;       // that frame's ViewFrame::PreExposure (UE SceneColorPreExposures)
    };

    // What TSR keeps per view between frames (SceneViewState::UpscalerHistory): the frame count since the
    // history reset and the persistent frames' records (UE FTSRHistory: AccumulatedFrameCount,
    // LastFrameRollingIndex, ViewMatrices[]). The textures themselves are HistoryDescs' (TemporalHistory).
    class TsrHistory final : public ITemporalUpscalerHistory
    {
    public:
        [[nodiscard]] std::string_view DebugName() const override
        {
            return "TSR";
        }

        uint64_t                                                   AccumulatedFrames = 0; // this frame's count
        std::array<TsrPersistentFrame, kTsrPersistentFrameCount> Frames{};
        uint32_t                                                   NextSlot = 0; // the oldest (overwritten next)
    };

    // The persistent frame to resurrect from this frame (UE GetResurrectionFrameRollingIndex: the oldest recorded
    // one), or -1: none recorded, or the oldest is the previous frame (UE bCanResurrectHistory).
    [[nodiscard]] int TsrResurrectionSlot( const TsrHistory& previous, uint64_t thisFrame );

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
        float     EnableFlickeringHeuristic = 1.0f; // TsrQuality::ShadingRejectionFlickering (UE: a uniform)
        float     ReprojectionField         = 1.0f; // TsrQuality::ReprojectionField (UE bReprojectionField)
        glm::mat4 InvProjection{ 1.0f };            // this frame, unjittered
        glm::mat4 Projection{ 1.0f };               // this frame, unjittered
        // This frame's unjittered clip -> the resurrection frame's clip: its ViewProjection x InvViewProjection
        // (UE ClipToResurrectionClip); identity while ResurrectionEnabled is 0.
        glm::mat4 ClipToResurrectionClip{ 1.0f };
        // UE :2531: square(max(r.TSR.ReprojectionField.AntiAliasPixelSpeed (0.125) / (Render / Output), 1 / 64)).
        float ReprojectionFieldAntiAliasVelocityThreshold = 0.0f;
        float ResurrectionEnabled                         = 0.0f; // a persistent frame is resurrectable
        float ThinGeometryDetection                       = 0.0f; // TsrQuality::ThinGeometryDetection
        float ThinGeometryFrameIndex                      = 0.0f; // TsrHistory::AccumulatedFrames mod 64
        float PreExposure                                 = 1.0f; // ViewFrame::PreExposure
        // PreExposure / PrevPreExposure (UE HistoryPreExposureCorrection): the colour history's correction.
        float HistoryPreExposureCorrection = 1.0f;
        // PreExposure / the resurrected frame's (UE ResurrectionPreExposureCorrection); 1 without resurrection.
        float ResurrectionPreExposureCorrection = 1.0f;
        float PreExposurePad                    = 0.0f;
    };
    static_assert( offsetof( TsrParams, PrevInvProjection ) == 128 );
    static_assert( offsetof( TsrParams, RenderSize ) == 192 );
    static_assert( offsetof( TsrParams, JitterUv ) == 216 );
    static_assert( offsetof( TsrParams, HistoryValid ) == 224 );
    static_assert( offsetof( TsrParams, FlickeringFramePeriod ) == 252 );
    static_assert( offsetof( TsrParams, DepthPixelRadiusScale ) == 256 );
    static_assert( offsetof( TsrParams, ReprojectionField ) == 268 );
    static_assert( offsetof( TsrParams, InvProjection ) == 272 );
    static_assert( offsetof( TsrParams, ClipToResurrectionClip ) == 400 );
    static_assert( offsetof( TsrParams, ReprojectionFieldAntiAliasVelocityThreshold ) == 464 );
    static_assert( offsetof( TsrParams, PreExposure ) == 480 );
    static_assert( sizeof( TsrParams ) == 496 );

    // The parameters of this frame's chain (pure). @p resurrection: the persistent frame resurrected from this
    // frame, null for none.
    [[nodiscard]] TsrParams MakeTsrParams( const ViewFrame& frame, RDG::Extent3D history, const TsrQuality& quality,
                                           const TsrPersistentFrame* resurrection );

    // The binding layouts of the TSR shaders as the graph sees them — the C++ mirror of each shader's
    // declarations; the backend re-validates them against the compiled reflection at record time.
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrClearPrevTexturesLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrDilateVelocityLayout();
    // @p thinGeometry: the TSR_THIN_GEOMETRY=1 variant also reprojects the coverage history.
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrDecimateHistoryLayout( bool thinGeometry );
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>&
    TsrDetectThinGeometryLayout( bool intensityLines );
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrMeasureFlickeringLumaLayout();
    // UE DIM_VARYING_RANGE_DETECTION / DIM_INTENSITY_LINE_DETECTION of TSR.DetectThinGeometry, from UE's
    // r.TSR.ThinGeometryDetection.Coverage.ShadingRange (3, VaryingRange) and .MinKeepLineContrast (0.30) at their
    // defaults, as UE derives them (TemporalSuperResolution.cpp :2747-2760): the varying range needs the
    // flickering heuristic or the intensity lines; the intensity lines need the varying range.
    [[nodiscard]] bool TsrThinGeometryVaryingRange( const TsrQuality& quality );
    [[nodiscard]] bool TsrThinGeometryIntensityLines( const TsrQuality& quality );
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>&
    TsrRejectShadingLayout( bool thinGeometry );
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrSpatialAntiAliasingLayout();
    // @p writesOutput: the TSR_WRITE_OUTPUT=1 variant (History == Output) also binds OutputTexture.
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>&
    TsrUpdateHistoryLayout( bool writesOutput );
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrResolveHistoryLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& TsrRecordPersistentFrameLayout();
} // namespace Desert::Graphic

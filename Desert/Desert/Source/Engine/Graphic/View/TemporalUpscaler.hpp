#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <vector>

namespace Desert::Graphic::RDG
{
    class Builder;
}

// TAA1 — THE TEMPORAL UPSCALER INTERFACE (UE: ITemporalUpscaler / UE::Renderer::Private::ITemporalUpscaler).
//
// One interface for every pass that turns the jittered RenderExtent scene colour (plus depth, velocity and its own
// history) into the anti-aliased OutputExtent scene colour. The engine's own TAA (same extent) and TAAU (below
// 100 %) are the first two implementations; a vendor upscaler (FSR, DLSS, XeSS, MetalFX) is a later implementation
// behind the same interface and needs no change here — which is why nothing below names a vendor, a vendor
// quality mode, or a vendor resource. What every implementation needs from the engine is exactly what UE hands
// its upscalers: colour, depth, velocity, the jitter, the extents, the reset flag, the exposure.
//
// WHERE IT RUNS IN THE FRAME. After everything that lights the scene at RenderExtent (deferred composite, generic
// and skinned forward geometry, glass, SSR, fog, clouds, particles — the Transparency phase), and BEFORE the post
// chain (auto exposure, bloom, light shafts, lens flare, tonemap), which then runs at OutputExtent on the upscaled
// image. The Debug and UI phases (editor grid, gizmos, collider lines, the UI canvas) move AFTER it, drawn with
// the unjittered ViewFrame::ViewProjection, so they are neither jittered nor smeared by the history. Under SSAA
// (Split.Mode == Supersample) the temporal pass, if any, runs at RenderExtent and the fixed downsample pass
// (SupersampleResolve) follows it at the same point; without a temporal method the downsample runs alone.
namespace Desert::Graphic
{
    struct TemporalUpscalerInputs
    {
        RDG::TextureRef SceneColor; // linear HDR, RenderExtent, jittered
        RDG::TextureRef SceneDepth; // reversed-Z DEPTH32F, RenderExtent, jittered (single-sample: resolved)
        RDG::TextureRef Velocity;   // kVelocityFormat, RenderExtent
        // Last frame's exposure (AutoExposure's 1x1 output, read in the shader): the resolve weights samples in a
        // tonemapped space (UE: HdrWeight), and the exposure must be the one the history was resolved under.
        RDG::TextureRef Exposure; // AutoExposureRenderer previous adapted luminance, 1x1 (AutoExposure.Previous)
        std::span<const HistoryRefs> History; // one per HistoryDescs() entry, same order
    };

    struct TemporalUpscalerOutputs
    {
        RDG::TextureRef SceneColor; // linear HDR, OutputExtent (Native/Upscale) or RenderExtent (Supersample)
    };

    class ITemporalUpscaler
    {
    public:
        virtual ~ITemporalUpscaler() = default;

        [[nodiscard]] virtual std::string_view DebugName() const = 0; // pass names, logs
        [[nodiscard]] virtual TemporalMethod   Method() const    = 0;

        // Whether this implementation can resolve @p split (TAA: Render == Output or Supersample; TAAU: Upscale).
        // SceneViewState::BeginFrame refuses a mismatch with both values named.
        [[nodiscard]] virtual bool Supports( const ResolutionSplit& split ) const = 0;

        // The histories this method carries between frames, at this split. Empty for an implementation whose
        // history is internal to it (it then reads only ViewFrame::HistoryReset).
        [[nodiscard]] virtual std::vector<HistoryTextureDesc>
        HistoryDescs( const ResolutionSplit& split ) const = 0;

        // Adds the method's passes. The written history (HistoryRefs::Current) must be written by these passes
        // only; the output may alias it (TAA's output IS its new history) — the implementation says so by
        // returning the same ref. Errors are a malformed call (a missing input, a history count different from
        // HistoryDescs); a pass that fails while the graph executes is an RDG-FAULT1 pass fault, not an error
        // here.
        [[nodiscard]] virtual Common::ResultStr<TemporalUpscalerOutputs>
        AddPasses( RDG::Builder& graph, const ViewFrame& frame, const TemporalUpscalerInputs& inputs ) const = 0;
    };

    // The implementations this build has, one per TemporalMethod other than None. Null for None. The objects are
    // stateless (all state is in SceneViewState), so one instance per SceneRenderer is enough.
    [[nodiscard]] std::unique_ptr<ITemporalUpscaler> CreateTemporalUpscaler( TemporalMethod method );

    // TWO EXTENTS PER VIEW (TAA1-B step 6). Every view target follows one of them: the RENDER set is what the
    // scene draws into before the temporal resolve (scene target, G-buffer, velocity, depth resolve, silhouette
    // mask, overdraw, outline) and is ResolutionSplit::Render; the OUTPUT set is what the post chain writes after
    // it (tonemap, FXAA, SMAA, the final image, the overlay target) and is ResolutionSplit::Output.
    enum class ViewTargetSet
    {
        Render,
        Output,
    };
    [[nodiscard]] ViewExtent ViewTargetSetExtent( ViewTargetSet set, const ResolutionSplit& split );

    // What one view renders this frame: the split and the temporal method that resolves it.
    struct ViewResolution
    {
        ResolutionSplit Split;
        TemporalMethod  Method = TemporalMethod::None;
        // The upscaler this view's percent runs (Scalability UpscalerForScale): the frame's ViewInputs::Upscaler.
        // Not the setting's resolved one - a viewport override can sit on the other side of 100 %.
        Common::Scalability::Upscaler Upscaler = Common::Scalability::Upscaler::None;
        // Not empty when the requested scale was clamped to one the method's upscaler supports: why (the caller
        // logs it; this function is pure).
        std::string Clamped;
    };

    // THE ONE PER-VIEW RESOLUTION FUNCTION: the resolved setting's percent, replaced by the editor viewport's
    // override when it has one (@p viewportOverridePercent), split against @p output, the method chosen for it
    // (SelectTemporalMethod: its refusal is this function's error), then clamped by the upscaler that implements
    // that method (@p upscalerFor: the view's object for a method, null for None): a split the upscaler does not
    // Support falls back to native scale (100 %) when that is supported, else the named error.
    [[nodiscard]] Common::ResultStr<ViewResolution>
    ResolveViewResolution( ViewExtent output, int settingPercent, std::optional<int> viewportOverridePercent,
                           const Common::Scalability::PathAntiAliasing&                     antiAliasing,
                           Common::Scalability::Upscaler                                    upscaler,
                           const std::function<const ITemporalUpscaler*( TemporalMethod )>& upscalerFor );

    // The fixed SSAA downsample (Split.Mode == Supersample): RenderExtent -> OutputExtent, a separable Catmull-Rom
    // (bicubic, B=0 C=0.5) reconstruction — not a box: at a non-integer ratio (150 %) a box filter aliases the
    // very edges SSAA was bought to smooth. Not an ITemporalUpscaler: it has no history, no jitter and no
    // velocity.
    //
    // An object and not a free function because it owns its compute pipeline (made on first record): a pipeline
    // held by a function-local static would be destroyed after the device at exit. One per SceneRenderer, like
    // the temporal upscaler. Two compute nodes, each named after the transient it writes:
    // "SupersampleResolve.Horizontal" (Render -> Output.Width x Render.Height) and "SupersampleResolve.Output"
    // (-> OutputExtent, the returned ref). Error (named) when the split is not
    // Supersample or
    // @p sceneColor is invalid.
    class SupersampleResolve
    {
    public:
        SupersampleResolve();
        ~SupersampleResolve();
        SupersampleResolve( const SupersampleResolve& )            = delete;
        SupersampleResolve& operator=( const SupersampleResolve& ) = delete;

        [[nodiscard]] Common::ResultStr<RDG::TextureRef> AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                                                    RDG::TextureRef sceneColor ) const;

    private:
        struct PipelineHolder;
        std::unique_ptr<PipelineHolder> m_Pipeline;
    };
} // namespace Desert::Graphic

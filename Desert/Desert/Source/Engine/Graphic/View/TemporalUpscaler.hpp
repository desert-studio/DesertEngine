#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/View/PathTracedGuides.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <glm/glm.hpp>

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
    // What an implementation carries between frames that is NOT a texture the engine allocates (UE:
    // ITemporalUpscaler::IHistory, held by FSceneViewState): a vendor SDK's context and the resources it owns.
    // Opaque to the engine. The view (SceneViewState::UpscalerHistory) keeps the one the last frame returned and
    // hands it back as TemporalUpscalerInputs::PrevHistory; the implementation keeps it, or replaces it when it is
    // not its own or no longer fits (extent, quality). Destruction may not free a GPU object a frame in flight
    // still records with: the implementation defers that (VulkanAllocator::RT_DestroyLater).
    class ITemporalUpscalerHistory
    {
    public:
        virtual ~ITemporalUpscalerHistory() = default;

        // NO BYTE ACCESSOR (RT-LEDGER-3): device memory a history holds is a resource-ledger row - our textures
        // through the allocator, a vendor context through its own row (ResourceKind::VendorUpscaler) - and the
        // view's budget reads the rows.
        [[nodiscard]] virtual std::string_view DebugName() const = 0;
    };

    struct TemporalUpscalerInputs
    {
        RDG::TextureRef SceneColor; // linear HDR, RenderExtent, jittered
        RDG::TextureRef SceneDepth; // reversed-Z DEPTH32F, RenderExtent, jittered (single-sample: resolved)
        RDG::TextureRef Velocity;   // kVelocityFormat, RenderExtent
        // How much of each pixel responsive translucency covers (ViewTargetFormats::kResponsiveAA, RenderExtent):
        // the current frame's weight rises to UE's 1/4 by it.
        RDG::TextureRef ResponsiveAA;
        // Last frame's exposure (AutoExposure's 1x1 output, read in the shader): the resolve weights samples in a
        // tonemapped space (UE: HdrWeight), and the exposure must be the one the history was resolved under.
        RDG::TextureRef Exposure; // AutoExposureRenderer previous adapted luminance, 1x1 (AutoExposure.Previous)
        // What the base passes say about each pixel's material (ViewTargetFormats::kThinGeometryCoverage,
        // RenderExtent, ViewRasterTargets.hpp CreateViewThinGeometryCoverage): 0 nothing / unlit, 0.5 a regular
        // surface, 1 two-sided foliage (UE TSRMeasureCoverage.usf EncodeThinGeometryMaterial). TSR's thin-geometry
        // detection reads it.
        RDG::TextureRef ThinGeometryCoverage;
        // What AddOpaquePasses returned this frame (invalid when it added none): TSR's luma of the opaque scene
        // (UE FlickeringLuma, TSRMeasureFlickeringLuma.usf), measured before the translucency composited into
        // SceneColor.
        RDG::TextureRef OpaqueLuma;
        // The graph's System.Black (RDGSystemTextures.hpp): what an implementation binds where an input of its own
        // does not exist this frame (UE GSystemTextures BlackDummy), never another input standing in for it.
        RDG::TextureRef              Black;
        // WHAT AN IMPLEMENTATION THAT TAKES THE LIGHTING SIGNALS RAW READS BESIDE THE COLOUR (TakesRawLightingSignals;
        // UE: the scene textures the DLSS plugin's Ray Reconstruction reads its guides from). The surface each
        // pixel shows, from the deferred G-buffer (RenderExtent; invalid on the forward path): base colour with
        // metallic in a (ViewTargetFormats::kGBufferA), world normal with roughness in a (kGBufferB). The
        // split-sum table the deferred composite weighs the reflection with (Renderer::GetBRDFTexture), so a
        // guide's specular reflectance is the one the colour was made with.
        RDG::TextureRef GBufferAlbedo;
        RDG::TextureRef GBufferNormal;
        RDG::TextureRef BrdfLut;
        // The traced reflection's ray per pixel (FrameTransients::ReflectionRay: SSR's full-resolution trace ray,
        // xyz the world point the reflection came from, w log2 pdf, -126 no ray), raw; invalid where the view
        // traced no reflection this frame.
        RDG::TextureRef ReflectionRay;
        // On a path-traced view whose paths go to the upscaler raw (System::PathTracer, THE DENOISER): the paths'
        // own first-surface guides, which replace the G-buffer's and the reflection ray above - the colour was
        // made by the paths, so only their factors demodulate it exactly. Invalid on every other view.
        PathTracedGuides PathGuides;
        std::span<const HistoryRefs> History; // one per HistoryDescs() entry, same order
        // The NewHistory this view's last frame returned; null on the view's first frame, after
        // SceneViewState::Reset and while the method is None. May be another implementation's (a method change):
        // an implementation uses only its own.
        std::shared_ptr<ITemporalUpscalerHistory> PrevHistory;
        // The renderer's resolved quality this frame (SceneRenderer: m_Quality). An implementation reads its own
        // parameters from it (DLSS: its preset per mode), as UE's upscaler plugins read their own console
        // variables; the interface names none of them.
        const Common::Scalability::ResolvedQuality* Quality = nullptr;
    };

    // What an implementation may measure on the OPAQUE scene, before the translucency composites into the scene
    // colour (UE: the main renderer's AddTSRMeasureFlickeringLuma, ahead of the translucency passes).
    struct TemporalUpscalerOpaqueInputs
    {
        RDG::TextureRef SceneColor; // linear HDR, RenderExtent, jittered: the lit opaque scene, no translucency yet
        RDG::TextureRef Exposure;   // as TemporalUpscalerInputs::Exposure
        const Common::Scalability::ResolvedQuality* Quality = nullptr; // as TemporalUpscalerInputs::Quality
    };

    struct TemporalUpscalerOutputs
    {
        RDG::TextureRef SceneColor; // linear HDR, OutputExtent (Native/Upscale) or RenderExtent (Supersample)
        // What the view keeps for the next frame (SceneViewState::SetUpscalerHistory); null for an implementation
        // whose state is all in HistoryDescs.
        std::shared_ptr<ITemporalUpscalerHistory> NewHistory;
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

        // Whether this method is also the denoiser of the view's noisy lighting signals (UE: an upscaler that
        // replaces IScreenSpaceDenoiser - DLSS Ray Reconstruction). True: the renderer hands the signals RAW -
        // the opaque reflection traced at every pixel (Reflections.DownsampleFactor 1) - and the screen-space
        // denoiser does not run for it (SceneRenderer AddFrameSSR); the method reads the surface guides of
        // TemporalUpscalerInputs. Default false: the signals arrive denoised.
        [[nodiscard]] virtual bool TakesRawLightingSignals() const;

        // The histories this method carries between frames, at this split and quality. Empty for an
        // implementation whose history is internal to it (it then reads only ViewFrame::HistoryReset). @p quality
        // is the renderer's resolved quality (ViewInputs::Scalability), from which an implementation reads its
        // own rows (TSR: its history screen percentage), as UE's upscalers read their own console variables; a
        // desc that changes with it recreates the history (TemporalHistory::Prepare -> HistoryResetReason).
        [[nodiscard]] virtual std::vector<HistoryTextureDesc>
        HistoryDescs( const ResolutionSplit&                      split,
                      const Common::Scalability::ResolvedQuality& quality ) const = 0;

        // THE SUB-PIXEL JITTER this method resolves (UE: the upscaler's own sample pattern), read by
        // SceneViewState::BeginFrame into ViewFrame::JitterSequenceLength / JitterPixels. Default: the engine's
        // Halton(2,3) sequence (TemporalJitterSequenceLength / TemporalJitterPixels). An implementation whose SDK
        // prescribes its own pattern overrides both. Pixels as ViewFrame::JitterPixels: render pixels, in the
        // frame's NDC orientation (JitterPixelsToNdc is 2 * pixels / RenderExtent).
        [[nodiscard]] virtual uint32_t  JitterSequenceLength( const ResolutionSplit& split ) const;
        [[nodiscard]] virtual glm::vec2 JitterPixels( uint32_t index, uint32_t length ) const;

        // ViewFrame::MaterialMipBias under this method. Default: log2( Render.Width / Output.Width ) at Upscale,
        // 0 at Native and Supersample (UE's screen-percentage mip bias under temporal upsampling). An
        // implementation whose SDK states its own rule overrides it.
        [[nodiscard]] virtual float MaterialMipBias( const ResolutionSplit& split ) const;

        // Adds the method's passes. The written history (HistoryRefs::Current) must be written by these passes
        // only; the output may alias it (TAA's output IS its new history) — the implementation says so by
        // returning the same ref. Errors are a malformed call (a missing input, a history count different from
        // HistoryDescs); a pass that fails while the graph executes is an RDG-FAULT1 pass fault, not an error
        // here.
        [[nodiscard]] virtual Common::ResultStr<TemporalUpscalerOutputs>
        AddPasses( RDG::Builder& graph, const ViewFrame& frame, const TemporalUpscalerInputs& inputs ) const = 0;

        // Adds the method's passes over the opaque scene; the renderer calls it after the opaque scene is
        // complete and before the translucency, and hands the result to AddPasses as
        // TemporalUpscalerInputs::OpaqueLuma. Default: no pass, an invalid ref. Errors as AddPasses'.
        [[nodiscard]] virtual Common::ResultStr<RDG::TextureRef>
        AddOpaquePasses( RDG::Builder& graph, const ViewFrame& frame, const TemporalUpscalerOpaqueInputs& inputs ) const;
    };

    // The implementations this build has, one per TemporalMethod other than None. Null for None, and for FSR in a
    // build without it (kFsrBuilt false: SelectTemporalMethod never selects it there). The objects are stateless
    // (all state is in SceneViewState, an SDK's context in its ITemporalUpscalerHistory), so one instance per
    // SceneRenderer is enough.
    [[nodiscard]] std::unique_ptr<ITemporalUpscaler> CreateTemporalUpscaler( TemporalMethod method );

    // Why the vendor method @p method cannot run on this device now, empty while it can and for the engine's own
    // methods. The catalog offers a vendor method from what its runtime said at device creation; a runtime that
    // fails later (DLSS: an exception inside slEvaluateFeature) refuses itself for the device's life, and this is
    // how the view hears of it before it builds a frame on that method.
    [[nodiscard]] std::string TemporalMethodUnavailable( TemporalMethod method );

    // Why a view whose effective render methods are these cannot run DLSS Ray Reconstruction, empty while it can.
    // Ray Reconstruction denoises ray-traced signals (NVIDIA: "RR replaces the denoisers"; UE's plugin offers it
    // where Lumen or ray-traced effects run); a view with none would pay its guides for a plain DLSS, so it is
    // refused by name and ReplaceUnavailableVendorMethods renders DLSS Super Resolution. The signals are the
    // ones the screen-space denoiser would otherwise clean (Common::Scalability::IsDenoisedSignal), on the
    // deferred path (@p deferredPath: the guides come from its G-buffer). PURE.
    [[nodiscard]] std::string RayReconstructionRefusal( Common::Scalability::ReflectionMethod reflections,
                                                        Common::Scalability::GIMethod         globalIllumination,
                                                        Common::Scalability::ShadowMethod     shadows,
                                                        bool                                  deferredPath );

    // The quality a view renders when a vendor method it resolves to is unavailable (@p unavailable: the reason
    // per method, TemporalMethodUnavailable in the renderer): DLSS Ray Reconstruction falls back to DLSS Super
    // Resolution first, and DLSS / FSR unavailable to the engine's TSR (Scalability Resolve's rule 4); Why names
    // every step and is empty when nothing was replaced. PURE.
    struct VendorMethodFallback
    {
        Common::Scalability::PathAntiAliasing AntiAliasing;
        std::string                           Why;
    };
    [[nodiscard]] VendorMethodFallback
    ReplaceUnavailableVendorMethods( const Common::Scalability::PathAntiAliasing&               antiAliasing,
                                     const std::function<std::string( TemporalMethod method )>& unavailable );

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
        // The reconstruction pass this view's percent runs (Scalability UpscalerForScale): the frame's
        // ViewInputs::Upscaler.
        // Not the setting's resolved one - a viewport override can sit on the other side of 100 %.
        Common::Scalability::Upscaler Upscaler = Common::Scalability::Upscaler::None;
        // Not empty when the requested scale was clamped to one the method's upscaler supports: why (the caller
        // logs it; this function is pure).
        std::string Clamped;
    };

    // THE ONE PER-VIEW RESOLUTION FUNCTION: the resolved setting's percent, replaced by the editor viewport's
    // override when it has one (@p viewportOverridePercent), split against @p output, the upscaler of that percent
    // (Scalability UpscalerForScale of the method and the percent), the method chosen for it
    // (SelectTemporalMethod: its refusal is this function's error), then clamped by the upscaler that implements
    // that method (@p upscalerFor: the view's object for a method, null for None): a split the upscaler does not
    // Support falls back to native scale (100 %) when that is supported, else the named error.
    [[nodiscard]] Common::ResultStr<ViewResolution>
    ResolveViewResolution( ViewExtent output, int settingPercent, std::optional<int> viewportOverridePercent,
                           const Common::Scalability::PathAntiAliasing&                     antiAliasing,
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

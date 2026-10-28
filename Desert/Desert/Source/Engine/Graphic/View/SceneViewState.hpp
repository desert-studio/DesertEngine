#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/Graphic/RDG/RDGFault.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic::RDG
{
    class Builder;
}

namespace Desert::Graphic
{
    class Image2D;
    class IImageFactory;       // Graphic/ImageFactory.hpp
    class IGraphImageImporter; // Graphic/GraphImageImporter.hpp
} // namespace Desert::Graphic

// TAA1 — THE PERSISTENT PER-VIEW STATE (UE: FSceneViewState). Everything a view must remember from one of its
// frames to the next: the previous matrices and time, the jitter position, whether the history may be read, the
// temporal history textures. The previous transforms of the objects are the scene's (GpuDriven/GpuScene.hpp).
//
// OWNERSHIP. One SceneViewState per SceneRenderer (a SceneRenderer is one view: the editor viewport, each asset
// preview, each capture), a member next to its ViewResources. NOT on the camera component: one camera can feed
// several views, and each view has its own previous frame — the preview that renders every tenth frame reprojects
// through ITS previous frame, not through the viewport's. NOT process-wide: two views never share a history.
//
// THE FRAME PROTOCOL (one call each, in order, on the main thread):
//   1. BeginFrame(inputs)   -> the frame's ViewFrame (immutable), history reset decided;
//   2. the graph is built: geometry passes ask Motion() for previous transforms, the temporal pass registers
//      History() through RegisterHistory;
//   3. EndFrame(report)     -> after Builder::Execute. Commits this frame as "previous", advances the jitter,
//   swaps
//      the history pair, ends the motion frame; an InvalidateHistory external lost to a fault makes the NEXT
//      frame reset with HistoryResetReason::PassFault.
// A frame that never reaches EndFrame (FrameFault, a minimised viewport that built no graph) is as if it never
// happened: the next BeginFrame still sees the last committed frame as previous, which is the frame the history
// textures and every Prev* matrix belong to — the three stay consistent with each other.
namespace Desert::Graphic
{
    // PURE. ViewInputs::CameraIdentity of the camera source @p sourceId (Core::Camera::GetSourceId: an entity
    // id, or the id issued to an outside-driven camera object, bit 32 set — Core/CameraSourceId.hpp) in the world
    // of @p sceneGeneration (Core::Scene::GetGeneration): the generation's low 31 bits above the 33-bit source id.
    // A reload — same Scene object, same entity ids, new generation — is a different camera, and so is switching
    // between two editor cameras: SceneViewState resets with CameraCut. An object address never enters it.
    // Generations repeat only after 2^31 scene loads.
    [[nodiscard]] constexpr uint64_t MakeViewCameraIdentity( const uint64_t sceneGeneration,
                                                             const uint64_t sourceId )
    {
        return ( ( sceneGeneration & 0x7FFFFFFFull ) << 33u ) | ( sourceId & 0x1FFFFFFFFull );
    }

    // What a view is asked to draw this frame. Filled by the SceneRenderer from its camera and the resolved
    // quality.
    struct ViewInputs
    {
        glm::mat4 View{ 1.0f };
        glm::mat4 Projection{ 1.0f }; // the camera's, unjittered (Core::MakePerspective / MakeOrthographic)
        glm::vec3 CameraPosition{ 0.0f };
        float     NearPlane = 0.0f;
        float     FarPlane  = 0.0f;

        // Which camera feeds the view (an entity id with its version and the scene's generation, or the editor
        // camera's fixed id). A different value from the last committed frame is a camera cut: the previous
        // matrices belong to another camera and reprojecting through them smears the whole frame.
        uint64_t CameraIdentity = 0;
        // An explicit cut asked for by gameplay or tooling: a teleport, a cinematic cut, a scene load, the
        // editor's "focus selection" jump (UE: FSceneView::bCameraCut). The view never GUESSES a cut from how far
        // the camera moved: a fast camera is not a cut, and a threshold would be a magic number that is wrong for
        // some scene.
        bool CameraCut = false;

        ViewExtent                            Output;                   // the presented size
        int                                   RenderScalePercent = 100; // ResolvedQuality RenderScalePercent
        Common::Scalability::PathAntiAliasing AntiAliasing; // ResolveAntiAliasingForPath( resolved, path )
        Common::Scalability::Upscaler         Upscaler = Common::Scalability::Upscaler::None;
        TemporalAAQuality                     Quality  = TemporalAAQuality::Medium;
        // The renderer's resolved quality (SceneRenderer m_Quality), outliving BeginFrame. Required whenever the
        // frame has a temporal upscaler: ITemporalUpscaler::HistoryDescs reads its own rows from it (TSR: the
        // history screen percentage). BeginFrame refuses a temporal frame without it, naming the upscaler.
        const Common::Scalability::ResolvedQuality* Scalability = nullptr;

        double TimeSeconds = 0.0; // the view's clock (scene time; paused scene -> unchanged -> DeltaSeconds 0)
        // The exposure this view last tonemapped with, as the CPU knows it (ScenePreExposure's source): manual ->
        // ManualExposureScale, auto -> key / the adapted luminance read back FramesInFlight frames ago; 0 = none
        // known (first frames, capture views: stored absolute).
        float LastExposure = 0.0f;

        ParallaxQuality Parallax;                   // ResolvedQuality Shading.Parallax*Steps
        glm::vec3       SunDirection{ 0.0f };       // toward the scene's directional light; zero = none
    };

    // ---- Previous per-object data -------------------------------------------------------------------------
    //
    // Not here: a primitive's previous world and previous bone palette are the scene's (GpuDriven/GpuScene.hpp,
    // PrevLocalToWorld and PrevBoneOffset, kept by GpuSceneRegistry per MotionKey), one for every view of the
    // scene, as in UE FGPUScene. A frame that never executed is discarded there (GpuSceneRegistry::BeginFrame).

    // ---- Velocity --------------------------------------------------------------------------------------------
    //
    // A GBuffer-class transient of every view, RenderExtent, written by EVERY pass that rasterises scene depth
    // (the depth and the velocity of a pixel must come from the same surface, or TAA reprojects a wall with a
    // character's motion). Value: current unjittered NDC.xyz minus previous unjittered NDC.xyz of the surface
    // point (ViewFrame rule: velocity never carries the jitter; z = the device depth's change, VelocityNdc).
    // Pixels no geometry covered (sky, clouds, far plane) keep the clear value 0 and the TAA pass derives their
    // motion from depth and the two view matrices.
    inline constexpr Core::Formats::ImageFormat kVelocityFormat = ViewTargetFormats::kVelocity;

    // RDG-FAULT1 fit. Velocity lost to a fault (every geometry pass that wrote it removed) is read as Black = zero
    // motion: the TAA resolve then treats each pixel as static and its neighbourhood clamp still bounds the error
    // — a defined picture, which is the FaultDefault rule. The history is a NEXT-frame input: losing its writer
    // must not let the next frame blend stale contents, so it is InvalidateHistory and the view resets
    // (PassFault).
    inline constexpr RDG::FaultDefault        kVelocityFaultDefault = RDG::FaultDefault::Black;
    inline constexpr RDG::ExternalFaultPolicy kHistoryFaultPolicy   = RDG::ExternalFaultPolicy::InvalidateHistory;

    // ---- THE VIEW'S CLOUD RECONSTRUCTION (UE: FSceneViewState::VolumetricCloudRenderTarget) ----------------
    // The volumetric clouds' temporal reconstruction (scatter + its depth guide, half the render extent) is a
    // view history like the eye adaptation: owned by the view, never by the cloud pass (two scene views drawing
    // one sky each reproject through their own previous frame). UE's protocol, the same one EyeAdaptationHistory
    // follows: the previous frame's pair is REGISTERED into the graph, this frame's pair is a TRANSIENT of the
    // graph (the resolve writes it, the composite samples its refs) and is EXTRACTED into the view at the end of
    // the frame. Two extraction targets alternate for the reason EyeAdaptationHistory gives.
    //
    // UNLIKE the eye adaptation, a frame without the pass drops the history: the resolve reprojects through the
    // view's previous matrices, which describe the frame just before, so a pair written two frames ago is not a
    // previous frame of anything. EndFrame keeps the new pair only when the resolve ran and survived.
    class VolumetricCloudHistory
    {
    public:
        struct Refs
        {
            RDG::TextureRef Scatter; // premultiplied radiance .rgb, transmittance .a
            RDG::TextureRef Guide;   // .x cloud front distance, .y scene distance (km)
        };

        // One texture of the pair at @p halfExtent: ViewTargetFormats::kCloudTrace (RGBA16F), storage + sampled.
        [[nodiscard]] static RDG::TextureDesc Desc( ViewExtent halfExtent );

        // Last frame's pair at @p halfExtent, registered into @p graph as "Clouds.History.Previous" and
        // "Clouds.HistoryGuide.Previous"; both invalid when there is none (first frame, Reset, a frame without the
        // resolve or with it lost to a fault, another extent). At most one call per graph, before CreateCurrent.
        [[nodiscard]] Refs RegisterPrevious( RDG::Builder& graph, ViewExtent halfExtent );
        // This frame's pair: two transients of @p graph ("Clouds.History", "Clouds.HistoryGuide") extracted into
        // the view for the next frame. At most one call per graph.
        [[nodiscard]] Refs CreateCurrent( RDG::Builder& graph, ViewExtent halfExtent );
        // The AddPass index of the resolve that writes the pair (Builder::GetPassCount() before its AddPass),
        // whose loss EndFrame checks. Not called -> the pair counts as unwritten.
        void SetWriterPass( uint32_t writerPass );

        // After Execute succeeded: the extracted pair becomes the previous one when its writer survived (@p
        // report); otherwise, and when this frame created none, the next frame has no previous. A frame that never
        // executed (FrameFault) never reaches it: the previous pair is untouched and still belongs to the last
        // committed frame.
        void EndFrame( const RDG::ExecuteReport& report );
        void Reset();

        [[nodiscard]] uint64_t HeldBytes() const;

    private:
        // [slot][0 scatter, 1 guide]
        std::array<std::array<RDG::ExternalTexture, 2>, 2> m_Slots;
        ViewExtent                                         m_PreviousExtent{};
        ViewExtent                                         m_CurrentExtent{};
        uint32_t                                           m_PreviousSlot = 0;
        bool                                               m_HasPrevious  = false;
        bool                                               m_Created      = false; // CreateCurrent ran this frame
        uint32_t                                           m_WriterPass   = UINT32_MAX;
    };

    // ---- THE VIEW'S SCREEN PROBE GATHER HISTORY (UE: FSceneViewState::Lumen.ScreenProbeGatherState) ---------
    // The screen probe gather's temporal filter (GlobalIllumination::ScreenProbeGather, UE
    // ScreenProbeTemporalReprojectionCS) accumulates the integrated diffuse, rough specular and backface diffuse
    // of every pixel over frames. That accumulation is a view history: two scene views drawing one scene each
    // reproject through their own previous frame, so it is owned here, never by the gather. The protocol is
    // VolumetricCloudHistory's: last frame's set REGISTERED into the graph, this frame's set a TRANSIENT of the
    // graph the temporal pass writes and the composite reads, EXTRACTED into the view after the frame; a frame
    // without the pass (GI off, another method, the pass lost to a fault) drops the history, because the next
    // frame's previous matrices describe this frame and nothing older. A camera cut or a teleport is the view's
    // HistoryReset (ViewInputs::CameraCut): the pass reads ViewFrame::HistoryValid with RegisterPrevious, as the
    // clouds do.
    class ScreenProbeGatherHistory
    {
    public:
        enum Texture : uint32_t
        {
            DiffuseIndirect = 0,     // rgb = irradiance / pi, a = the pixel's distance to the camera (cm)
            RoughSpecularIndirect,   // rgb = radiance, a = 1
            BackfaceDiffuseIndirect, // rgb = irradiance / pi from -N, a = 1
            Guide,                   // xyz = world normal, w = frames accumulated (UE NumFramesAccumulated) plus
                   // the foliage flag (UE bHasBackfaceDiffuse; SPPackGuideW, ScreenProbeTemporal)
            TextureCount
        };
        using Refs = std::array<RDG::TextureRef, TextureCount>;

        // One texture of the set at @p extent (the G-buffer's): kGIFinalGather, the guide kGIFinalGatherGuide.
        [[nodiscard]] static RDG::TextureDesc Desc( Texture texture, ViewExtent extent );

        // Last frame's set at @p extent, registered into @p graph as "ScreenProbes.<name>.Previous"; every ref
        // invalid when there is none (first frame, Reset, a frame without the temporal pass or with it lost to a
        // fault, another extent). At most one call per graph, before CreateCurrent.
        [[nodiscard]] Refs RegisterPrevious( RDG::Builder& graph, ViewExtent extent );
        // This frame's set: transients of @p graph ("ScreenProbes.<name>") extracted into the view for the next
        // frame. At most one call per graph.
        [[nodiscard]] Refs CreateCurrent( RDG::Builder& graph, ViewExtent extent );
        // The AddPass index of the temporal pass that writes the set (Builder::GetPassCount() before its
        // AddPass), whose loss EndFrame checks. Not called -> the set counts as unwritten.
        void SetWriterPass( uint32_t writerPass );

        // After Execute succeeded: the extracted set becomes the previous one when its writer survived (@p
        // report); otherwise, and when this frame created none, the next frame has no previous.
        void EndFrame( const RDG::ExecuteReport& report );
        void Reset();

        [[nodiscard]] uint64_t HeldBytes() const;

    private:
        // [slot][Texture]
        std::array<std::array<RDG::ExternalTexture, TextureCount>, 2> m_Slots;
        ViewExtent                                                    m_PreviousExtent{};
        ViewExtent                                                    m_CurrentExtent{};
        uint32_t                                                      m_PreviousSlot = 0;
        bool                                                          m_HasPrevious  = false;
        bool                                                          m_Created      = false;
        uint32_t                                                      m_WriterPass   = UINT32_MAX;
    };

    // ---- Temporal history ------------------------------------------------------------------------------------
    //
    // The textures a temporal method carries from frame to frame, owned here as RDG externals and registered into
    // each frame's graph. A PAIR per declared history (read previous, write current), swapped at EndFrame: the
    // write side is a registered external with kHistoryFaultPolicy, so RDG-FAULT1 reports its loss by index and
    // the read side is never written in the frame that reads it. Their shape is declared by the ITemporalUpscaler
    // (HistoryDescs): TAA/TAAU declare one RGBA16F colour history at OutputExtent; an upscaler that keeps its own
    // history internally declares none and receives only ViewFrame::HistoryReset.
    // Pair: read Previous, write Current, swapped at EndFrame (every temporal colour history). Persistent: ONE
    // texture kept across frames and never swapped, registered once per frame under Name with Previous == Current
    // (the same ref): contents an upscaler records only on some frames and reads on later ones (UE TSR's
    // persistent resurrection frames, r.TSR.Resurrection). Its contents are defined iff the upscaler recorded
    // them since the last history reset; the upscaler tracks that in its ITemporalUpscalerHistory.
    enum class HistoryLifetime : uint8_t
    {
        Pair,
        Persistent,
    };

    struct HistoryTextureDesc
    {
        RDG::TextureDesc Desc;
        HistoryLifetime  Lifetime = HistoryLifetime::Pair;
        // Graph names of the two sides as registered each frame, e.g. "TAA.History" (written this frame) and
        // "TAA.History.Previous" (read this frame). Both static strings (the graph keeps a view while building),
        // non-empty and different from each other and from every other history's names —
        // SceneViewState::BeginFrame refuses an upscaler whose descs break that, naming it: two externals under
        // one name make every graph dump, fault report and capture ambiguous about which side a pass touched.
        // A Persistent history has one side: Name only, PreviousName empty (BeginFrame refuses anything else).
        const char* Name         = "";
        const char* PreviousName = "";

        bool operator==( const HistoryTextureDesc& other ) const;
    };

    struct HistoryRefs
    {
        RDG::TextureRef Previous; // sampled; contents defined iff ViewFrame::HistoryValid()
        RDG::TextureRef Current;  // written by the temporal pass this frame
    };

    class TemporalHistory
    {
    public:
        // Makes the pairs match @p descs: same descs -> kept; anything else (count, size, format) -> all recreated
        // and the return value is true, which BeginFrame turns into HistoryResetReason::Resize /
        // TemporalMethodChange. Empty @p descs releases them (TemporalMethod::None holds no history memory).
        [[nodiscard]] bool Prepare( std::span<const HistoryTextureDesc> descs );

        // THE DEVICE STEP. Creates the GPU image of every pair side that has none — after Prepare recreated the
        // pairs (first use, resize, method change), i.e. 2 images per history — through @p images, and imports
        // each into its RDG::ExternalTexture through @p importer (the image's graph desc, its physical image, its
        // recorded layout and the hook that writes the final layout back: Renderer::ImportImage). Images are
        // 2D, Storage | Sample (the temporal pass writes Current from compute and samples Previous). Nothing
        // to do -> success without a call. All or nothing: on any failure no side keeps a new image and the
        // error names the history and side (a factory that made no image, an import error, an image whose
        // graph desc is not the declared one). Prepare stays device-free; the owner of the view calls this
        // after SceneViewState::BeginFrame and before Register. Implemented in TemporalHistoryPhysical.cpp
        // (interfaces only: the device lives behind the two seams).
        [[nodiscard]] Common::BoolResultStr AllocatePhysical( const IImageFactory&       images,
                                                              const IGraphImageImporter& importer );
        [[nodiscard]] bool                  HasPhysical() const; // every side has its image (true when no history)

        // Registers each pair into @p graph (Current with kHistoryFaultPolicy) and remembers the Current indices
        // for EndFrame. One call per frame, by the temporal pass's owner.
        [[nodiscard]] std::vector<HistoryRefs> Register( RDG::Builder& graph );

        // Whether @p report lost any Current registered this frame.
        [[nodiscard]] bool LostToFault( const RDG::ExecuteReport& report ) const;

        void Swap(); // Current becomes Previous
        void Release();

        // The pairs' descs as Prepare last matched them. NOT a byte count: the history images are Image2D ledger
        // rows (AllocatePhysical runs inside the view's ledger scope), and the view's budget reads those rows.
        [[nodiscard]] std::span<const HistoryTextureDesc> Descs() const
        {
            return m_Descs;
        }

    private:
        std::vector<HistoryTextureDesc>                  m_Descs;
        std::vector<std::array<RDG::ExternalTexture, 2>> m_Pairs;
        // The images behind m_Pairs, same indices; empty until AllocatePhysical, cleared by Prepare / Release.
        std::vector<std::array<std::shared_ptr<Image2D>, 2>> m_Images;
        uint32_t                                             m_CurrentSlot = 0;
        std::vector<uint32_t>                                m_RegisteredCurrent; // RDG indices of this frame
    };

    class ITemporalUpscaler;
    class ITemporalUpscalerHistory;

    // ---- THE VIEW STATE ------------------------------------------------------------------------------------
    class SceneViewState
    {
    public:
        SceneViewState() = default;
        // Externals are registered by address; a copy or move would leave a graph pointing at the wrong object.
        SceneViewState( const SceneViewState& )            = delete;
        SceneViewState& operator=( const SceneViewState& ) = delete;

        // Builds this frame's ViewFrame. HistoryReset is decided in this order, first match wins:
        //   FirstFrame (nothing committed) > PassFault (the last EndFrame saw the history lost) > CameraCut
        //   (inputs.CameraCut, or CameraIdentity differs) > Resize (the split differs) > TemporalMethodChange.
        // On any reset every Prev* field equals its current value, DeltaSeconds is 0 and the jitter index restarts
        // at 0. @p upscaler is the implementation for the selected method (null for TemporalMethod::None); its
        // HistoryDescs prepare History(). Errors: SelectTemporalMethod's, MakeResolutionSplit's, an upscaler that
        // does not match the method, a non-finite matrix in @p inputs (with the field named).
        [[nodiscard]] Common::ResultStr<ViewFrame> BeginFrame( const ViewInputs&        inputs,
                                                               const ITemporalUpscaler* upscaler );

        [[nodiscard]] VolumetricCloudHistory&         Clouds();
        [[nodiscard]] const VolumetricCloudHistory&   Clouds() const; // HeldBytes: SceneRenderer::HeldBytes
        [[nodiscard]] ScreenProbeGatherHistory&       ScreenProbeGather();
        [[nodiscard]] const ScreenProbeGatherHistory& ScreenProbeGather() const; // HeldBytes: SceneRenderer
        [[nodiscard]] TemporalHistory& History();

        // The vendor upscaler state the last frame returned (TemporalUpscalerOutputs::NewHistory), handed back as
        // TemporalUpscalerInputs::PrevHistory. Dropped by Reset and by a BeginFrame whose method is None.
        [[nodiscard]] const std::shared_ptr<ITemporalUpscalerHistory>& UpscalerHistory() const
        {
            return m_UpscalerHistory;
        }
        void SetUpscalerHistory( std::shared_ptr<ITemporalUpscalerHistory> history )
        {
            m_UpscalerHistory = std::move( history );
        }

        // After Builder::Execute succeeded. Commits the frame BeginFrame returned as the previous one, advances
        // the jitter index modulo its length, swaps the history pairs, ends the motion frame. If @p report lists a
        // Current history as lost, the next BeginFrame resets with PassFault (logged once per view by the
        // reporter RDG-FAULT1 already owns, not here).
        void EndFrame( const RDG::ExecuteReport& report );

        // The view is torn down or re-targeted to an unrelated output: drops the history memory and the motion
        // records; the next frame is FirstFrame.
        void Reset();

    private:
        VolumetricCloudHistory   m_Clouds;
        ScreenProbeGatherHistory m_ScreenProbeGather;

        bool            m_HasCommitted = false;
        ViewFrame       m_Pending;                     // returned by the last BeginFrame, committed by EndFrame
        ViewFrame       m_Committed;                   // the previous frame
        uint64_t        m_CommittedCameraIdentity = 0; // the camera of m_Committed (written by EndFrame only)
        uint64_t        m_PendingCameraIdentity   = 0; // the camera of m_Pending (written by BeginFrame)
        bool            m_FrameOpen               = false; // a BeginFrame succeeded and its EndFrame has not run
        bool            m_PendingFaultReset       = false;
        uint32_t        m_JitterIndex             = 0;
        TemporalHistory m_History;
        std::shared_ptr<ITemporalUpscalerHistory> m_UpscalerHistory;
    };
} // namespace Desert::Graphic

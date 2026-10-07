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
// temporal history textures, and the previous transforms of every object it drew (MotionHistory).
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
        // Which scene the view draws (its generation). A change clears MotionHistory: entity ids are not stable
        // across scenes, so a previous transform found under the same id would belong to another object.
        uint64_t SceneIdentity = 0;

        ViewExtent                            Output;                   // the presented size
        int                                   RenderScalePercent = 100; // ResolvedQuality RenderScalePercent
        Common::Scalability::PathAntiAliasing AntiAliasing; // ResolveAntiAliasingForPath( resolved, path )
        Common::Scalability::Upscaler         Upscaler = Common::Scalability::Upscaler::None;
        TemporalAAQuality                     Quality  = TemporalAAQuality::Medium;

        double TimeSeconds = 0.0; // the view's clock (scene time; paused scene -> unchanged -> DeltaSeconds 0)
    };

    // ---- Previous per-object data -------------------------------------------------------------------------
    //
    // WHERE IT LIVES: here, per view, on the CPU; uploaded each frame into the per-(frame x view) object buffer
    // the geometry passes read (GpuObjectMotion below). NEVER on a component: a component is shared by every view
    // and serialised, and "previous" is a property of a view's last frame, not of the object.
    //
    // An object is identified by the entity that owns the draw and a slot inside it (a model with several skinned
    // meshes has one bone palette per slot). Static instanced batches (HLOD, scattered props) are NOT tracked per
    // instance: their instances are world-static by construction (moving one rebuilds the batch), so their
    // previous transform IS their transform and velocity comes from the camera alone. Wind on those instances is
    // time animation, handled by evaluating the wind at ViewFrame::PrevTimeSeconds, not by a stored transform.
    struct MotionKey
    {
        uint32_t Entity = 0; // entt id incl. version
        uint32_t Slot   = 0;

        bool operator==( const MotionKey& ) const = default;
    };

    struct MotionKeyHash
    {
        [[nodiscard]] std::size_t operator()( const MotionKey& key ) const noexcept
        {
            return std::hash<uint64_t>{}( ( static_cast<uint64_t>( key.Entity ) << 32u ) | key.Slot );
        }
    };

    class MotionHistory
    {
    public:
        // Records @p current as this frame's world transform of @p key and returns the one the view drew it with
        // in its previous frame — or @p current when it was not drawn then (it just appeared, became visible, or
        // the history was reset): an object with no previous frame has no object motion, only camera motion.
        // Calling it twice for one key in a frame keeps the first record and returns the same previous transform.
        [[nodiscard]] glm::mat4 PreviousTransform( MotionKey key, const glm::mat4& current );

        // The same for a skinned slot's bone palette. Returns the previous palette, or @p current when the slot
        // was not drawn last frame or its bone count changed (a mesh swap: the old palette indexes other bones).
        // The span stays valid until EndFrame.
        [[nodiscard]] std::span<const glm::mat4> PreviousBones( MotionKey                  key,
                                                                std::span<const glm::mat4> current );

        // This frame's records become "previous"; a key not recorded this frame is dropped (an object that was not
        // drawn must not reappear later with a stale transform: it reappears with no object motion).
        void EndFrame();
        // The frame that recorded since the last EndFrame never ended (SceneViewState: "as if it never happened"):
        // drops only its records. The previous frame's records stay, because they still belong to the last
        // committed frame, which the next frame's Prev* matrices and history textures also belong to.
        void DiscardCurrent();
        // Scene changed (ViewInputs::SceneIdentity) or the view was re-created.
        void Clear();

        [[nodiscard]] std::size_t TrackedTransforms() const; // previous-frame records, for the view's memory line
        [[nodiscard]] std::size_t TrackedPalettes() const;

    private:
        std::unordered_map<MotionKey, glm::mat4, MotionKeyHash>              m_PrevTransforms;
        std::unordered_map<MotionKey, glm::mat4, MotionKeyHash>              m_CurTransforms;
        std::unordered_map<MotionKey, std::vector<glm::mat4>, MotionKeyHash> m_PrevBones;
        std::unordered_map<MotionKey, std::vector<glm::mat4>, MotionKeyHash> m_CurBones;
    };

    // What every geometry pass reads per draw instead of today's single `mat4 Transform` push constant: two mat4
    // are 128 bytes, the whole of Vulkan's guaranteed push-constant space (maxPushConstantsSize >= 128), with no
    // room left for the material index the push block also carries. So the pair moves into a per-(frame x view)
    // storage buffer (contract §2: per-frame renderer state), indexed by a draw index in the push block. std430
    // layout; the GLSL twin lives in Common/ObjectMotion.glslh and a test asserts the two sizes agree.
    struct GpuObjectMotion
    {
        glm::mat4 World{ 1.0f };
        glm::mat4 PrevWorld{ 1.0f };
        // Skinned: where this frame's palette and the PREVIOUS one (MotionHistory::PreviousBones) start in the
        // view's ObjectBones buffer — ONE per-(frame x view) buffer holding both frames' palettes of every skinned
        // primitive, so every pass that draws the primitive skins it from the same bytes. 0 for a rigid primitive.
        uint32_t BoneOffset     = 0;
        uint32_t PrevBoneOffset = 0;
        uint32_t Pad0           = 0;
        uint32_t Pad1           = 0;
    };
    static_assert( sizeof( GpuObjectMotion ) == 144, "std430 twin in Common/ObjectMotion.glslh" );
    static_assert( offsetof( GpuObjectMotion, BoneOffset ) == 128, "std430 twin in Common/ObjectMotion.glslh" );
    static_assert( offsetof( GpuObjectMotion, PrevBoneOffset ) == 132, "std430 twin in Common/ObjectMotion.glslh" );

    // ---- Velocity --------------------------------------------------------------------------------------------
    //
    // A GBuffer-class transient of every view, RenderExtent, written by EVERY pass that rasterises scene depth
    // (the depth and the velocity of a pixel must come from the same surface, or TAA reprojects a wall with a
    // character's motion). Value: current unjittered NDC.xy minus previous unjittered NDC.xy of the surface point
    // (ViewFrame rule: velocity never carries the jitter). Pixels no geometry covered (sky, clouds, far plane)
    // keep the clear value 0 and the TAA pass derives their motion from depth and the two view matrices.
    inline constexpr Core::Formats::ImageFormat kVelocityFormat = ViewTargetFormats::kVelocity;

    // RDG-FAULT1 fit. Velocity lost to a fault (every geometry pass that wrote it removed) is read as Black = zero
    // motion: the TAA resolve then treats each pixel as static and its neighbourhood clamp still bounds the error
    // — a defined picture, which is the FaultDefault rule. The history is a NEXT-frame input: losing its writer
    // must not let the next frame blend stale contents, so it is InvalidateHistory and the view resets
    // (PassFault).
    inline constexpr RDG::FaultDefault        kVelocityFaultDefault = RDG::FaultDefault::Black;
    inline constexpr RDG::ExternalFaultPolicy kHistoryFaultPolicy   = RDG::ExternalFaultPolicy::InvalidateHistory;

    // ---- Temporal history ------------------------------------------------------------------------------------
    //
    // The textures a temporal method carries from frame to frame, owned here as RDG externals and registered into
    // each frame's graph. A PAIR per declared history (read previous, write current), swapped at EndFrame: the
    // write side is a registered external with kHistoryFaultPolicy, so RDG-FAULT1 reports its loss by index and
    // the read side is never written in the frame that reads it. Their shape is declared by the ITemporalUpscaler
    // (HistoryDescs): TAA/TAAU declare one RGBA16F colour history at OutputExtent; an upscaler that keeps its own
    // history internally declares none and receives only ViewFrame::HistoryReset.
    struct HistoryTextureDesc
    {
        RDG::TextureDesc Desc;
        // Graph names of the two sides as registered each frame, e.g. "TAA.History" (written this frame) and
        // "TAA.History.Previous" (read this frame). Both static strings (the graph keeps a view while building),
        // non-empty and different from each other and from every other history's names —
        // SceneViewState::BeginFrame refuses an upscaler whose descs break that, naming it: two externals under
        // one name make every graph dump, fault report and capture ambiguous about which side a pass touched.
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

        [[nodiscard]] uint64_t HeldBytes() const; // counted in the view's budget (SceneRenderer::HeldBytes)

    private:
        std::vector<HistoryTextureDesc>                  m_Descs;
        std::vector<std::array<RDG::ExternalTexture, 2>> m_Pairs;
        // The images behind m_Pairs, same indices; empty until AllocatePhysical, cleared by Prepare / Release.
        std::vector<std::array<std::shared_ptr<Image2D>, 2>> m_Images;
        uint32_t                                         m_CurrentSlot = 0;
        std::vector<uint32_t>                            m_RegisteredCurrent; // RDG indices of this frame
    };

    class ITemporalUpscaler;

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

        [[nodiscard]] TemporalHistory& History();
        [[nodiscard]] MotionHistory&   Motion();

        // After Builder::Execute succeeded. Commits the frame BeginFrame returned as the previous one, advances
        // the jitter index modulo its length, swaps the history pairs, ends the motion frame. If @p report lists a
        // Current history as lost, the next BeginFrame resets with PassFault (logged once per view by the
        // reporter RDG-FAULT1 already owns, not here).
        void EndFrame( const RDG::ExecuteReport& report );

        // The view is torn down or re-targeted to an unrelated output: drops the history memory and the motion
        // records; the next frame is FirstFrame.
        void Reset();

        [[nodiscard]] uint64_t HeldBytes() const;

    private:
        bool            m_HasCommitted = false;
        ViewFrame       m_Pending;   // returned by the last BeginFrame, committed by EndFrame
        ViewFrame       m_Committed; // the previous frame
        uint64_t        m_CommittedCameraIdentity = 0;     // the camera of m_Committed (written by EndFrame only)
        uint64_t        m_PendingCameraIdentity   = 0;     // the camera of m_Pending (written by BeginFrame)
        bool            m_FrameOpen               = false; // a BeginFrame succeeded and its EndFrame has not run
        uint64_t        m_SceneIdentity           = 0;
        bool            m_PendingFaultReset       = false;
        uint32_t        m_JitterIndex             = 0;
        TemporalHistory m_History;
        MotionHistory   m_Motion;
    };
} // namespace Desert::Graphic

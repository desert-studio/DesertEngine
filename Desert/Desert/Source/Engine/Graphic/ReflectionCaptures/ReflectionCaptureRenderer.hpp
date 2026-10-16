#pragma once

#include <Engine/Assets/FrameRetireQueue.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/GlobalIllumination/WorldRadianceCache.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ShaderProtocols/DirectionLight.hpp>
#include <Common/Core/Timestep.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Graphic
{
    class SceneRenderer;
    class FrameTextures;
    class ComputePipeline;
    class Shader;
    namespace RDG
    {
        class Builder;
    }

    // ONE CAPTURE AS THE LIGHTING PASSES READ IT — Shaders/Common/ReflectionCaptureGpu.glslh, std430, the same
    // layout (four vec4s, a uvec4, a mat4: no member straddles a 16-byte boundary, so std430 adds no padding).
    struct ReflectionCaptureGpu
    {
        glm::vec4  PositionRadius{ 0.0f };          // xyz = influence centre (world cm), w = sphere radius
        glm::vec4  CaptureOffsetBrightness{ 0.0f }; // xyz = capture point - centre (world cm), w = Brightness
        glm::vec4  BoxExtentTransition{ 0.0f };     // xyz = box half extent (cm), w = box transition (cm)
        glm::uvec4 ShapeCube{ 0u };                 // x = 0 sphere / 1 box, y = cube index in the array
        glm::mat4  WorldToBox{ 1.0f };              // world -> box space ([-1, 1]^3)
    };
    static_assert( sizeof( ReflectionCaptureGpu ) == 128, "ReflectionCaptureGpu must match the std430 struct" );

    // THE CAPTURE CUBE'S FACE, for every capture of the scene: one cube array holds them all, and an array has one
    // face size. It is the Scalability parameter Reflections.CaptureSize (UE: r.ReflectionCaptureResolution, a
    // project-wide power of two); the mips are its whole chain, the last one 1x1. A size that is not a power of
    // two inside the parameter's range is refused by the renderer (logged, no captures), never rounded.
    [[nodiscard]] uint32_t                      ReflectionCaptureFaceSize();
    inline constexpr Core::Formats::ImageFormat kReflectionCaptureFormat = Core::Formats::ImageFormat::RGBA16F;

    /// One capture of the scene, gathered from the registry (pure: no device; the order is the entity order, which
    /// is what the cube indices are assigned by).
    struct ReflectionCaptureEntry
    {
        entt::entity         Entity = entt::null;
        ReflectionCaptureGpu Gpu;
        float                InfluenceRadius = 0.0f;  // the sort key: the smallest influence composites first
        glm::vec3            CapturePosition{ 0.0f }; // where the six face views are rendered from (world cm)
    };
    [[nodiscard]] std::vector<ReflectionCaptureEntry> GatherReflectionCaptures( const entt::registry& registry );

    /// What the lighting pass reads: the header (x = count) and the captures sorted smallest influence first, only
    /// those @p captured (by cube index) — a capture whose cube has not been written yet is not read.
    [[nodiscard]] std::vector<std::byte>
    PackReflectionCaptures( const std::vector<ReflectionCaptureEntry>& captures,
                            const std::vector<bool>&                   captured );

    /**
     * @brief WHAT A CAPTURE'S STORED DIFFUSE IS MEASURED WITH: the view's own diffuse estimator, the one its lit
     * pixels receive (REFL-CAPTURE-NORM).
     *
     * A capture's reflection is normalised (UE ComputeMixingWeight, Common/ReflectionEnvironment.glslh) by the
     * ratio "diffuse at the pixel / diffuse at the capture point". UE takes the pixel side from the lightmap (or,
     * with no static lighting, from the SKY LIGHT's irradiance) and the capture side from the captured cube's own
     * mean, so a capture of a lamp-lit street at night is divided by the bright cube and multiplied by the dark
     * sky: black. Here BOTH sides come from the same estimator - the active GI's - so the ratio carries only how
     * the lighting varies between the two points, and where the estimator is right at the capture point the ratio
     * is exactly 1. RadianceCache set: the world radiance cache (GIMethod ProbeVolume / SoftwareTraced) - the
     * probes the pixels' own final gather reads; the capture points are marked into it (CapturePoints).
     *   RadianceCache unset: the view's GI has no world-space diffuse (Off, ScreenSpace, a forward view); its
     * pixels are lit by the sky's irradiance cube, and so is the capture point. Environment names that sky bake
     * (EnvironmentIdentity): a new one is a new estimator, and every written cube is measured again under it.
     */
    struct ReflectionCaptureLighting
    {
        std::optional<GlobalIllumination::RadianceCacheFrame> RadianceCache;
        RDG::TextureRef                                       EnvIrradiance; // SceneViewInputs::EnvIrradiance
        uint64_t                                              Environment = 0; // EnvironmentIdentity of that sky
        glm::vec4                                             SkyYawCosSin{ 1.0f, 0.0f, 0.0f, 0.0f }; // SkyLookGPU
        glm::vec4                                             SkyGain{ 1.0f };
    };

    // THE PERCEPTIBLE STEP OF A SUN'S COLOUR for the captures: a sun whose illuminance changed by more than this
    // ratio in some channel against the one the captures were last triggered under recaptures them. An even change
    // of all three channels is carried by the normalisation (the stored diffuse is the lighting's at capture time,
    // the pixel's is this frame's), so this step is about HUE - a sunset reddening - and is a quarter: about a
    // third of a stop in one channel against the others, the step a graded image shows.
    inline constexpr float kReflectionCaptureSunRatio = 1.25f;

    /// Whether the suns @p current differ PERCEPTIBLY from @p reference for the reflection captures (pure):
    /// another count, a direction turned by more than @p thresholdDeg (the sky's Rebake Sun Angle - one rule for
    /// when a moving sun is a new lighting), or a channel's illuminance ratio between the two past
    /// kReflectionCaptureSunRatio relative to the others (a dark sun - all channels 0 - against a lit one is a
    /// change).
    [[nodiscard]] bool
    ReflectionCaptureSunChanged( const std::vector<ShaderProtocols::DirectionLightPayload>& reference,
                                 const std::vector<ShaderProtocols::DirectionLightPayload>& current,
                                 float                                                      thresholdDeg );

    /**
     * @brief THE REFLECTION CAPTURES OF ONE SCENE (UE: FReflectionEnvironmentSceneData + the capture update).
     *
     * Owned by the scene (Core::Scene), as UE's are by FScene: ONE set of captures, shared by every view of the
     * scene - a second viewport on the same world reads the same cubes and costs no second capture.
     *
     * Every capture is rendered from its capture point into six 90-degree face views - SceneRenderers of the
     * SceneCapture profile (UE's capture view family): the lit HDR scene, its sky and clouds, no particles and
     * no post chain, lit by the SCENE'S sky light (the reading view's environment, lent to each face:
     * SceneRenderer::BorrowEnvironment), so a face bakes no sky of its own -
     * resampled into a radiance cube, given its mip chain, GGX-prefiltered into the capture's layers of the
     * scene's cube array (Common/ReflectionCaptureMip.glslh: the one roughness ramp), and the diffuse at its
     * capture point (ReflectionCaptureLighting) stored in alpha - the ONE home of the capture's brightness,
     * written with the cube it describes. The lighting pass reads them through SceneViewInputs
     * (Common/ReflectionEnvironment.glslh).
     *
     * WHEN A CAPTURE IS RENDERED - by itself, never by a "Build Reflection Captures" command (UE leaves a relit
     * level's captures stale until someone presses it):
     *   - the STRUCTURE changed (a capture new, moved or edited, the set of captures, the face size): every
     * capture is rendered again and none is read until it has been;
     *   - the LIGHTING changed PERCEPTIBLY (ReflectionCaptureRelit): a new sky bake (the sky's own rule already
     * throttles a moving sun to its Rebake Sun Angle), a point or spot light added, removed, hidden, moved or
     * re-coloured, or a sun that turned past that same angle or changed colour past kReflectionCaptureSunRatio.
     * A sun that only dims or brightens evenly does not recapture: the normalisation is measured at the time of
     * the capture and the pixel's diffuse is this frame's, so the ratio already carries it. Every capture is
     * marked stale and KEEPS BEING READ while it is rendered again, so a relight never blinks the reflections out.
     *
     * THE BUDGET IS ONE FACE VIEW PER FRAME (UE's time-sliced real-time sky capture,
     * r.SkyLight.RealTimeReflectionCapture.TimeSlice, applied to the scene's captures): the capture under update
     * renders one of its six face views each frame, round-robin over the faces that have not settled, under the
     * sun as it was when that capture started (its six faces are one lighting instant, not six). A face has
     * settled when it held one sky environment for one render after its first (its sky-view LUT is a frame late)
     * - kCaptureSettleFrames when it draws a cloud layer, whose history converges - or after kCaptureFrameBudget
     * renders whatever its sky does. Only when all six have settled is the cube written -
     * assembled, mipped, filtered with UE's real-time sample counts - in one frame, so the array holds either the
     * previous cube or the new one, never a half-written mix. Captures are taken round-robin from the one after
     * the last, so a lighting that keeps changing refreshes every capture in turn.
     *
     * THE STORED DIFFUSE IS RE-MEASURED UNTIL IT IS MEASURED: every frame, the first view that reads captures runs
     * ReflectionCaptureDiffuse over every written cube, and a cube whose stored diffuse is still 0 ("not
     * measured": the radiance cache had no traced probes around its point when it was written) is measured again
     * - on the GPU, where the answer is - until the estimator answers. A cube being written is measured
     * unconditionally.
     *
     * AND AGAIN WHEN THE ESTIMATOR CHANGES: a new sky bake (or look) under the sky's estimator, or a switch
     * between the sky and the radiance cache, re-measures every written cube in the frame it is first seen. The
     * stored diffuse and the pixel's then come from ONE sky light, so the ratio stays 1 at the capture point while
     * the cube is rendered again; measured once at capture time instead, a dusk sky's falling irradiance over the
     * stored one dimmed every lamp-lit cube by the sky's ratio until its re-capture landed (REFL-CAPTURE-NORM-2:
     * a ~4 % whole-frame dip on the Bistro running day). An even change of the sun with no new sky bake is still
     * carried by the ratio under the radiance cache.
     *
     * A view whose profile does not read captures (previews, thumbnails, the face views themselves) is handed
     * an empty list and the scene's one-cube black array: the lighting shader declares the bindings.
     */
    class ReflectionCaptureRenderer
    {
    public:
        ReflectionCaptureRenderer();
        ~ReflectionCaptureRenderer();
        ReflectionCaptureRenderer( const ReflectionCaptureRenderer& )            = delete;
        ReflectionCaptureRenderer& operator=( const ReflectionCaptureRenderer& ) = delete;

        /// One face view of the capture under update, one complete frame (open, record, render, close). Called by
        /// the scene ONCE per frame, BEFORE any of its views opens its own frame: two renderers' frame brackets
        /// never interleave (Core::Scene's view loop). @p record replays the scene's recorded draws into a face
        /// renderer. @p environment is the scene's sky light as the reading view holds it: lent to the face view
        /// (it lights the face), and a new bake recaptures everything.
        void RenderCaptureViews( const Core::Scene& scene, const std::function<void( SceneRenderer& )>& record,
                                 Common::Timestep timestep, Common::Timestep realTimestep,
                                 const ShaderProtocols::DirectionLight& dirLights,
                                 const std::optional<Environment>&      environment );

        /// In a view's graph, before any lit node: the scene's array and capture list published to @p textures
        /// (SceneViewInputs). @p readsCaptures false (ViewProfile::ReflectionCaptures): the empty list and the
        /// black array.
        void AddPasses( RDG::Builder& graph, FrameTextures& textures, bool readsCaptures );

        /// In the same graph, once the view's diffuse estimator of this frame exists (deferred: after the traced
        /// GI): the first view that reads captures in a frame writes the pending capture's cube when its six faces
        /// have settled, and measures @p lighting at the capture points whose stored diffuse is not measured yet
        /// (and at the one being written). A view whose AddPasses published no capturing array records nothing.
        void AddCaptureUpdate( RDG::Builder& graph, FrameTextures& textures,
                               const ReflectionCaptureLighting& lighting );

        /// Every capture point of the scene (world cm), for the radiance cache to keep probes around (its cells
        /// otherwise exist only where the screen's pixels read them, and a capture hangs in the air).
        [[nodiscard]] std::vector<glm::vec3> CapturePoints() const;

    private:
        struct FaceView;

        // A cube array shared by the views' graphs: extracted when (re)created, registered every frame after.
        struct SharedArray
        {
            RDG::ExternalTexture Texture;
            uint32_t             Cubes    = 0;
            uint32_t             FaceSize = 0;
            bool                 Built    = false;
        };

        [[nodiscard]] bool EnsurePipelines();
        // The array registered into @p graph, created (and every cube written black) when it does not exist or
        // has another shape. Returns the ref and whether it was created in this graph.
        [[nodiscard]] std::pair<RDG::TextureRef, bool> AcquireArray( RDG::Builder& graph, FrameTextures& textures,
                                                                     SharedArray& array, uint32_t cubes,
                                                                     uint32_t faceSize, std::string_view name );
        // The cube's radiance, mips and GGX filter (alpha 0: "not measured" until AddDiffusePass stores it), from
        // the face views, or black for the cubes of a fresh array.
        void AddCapturePasses( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef array, uint32_t cube,
                               uint32_t faceSize, bool black );
        // ReflectionCaptureDiffuse over every written cube of @p array: stores @p lighting's diffuse in the last
        // mip's alpha where it is not measured yet, and unconditionally into @p forcedCube (kNoCapture: none) -
        // into every written cube when @p forceAll (the estimator changed).
        void AddDiffusePass( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef array,
                             uint32_t forcedCube, bool forceAll, const ReflectionCaptureLighting& lighting );
        // The sky's Rebake Sun Angle of the scene's primary sky (lowest entity), or that field's default without
        // one.
        [[nodiscard]] static float SunAngleThreshold( const entt::registry& registry );

        // The face size the captures are rendered at (Reflections.CaptureSize when they were last gathered).
        uint32_t m_FaceSize = 0;

        // The scene's captures as last gathered, and what has been written into the array (by cube index).
        std::vector<ReflectionCaptureEntry> m_Captures;
        std::vector<bool>                   m_Captured; // written at least once: the lighting reads it
        std::vector<bool>                   m_Done;     // settled: not rendered again until something changes
        uint64_t                            m_Signature         = 0; // the structure (see the class comment)
        // The lighting the captures were last triggered under: the exact part (the sky bake, the local lights)
        // and the suns, compared by ReflectionCaptureSunChanged.
        uint64_t                                            m_LightingSignature = 0;
        std::vector<ShaderProtocols::DirectionLightPayload> m_ReferenceSuns;
        uint32_t                                            m_LastCaptured = 0; // the round-robin's last capture

        // The array AddPasses registered into the graph being built, for AddCaptureUpdate in the same graph
        // (invalid: that view captures nothing).
        RDG::TextureRef m_GraphArray;

        // The capture whose faces are being rendered, under the suns it started with; per face, how many times it
        // was rendered, how many consecutive renders held one sky environment, and that environment (the scene's,
        // lent to the face: a face rendered across a sky re-bake settles again under the new one).
        static constexpr uint32_t       kNoCapture = 0xFFFFFFFFu;
        uint32_t                        m_Pending  = kNoCapture;
        ShaderProtocols::DirectionLight m_PendingSuns;
        bool                            m_PendingRelit = false; // relit while rendered: still stale once written
        std::array<uint32_t, 6>         m_FaceRenders{};
        std::array<uint32_t, 6>         m_FaceHeld{};
        std::array<uint32_t, 6>         m_FaceSettle{}; // the held renders that settle it (its clouds or none)
        std::array<uint64_t, 6>         m_FaceEnvironment{};
        uint32_t                        m_LastFace = 5u; // the face round's last face
        bool     m_WriteCapture  = false;                // all six settled this frame: AddCaptureUpdate writes it
        uint64_t m_MeasuredFrame = ~0ull;                // the frame the diffuse was last re-measured in
        // The estimator the written cubes were last measured with (MeasureKey: the radiance cache, or the sky bake
        // and look): another one re-measures them all.
        uint64_t m_MeasuredKey = 0;

        std::array<std::unique_ptr<FaceView>, 6> m_Faces;
        // Face views released once every capture settled, kept alive until no frame in flight can still execute
        // their passes: a view's pipelines are destroyed with it, at once (VulkanPipeline::Release), and the frame
        // that rendered the last face is still on the GPU when the next one releases them.
        Assets::FrameRetireQueue<std::unique_ptr<FaceView>> m_RetiringFaces;

        SharedArray m_Cubes; // the scene's captures
        SharedArray m_Black; // one black cube, for the views that read none
        // Arrays retired by a reshape stay alive while the frames in flight may still sample them.
        std::vector<std::unique_ptr<RDG::ExternalTexture>> m_Retired;

        std::shared_ptr<ComputePipeline> m_Assemble;
        std::shared_ptr<ComputePipeline> m_Downsample;
        std::shared_ptr<ComputePipeline> m_Filter;
        std::shared_ptr<ComputePipeline> m_Diffuse;
        bool                             m_PipelinesFailed = false;
        mutable ShaderBindingLayoutCache m_AssembleLayout;
        mutable ShaderBindingLayoutCache m_DownsampleLayout;
        mutable ShaderBindingLayoutCache m_FilterLayout;
        mutable ShaderBindingLayoutCache m_DiffuseLayout;
    };
} // namespace Desert::Graphic

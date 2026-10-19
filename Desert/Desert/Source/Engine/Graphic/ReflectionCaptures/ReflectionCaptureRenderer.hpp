#pragma once

#include <Engine/Assets/FrameRetireQueue.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ShaderProtocols/DirectionLight.hpp>
#include <Common/Core/Timestep.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
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
    // project-wide power of two); the mips are its whole chain, the last one 1x1 (the capture's average
    // brightness is read from it - ReflectionCaptureFilter.shader). A size that is not a power of two inside
    // the parameter's range is refused by the renderer (logged, no captures), never rounded.
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
     * @brief THE REFLECTION CAPTURES OF ONE SCENE (UE: FReflectionEnvironmentSceneData + the capture update).
     *
     * Owned by the scene (Core::Scene), as UE's are by FScene: ONE set of captures, shared by every view of the
     * scene - a second viewport on the same world reads the same cubes and costs no second capture.
     *
     * Every capture is rendered from its capture point into six 90-degree face views - complete SceneRenderers
     * of this engine, so the cube holds the lit scene, the sky and its clouds exactly as a viewport draws them -
     * resampled into a radiance cube, given its mip chain, GGX-prefiltered into the capture's layers of the
     * scene's cube array (Common/ReflectionCaptureMip.glslh: the one roughness ramp), and its average brightness
     * stored in alpha. The lighting pass reads them through SceneViewInputs (Common/ReflectionEnvironment.glslh).
     *
     * WHEN A CAPTURE IS RENDERED: when it is new, moved or edited, when the set of captures changes, when the
     * face size changes, and when the scene's sky environment is re-baked (a moved sun relights every capture) -
     * UE's "recapture on change", never every frame. One capture per frame while any is pending.
     *
     * A view whose profile does not read captures (previews, thumbnails, the face views themselves) is handed
     * an empty list and the scene's one-cube black array: the lighting shader declares the bindings.
     */
    class ReflectionCaptureRenderer
    {
    public:
        // @p scene is the owning Core::Scene's ledger instance (Scene::GetLedgerInstance): the capture arrays and
        // the face views' rows are the scene's.
        explicit ReflectionCaptureRenderer( ResourceOwnerInstance scene );
        ~ReflectionCaptureRenderer();
        ReflectionCaptureRenderer( const ReflectionCaptureRenderer& )            = delete;
        ReflectionCaptureRenderer& operator=( const ReflectionCaptureRenderer& ) = delete;

        /// The six face views of the capture under update, each one complete frame (open, record, render, close).
        /// Called by the scene ONCE per frame, BEFORE any of its views opens its own frame: two renderers' frame
        /// brackets never interleave (Core::Scene's view loop). @p record replays the scene's recorded draws into
        /// a face renderer. @p environment identifies the scene's sky bake as its views hold it (a new bake
        /// recaptures everything).
        void RenderCaptureViews( const Core::Scene& scene, const std::function<void( SceneRenderer& )>& record,
                                 Common::Timestep timestep, Common::Timestep realTimestep,
                                 const ShaderProtocols::DirectionLight& dirLights, uint64_t environment );

        /// In a view's graph, before any lit node: the scene's array and capture list published to @p textures
        /// (SceneViewInputs). The first view that reads captures after a face-view frame also records the passes
        /// that turn the face views into the capture's cube; the others register the array as it is.
        /// @p readsCaptures false (ViewProfile::ReflectionCaptures): the empty list and the black array.
        void AddPasses( RDG::Builder& graph, FrameTextures& textures, bool readsCaptures );

        /// The three compute programs that turn face views into a capture's cube, built on the first call; false
        /// when one did not build on this device (then for good, logged once). Asked by every view's BeginScene
        /// before it resolves its methods: the Reflections methods that read the captures are then refused by
        /// FailedProgram()'s name (SceneRenderer, ProgramRefusals).
        [[nodiscard]] bool EnsurePipelines();
        /// The first program that did not build (static storage), empty while EnsurePipelines succeeds.
        [[nodiscard]] std::string_view FailedProgram() const
        {
            return m_FailedProgram;
        }

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

        // The array registered into @p graph, created (and every cube written black) when it does not exist or
        // has another shape. Returns the ref and whether it was created in this graph.
        [[nodiscard]] std::pair<RDG::TextureRef, bool> AcquireArray( RDG::Builder& graph, FrameTextures& textures,
                                                                     SharedArray& array, uint32_t cubes,
                                                                     uint32_t faceSize, std::string_view name );
        void AddCapturePasses( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef array, uint32_t cube,
                               uint32_t faceSize, bool black );

        // The face size the captures are rendered at (Reflections.CaptureSize when they were last gathered).
        uint32_t m_FaceSize = 0;

        // The scene's captures as last gathered, and what has been written into the array (by cube index).
        std::vector<ReflectionCaptureEntry> m_Captures;
        std::vector<bool>                   m_Captured; // written at least once: the lighting reads it
        std::vector<bool>                   m_Done;     // settled: not rendered again until something changes
        uint64_t                            m_Signature = 0;

        // The capture whose faces are being rendered, and how many consecutive frames its face views have had a
        // sky environment (the face views read the scene's sky, SceneSkyEnvironment; the capture is written once
        // it has landed).
        static constexpr uint32_t kNoCapture               = 0xFFFFFFFFu;
        uint32_t                  m_Pending                = kNoCapture;
        uint32_t                  m_PendingSettled         = 0;
        uint64_t                  m_PendingEnvironment     = 0;
        bool                      m_FacesRenderedThisFrame = false;

        std::array<std::unique_ptr<FaceView>, 6> m_Faces;
        // Face views released once every capture settled, kept alive until no frame in flight can still execute
        // their passes: a view's pipelines are destroyed with it, at once (VulkanPipeline::Release), and the frame
        // that rendered the last face is still on the GPU when the next one releases them.
        Assets::FrameRetireQueue<std::unique_ptr<FaceView>> m_RetiringFaces;

        ResourceOwnerInstance m_Scene; // see the constructor

        SharedArray m_Cubes; // the scene's captures
        SharedArray m_Black; // one black cube, for the views that read none
        // Arrays retired by a reshape stay alive while the frames in flight may still sample them.
        std::vector<std::unique_ptr<RDG::ExternalTexture>> m_Retired;

        std::shared_ptr<ComputePipeline> m_Assemble;
        std::shared_ptr<ComputePipeline> m_Downsample;
        std::shared_ptr<ComputePipeline> m_Filter;
        bool                             m_PipelinesFailed = false;
        std::string_view                 m_FailedProgram;
        mutable ShaderBindingLayoutCache m_AssembleLayout;
        mutable ShaderBindingLayoutCache m_DownsampleLayout;
        mutable ShaderBindingLayoutCache m_FilterLayout;
    };
} // namespace Desert::Graphic

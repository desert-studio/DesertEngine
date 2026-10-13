#pragma once

#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
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

    // THE CAPTURE CUBE'S FACE, for every capture of every scene: one cube array holds them all, and an array has
    // one face size. UE's r.ReflectionCaptureResolution default (128); the mips are its whole chain, the last one
    // 1x1 (the capture's average brightness is read from it - ReflectionCaptureFilter.shader).
    inline constexpr uint32_t kReflectionCaptureFaceSize = 128u;
    inline constexpr uint32_t kReflectionCaptureMips = Core::Formats::MipChainLength( kReflectionCaptureFaceSize );
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
     * @brief THE REFLECTION CAPTURES OF ONE VIEW (UE: FReflectionEnvironmentSceneData + the capture update).
     *
     * Every capture of the scene is rendered from its capture point into six 90-degree face views — complete
     * SceneRenderers of this engine, so the cube holds the lit scene, the sky and its clouds exactly as a viewport
     * draws them — resampled into a radiance cube, given its mip chain, GGX-prefiltered into the capture's layers
     * of the view's cube array, and its average brightness stored in alpha. The lighting pass reads them through
     * SceneViewInputs (Common/ReflectionEnvironment.glslh).
     *
     * WHEN A CAPTURE IS RENDERED: when it is new, moved or edited, when the set of captures changes, and when the
     * view's sky environment is re-baked (a moved sun relights every capture) — UE's "recapture on change", never
     * every frame. One capture per frame while any is pending, so a scene of many captures does not stall a frame.
     *
     * A view whose profile does not own captures (previews, thumbnails, the face views themselves) still has one:
     * the lighting shader declares the bindings, so it is handed an empty list and a one-cube black array.
     */
    class ReflectionCaptureRenderer
    {
    public:
        /// @p ownsCaptures: the view renders the scene's captures (ViewProfile::ReflectionCaptures).
        explicit ReflectionCaptureRenderer( bool ownsCaptures );
        ~ReflectionCaptureRenderer();
        ReflectionCaptureRenderer( const ReflectionCaptureRenderer& )            = delete;
        ReflectionCaptureRenderer& operator=( const ReflectionCaptureRenderer& ) = delete;

        /// The six face views of the capture under update, each one complete frame (open, record, render, close).
        /// Called by the scene BEFORE the owning view opens its own frame: two renderers' frame brackets never
        /// interleave (Core::Scene's view loop). @p record replays the scene's recorded draws into a face
        /// renderer.
        /// @p environment identifies the owning view's sky bake (a new bake recaptures everything).
        void RenderCaptureViews( const Core::Scene& scene, const std::function<void( SceneRenderer& )>& record,
                                 Common::Timestep timestep, Common::Timestep realTimestep,
                                 const ShaderProtocols::DirectionLight& dirLights, uint64_t environment );

        /// In the owning view's graph, before any lit node: the passes that turn this frame's face views into the
        /// capture's cube, then the array and the capture list published to @p textures (SceneViewInputs).
        void AddPasses( RDG::Builder& graph, FrameTextures& textures );

    private:
        struct FaceView;

        [[nodiscard]] bool EnsurePipelines();
        void AddCapturePasses( RDG::Builder& graph, FrameTextures& textures, RDG::TextureRef array, uint32_t cube,
                               bool black );

        bool m_OwnsCaptures = false;

        // The scene's captures as last gathered, and what has been written into the array (by cube index).
        std::vector<ReflectionCaptureEntry> m_Captures;
        std::vector<bool>                   m_Captured; // written at least once: the lighting reads it
        std::vector<bool>                   m_Done;     // settled: not rendered again until something changes
        uint64_t                            m_Signature = 0;

        // The capture whose faces are being rendered, and how many consecutive frames its face views have had a
        // sky environment (the face views bake their own sky; the capture is written once they have it).
        static constexpr uint32_t kNoCapture               = 0xFFFFFFFFu;
        uint32_t                  m_Pending                = kNoCapture;
        uint32_t                  m_PendingSettled         = 0;
        uint64_t                  m_PendingEnvironment     = 0;
        bool                      m_FacesRenderedThisFrame = false;

        std::array<std::unique_ptr<FaceView>, 6> m_Faces;

        // The array: extracted when (re)created, registered every frame after. Retired arrays live one more frame.
        RDG::ExternalTexture                               m_Array;
        uint32_t                                           m_ArrayCubes = 0;
        bool                                               m_ArrayBuilt = false;
        std::vector<std::unique_ptr<RDG::ExternalTexture>> m_Retired;

        std::shared_ptr<ComputePipeline> m_Assemble;
        std::shared_ptr<ComputePipeline> m_Downsample;
        std::shared_ptr<ComputePipeline> m_Filter;
        bool                             m_PipelinesFailed = false;
        mutable ShaderBindingLayoutCache m_AssembleLayout;
        mutable ShaderBindingLayoutCache m_DownsampleLayout;
        mutable ShaderBindingLayoutCache m_FilterLayout;
    };
} // namespace Desert::Graphic

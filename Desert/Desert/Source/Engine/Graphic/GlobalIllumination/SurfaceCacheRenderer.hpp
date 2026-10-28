#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>
#include <Engine/Graphic/DistanceField/GlobalDistanceField.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>
#include <Engine/Graphic/GlobalIllumination/SurfaceCacheScene.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    class ComputePipeline;
    class Framebuffer;
    class GraphicsPipeline;
    class Image;
    class Image2D;
    class Shader;
} // namespace Desert::Graphic

// THE SURFACE CACHE'S GPU HALF (GI-SURFACE-CACHE-2): the atlases, the capture atlas and the passes that fill and
// show them. UE: FLumenSceneData's atlases + LumenSceneCardCapture.cpp (the capture atlas, then
// CopyCardsToSurfaceCache) + the "Lumen Scene" / "Surface Cache" visualisations (LumenVisualize.cpp).
//
// THE CAPTURE IS THE G-BUFFER PASS, NOT A SHADER OF ITS OWN (UE LumenCardPixelShader writes the same material
// outputs): MeshRenderer draws the card's mesh through the card's orthographic camera with the surface's own
// view-pass G-buffer cell (MeshRenderer::CardCapturePassState) into this renderer's capture framebuffer, whose
// slots are the G-buffer's formats - the layout that pipeline is built against. AddCopyPass then moves each
// captured card's pages out of it into the atlas layers.
namespace Desert::Graphic::SurfaceCache
{
    class SurfaceCacheRenderer
    {
    public:
        // UE r.LumenScene.SurfaceCache.CardCaptureAtlas: the frame's captures share one atlas of this side; a
        // card that does not fit waits for the next frame (SurfaceCacheScene::Update packs it).
        static constexpr uint32_t kCaptureAtlasSize = 1024;
        // UE r.LumenScene.SurfaceCache.CardCapturesPerFrame (300): the cap on one frame's captures.
        static constexpr uint32_t kMaxCapturesPerFrame = 256;

        using ImportImage = std::function<RDG::TextureRef( const std::shared_ptr<Image>&, std::string_view )>;

        // The captured layers this renderer owns (SurfaceCache::Layer Depth..Subsurface).
        static constexpr std::array<Layer, 6> kCapturedLayers{ Layer::Depth,  Layer::Albedo,   Layer::Opacity,
                                                               Layer::Normal, Layer::Emissive, Layer::Subsurface };
        // The lit layers: the direct lighting (GI-SURFACE-CACHE-4), the indirect lighting off the world radiance
        // cache (GI-CARD-LOOKUP) and their combine. The radiosity count arrives with the radiosity probes.
        static constexpr std::array<Layer, 3> kLitLayers{ Layer::DirectLighting, Layer::IndirectLighting,
                                                          Layer::FinalLighting };

        // The frame's records as graph buffers, uploaded once and read by every pass of the frame.
        struct FrameBuffers
        {
            RDG::BufferRef Cards;
            RDG::BufferRef Pages;
            RDG::BufferRef MeshCards;
            RDG::BufferRef InstanceRows;  // contract B3: the instance table
            RDG::BufferRef LightingPages; // the pages relit this frame (SceneFrame::LightingPages)
        };
        // What the card lighting is lit by: THE SAME inputs the deferred composite reads this frame (its scene
        // view inputs - cascades, cloud map, local shadow atlas -, the view's light buffers, the composite's
        // ShadowUB and cloud values, the sun), plus what a texel off screen needs: the world light grid of those
        // light buffers (Lights.WorldGridParams / WorldGridCells, "LightGridWorld") for its light list, and the
        // active GI tracer of the frame for the shadow the view's maps do not hold (GITraceVisibility).
        struct DirectLightingInputs
        {
            SceneViewInputs                 View;
            ViewLightBuffers                Lights;
            SceneResources::ShadowUBData    Shadow{};
            CloudShadowUniforms             Cloud;
            glm::vec4                       LightDir{ 0.0f, -1.0f, 0.0f, 0.0f };
            glm::vec4                       LightColor{ 0.0f };
            // Atmosphere light 1 (the moon): direction its light travels, colour/intensity (0: none).
            glm::vec4                       Light1Dir{ 0.0f, -1.0f, 0.0f, 0.0f };
            glm::vec4                       Light1Color{ 0.0f };
            GlobalIllumination::TracerFrame Tracer;
            // The indirect lighting runs this frame (AddIndirectLightingPass after the radiance cache update): a
            // page's FinalLighting keeps the IndirectLighting it last received; otherwise it is direct only.
            bool IndirectLighting = false;
        };
        // The world radiance cache of this frame, as the indirect lighting reads it (GlobalIllumination::
        // RadianceCacheFrame: its read section, Common/RadianceCache.glslh RadianceCacheIrradianceAt).
        struct IndirectLightingInputs
        {
            RDG::BufferRef  Params;
            RDG::BufferRef  Indirection;
            RDG::BufferRef  Probes;
            RDG::TextureRef Irradiance;
            RDG::TextureRef Depth;
        };

        // Shaders, the copy pipeline and the capture framebuffer. A renderer may be made before it is initialised
        // (a reflection capture makes its faces' one cache with the faces; the first face that brings the cache up
        // initialises it), so its holders ask IsInitialized rather than whether it exists.
        [[nodiscard]] Common::BoolResultStr Initialize();
        // The capture framebuffer is the last thing Initialize assigns: true only once everything is.
        [[nodiscard]] bool IsInitialized() const
        {
            return m_CaptureBuffer != nullptr;
        }
        // The machine's budget; a new atlas side recreates the layers and drops every capture.
        [[nodiscard]] Common::BoolResultStr Configure( const SurfaceCacheBudget& budget );

        // What a view's frame of the cache works from: the scene's records (SurfaceCacheScene::Update) and whether
        // this call updated the cache - true: the view records the frame's capture, copy, direct and indirect
        // lighting into its graph; false: the frame's update already ran in an earlier graph, the view uploads
        // the same records and reads the same atlases, recording nothing that writes them.
        struct SceneUpdate
        {
            SceneFrame Scene;
            bool       Updated = false;
        };
        // Brings the scene up to @p instances at @p camera, once per @p engineFrame (UPDATED AT MOST ONCE PER
        // ENGINE FRAME, as GlobalDistanceField::AddUpdatePasses): the first call of a frame updates the scene and
        // its caller records the passes that write the atlases; a later one in the same frame for the same camera
        // and instances hands back the same records with Updated false; a later one for another camera or
        // instance set is refused - the scene picked one frame's captures and relit pages, and a second update in
        // the frame would capture and relight over what the frame's earlier graph reads.
        // @p lighting is the frame's directional lights: a significant change from those the lit pages were last
        // fully relit under (GlobalIllumination::InvalidatesLightingHistory) relights every visible page this
        // frame.
        [[nodiscard]] Common::ResultStr<SceneUpdate>
        UpdateScene( const std::vector<SceneInstance>& instances, const glm::vec3& camera, uint64_t engineFrame,
                     const GlobalIllumination::DirectionalLighting& lighting );
        // The scene's tables (rows by placement, the row of an instance). Its frame update is UpdateScene's.
        [[nodiscard]] const SurfaceCacheScene& GetScene() const
        {
            return m_Scene;
        }
        [[nodiscard]] const std::shared_ptr<Framebuffer>& GetCaptureBuffer() const
        {
            return m_CaptureBuffer;
        }

        // The capture framebuffer's graph textures (TargetsOf of GetCaptureBuffer): albedo, normal, the shading
        // word (its model and payload: the subsurface colour), emissive and depth of the frame's capture atlas.
        struct CaptureTextures
        {
            RDG::TextureRef Albedo;
            RDG::TextureRef Normal;
            RDG::TextureRef ShadingWord;
            RDG::TextureRef Emissive;
            RDG::TextureRef Depth;
        };
        // UE CopyCardsToSurfaceCache: one 8 x 8 group per tile of every page of the frame's captures.
        [[nodiscard]] Common::BoolResultStr AddCopyPass( RDG::Builder& graph, const ImportImage& import,
                                                         const CaptureTextures& capture, const SceneFrame& frame );
        [[nodiscard]] FrameBuffers          UploadRecords( RDG::Builder& graph, const SceneFrame& frame ) const;
        // What a traced hit reads of this frame's cache (GIHitRadiance): @p buffers and the atlas layers.
        [[nodiscard]] GlobalIllumination::SurfaceCacheFrame HitReads( const ImportImage&  import,
                                                                      const FrameBuffers& buffers ) const;
        // UE LumenSceneDirectLighting: the frame's LightingPages relit into DirectLighting and FinalLighting.
        [[nodiscard]] Common::BoolResultStr AddDirectLightingPass( RDG::Builder& graph, const ImportImage& import,
                                                                   const SceneFrame&           frame,
                                                                   const FrameBuffers&         buffers,
                                                                   const DirectLightingInputs& inputs );
        // The radiosity slot (SurfaceCacheIndirectLighting.shader): the frame's LightingPages take the radiance
        // cache's irradiance as IndirectLighting and recombine FinalLighting. After the cache's update.
        [[nodiscard]] Common::BoolResultStr
        AddIndirectLightingPass( RDG::Builder& graph, const ImportImage& import, const SceneFrame& frame,
                                 const FrameBuffers& buffers, const IndirectLightingInputs& inputs );
        // "Surface Cache Atlas": the four captured layers, a quadrant each (albedo, normal, depth, opacity).
        [[nodiscard]] Common::BoolResultStr AddAtlasViewPass( RDG::Builder& graph, const ImportImage& import,
                                                              RDG::TextureRef target );
        // "Lumen Scene": the scene drawn from its cards alone - every captured card's surface (its depth layer
        // placed back into the world) shaded with its albedo, or with its FinalLighting when @p lit ("Lumen Scene
        // (Lit)"), depth-tested against the other cards.
        [[nodiscard]] Common::BoolResultStr AddSceneViewPass( RDG::Builder& graph, const ImportImage& import,
                                                              RDG::TextureRef target, const SceneFrame& frame,
                                                              const FrameBuffers& buffers, bool lit,
                                                              const glm::mat4& viewProjection,
                                                              const glm::vec3& cameraPosition );

        // The direct lighting's program and pipeline for @p tracer's permutation (its shadow rays), built on the
        // first call for that tracer. Asked by the view before the frame's passes, so a permutation that does not
        // build refuses the methods it serves by name (SceneRenderer::AddFrameSurfaceCache, ProgramRefusals).
        [[nodiscard]] Common::BoolResultStr EnsureLightingProgram( const GlobalIllumination::Pipeline& tracer );
        // Every variant EnsureLightingProgram can acquire (GlobalIllumination::TracerProgramVariants of
        // SurfaceCacheDirectLighting), cooked by the package (PackageCook CookShaderVariants).
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants();

    private:
        [[nodiscard]] Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
        ViewPipeline( std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>>& cache,
                      const std::shared_ptr<Shader>& shader, Core::Formats::ImageFormat format, uint32_t samples,
                      bool depth, std::string_view name );

        SurfaceCacheScene m_Scene;
        // THE FRAME'S UPDATE (once per frame): the engine frame it ran in, what it was asked for (the camera and
        // the identity of the instance list: key, placement, surface and capture readiness of each, in order),
        // and the records a later call of that frame hands back without updating anything.
        struct FrameUpdate
        {
            uint64_t   Frame = 0;
            glm::vec3  Camera{ 0.0f };
            uint64_t   Instances = 0;
            SceneFrame Scene;
        };
        std::optional<FrameUpdate> m_FrameUpdate;
        // The directional lighting of the last full relight (empty until the first update).
        std::optional<GlobalIllumination::DirectionalLighting>                m_LitBy;
        uint32_t                                                              m_AtlasSize = 0;
        std::array<std::shared_ptr<Image2D>, kCapturedLayers.size()>          m_Layers;
        std::array<std::shared_ptr<Image2D>, 3>                               m_LitLayers;
        std::shared_ptr<Framebuffer>                                          m_CaptureBuffer;
        std::shared_ptr<Shader>                                               m_CopyShader;
        std::shared_ptr<ComputePipeline>                                      m_CopyPipeline;
        mutable ShaderBindingLayoutCache                                      m_CopyLayout;
        std::shared_ptr<Shader>                                               m_AtlasViewShader;
        mutable ShaderBindingLayoutCache                                      m_AtlasViewLayout;
        std::shared_ptr<Shader>                                               m_SceneViewShader;
        mutable ShaderBindingLayoutCache                                      m_SceneViewLayout;
        std::shared_ptr<Shader>                                               m_LightingShader;
        mutable ShaderBindingLayoutCache                                      m_LightingLayout;
        std::shared_ptr<GraphicsPipeline>                                     m_LightingPipeline;
        GlobalIllumination::Pipeline                                          m_LightingProgramFor{};
        std::shared_ptr<Shader>                                               m_IndirectShader;
        mutable ShaderBindingLayoutCache                                      m_IndirectLayout;
        std::shared_ptr<GraphicsPipeline>                                     m_IndirectPipeline;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_AtlasViewPipelines;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_SceneViewPipelines;
    };
} // namespace Desert::Graphic::SurfaceCache

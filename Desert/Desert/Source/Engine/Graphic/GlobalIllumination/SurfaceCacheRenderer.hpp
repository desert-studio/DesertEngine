#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>
#include <Engine/Graphic/DistanceField/GlobalDistanceField.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>
#include <Engine/Graphic/GlobalIllumination/SurfaceCacheScene.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RenderMethods.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/EmissiveLightsRenderer.hpp>

#include <entt/entt.hpp>
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
    // ONE PER SCENE (UE: FScene's FLumenSceneData - the Lumen scene is the scene's, a view owns no cards):
    // Core::Scene::GetSurfaceCache, read by every view of that scene (the editor viewports, PIE, which plays in
    // the edited Scene). A reflection capture's face views stand at one capture point and keep one of their own
    // (ReflectionCaptureRenderer::m_FaceSurfaceCache, SceneRenderer::ShareSurfaceCache).
    class SurfaceCacheRenderer
    {
    public:
        // @p owner: the ledger instance whose rows the cache's device objects are - the scene's
        // (Scene::GetLedgerInstance). Every call that creates one (Initialize, Configure, the programs and view
        // pipelines) opens its scope, whichever view asked first.
        explicit SurfaceCacheRenderer( ResourceOwnerInstance owner );
        SurfaceCacheRenderer( const SurfaceCacheRenderer& )            = delete;
        SurfaceCacheRenderer& operator=( const SurfaceCacheRenderer& ) = delete;

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
            // The scene's emissive triangles as lights (GI-EMISSIVE-LIGHTS; RayTracing::EmissiveLightsFrame, the
            // path tracer's one list): required (Valid) under either GI tracer, whose programs sample them;
            // the list is built from the scene's meshes, a TLAS only adds its hit lookup.
            RayTracing::EmissiveLightsFrame Emissive;
            // The indirect lighting runs this frame (AddIndirectLightingPass after the radiance cache update): a
            // page's FinalLighting keeps the IndirectLighting it last received; otherwise it is direct only.
            bool IndirectLighting = false;
        };
        // What the radiosity (UE LumenRadiosity) is traced and integrated with: the frame's tracer and the GI ray
        // contract's reads - the same a radiance cache probe traces with (WorldRadianceCache::
        // RadianceCacheUpdateInputs: GI frame record, lights, world light grid, sky cube, G-buffer, the cards a
        // hit reads first) - and the world radiance cache of this frame (GlobalIllumination::RadianceCacheFrame's
        // read section): the bounce of a hit no card holds, and the far side of a leaf.
        struct IndirectLightingInputs
        {
            RDG::BufferRef  Params;
            RDG::BufferRef  Indirection;
            RDG::BufferRef  Probes;
            RDG::TextureRef Irradiance;
            RDG::TextureRef Depth;

            GlobalIllumination::TracerFrame       Tracer;
            GlobalIllumination::SurfaceCacheFrame Hits;
            RDG::BufferRef                        GIFrame;
            RDG::BufferRef                        PointLights;
            RDG::BufferRef                        SpotLights;
            RDG::BufferRef                        WorldGridParams;
            RDG::BufferRef                        WorldGridCells;
            RDG::TextureRef                       EnvSpecular;
            RDG::TextureRef                       GBufferDepth;
            RDG::TextureRef                       GBufferA;
            RDG::TextureRef                       GBufferEmissive;
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
        // THE FRAME'S VIEWS, stated by the cache's owner once per engine frame BEFORE any view of the frame opens
        // (Core::Scene::OnUpdate for the scene's cache, ReflectionCaptureRenderer for a capture point's): what the
        // frame's one update is cut from, whichever view runs it (UE: the Lumen scene update gathers every view
        // origin of the family and reads the r.LumenScene settings, never one view's).
        struct FrameViews
        {
            uint64_t Frame = 0;
            // Every view's origin: the cards' distances, so their resolutions and capture order (SurfaceCacheScene::
            // Update). Never empty when stated.
            std::vector<glm::vec3> Origins;
            // The machine's budget (Scalability GlobalIllumination group, QualityState::Resolved).
            SurfaceCacheBudget Budget;
            // The tracer the cards are lit with (their shadow rays): the scene's GI method - its unbound post
            // process settings under the machine's ceiling and the device - when traced, else the software walk
            // every device runs. The updating view records the lighting with it and builds what it reads.
            GlobalIllumination::Pipeline Tracer;
        };
        // States @p origins of engine frame @p engineFrame and resolves the budget and the card tracer from the
        // scene: @p registry's unbound post-process volumes (ResolveViewSettings without a view position) and
        // @p deferred (the scene's rendering path) under the machine's quality and the device catalogue, minus the
        // tracers whose lighting program did not build here (RefuseLightingTracer).
        void BeginFrame( uint64_t engineFrame, std::vector<glm::vec3> origins, const entt::registry& registry,
                         bool deferred );
        // The views stated for @p engineFrame; null when BeginFrame did not run in it.
        [[nodiscard]] const FrameViews* ViewsOf( uint64_t engineFrame ) const
        {
            return m_FrameViews && m_FrameViews->Frame == engineFrame ? &*m_FrameViews : nullptr;
        }
        // @p tracer's lighting program did not build (EnsureLightingProgram failed): the GI methods it serves are
        // refused for the cache's tracer from the next BeginFrame on (the view refuses them for its own GI).
        void RefuseLightingTracer( const GlobalIllumination::Pipeline& tracer );

        // The frame's budget (FrameViews::Budget); a new atlas side recreates the layers and drops every capture.
        // Asked by the view that updates the cache this frame (UpdatedIn false).
        [[nodiscard]] Common::BoolResultStr Configure( const SurfaceCacheBudget& budget );
        // Whether UpdateScene already ran in @p engineFrame: a later view of the frame reads that update.
        [[nodiscard]] bool UpdatedIn( uint64_t engineFrame ) const
        {
            return m_FrameUpdate && m_FrameUpdate->Frame == engineFrame;
        }
        // Whether the atlases exist (Configure ran with a budget): the cache's resident instance, counted by the
        // editor state ("views.surfaceCaches").
        [[nodiscard]] bool IsConfigured() const
        {
            return m_AtlasSize != 0;
        }

        // What a view's frame of the cache works from: the scene's records (SurfaceCacheScene::Update) and whether
        // this call updated the cache - true: the view records the frame's capture, copy, direct and indirect
        // lighting into its graph; false: the frame's update already ran in an earlier graph, the view uploads
        // the same records and reads the same atlases, recording nothing that writes them.
        struct SceneUpdate
        {
            SceneFrame Scene;
            bool       Updated = false;
        };
        // Brings the scene up to @p instances seen from every origin of the frame's views (BeginFrame, required in
        // @p engineFrame), once per @p engineFrame (UPDATED AT MOST ONCE PER ENGINE FRAME, as
        // GlobalDistanceField::AddUpdatePasses): the first call of a frame updates the scene and its caller
        // records the passes that write the atlases; every later one in the same frame - another view of the
        // scene - hands back the frame's records with Updated false and reads the same atlases (UE: one Lumen
        // scene update per scene render, read by all its views). A second update in the frame would capture and
        // relight over what the frame's earlier graph reads.
        // @p lighting is the frame's directional lights: a significant change from those the lit pages were last
        // fully relit under (GlobalIllumination::InvalidatesLightingHistory) relights every visible page this
        // frame.
        [[nodiscard]] Common::ResultStr<SceneUpdate>
        UpdateScene( const std::vector<SceneInstance>& instances, uint64_t engineFrame,
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
        // UE LumenRadiosity: the frame's LightingPages trace their radiosity probes (SurfaceCacheRadiosityTrace.shader,
        // into the radiosity trace atlas with its history), then integrate them into IndirectLighting and recombine
        // FinalLighting (SurfaceCacheIndirectLighting.shader). After the radiance cache's update.
        [[nodiscard]] Common::BoolResultStr
        AddIndirectLightingPass( RDG::Builder& graph, const ImportImage& import, const SceneFrame& frame,
                                 const FrameBuffers& buffers, const IndirectLightingInputs& inputs );
        // "Surface Cache Atlas": the four captured layers, a quadrant each (albedo, normal, depth, opacity).
        [[nodiscard]] Common::BoolResultStr AddAtlasViewPass( RDG::Builder& graph, const ImportImage& import,
                                                              RDG::TextureRef target );
        // "Lumen Scene": the scene drawn from its cards alone - every captured card's surface (its depth layer
        // placed back into the world) shaded with its albedo, or with its FinalLighting when @p lit ("Lumen Scene
        // (Lit)", scaled by the view's @p preExposure as scene colour is), depth-tested against the other cards.
        [[nodiscard]] Common::BoolResultStr AddSceneViewPass( RDG::Builder& graph, const ImportImage& import,
                                                              RDG::TextureRef target, const SceneFrame& frame,
                                                              const FrameBuffers& buffers, bool lit,
                                                              const glm::mat4& viewProjection,
                                                              const glm::vec3& cameraPosition, float preExposure );

        // The direct lighting's program and pipeline for @p tracer's permutation (its shadow rays), built on the
        // first call for that tracer. Asked by the view before the frame's passes, so a permutation that does not
        // build refuses the methods it serves by name (SceneRenderer::AddFrameSurfaceCache, ProgramRefusals).
        [[nodiscard]] Common::BoolResultStr EnsureLightingProgram( const GlobalIllumination::Pipeline& tracer );
        // The radiosity trace's program and pipeline for @p tracer's permutation, built on the first call for it.
        [[nodiscard]] Common::BoolResultStr EnsureRadiosityProgram( const GlobalIllumination::Pipeline& tracer );
        // Every variant EnsureLightingProgram and EnsureRadiosityProgram can acquire (GlobalIllumination::
        // TracerProgramVariants of SurfaceCacheDirectLighting and SurfaceCacheRadiosityTrace), cooked by the
        // package (PackageCook CookShaderVariants).
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants();

    private:
        [[nodiscard]] Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
        ViewPipeline( std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>>& cache,
                      const std::shared_ptr<Shader>& shader, Core::Formats::ImageFormat format, uint32_t samples,
                      bool depth, std::string_view name );

        SurfaceCacheScene m_Scene;
        // THE FRAME'S UPDATE (once per frame): the engine frame it ran in and the records a later call of that
        // frame hands back without updating anything.
        struct FrameUpdate
        {
            uint64_t   Frame = 0;
            SceneFrame Scene;
        };
        ResourceOwnerInstance     m_LedgerInstance; // see the constructor
        std::optional<FrameUpdate> m_FrameUpdate;
        std::optional<FrameViews>  m_FrameViews; // BeginFrame's, the latest frame
        // The tracers whose lighting program did not build (RefuseLightingTracer): BeginFrame resolves under them.
        ProgramRefusals m_Refusals;
        // The directional lighting of the last full relight (empty until the first update).
        std::optional<GlobalIllumination::DirectionalLighting>                m_LitBy;
        uint32_t                                                              m_AtlasSize = 0;
        std::array<std::shared_ptr<Image2D>, kCapturedLayers.size()>          m_Layers;
        std::array<std::shared_ptr<Image2D>, 3>                               m_LitLayers;
        // The radiosity probes' traced radiance, one hemisphere stratum per atlas texel (UE
        // RadiosityTraceRadianceAtlas, PF_FloatR11G11B10), blended with its history.
        std::shared_ptr<Image2D> m_RadiosityTrace;
        // The directional lights changed this frame (a full relight): the radiosity keeps no history.
        bool                     m_ResetRadiosity = true;
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
        std::shared_ptr<Shader>                                               m_RadiosityShader;
        mutable ShaderBindingLayoutCache                                      m_RadiosityLayout;
        std::shared_ptr<GraphicsPipeline>                                     m_RadiosityPipeline;
        GlobalIllumination::Pipeline                                          m_RadiosityProgramFor{};
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_AtlasViewPipelines;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_SceneViewPipelines;
    };
} // namespace Desert::Graphic::SurfaceCache

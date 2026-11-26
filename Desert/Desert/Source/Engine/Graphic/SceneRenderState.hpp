#pragma once

// THE SCENE'S RENDERER STATE (UE: FScene - the renderer module's half of a world): every renderer object cached on
// a Core::Scene and read by each of its views. The scene owns it through Core::ISceneRenderState (Engine module),
// made by the maker StartupRendererModule registers; renderer code reaches it with RenderStateOf( scene ).

#include <Engine/Assets/FrameRetireQueue.hpp>
#include <Engine/Core/SceneRenderState.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <memory>

namespace Desert::Graphic::RayTracing
{
    class RayTracingSceneCache;
}

namespace Desert::Graphic::GpuDriven
{
    class GpuSceneRegistry;
}

namespace Desert::Graphic::DistanceField
{
    class DistanceFieldSceneData;
}

namespace Desert::Graphic::SurfaceCache
{
    class SurfaceCacheRenderer;
}

namespace Desert::Graphic
{
    class ReflectionCaptureRenderer;
    class SceneSkyLight;
    class SceneAtmosphere;
    class LocalShadowCache;

    class SceneRenderState final : public Core::ISceneRenderState
    {
    public:
        explicit SceneRenderState( ResourceOwnerInstance ledger );
        ~SceneRenderState() override;

        SceneRenderState( const SceneRenderState& )            = delete;
        SceneRenderState& operator=( const SceneRenderState& ) = delete;

        void BeginFrame( const entt::registry& registry, bool deferred, std::uint64_t absoluteFrame,
                         std::uint32_t framesInFlight, std::vector<glm::vec3> viewOrigins ) override;
        void RenderReflectionCaptures( const Core::Scene&                           scene,
                                       const std::function<void( SceneRenderer& )>& record,
                                       Common::Timestep timestep, Common::Timestep realTimestep,
                                       const ShaderProtocols::DirectionLight& dirLights,
                                       const std::optional<Environment>&      environment,
                                       bool                                   skyRecapturesForSun ) override;
        void Clear( std::uint64_t clearFrame ) override;
        void ReleaseParked() override;

        // THE SCENE'S REFLECTION CAPTURES (UE: FScene's FReflectionEnvironmentSceneData): one set, rendered once
        // per frame before the views (Scene::OnUpdate) and read by every view of this scene. A view that renders a
        // const scene publishes them into its own graph.
        [[nodiscard]] ReflectionCaptureRenderer& GetReflectionCaptures() const
        {
            return *m_ReflectionCaptures;
        }

        // THE SCENE'S RAY-TRACING GEOMETRY (UE: FScene's GPU skin cache and cached RT geometry): skinned BLASes
        // skinned once per frame and hidden-submesh BLAS variants, shared by every view's TLAS.
        [[nodiscard]] RayTracing::RayTracingSceneCache& GetRayTracingCache() const
        {
            return *m_RayTracingCache;
        }

        // THE SCENE'S GPU SCENE (UE FScene::GPUScene, GpuDriven/GpuSceneRegistry.hpp): the persistent primitive /
        // instance / palette tables every view of this scene reads (velocity, skin cache, TLAS custom index, GI
        // card rows), synced by each view's MeshRenderer::SyncGpuScene.
        [[nodiscard]] GpuDriven::GpuSceneRegistry& GetGpuScene() const
        {
            return *m_GpuScene;
        }

        // THE SCENE'S SKY LIGHT (UE: FScene's SkyLight and its real-time capture): one capture of the procedural
        // sky's light, shared by every view of this scene and every face view of its reflection captures; a view
        // holds it weakly (SkyboxRenderer).
        [[nodiscard]] std::weak_ptr<SceneSkyLight> GetSkyLight() const
        {
            return m_SkyLight;
        }
        // THE SCENE'S ATMOSPHERE (UE: FSkyAtmosphereRenderSceneInfo): its LUT programs and the view-independent
        // LUTs, recorded once per frame by the scene's first view; held weakly by a view, like the sky light.
        [[nodiscard]] std::weak_ptr<SceneAtmosphere> GetAtmosphere() const
        {
            return m_Atmosphere;
        }

        // THE SCENE'S CACHED POINT/SPOT-LIGHT SHADOW DEPTH (UE: FScene's FCachedShadowMapData per light): one
        // atlas for every view of this scene, the reflection captures' face views included.
        [[nodiscard]] LocalShadowCache& GetLocalShadows() const
        {
            return *m_LocalShadows;
        }

        // THE SCENE'S MESH DISTANCE FIELDS (UE: FScene's FDistanceFieldSceneData): one brick atlas, one set of
        // records and one streaming for every view of this scene, the reflection captures' face views included.
        [[nodiscard]] DistanceField::DistanceFieldSceneData& GetDistanceFields() const
        {
            return *m_DistanceFields;
        }

        // THE SCENE'S SURFACE CACHE (UE: FScene's FLumenSceneData): one card atlas, one capture and one relight
        // per engine frame for every view of this scene - the editor viewports and PIE, which plays in this
        // Scene. Its device objects are made by the first view that brings it up.
        [[nodiscard]] SurfaceCache::SurfaceCacheRenderer& GetSurfaceCache() const
        {
            return *m_SurfaceCache;
        }

    private:
        // The scene's ledger instance: every device object above is a row of it.
        ResourceOwnerInstance m_Ledger;

        // Never null: made by the constructor, remade by Clear.
        std::unique_ptr<ReflectionCaptureRenderer>             m_ReflectionCaptures;
        std::unique_ptr<RayTracing::RayTracingSceneCache>      m_RayTracingCache;
        std::unique_ptr<GpuDriven::GpuSceneRegistry>           m_GpuScene;
        std::shared_ptr<SceneSkyLight>                         m_SkyLight; // shared: a view holds it weakly
        std::shared_ptr<SceneAtmosphere>                       m_Atmosphere;
        std::unique_ptr<LocalShadowCache>                      m_LocalShadows;
        std::unique_ptr<DistanceField::DistanceFieldSceneData> m_DistanceFields;
        std::unique_ptr<SurfaceCache::SurfaceCacheRenderer>    m_SurfaceCache;
        // The surface caches and capture renderers Clear dropped with their world, kept until no frame in flight
        // reads them (collected at the fence by BeginFrame).
        Assets::FrameRetireQueue<std::unique_ptr<SurfaceCache::SurfaceCacheRenderer>> m_RetiringSurfaceCaches;
        Assets::FrameRetireQueue<std::unique_ptr<ReflectionCaptureRenderer>>          m_RetiringReflectionCaptures;
    };

    /// The renderer state of @p scene. A scene of a process that runs the renderer always has one (the Renderer
    /// module's StartupModule registered its maker before any scene was made).
    [[nodiscard]] SceneRenderState& RenderStateOf( const Core::Scene& scene );

    /// The Renderer module's registration with the engine (Desert/Launch/LinkedModules.cpp): the scene renderer
    /// state maker (Core::ModuleHookLists::SceneRenderState). Idempotent.
    void StartupRendererModule();
} // namespace Desert::Graphic

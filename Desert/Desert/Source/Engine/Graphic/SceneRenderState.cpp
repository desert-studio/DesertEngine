#include <Engine/Graphic/SceneRenderState.hpp>

#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/RenderTask.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Graphic/DistanceField/DistanceFieldSceneData.hpp>
#include <Engine/Graphic/GlobalIllumination/SurfaceCacheRenderer.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneRegistry.hpp>
#include <Engine/Graphic/LocalShadowCache.hpp>
#include <Engine/Graphic/ReflectionCaptures/ReflectionCaptureRenderer.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneCache.hpp>
#include <Engine/Graphic/Systems/Scene/Skybox/SceneAtmosphere.hpp>
#include <Engine/Graphic/Systems/Scene/Skybox/SceneSkyLight.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Core/Profiler.hpp>

namespace Desert::Graphic
{
    SceneRenderState::SceneRenderState( ResourceOwnerInstance ledger )
         : m_Ledger( ledger ), m_ReflectionCaptures( std::make_unique<ReflectionCaptureRenderer>( ledger ) ),
           m_RayTracingCache( std::make_unique<RayTracing::RayTracingSceneCache>( ledger ) ),
           m_GpuScene( std::make_unique<GpuDriven::GpuSceneRegistry>( ledger ) ),
           m_SkyLight( std::make_shared<SceneSkyLight>( ledger ) ),
           m_Atmosphere( std::make_shared<SceneAtmosphere>() ),
           m_LocalShadows( std::make_unique<LocalShadowCache>( ledger ) ),
           m_DistanceFields( std::make_unique<DistanceField::DistanceFieldSceneData>( ledger ) ),
           m_SurfaceCache( std::make_unique<SurfaceCache::SurfaceCacheRenderer>( ledger ) )
    {
    }

    SceneRenderState::~SceneRenderState() = default;

    void SceneRenderState::BeginFrame( const entt::registry& registry, bool deferred, std::uint64_t absoluteFrame,
                                       std::uint32_t framesInFlight, std::vector<glm::vec3> viewOrigins )
    {
        // THE SCENE'S SURFACE CACHE, ONCE FOR EVERY VIEW (UE: the Lumen scene update gathers every view origin of
        // the family before any view renders): the frame's view origins and the scene's budget and card tracer are
        // stated before the first view opens, so the one update the first view to reach the cache runs captures
        // the cards in front of every viewport, not its own.
        ::Desert::Engine::RenderTask::Get().AfterViews(
             [this, absoluteFrame, framesInFlight]()
             {
                 ::Desert::Engine::RenderTask::CheckRenderStateWrite( "Scene: retire queues (Collect)" );
                 m_RetiringSurfaceCaches.Collect( absoluteFrame, framesInFlight );
                 m_RetiringReflectionCaptures.Collect( absoluteFrame, framesInFlight );
                 m_RetiringSkyLights.Collect( absoluteFrame, framesInFlight );
             } );
        // The registry is read HERE, on the game thread (the scene's unbound volumes); the command takes the
        // values. The cache outlives the command: a Clear parks it (m_RetiringSurfaceCaches) for frames.
        const Core::PostProcessSettings           scenePost = ResolveViewSettings( registry, std::nullopt ).Post;
        SurfaceCache::SurfaceCacheRenderer* const cache     = m_SurfaceCache.get();
        ::Desert::Engine::RenderTask::Get().Enqueue( "Scene: Surface cache frame",
                                                     [cache, absoluteFrame, origins = std::move( viewOrigins ),
                                                      scenePost, deferred]() -> Common::BoolResultStr
                                                     {
                                                         DESERT_PROFILE_SCOPE( "Scene: Surface cache frame" );
                                                         cache->BeginFrame( absoluteFrame, origins, scenePost,
                                                                            deferred );
                                                         return BOOLSUCCESS;
                                                     } );
    }

    void SceneRenderState::RenderReflectionCaptures( const Core::Scene&                           scene,
                                                     const std::function<void( SceneRenderer& )>& record,
                                                     Common::Timestep timestep, Common::Timestep realTimestep,
                                                     const ShaderProtocols::DirectionLight& dirLights,
                                                     const std::optional<Environment>&      environment,
                                                     bool                                   skyRecapturesForSun )
    {
        m_ReflectionCaptures->RenderCaptureViews( scene, record, timestep, realTimestep, dirLights, environment,
                                                  skyRecapturesForSun );
    }

    void SceneRenderState::Clear( std::uint64_t clearFrame )
    {
        // The GPU scene is keyed by entity (MotionKey): the restarted ids would inherit the old world's
        // primitives and previous transforms, so the reloaded world gets a registry of its own.
        m_GpuScene = std::make_unique<GpuDriven::GpuSceneRegistry>( m_Ledger );
        // THE OLD WORLD'S RENDERER STATE GOES WITH IT. Kept, the distance-field brick atlas, the captures' cubes,
        // the cached shadow depth and the ray-tracing variants of the old world stayed resident under the next
        // scene and were still there when a reopen built the same ones again (FIELD-MEM p3). Their device objects
        // are queued for deletion here.
        // The captures' renderer records into the views' frame graphs (faces, its face surface cache and global
        // distance field, its own retire queues): frames in flight still read them, so it is parked whole like the
        // surface cache below and dies with its queues once those frames retired (BeginFrame collects).
        m_RetiringReflectionCaptures.Park( std::move( m_ReflectionCaptures ), clearFrame );
        m_ReflectionCaptures = std::make_unique<ReflectionCaptureRenderer>( m_Ledger );
        m_RayTracingCache    = std::make_unique<RayTracing::RayTracingSceneCache>( m_Ledger );
        m_LocalShadows       = std::make_unique<LocalShadowCache>( m_Ledger );
        // The brick atlas is the project's pool, not the world's: the next world takes it over empty
        // (DistanceFieldSceneData::AdoptPool) - freed and allocated again, Stop's 384 MiB was resident twice
        // while the driver gave the old one back (FOLIAGE-LOOK-8).
        {
            auto next = std::make_unique<DistanceField::DistanceFieldSceneData>( m_Ledger );
            next->AdoptPool( *m_DistanceFields );
            m_DistanceFields = std::move( next );
        }
        // The surface cache's atlases, capture framebuffer and pipelines are still read by the frames in flight
        // (its pipelines are released at once, not queued): parked until those frames retired (BeginFrame
        // collects).
        m_RetiringSurfaceCaches.Park( std::move( m_SurfaceCache ), clearFrame );
        m_SurfaceCache = std::make_unique<SurfaceCache::SurfaceCacheRenderer>( m_Ledger );
        // THE SKY LIGHT IS THE WORLD'S (UE: FScene's SkyLight). Kept, the next world inherited the old world's
        // captured cube and stamps: its first capture was the deferred, time-sliced one ("the sun is 172 deg from
        // the captured one"), and for its 54 frames and the settle every GI miss read the old world's sky
        // (RT-HWGI-NIGHT). The new one has no environment, so its first capture is whole and immediate
        // (SkyboxRenderer: !hasEnvironment), and until it lands the miss reads the black cube.
        m_RetiringSkyLights.Park( std::move( m_SkyLight ), clearFrame );
        m_SkyLight = std::make_shared<SceneSkyLight>( m_Ledger );
    }

    void SceneRenderState::ReleaseParked()
    {
        m_RetiringSurfaceCaches.Clear();
        m_RetiringReflectionCaptures.Clear();
        m_RetiringSkyLights.Clear();
    }

    SceneRenderState& RenderStateOf( const Core::Scene& scene )
    {
        Core::ISceneRenderState* const state = scene.GetRenderState();
        DESERT_VERIFY( state != nullptr, "a scene rendered without the Renderer module's state" );
        return static_cast<SceneRenderState&>( *state );
    }

    void StartupRendererModule()
    {
        Core::ModuleHooks().SceneRenderState =
             []( ResourceOwnerInstance ledger ) -> std::unique_ptr<Core::ISceneRenderState>
        { return std::make_unique<SceneRenderState>( ledger ); };
    }
} // namespace Desert::Graphic

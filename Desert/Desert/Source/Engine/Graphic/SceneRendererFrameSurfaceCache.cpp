// SceneRenderer's surface-cache node (GI-SURFACE-CACHE-2, GI-CARD-LOOKUP): the Lumen scene update, the card
// capture through the G-buffer view pass, the copy into the atlas, the direct lighting of the cards - in the lit
// frame, before the traced GI whose hits read it (GIHitRadiance) - and the three view modes that show it.
#include <Common/Core/DevInstruments.hpp>
#include <Engine/Assets/MeshCardRepresentation.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/GlobalIllumination/SurfaceCacheRenderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    bool SceneRenderer::SurfaceCacheRuns() const
    {
        if ( m_RenderPath != Core::RenderPath::Deferred )
            return false;
        // The GI's cards only where the view runs the GI (RadianceCacheRuns asks the same of its profile): a
        // capture's six face views carry the scene's GI method but not the GI, and each brought up its own cards
        // and their global distance field (384 MiB apiece) - 7 fields for one capture, past 6 GB on Starter
        // (REFL-CAPTURE-NORM).
        if ( ( m_GIPipeline.SurfaceCache && m_ViewProfile.GlobalIllumination ) || ReflectionsTraced() )
            return true;
#if DESERT_DEV_INSTRUMENTS
        // Its views bring it up on their own (and light its cards) under any GI method.
        const DeferredDebugMode mode = m_DebugView.DeferredDebug;
        return mode == DeferredDebugMode::SurfaceCacheAtlas || mode == DeferredDebugMode::SurfaceCacheScene ||
               mode == DeferredDebugMode::SurfaceCacheSceneLit;
#else
        return false;
#endif // DESERT_DEV_INSTRUMENTS
    }

    SurfaceCache::SurfaceCacheRenderer* SceneRenderer::ReadSurfaceCache() const
    {
        SurfaceCache::SurfaceCacheRenderer* const cache =
             m_SharedSurfaceCache != nullptr ? m_SharedSurfaceCache : m_FrameSurfaceCache;
        return cache != nullptr && cache->IsInitialized() ? cache : nullptr;
    }

    SurfaceCache::SurfaceCacheRenderer* SceneRenderer::BoundSurfaceCache() const
    {
        return m_SharedSurfaceCache != nullptr ? m_SharedSurfaceCache : m_FrameSurfaceCache;
    }

    const SurfaceCache::SurfaceCacheRenderer::FrameViews* SceneRenderer::SurfaceCacheFrameViews() const
    {
        const SurfaceCache::SurfaceCacheRenderer* const cache = BoundSurfaceCache();
        return cache != nullptr ? cache->ViewsOf( Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() )
                                : nullptr;
    }

    GlobalIllumination::Pipeline SceneRenderer::SurfaceCacheTracer() const
    {
        // THE CACHE'S, NOT THE VIEW'S: one scene's cards are lit once per frame for every view, with the tracer the
        // cache's owner resolved from the scene (SurfaceCacheRenderer::BeginFrame). Untraced (nothing to walk)
        // when the owner stated no views this frame - AddFrameSurfaceCache refuses that frame by name.
        const SurfaceCache::SurfaceCacheRenderer::FrameViews* const views = SurfaceCacheFrameViews();
        return views != nullptr ? views->Tracer : GlobalIllumination::Pipeline{};
    }

    bool SceneRenderer::SurfaceCacheUpdateTracesRayQuery() const
    {
        if ( !SurfaceCacheRuns() )
            return false;
        const SurfaceCache::SurfaceCacheRenderer* const cache = BoundSurfaceCache();
        if ( cache == nullptr ||
             cache->UpdatedIn( Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() ) )
            return false;
        const GlobalIllumination::Pipeline tracer = SurfaceCacheTracer();
        return tracer.Traced && tracer.Tracer == GlobalIllumination::Tracer::HardwareRayQuery;
    }

    GlobalIllumination::TracerFrame SceneRenderer::MakeTracerFrame( RDG::Builder&                       graph,
                                                                    const FrameTextures&                textures,
                                                                    const GlobalIllumination::Pipeline& pipeline )
    {
        const FrameTransients&          t = textures.Transients;
        GlobalIllumination::TracerFrame tracer{ .Pipeline                      = pipeline,
                                                .GlobalDistanceField           = t.GlobalDistanceField,
                                                .GlobalDistanceFieldObjectGrid = t.GlobalDistanceFieldObjectGrid,
                                                .MeshDistanceFields            = t.MeshDistanceFields };
        // The software walk's clipmap and near-field grid placement is its own record ("GITracerSoftwareUB").
        if ( GlobalIllumination::TracerReadsGlobalDistanceField( pipeline ) )
        {
            const GlobalIllumination::TracerSoftwarePayload payload{
                 .GlobalDistanceFieldLevels = t.GlobalDistanceFieldLevels,
                 .MeshSDFGrid               = t.MeshDistanceFields.NearFieldGridPlacement };
            tracer.GlobalDistanceFieldLevels =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( payload ) }, "GI.Tracer.GlobalDistanceFieldLevels" );
            graph.QueueBufferUpload( tracer.GlobalDistanceFieldLevels, std::as_bytes( std::span( &payload, 1 ) ) );
        }
        // The ray-query walk's own records: this frame's TLAS and its segment records (RT-SCENE / RT-SHADOWS), and
        // the instance -> surface cache row table. Left invalid when the TLAS was not built: TracerFrameMissing
        // names it, the consumer refuses the frame.
        if ( pipeline.Traced && pipeline.Tracer == GlobalIllumination::Tracer::HardwareRayQuery )
        {
            const RayTracing::RayTracingSceneFrame& scene = m_RayTracingScene.GetFrame();
            if ( scene.Built )
            {
                tracer.Tlas                     = scene.Tlas;
                tracer.Metadata                 = scene.Metadata;
                tracer.InstanceSurfaceCacheRows = AddFrameInstanceSurfaceCacheRows( graph );
            }
        }
        return tracer;
    }

    RDG::BufferRef SceneRenderer::AddFrameInstanceSurfaceCacheRows( RDG::Builder& graph )
    {
        if ( m_TracedFrameShared.InstanceSurfaceCacheRows.IsValid() )
            return m_TracedFrameShared.InstanceSurfaceCacheRows;
        SurfaceCache::SurfaceCacheRenderer* const cache = ReadSurfaceCache();
        if ( cache == nullptr || !m_RayTracingScene.GetFrame().Built )
            return {};
        // UE GetMeshCardsIndexFromSceneInstanceIndex: a hit's scene instance -> its cards. Here the TLAS custom
        // index is the primitive's GPU-scene row (RayTracingInstance::CustomIndex), and the surface cache knows
        // an instance by its placement (mesh, world: SurfaceCacheScene::PlacementKey) - the key the global
        // distance field's object grid uses for the same rows. A row with no surface cache instance (a skinned
        // mesh, a landscape tile, a mesh the cache does not hold) reads kNoInstance: the hit is lit in place.
        const std::unordered_map<uint64_t, uint32_t>           rows = cache->GetScene().RowsByPlacement();
        const std::span<const RayTracing::RayTracingPrimitive> primitives = m_RayTracingScene.GetPrimitives();
        uint32_t                                               count      = 1u; // a graph buffer cannot be empty
        // A primitive with no GPU-scene row (kNoGpuSceneRow: the GPU-instanced grass) has no entry: its hits read
        // past the table, GI_NO_INSTANCE.
        for ( const RayTracing::RayTracingPrimitive& primitive : primitives )
            if ( primitive.GpuSceneRow <= RayTracing::kMaxGpuSceneRow )
                count = std::max( count, primitive.GpuSceneRow + 1u );
        std::vector<uint32_t> table( count, SurfaceCache::kNoInstance );
        for ( const RayTracing::RayTracingPrimitive& primitive : primitives )
        {
            if ( primitive.SourceMesh == nullptr || primitive.GpuSceneRow > RayTracing::kMaxGpuSceneRow )
                continue;
            const auto it = rows.find(
                 SurfaceCache::SurfaceCacheScene::PlacementKey( primitive.SourceMesh, primitive.World ) );
            if ( it != rows.end() )
                table[primitive.GpuSceneRow] = it->second;
        }
        const RDG::BufferRef buffer = graph.CreateBuffer( RDG::BufferDesc{ table.size() * sizeof( uint32_t ) },
                                                          "GI.Tracer.InstanceSurfaceCacheRows" );
        graph.QueueBufferUpload( buffer, std::as_bytes( std::span<const uint32_t>( table ) ) );
        m_TracedFrameShared.InstanceSurfaceCacheRows = buffer;
        return buffer;
    }

    void SceneRenderer::AddFrameSurfaceCache( RDG::Builder& graph, FrameTextures& textures,
                                              const ViewFrame& frame )
    {
        m_SurfaceCacheFrame.reset();
        if ( !SurfaceCacheRuns() )
            return;
        const auto  it = m_RenderSystems.find( "MeshSystem" );
        auto* const meshRenderer =
             it != m_RenderSystems.end() ? UNIQUE_GET_AS( System::MeshRenderer, it->second ) : nullptr;
        if ( meshRenderer == nullptr )
        {
            LOG_ERROR( "[SceneRenderer] Surface Cache: no MeshSystem" );
            return;
        }
        // The cache this view reads: the one lent to it (ShareSurfaceCache: a reflection capture's face views share
        // their capture's), else its scene's (Scene::GetSurfaceCache, one for every view of the scene: UE's Lumen
        // scene is FScene's); initialised by the first view that brings it up.
        SurfaceCache::SurfaceCacheRenderer* const cache = BoundSurfaceCache();
        if ( cache == nullptr )
        {
            LOG_ERROR( "[SceneRenderer] Surface Cache: no scene bound (AddFrameSurfaceCache outside BeginScene)" );
            return;
        }
        const uint64_t engineFrame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        // The frame's views, budget and card tracer: the cache owner's (Scene::OnUpdate, a reflection capture),
        // stated before the frame's first view opened.
        const SurfaceCache::SurfaceCacheRenderer::FrameViews* const frameViews = cache->ViewsOf( engineFrame );
        if ( frameViews == nullptr )
        {
            LOG_ERROR( "[SceneRenderer] Surface Cache: its owner stated no views for frame {} "
                       "(SurfaceCacheRenderer::BeginFrame)",
                       engineFrame );
            return;
        }
        // THE FRAME'S FIRST VIEW UPDATES THE CACHE (SurfaceCacheRenderer::UpdateScene) from what the owner stated
        // for every view (its origins, budget and card tracer); a later view of the same frame reads that update
        // and records nothing that writes the atlases, so it neither re-cuts the budget nor asks for the program.
        const bool updatesCache = !cache->UpdatedIn( engineFrame );
        if ( !cache->IsInitialized() )
        {
            if ( const auto initialized = cache->Initialize(); !initialized )
            {
                // The cards light every traced GI method's hits and the traced reflections' hits.
                RefuseProgramMethods( GlobalIllumination::kCacheGIMethods, "SurfaceCache",
                                      initialized.GetError() );
                RefuseProgramMethods( kTracedReflectionMethods, "SurfaceCache", initialized.GetError() );
                return;
            }
        }
        // The cards' direct lighting for this frame's tracer (its shadow rays), asked before anything is
        // recorded. The hardware permutation serves HardwareTraced GI alone: refused, GI walks down to the
        // software tracer and the cards are relit by it. The software permutation lights the cards for every
        // other reader - the software-traced GI methods and, whenever GI is not hardware traced, the traced
        // reflections' hits (SurfaceCacheTracer) - so it refuses those too.
        if ( updatesCache )
        {
            const GlobalIllumination::Pipeline cardTracer = frameViews->Tracer;
            // The cards' local shadows are the cache's (FrameViews::VirtualLocalShadows, resolved from the scene by
            // the owner): the view recording the update must have built the same kind - the atlas, or the virtual
            // maps - since the lighting reads this view's. A view whose Shadows method differs is refused by name.
            if ( frameViews->VirtualLocalShadows != LocalShadowsAreVirtual() )
            {
                LOG_ERROR( "[SceneRenderer] Surface Cache: the cache's local shadows are {} and this view, which "
                           "updates it this frame, draws {} (its Shadows method differs from the scene's)",
                           frameViews->VirtualLocalShadows ? "virtual maps" : "the atlas",
                           LocalShadowsAreVirtual() ? "virtual maps" : "the atlas" );
                return;
            }
            if ( const auto program = cache->EnsureLightingProgram( cardTracer, frameViews->VirtualLocalShadows );
                 !program )
            {
                // The cache resolves its next tracer without it; the view lowers its own GI off it.
                cache->RefuseLightingTracer( cardTracer );
                RefuseProgramMethods( GlobalIllumination::TraceProgramGIMethods( cardTracer, false ),
                                      "SurfaceCacheDirectLighting", program.GetError() );
                if ( cardTracer.Tracer != GlobalIllumination::Tracer::HardwareRayQuery )
                    RefuseProgramMethods( kTracedReflectionMethods, "SurfaceCacheDirectLighting",
                                          program.GetError() );
                return;
            }
            // The machine's budget (Scalability GlobalIllumination group, stated with the frame's views), every
            // frame: a quality change re-cuts it.
            if ( const auto configured = cache->Configure( frameViews->Budget ); !configured )
            {
                LOG_ERROR( "[SceneRenderer] Surface Cache: {}", configured.GetError() );
                return;
            }
        }

        // THE LUMEN SCENE: every static, instanced, generic and skinned mesh of the frame with cards, whole - the
        // cache holds what is off-screen too (a traced ray leaves the frustum), seen from every view of the frame.
        // Updated at most once per engine frame (SurfaceCacheRenderer::UpdateScene): a cache another view
        // already updated this frame is read - its records uploaded into this graph, its atlases imported - and
        // nothing that writes the atlases (capture, copy, direct and indirect lighting) is recorded again.
        std::vector<SurfaceCache::SceneInstance> instances;
        meshRenderer->GatherSurfaceCacheInstances( instances );
        auto update = cache->UpdateScene( instances, engineFrame, GetFrameDirectionalLighting() );
        if ( !update )
        {
            LOG_ERROR( "[SceneRenderer] Surface Cache: {}", update.GetError() );
            return;
        }
        SurfaceCache::SurfaceCacheRenderer::SceneUpdate taken   = update.ExtractValue();
        const bool                                      updated = taken.Updated;
        SurfaceCache::SceneFrame                        scene   = std::move( taken.Scene );

        const auto import = [&textures]( const std::shared_ptr<Image>& image, const std::string_view name )
        { return textures.Import( image, name ); };

        // THE CAPTURE: the cards picked this frame, each drawn by MeshRenderer into its rectangle of the capture
        // atlas through its surface's own view-pass G-buffer cell (albedo, normal, emissive; reversed-Z depth
        // cleared to 0).
        if ( updated && !scene.Captures.empty() )
        {
            const std::string passName =
                 std::format( "SurfaceCache: Card Capture ({} cards)", scene.Captures.size() );
            const auto targets =
                 TargetsOf( textures, cache->GetCaptureBuffer(), "SurfaceCacheCapture", passName );
            if ( !targets || targets->Colors.size() < 4 || !targets->Depth.IsValid() )
            {
                LOG_ERROR( "[SceneRenderer] Surface Cache: the capture atlas cannot be declared" );
                return;
            }
            const std::vector<SurfaceCache::CardCapture>& captures = scene.Captures;
            // The G-buffer layout's velocity and thin-geometry-coverage slots: written by the cell (its pipeline is
            // built against GBufferLayout's six colours), read by nobody here.
            RDG::TextureDesc velocityDesc;
            velocityDesc.Size              = { SurfaceCache::SurfaceCacheRenderer::kCaptureAtlasSize,
                                               SurfaceCache::SurfaceCacheRenderer::kCaptureAtlasSize, 1 };
            velocityDesc.Format            = ViewTargetFormats::kVelocity;
            const RDG::TextureRef velocity = graph.CreateTexture( velocityDesc, "SurfaceCacheCapture.Velocity" );
            RDG::TextureDesc coverageDesc  = velocityDesc;
            coverageDesc.Format            = ViewTargetFormats::kThinGeometryCoverage;
            const RDG::TextureRef coverage =
                 graph.CreateTexture( coverageDesc, "SurfaceCacheCapture.ThinGeometryCoverage" );
            // The cell's one scene input (the rain occlusion map of the wetness), as the scene's G-buffer pass.
            const SceneViewInputs captureView = SceneViewInputsOf( textures.GraphRefs() );
            graph.AddPass(
                 passName, RDG::PassFlags::Raster | RDG::PassFlags::NeverCull,
                 [&]( RDG::PassBuilder& pass )
                 {
                     meshRenderer->DeclareCardCaptureDraws( pass, captures, captureView );
                     // An invalid colour is the unused shading-word slot: no target.
                     for ( uint32_t slot = 0; slot < targets->Colors.size(); ++slot )
                         if ( targets->Colors[slot].IsValid() )
                             pass.ColorTarget( slot, targets->Colors[slot],
                                               RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                     pass.ColorTarget( kGBufferVelocitySlot, velocity,
                                       RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                     pass.ColorTarget( kGBufferThinGeometryCoverageSlot, coverage,
                                       RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                     pass.DepthTarget( targets->Depth, RDG::LoadOp::ClearDepth( Core::kDepthClear ), true );
                 },
                 [meshRenderer]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return meshRenderer->RenderCardCaptureManual( context ); } );
            if ( !targets->Colors[2].IsValid() )
            {
                LOG_ERROR( "[SceneRenderer] Surface Cache: the capture atlas has no shading word (the subsurface "
                           "colour of a card)" );
                return;
            }
            const SurfaceCache::SurfaceCacheRenderer::CaptureTextures capture{ .Albedo      = targets->Colors[0],
                                                                               .Normal      = targets->Colors[1],
                                                                               .ShadingWord = targets->Colors[2],
                                                                               .Emissive    = targets->Colors[3],
                                                                               .Depth       = targets->Depth };
            if ( const auto copied = cache->AddCopyPass( graph, import, capture, scene ); !copied )
            {
                LOG_ERROR( "[SceneRenderer] Surface Cache: {}", copied.GetError() );
                return;
            }
        }

        // THE DIRECT LIGHTING (UE LumenSceneDirectLighting): the frame's relit pages, lit by exactly what the
        // deferred composite read this frame - its scene view inputs (cascades, cloud map, local shadow atlas),
        // the view's light buffers (AddFrameLightGrid), the composite's ShadowUB and cloud values, the frame sun.
        const SurfaceCache::SurfaceCacheRenderer::FrameBuffers records =
             cache->UploadRecords( graph, scene );
        if ( updated )
        {
            const FrameGraphRefs                                     refs = textures.GraphRefs();
            SurfaceCache::SurfaceCacheRenderer::DirectLightingInputs lighting;
            lighting.View   = SceneViewInputsOf( refs );
            lighting.Lights = refs.Transients.Lights;
            // The card texels read every directional light's cascades (the cascade arm): under a screen-mask
            // sun with no cascade reader this frame the sun's slot publishes no cascade (DropDirectionalShadow;
            // with no other slot casting, the view's cascades are the far-plane stand-in, AddFrameNoSunCascades)
            // and the tracer's rays shadow the cards; the moon's slot keeps its cascades.
            DeferredShadowInput shadow;
            shadow.Sun = DeferredCascadeShadow{ .Lights = meshRenderer->DirectionalShadows() };
            // A card texel has no screen depth to march: the block states the view's contact settings with no
            // steps (GetContactShadowSettings), so the cache reads the same ShadowUB layout with the march off.
            shadow.Contact      = GetContactShadowSettings( frame, false );
            shadow.Filter       = GetShadowFilterSettings();
            lighting.Shadow     = PackDeferredShadow( shadow );
            lighting.Cloud      = CloudShadowPackUniforms( GetCloudShadowInput() );
            const FrameSun sun  = GetFrameSun();
            lighting.LightDir   = sun.Direction;
            lighting.LightColor = sun.ColorIntensity;
            // Atmosphere light 1 (the moon), lit as the deferred composite lights it: its own cascades.
            const FrameSun light1 = GetFrameLight1();
            lighting.Light1Dir    = light1.Direction;
            lighting.Light1Color  = light1.ColorIntensity;
            // The shadow rays of the texels the view's maps do not hold: the active GI tracer's visibility.
            lighting.Tracer = MakeTracerFrame( graph, textures, SurfaceCacheTracer() );
            // The traced GI relights the pages' indirect term after its radiance cache update (AddFrameTracedGI).
            lighting.IndirectLighting = RadianceCacheRuns();
            lighting.VirtualLocalShadows = frameViews->VirtualLocalShadows;
            if ( const auto lit = cache->AddDirectLightingPass( graph, import, scene, records, lighting );
                 !lit )
            {
                LOG_ERROR( "[SceneRenderer] Surface Cache: {}", lit.GetError() );
                return;
            }
        }

        // Said when the scene settles or grows, not per frame.
        const uint64_t census = ( static_cast<uint64_t>( scene.Cards.size() ) << 32 ) | scene.CapturedCards;
        if ( scene.Captures.empty() && census != m_SurfaceCacheLoggedCensus )
        {
            m_SurfaceCacheLoggedCensus = census;
            LOG_INFO( "[SurfaceCache] settled: {} instance(s), {} card(s), {} resident, {} captured, {} at high "
                      "resolution, {} page(s)",
                      scene.MeshCards.size(), scene.Cards.size(), scene.ResidentCards, scene.CapturedCards,
                      scene.HighResCards, scene.Pages.size() );
            // The card build's coverage report over the meshes loaded so far (MeshCardRepresentation.hpp).
            LOG_INFO( "[SurfaceCache] card coverage: {}",
                      Assets::FormatMeshCardsCoverage( Assets::MeshCardsCoverageSummary(), 12 ) );
            if ( scene.MissingMinimumCards != 0 )
            {
                LOG_WARN(
                     "[SurfaceCache] the atlas cannot hold the smallest resolution of {} of {} card(s): raise "
                     "GlobalIllumination.SurfaceCacheAtlasSize of this quality; their hits are lit in place",
                     scene.MissingMinimumCards, scene.Cards.size() );
            }
        }

        SurfaceCacheFrameRecord record;
        record.Scene   = std::move( scene );
        record.Buffers = records;
        record.Hits    = cache->HitReads( import, records );
        record.Updated = updated;
        m_SurfaceCacheFrame.emplace( std::move( record ) );
    }

#if DESERT_DEV_INSTRUMENTS
    void SceneRenderer::AddFrameSurfaceCacheView( RDG::Builder& graph, FrameTextures& textures,
                                                  const ViewFrame& frame, const RDG::TextureRef target )
    {
        const bool atlasView = m_DebugView.DeferredDebug == DeferredDebugMode::SurfaceCacheAtlas;
        const bool sceneView = m_DebugView.DeferredDebug == DeferredDebugMode::SurfaceCacheScene;
        const bool litView   = m_DebugView.DeferredDebug == DeferredDebugMode::SurfaceCacheSceneLit;
        // Like the G-buffer view modes, Deferred only: on Forward the mode draws the lit frame.
        if ( ( !atlasView && !sceneView && !litView ) || m_RenderPath != Core::RenderPath::Deferred )
            return;
        SurfaceCache::SurfaceCacheRenderer* const cache = ReadSurfaceCache();
        if ( cache == nullptr || !m_SurfaceCacheFrame || !target.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] Debug: Surface Cache not drawn: {}",
                       !target.IsValid() ? "the frame has no post input"
                                         : "the surface cache was not brought up this frame" );
            return;
        }
        const auto import = [&textures]( const std::shared_ptr<Image>& image, const std::string_view name )
        { return textures.Import( image, name ); };
        const SurfaceCacheFrameRecord& record = *m_SurfaceCacheFrame;
        const auto                     drawn =
             atlasView ? cache->AddAtlasViewPass( graph, import, target )
                                           : cache->AddSceneViewPass( graph, import, target, record.Scene, record.Buffers,
                                                                               litView, frame.ViewProjection, frame.CameraPosition,
                                                                               frame.PreExposure );
        if ( !drawn )
            LOG_ERROR( "[SceneRenderer] Debug: Surface Cache: {}", drawn.GetError() );
        // The record's graph buffers die with this graph.
        m_SurfaceCacheFrame.reset();
    }
#endif // DESERT_DEV_INSTRUMENTS
} // namespace Desert::Graphic

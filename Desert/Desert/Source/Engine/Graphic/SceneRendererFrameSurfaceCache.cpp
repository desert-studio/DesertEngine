// SceneRenderer's surface-cache node (GI-SURFACE-CACHE-2, GI-CARD-LOOKUP): the Lumen scene update, the card
// capture through the G-buffer view pass, the copy into the atlas, the direct lighting of the cards - in the lit
// frame, before the traced GI whose hits read it (GIHitRadiance) - and the three view modes that show it.
#include <Common/Core/DevInstruments.hpp>
#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/GlobalIllumination/SurfaceCacheRenderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include <format>
#include <span>

namespace Desert::Graphic
{
    bool SceneRenderer::SurfaceCacheRuns() const
    {
        if ( m_RenderPath != Core::RenderPath::Deferred )
            return false;
        if ( m_GIPipeline.SurfaceCache )
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

    GlobalIllumination::Pipeline SceneRenderer::SurfaceCacheTracer() const
    {
        if ( m_GIPipeline.Traced )
            return m_GIPipeline;
        // A cache brought up by its views alone, under an untraced GI method: its shadow rays walk the software
        // tracer, the one every device runs.
        GlobalIllumination::Pipeline software;
        software.Traced = true;
        return software;
    }

    GlobalIllumination::TracerFrame
    SceneRenderer::MakeTracerFrame( RDG::Builder& graph, const FrameTextures& textures,
                                    const GlobalIllumination::Pipeline& pipeline ) const
    {
        const FrameTransients&          t = textures.Transients;
        GlobalIllumination::TracerFrame tracer{ .Pipeline            = pipeline,
                                                .GlobalDistanceField = t.GlobalDistanceField };
        // The software walk's clipmap placement is its own record ("GITracerSoftwareUB").
        if ( GlobalIllumination::TracerReadsGlobalDistanceField( pipeline ) )
        {
            tracer.GlobalDistanceFieldLevels = graph.CreateBuffer(
                 RDG::BufferDesc{ sizeof( t.GlobalDistanceFieldLevels ) }, "GI.Tracer.GlobalDistanceFieldLevels" );
            graph.QueueBufferUpload( tracer.GlobalDistanceFieldLevels,
                                     std::as_bytes( std::span( t.GlobalDistanceFieldLevels ) ) );
        }
        return tracer;
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
        if ( !m_SurfaceCache )
        {
            auto cache = std::make_unique<SurfaceCache::SurfaceCacheRenderer>();
            if ( const auto initialized = cache->Initialize(); !initialized )
            {
                LOG_ERROR( "[SceneRenderer] Surface Cache: {}", initialized.GetError() );
                return;
            }
            m_SurfaceCache = std::move( cache );
        }
        // The machine's budget (Scalability GlobalIllumination group), every frame: a quality change re-cuts it.
        if ( const auto configured =
                  m_SurfaceCache->Configure( SurfaceCache::MakeSurfaceCacheBudget( m_Quality ) );
             !configured )
        {
            LOG_ERROR( "[SceneRenderer] Surface Cache: {}", configured.GetError() );
            return;
        }

        // THE LUMEN SCENE: every static, instanced, generic and skinned mesh of the frame with cards, whole - the
        // cache holds what is off-screen too (a traced ray leaves the frustum). Unjittered ViewFrame position.
        std::vector<SurfaceCache::SceneInstance> instances;
        meshRenderer->GatherSurfaceCacheInstances( instances );
        SurfaceCache::SceneFrame scene = m_SurfaceCache->GetScene().Update( instances, frame.CameraPosition );

        const auto import = [&textures]( const std::shared_ptr<Image>& image, const std::string_view name )
        { return textures.Import( image, name ); };

        // THE CAPTURE: the cards picked this frame, each drawn by MeshRenderer into its rectangle of the capture
        // atlas through its surface's own view-pass G-buffer cell (albedo, normal, emissive; reversed-Z depth
        // cleared to 0).
        if ( !scene.Captures.empty() )
        {
            const std::string passName =
                 std::format( "SurfaceCache: Card Capture ({} cards)", scene.Captures.size() );
            const auto targets =
                 TargetsOf( textures, m_SurfaceCache->GetCaptureBuffer(), "SurfaceCacheCapture", passName );
            if ( !targets || targets->Colors.size() < 4 || !targets->Depth.IsValid() )
            {
                LOG_ERROR( "[SceneRenderer] Surface Cache: the capture atlas cannot be declared" );
                return;
            }
            const std::vector<SurfaceCache::CardCapture>& captures = scene.Captures;
            // The G-buffer layout's velocity slot: written by the cell, read by nobody here.
            RDG::TextureDesc velocityDesc;
            velocityDesc.Size              = { SurfaceCache::SurfaceCacheRenderer::kCaptureAtlasSize,
                                               SurfaceCache::SurfaceCacheRenderer::kCaptureAtlasSize, 1 };
            velocityDesc.Format            = ViewTargetFormats::kVelocity;
            const RDG::TextureRef velocity = graph.CreateTexture( velocityDesc, "SurfaceCacheCapture.Velocity" );
            graph.AddPass(
                 passName, RDG::PassFlags::Raster | RDG::PassFlags::NeverCull,
                 [&]( RDG::PassBuilder& pass )
                 {
                     meshRenderer->DeclareCardCaptureDraws( pass, captures );
                     // An invalid colour is the unused shading-word slot: no target.
                     for ( uint32_t slot = 0; slot < targets->Colors.size(); ++slot )
                         if ( targets->Colors[slot].IsValid() )
                             pass.ColorTarget( slot, targets->Colors[slot],
                                               RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                     pass.ColorTarget( kGBufferVelocitySlot, velocity,
                                       RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
                     pass.DepthTarget( targets->Depth, RDG::LoadOp::ClearDepth( Core::kDepthClear ), true );
                 },
                 [meshRenderer]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return meshRenderer->RenderCardCaptureManual( context ); } );
            const SurfaceCache::SurfaceCacheRenderer::CaptureTextures capture{ .Albedo   = targets->Colors[0],
                                                                               .Normal   = targets->Colors[1],
                                                                               .Emissive = targets->Colors[3],
                                                                               .Depth    = targets->Depth };
            if ( const auto copied = m_SurfaceCache->AddCopyPass( graph, import, capture, scene ); !copied )
            {
                LOG_ERROR( "[SceneRenderer] Surface Cache: {}", copied.GetError() );
                return;
            }
        }

        // THE DIRECT LIGHTING (UE LumenSceneDirectLighting): the frame's relit pages, lit by exactly what the
        // deferred composite read this frame - its scene view inputs (cascades, cloud map, local shadow atlas),
        // the view's light buffers (AddFrameLightGrid), the composite's ShadowUB and cloud values, the frame sun.
        const SurfaceCache::SurfaceCacheRenderer::FrameBuffers records =
             m_SurfaceCache->UploadRecords( graph, scene );
        {
            const FrameGraphRefs                                     refs = textures.GraphRefs();
            SurfaceCache::SurfaceCacheRenderer::DirectLightingInputs lighting;
            lighting.View   = SceneViewInputsOf( refs );
            lighting.Lights = refs.Transients.Lights;
            // The card texels read the sun's cascades (the cascade arm): under a ray-traced sun with no
            // cascade reader this frame, the view's cascades are the far-plane stand-in (AddFrameNoSunCascades)
            // and the tracer's rays shadow the cards.
            DeferredShadowInput shadow;
            shadow.Sun = DeferredCascadeShadow{ .CascadeVP            = meshRenderer->GetCascadeViewProj(),
                                                .Count                = meshRenderer->GetValidCascadeCount(),
                                                .Enabled              = meshRenderer->AreShadowsEnabled(),
                                                .CascadeWorldPerTexel = meshRenderer->GetCascadeWorldPerTexel(),
                                                .CascadeDepthRange    = meshRenderer->GetCascadeDepthRange(),
                                                .SourceTanHalf        = meshRenderer->GetSunSourceTanHalf() };
            // A card texel has no screen depth to march: the block states the view's contact settings with no
            // steps (GetContactShadowSettings), so the cache reads the same ShadowUB layout with the march off.
            shadow.Contact      = GetContactShadowSettings( frame, false );
            shadow.Filter       = GetShadowFilterSettings();
            lighting.Shadow     = PackDeferredShadow( shadow );
            lighting.Cloud      = CloudShadowPackUniforms( GetCloudShadowInput() );
            const FrameSun sun  = GetFrameSun();
            lighting.LightDir   = sun.Direction;
            lighting.LightColor = sun.ColorIntensity;
            // The shadow rays of the texels the view's maps do not hold: the active GI tracer's visibility.
            lighting.Tracer = MakeTracerFrame( graph, textures, SurfaceCacheTracer() );
            // The traced GI relights the pages' indirect term after its radiance cache update (AddFrameTracedGI).
            lighting.IndirectLighting = RadianceCacheRuns();
            if ( const auto lit = m_SurfaceCache->AddDirectLightingPass( graph, import, scene, records, lighting );
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
        record.Hits    = m_SurfaceCache->HitReads( import, records );
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
        if ( !m_SurfaceCache || !m_SurfaceCacheFrame || !target.IsValid() )
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
             atlasView ? m_SurfaceCache->AddAtlasViewPass( graph, import, target )
                                           : m_SurfaceCache->AddSceneViewPass( graph, import, target, record.Scene, record.Buffers,
                                                                               litView, frame.ViewProjection, frame.CameraPosition );
        if ( !drawn )
            LOG_ERROR( "[SceneRenderer] Debug: Surface Cache: {}", drawn.GetError() );
        // The record's graph buffers die with this graph.
        m_SurfaceCacheFrame.reset();
    }
#endif // DESERT_DEV_INSTRUMENTS
} // namespace Desert::Graphic

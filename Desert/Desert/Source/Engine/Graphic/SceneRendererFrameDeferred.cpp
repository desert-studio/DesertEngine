#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
#include <Engine/Graphic/RenderPhaseRegistry.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/RenderConfig.hpp>
#include <Engine/Graphic/PostProcessing/LensFlareRules.hpp>
#include <Engine/Graphic/PostProcessing/LightShaftRules.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Engine/Graphic/ViewBudgetGate.hpp>

#include <mutex>
#include <Common/Core/Units.hpp>

#include <Common/Core/Profiler.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <atomic>
#include <chrono>
#include <format>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Graphic/DeferredFrameNodes.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/SceneDepthResolveRenderer.hpp>

namespace Desert::Graphic
{
    namespace
    {
        // What the engine's RenderPass clears to when it opens with clear (RenderPassSpecification): the
        // graph nodes below that replace such a pass clear to the same values.
        RDG::LoadOp EngineClearColor()
        {
            const glm::vec4 c = RenderPassSpecification{}.ClearColor.Color;
            return RDG::LoadOp::ClearColor( c.r, c.g, c.b, c.a );
        }

        void ReadAll( RDG::PassBuilder& pass, const std::vector<RDG::TextureRef>& refs, RDG::Access access )
        {
            for ( const RDG::TextureRef ref : refs )
                pass.Read( ref, access );
        }
    } // namespace

    void SceneRenderer::ImportSceneViewTextures( FrameTextures& textures )
    {
        FrameTransients& view = textures.Transients;
        if ( const auto it = m_RenderSystems.find( "MeshSystem" ); it != m_RenderSystems.end() )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            const auto*    mesh = UNIQUE_GET_AS( System::MeshRenderer, it->second );
            const uint32_t cascades =
                 mesh ? std::min( mesh->GetValidCascadeCount(), kSceneViewShadowCascades ) : 0u;
            for ( uint32_t c = 0; c < cascades; ++c )
                view.ShadowCascades[c] =
                     textures.Import( mesh->GetCascadeShadowImage( c ), std::format( "CSM.Cascade{}", c ) );
        }
        auto* images = Runtime::ResourceRegistry::GetImageService();
        if ( const auto env = GetEnvironment(); env.has_value() )
        {
            if ( env->IrradianceMap.IsValid() )
                view.EnvIrradiance = textures.Import( images->Share( env->IrradianceMap ), "Env.Irradiance" );
            if ( env->PreFilteredMap.IsValid() )
                view.EnvSpecular = textures.Import( images->Share( env->PreFilteredMap ), "Env.Specular" );
        }
        if ( const auto& brdf = Renderer::GetInstance().GetBRDFTexture();
             brdf && brdf->GetImageHandle().IsValid() )
            view.BrdfLut = textures.Import( images->Share( brdf->GetImageHandle() ), "BRDF.LUT" );
        if ( const auto it = m_RenderSystems.find( "SkyboxSystem" ); it != m_RenderSystems.end() )
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            if ( const auto* sky = UNIQUE_GET_AS( System::SkyboxRenderer, it->second );
                 sky && sky->SkyPassSamplesLuts() )
            {
                view.SkyTransmittanceLut = textures.Import( sky->GetTransmittanceLut(), "Sky.TransmittanceLut" );
                view.SkyViewLut          = textures.Import( sky->GetSkyViewLut(), "Sky.SkyViewLut" );
            }
    }

    void SceneRenderer::AddFrameClearMainFramebuffer( RDG::Builder& graph, FrameTextures& textures )
    {
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "ClearMainFramebuffer" );
        if ( !targets )
            return;
        const RDG::ImportedFramebuffer& target = *targets;
        graph.AddPass(
             "ClearMainFramebuffer", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 for ( uint32_t i = 0; i < target.Colors.size(); ++i )
                     pass.ColorTarget( i, target.Colors[i], EngineClearColor() );
                 if ( target.Depth.IsValid() )
                 {
                     const glm::vec2 depthStencil = RenderPassSpecification{}.ClearColor.DepthStencil;
                     pass.DepthTarget( target.Depth,
                                       RDG::LoadOp::ClearDepth( depthStencil.x,
                                                                static_cast<uint32_t>( depthStencil.y ) ) );
                 }
                 for ( uint32_t i = 0; i < target.Resolves.size(); ++i )
                     pass.ResolveTarget( i, target.Resolves[i] );
             },
             []( RDG::PassContext& ) -> Common::BoolResultStr { return BOOLSUCCESS; } );
    }

    void SceneRenderer::AddFrameDepthResolve( RDG::Builder& graph, FrameTextures& textures )
    {
        // Copy the G-buffer depth (static opaque geometry) into the scene target depth. The deferred composite
        // writes only colour, so without this depth-tested overlays (grid, colliders) never get occluded by
        // static meshes. The graph brings both into the transfer layouts; "Deferred: Composite" below takes the
        // target depth back as its depth attachment before the forward-over-composite draws. Nothing reads the
        // G-buffer depth again this frame, so the graph ends it in the attachment layout the next frame's
        // G-buffer pass begins from.
        if ( !m_TargetFramebuffer || m_TargetFramebuffer->GetDepthAttachmentCount() == 0 ||
             m_GBuffer->GetDepthAttachmentCount() == 0 )
            return;
        const std::shared_ptr<Image2D> source = m_GBuffer->GetDepthAttachmentImage();
        const std::shared_ptr<Image2D> target = m_TargetFramebuffer->GetDepthAttachmentImage();
        const RDG::TextureRef          sourceRef =
             textures.Import( source, "GBuffer.Depth", DeferredFrameNodes::kGBufferDepthFinal );
        const RDG::TextureRef targetRef = textures.Depth( m_TargetFramebuffer, "SceneColor" );
        auto* expand = UNIQUE_GET_AS( System::DepthExpandRenderer, m_RenderSystems["DepthExpandSystem"] );
        DeferredFrameNodes::AddDepthToScene(
             graph, m_TargetFramebuffer->GetSpecification().Samples, sourceRef, targetRef,
             [source, target]( RDG::PassContext& ) -> Common::BoolResultStr
             { return Renderer::GetInstance().CopyDepthImage( source.get(), target.get() ); },
             [expand]( RDG::PassBuilder& pass, RDG::TextureRef depth )
             {
                 if ( expand )
                     expand->DeclareBindings( pass, depth );
             },
             [expand]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 if ( !expand )
                     return Common::MakeError( "Deferred: DepthExpand has no DepthExpandSystem" );
                 return expand->Record( context );
             } );
    }

    std::shared_ptr<Image2D> SceneRenderer::GetComputeSceneDepth() const
    {
        if ( !m_TargetFramebuffer || m_TargetFramebuffer->GetDepthAttachmentCount() == 0 )
            return nullptr;
        if ( m_TargetFramebuffer->GetSpecification().Samples <= 1 )
            return m_TargetFramebuffer->GetDepthAttachmentImage();
        const auto found = m_RenderSystems.find( "SceneDepthResolveSystem" );
        if ( found == m_RenderSystems.end() )
            return nullptr;
        auto* resolve = UNIQUE_GET_AS( System::SceneDepthResolveRenderer, found->second );
        return resolve && resolve->IsReady() ? resolve->GetFramebuffer()->GetDepthAttachmentImage() : nullptr;
    }

    void SceneRenderer::AddFrameSceneDepthResolve( RDG::Builder& graph, FrameTextures& textures )
    {
        if ( !m_TargetFramebuffer || m_TargetFramebuffer->GetDepthAttachmentCount() == 0 )
            return;
        auto* resolve =
             UNIQUE_GET_AS( System::SceneDepthResolveRenderer, m_RenderSystems["SceneDepthResolveSystem"] );
        if ( !resolve || !resolve->IsReady() )
            return;
        const RDG::TextureRef sceneDepth = textures.Depth( m_TargetFramebuffer, "SceneColor" );
        DeferredFrameNodes::AddSceneDepthResolve(
             graph, m_TargetFramebuffer->GetSpecification().Samples, sceneDepth,
             textures.Depth( resolve->GetFramebuffer(), "SceneDepthResolved" ),
             [resolve]( RDG::PassBuilder& pass, RDG::TextureRef depth )
             { resolve->DeclareBindings( pass, depth ); },
             [resolve]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return resolve->Record( context ); } );
    }

    void SceneRenderer::AddFrameSSAO( RDG::Builder& graph, FrameTextures& textures,
                                      const std::vector<RDG::TextureRef>& gbuffer, const glm::mat4& viewProj,
                                      const glm::vec4& cameraPos )
    {
        // SSAO first (reads the G-buffer world pos + normal into the AO buffer); the lighting pass below
        // multiplies its ambient term by this. Skipped when disabled (the shader uses AO=1 then).
        //
        // The hemisphere radius and depth bias are WORLD distances, and a world unit is a centimetre;
        // the classic recipe (John Chapman-style hemisphere SSAO, radius 0.5, bias 0.025) states them in
        // METRES.
        constexpr float kSSAORadius = Common::Units::Metres( 0.5f );   // literature: 0.5 m
        constexpr float kSSAOBias   = Common::Units::Metres( 0.025f ); // literature: 0.025 m

        if ( !m_EnableSSAO )
            return;
        auto* ssao = UNIQUE_GET_AS( System::SSAORenderer, m_RenderSystems["SSAOSystem"] );
        if ( !ssao )
            return;
        if ( gbuffer.size() < 3 )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: SSAO needs the G-buffer normal and position, the graph has {}",
                       gbuffer.size() );
            return;
        }
        // The AO image lives within this graph (UE: a transient from the scene textures' desc): the G-buffer's
        // size, the SSAO format, one mip, one sample.
        auto desc = graph.GetTextureDesc( gbuffer[0] );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: SSAO: {}", desc.GetError() );
            return;
        }
        RDG::TextureDesc aoDesc        = desc.GetValue();
        aoDesc.Format                  = ViewTargetFormats::kSSAO;
        aoDesc.Mips                    = 1;
        aoDesc.Layers                  = 1;
        aoDesc.Samples                 = 1;
        const RDG::TextureRef ao       = graph.CreateTexture( aoDesc, "SSAO" );
        // A lost SSAO term is "no occlusion": the composite goes on lit, unoccluded (RDG-FAULT1).
        graph.SetFaultDefault( ao, RDG::FaultDefault::White );
        const RDG::TextureRef worldPos = gbuffer[2]; // GBufferC
        const RDG::TextureRef normal   = gbuffer[1]; // GBufferB
        textures.Transients.SSAO       = ao;         // -> Deferred: Composite (u_SSAO)
        graph.AddPass(
             "Deferred: SSAO", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ssao->DeclareBindings( pass, worldPos, normal );
                 pass.ColorTarget( 0, ao, EngineClearColor() ); // AO is fully recomputed each frame
             },
             [ssao, viewProj, cameraPos]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 return ssao->Record( context, viewProj, cameraPos, kSSAORadius, kSSAOBias,
                                      /*power*/ 1.5f, /*samples*/ 16 );
             } );
    }

    RDG::TextureRef SceneRenderer::AddFrameGIResolve( RDG::Builder& graph, FrameTextures& textures,
                                                      const std::vector<RDG::TextureRef>& gbuffer,
                                                      const std::vector<RDG::TextureRef>& rsm,
                                                      System::MeshRenderer*               meshRenderer,
                                                      const glm::mat4& viewProj, const glm::vec4& lightColor )
    {
        auto* gi = UNIQUE_GET_AS( System::GIResolveRenderer, m_RenderSystems["GISystem"] );
        if ( !gi || !gi->Prepare() )
            return {};
        if ( gbuffer.size() < 3 || rsm.size() < 3 )
        {
            LOG_ERROR(
                 "[SceneRenderer] Deferred: GIResolve needs 3 G-buffer and 3 RSM colours, the graph has {} and {}",
                 gbuffer.size(), rsm.size() );
            return {};
        }
        // The raw gather lives within this graph (UE: a transient from the scene textures' desc); the
        // accumulation pair is history and stays external.
        auto desc = graph.GetTextureDesc( gbuffer[0] );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: GIResolve: {}", desc.GetError() );
            return {};
        }
        RDG::TextureDesc gatherDesc   = desc.GetValue();
        gatherDesc.Format             = ViewTargetFormats::kGIResolve;
        gatherDesc.Mips               = 1;
        gatherDesc.Layers             = 1;
        gatherDesc.Samples            = 1;
        const RDG::TextureRef gather  = graph.CreateTexture( gatherDesc, "GI.Gather" );
        // A lost GI gather adds no bounce light (RDG-FAULT1).
        graph.SetFaultDefault( gather, RDG::FaultDefault::Black );
        const RDG::TextureRef accum   = textures.Import( gi->GetAccumImage(), "GI" );
        const RDG::TextureRef history = textures.Import( gi->GetHistoryImage(), "GI.History" );
        textures.Transients.GIResolve = gather;
        const System::GIGatherInputs inputs{ .GBufferNormal   = gbuffer[1],
                                             .GBufferWorldPos = gbuffer[2],
                                             .RSMAlbedo       = rsm[0],
                                             .RSMNormal       = rsm[1],
                                             .RSMWorldPos     = rsm[2] };
        const float                  giIntensity = m_GIIntensity;
        graph.AddPass(
             "Deferred: GIResolve", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 gi->DeclareGatherBindings( pass, inputs );
                 pass.ColorTarget( 0, gather, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [gi, meshRenderer, lightColor, giIntensity]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 // Read when the node runs: the RSM node before it is what sets this frame's light matrix.
                 return gi->RecordGather( context, meshRenderer->GetRSMViewProj(), lightColor, giIntensity );
             } );
        const RDG::TextureRef worldPos = gbuffer[2];
        graph.AddPass(
             "Deferred: GITemporal", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 gi->DeclareTemporalBindings( pass, gather, history, worldPos );
                 pass.ColorTarget( 0, accum, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [gi, viewProj]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return gi->RecordTemporal( context, viewProj ); } );
        return accum;
    }

    void SceneRenderer::AddFrameComposite( RDG::Builder& graph, FrameTextures& textures,
                                           const std::vector<RDG::TextureRef>& gbuffer, RDG::TextureRef giAccum,
                                           const std::vector<RDG::TextureRef>& shadowReads,
                                           System::MeshRenderer* meshRenderer, const glm::vec4& lightDir,
                                           const glm::vec4& lightColor, const glm::vec4& cameraPos )
    {
        // LOAD/STORE on every scene-target attachment. The depth is declared written because the passes after
        // this one that are not graph nodes yet (forward meshes, glass, fog, clouds, overlays) begin their own
        // render passes on it in the depth-attachment layout, and DepthResolve left it a transfer destination.
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: Composite" );
        if ( !targets )
            return;
        if ( gbuffer.size() < 4 )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: Composite needs the 4 G-buffer colours, the graph has {}",
                       gbuffer.size() );
            return;
        }
        const RDG::ImportedFramebuffer& target = *targets;

        // Every input by name; an input not produced this frame is the system texture neutral for it (UE
        // GSystemTextures), so the lighting is drawn in every mode (SSAO off, GI not RSM, fewer cascades).
        const FrameGraphRefs            refs = textures.GraphRefs();
        System::DeferredCompositeInputs inputs;
        inputs.GBufferA         = gbuffer[0];
        inputs.GBufferB         = gbuffer[1];
        inputs.GBufferC         = gbuffer[2];
        inputs.GBufferEmissive  = gbuffer[3];
        inputs.SSAO             = refs.Transients.SSAO.IsValid() ? refs.Transients.SSAO : refs.System.White;
        inputs.GI               = giAccum.IsValid() ? giAccum : refs.System.Black;
        inputs.View                        = SceneViewInputsOf( refs );
        std::vector<RDG::TextureRef> reads = shadowReads;
        for ( const RDG::TextureRef ref : { inputs.GBufferA, inputs.GBufferB, inputs.GBufferC,
                                            inputs.GBufferEmissive, inputs.SSAO, inputs.GI } )
            reads.push_back( ref );
        const std::vector<RDG::TextureRef> view = inputs.View.Refs();
        reads.insert( reads.end(), view.begin(), view.end() );
        std::vector<RDG::TextureRef> declared; // one declaration per texture (System.White can fill several slots)
        for ( const RDG::TextureRef ref : reads )
            if ( ref.IsValid() && std::find( declared.begin(), declared.end(), ref ) == declared.end() )
                declared.push_back( ref );

        graph.AddPass(
             "Deferred: Composite", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ReadAll( pass, declared, RDG::Access::SampledGraphics );
                 DeferredFrameNodes::LoadTarget( pass, target );
             },
             [this, inputs, meshRenderer, lightDir, lightColor,
              cameraPos]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DeferredShadowInput shadow;
                 if ( meshRenderer )
                 {
                     shadow.CascadeVP            = meshRenderer->GetCascadeViewProj();
                     shadow.Count                = meshRenderer->GetValidCascadeCount();
                     shadow.Bias                 = meshRenderer->GetShadowBias();
                     shadow.Enabled              = meshRenderer->AreShadowsEnabled();
                     shadow.CascadeWorldPerTexel = meshRenderer->GetCascadeWorldPerTexel();
                 }

                 const CloudShadowInput cloudShadow = GetCloudShadowInput();

                 DeferredEnvironmentInput environment;
                 {
                     auto* imageService = Runtime::ResourceRegistry::GetImageService();
                     if ( const auto& env = GetEnvironment(); env.has_value() )
                     {
                         environment.Look = env->Look;
                         if ( env->IrradianceMap.IsValid() )
                             environment.Irradiance =
                                  static_cast<ImageCube*>( imageService->Resolve( env->IrradianceMap ) );
                         if ( env->PreFilteredMap.IsValid() )
                             environment.Prefiltered =
                                  static_cast<ImageCube*>( imageService->Resolve( env->PreFilteredMap ) );
                     }
                     if ( const auto& brdf = Renderer::GetInstance().GetBRDFTexture();
                          brdf && brdf->GetImageHandle().IsValid() )
                         environment.BrdfLut =
                              static_cast<Image2D*>( imageService->Resolve( brdf->GetImageHandle() ) );
                 }

                 const float giIntensity = ( m_GIMode == Core::GIMode::ScreenSpace ) ? m_GIIntensity : 0.0f;
                 return UNIQUE_GET_AS( System::DeferredLightingRenderer,
                                       m_RenderSystems["DeferredLightingSystem"] )
                      ->Record( context, inputs, lightDir, lightColor, cameraPos,
                                static_cast<int>( m_DebugView.DeferredDebug ), GetPointLights(), GetSpotLights(),
                                shadow, giIntensity, m_EnableSSAO, static_cast<int>( m_GIMode ), cloudShadow,
                                environment );
             } );
    }

    RDG::TextureRef SceneRenderer::AddFrameSceneCopy( RDG::Builder& graph, FrameTextures& textures,
                                                      const std::vector<RDG::TextureRef>& sceneColor,
                                                      System::CopyRenderer*               copy )
    {
        if ( !copy || sceneColor.empty() )
            return {};
        // The snapshot lives within this graph (UE: a transient from the scene colour's desc): the scene
        // colour's size, the copy format, one mip, one layer, one sample (the scene colour ref is the
        // attachment 0 the copy always read).
        const auto desc = graph.GetTextureDesc( sceneColor.front() );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: SceneCopy: {}", desc.GetError() );
            return {};
        }
        RDG::TextureDesc copyDesc          = desc.GetValue();
        copyDesc.Format                    = ViewTargetFormats::kSceneColorCopy;
        copyDesc.Mips                      = 1;
        copyDesc.Layers                    = 1;
        copyDesc.Samples                   = 1;
        const RDG::TextureRef sceneCopy    = graph.CreateTexture( copyDesc, "SceneCopy" );
        const RDG::TextureRef source       = sceneColor.front();
        textures.Transients.SceneColorCopy = sceneCopy; // -> Deferred: SSR (u_SceneColor), Deferred: Glass
        graph.AddPass(
             "Deferred: SceneCopy", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 // The copy samples attachment 0; the other scene-colour refs stay plain reads of the node.
                 copy->DeclareBindings( pass, source );
                 ReadAll( pass, { sceneColor.begin() + 1, sceneColor.end() }, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, sceneCopy, EngineClearColor() );
             },
             [copy]( RDG::PassContext& context ) -> Common::BoolResultStr { return copy->Record( context ); } );
        return sceneCopy;
    }

    void SceneRenderer::AddFrameSSR( RDG::Builder& graph, FrameTextures& textures,
                                     const std::vector<RDG::TextureRef>& gbuffer, RDG::TextureRef sceneCopy,
                                     const glm::mat4& viewProj, const glm::vec4& cameraPos )
    {
        auto* ssr = UNIQUE_GET_AS( System::SSRRenderer, m_RenderSystems["SSRSystem"] );
        if ( !ssr || !sceneCopy.IsValid() )
            return;
        const std::optional<System::SSRRenderer::TraceTargets> traceTargets = ssr->Prepare();
        if ( !traceTargets )
            return;
        // Transients of this graph, sized from this frame's view: written by the trace, read by the tiled passes.
        const RDG::TextureRef          trace   = graph.CreateTexture( traceTargets->Trace, "SSR.Trace" );
        const RDG::TextureRef          tiles   = graph.CreateTexture( traceTargets->TileMask, "SSR.TileMask" );
        // A lost trace reflects nothing; a lost tile mask marks no tile (RDG-FAULT1).
        graph.SetFaultDefault( trace, RDG::FaultDefault::Black );
        graph.SetFaultDefault( tiles, RDG::FaultDefault::Black );
        const RDG::TextureRef          accum   = textures.Import( ssr->GetAccumImage(), "SSR" );
        const RDG::TextureRef          history = textures.Import( ssr->GetHistoryImage(), "SSR.History" );
        const auto targets = TargetsOf( textures, m_TargetFramebuffer, "SceneColor", "Deferred: SSRComposite" );
        if ( !targets )
            return;
        const RDG::ImportedFramebuffer& target = *targets;
        if ( gbuffer.size() < 3 )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: SSR needs the G-buffer albedo, normal and world position, the "
                       "graph has {}",
                       gbuffer.size() );
            return;
        }
        // Sampled by name in every SSR pass (no G-buffer image crosses into a pass exec).
        const System::SSRRenderer::GBufferInputs inputs{ gbuffer[0], gbuffer[1], gbuffer[2] };
        graph.AddPass(
             "Deferred: SSR", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 ssr->DeclareTraceBindings( pass, trace, tiles, inputs, sceneCopy );
                 // The G-buffer colours past the three the trace samples stay declared as before.
                 ReadAll( pass, { gbuffer.begin() + 3, gbuffer.end() }, RDG::Access::SampledCompute );
             },
             [this, ssr, viewProj, cameraPos]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 constexpr float kSSRThickness = Common::Units::Metres( 0.5f ); // literature: 0.5 m
                 return ssr->RecordTrace( context, viewProj, cameraPos, /*maxSteps*/ 32, m_SSRMaxDistance,
                                          m_SSRIntensity, kSSRThickness );
             } );
        graph.AddPass(
             "Deferred: SSRResolve", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ssr->DeclareResolveBindings( pass, trace, tiles, history, inputs );
                 // Every G-buffer colour but the world position the block samples stays declared as before.
                 ReadAll( pass, { gbuffer[0], gbuffer[1] }, RDG::Access::SampledGraphics );
                 ReadAll( pass, { gbuffer.begin() + 3, gbuffer.end() }, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, accum, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [ssr]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return ssr->RecordResolve( context ); } );
        graph.AddPass(
             "Deferred: SSRComposite", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ssr->DeclareCompositeBindings( pass, accum, tiles, inputs );
                 // Every G-buffer colour but the normal the block samples stays declared as before.
                 ReadAll( pass, { gbuffer[0], gbuffer[2] }, RDG::Access::SampledGraphics );
                 ReadAll( pass, { gbuffer.begin() + 3, gbuffer.end() }, RDG::Access::SampledGraphics );
                 DeferredFrameNodes::LoadTarget( pass, target ); // blend over the scene
             },
             [ssr, viewProj]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return ssr->RecordComposite( context, viewProj ); } );
    }
} // namespace Desert::Graphic

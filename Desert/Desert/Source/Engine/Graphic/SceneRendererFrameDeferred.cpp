#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
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
#include <Engine/Graphic/Systems/Scene/Deferred/GraphColorResolveRenderer.hpp>

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

    void SceneRenderer::ImportSceneViewTextures( RDG::Builder& graph, FrameTextures& textures )
    {
        FrameTransients& view = textures.Transients;
        if ( const auto it = m_RenderSystems.find( "MeshSystem" ); it != m_RenderSystems.end() )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            auto* mesh = UNIQUE_GET_AS( System::MeshRenderer, it->second );
            if ( mesh != nullptr )
            {
                view.ShadowCascades = mesh->AcquireCascadeMaps( graph );
                if ( const auto atlas = mesh->GetLocalShadowAtlasImage() )
                    view.LocalShadowAtlas = textures.Import( atlas, "LocalShadow.Atlas" );
            }
        }
        // No cascades (shadows budgeted off, no mesh system): the lit shaders still declare a sampler2DArray,
        // which takes no 2D stand-in, so the view gets a one-texel array cleared to the far plane - 1 under the
        // cascades' standard Z, "no occluder" - and the shader's cascade count (0) never reads it anyway.
        if ( !view.ShadowCascades.IsValid() )
        {
            view.ShadowCascades = graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = 1, .Height = 1 },
                                                                         .Format = ViewTargetFormats::kShadowDepth,
                                                                         .Layers = 1,
                                                                         .Dim    = RDG::TextureDim::Tex2DArray },
                                                       "CSM.None" );
            graph.AddPass(
                 "CSM.None", RDG::PassFlags::Raster, [&]( RDG::PassBuilder& pass )
                 { pass.DepthTarget( view.ShadowCascades, RDG::LoadOp::ClearDepth( 1.0f ) ); },
                 []( RDG::PassContext& ) -> Common::BoolResultStr { return BOOLSUCCESS; } );
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
                 sky != nullptr && sky->SkyPassSamplesLuts() )
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
        // The first node of the frame on both paths: the scene colour to the engine grey, and every graph colour
        // (the view's velocity) to its own clear (no motion) - FrameTextures::ColorLoads.
        const std::vector<RDG::LoadOp> colors = textures.ColorLoads( *targets, EngineClearColor() );
        graph.AddPass(
             "ClearMainFramebuffer", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 for ( uint32_t i = 0; i < target.Colors.size(); ++i )
                     pass.ColorTarget( i, target.Colors[i], colors[i] );
                 if ( target.Depth.IsValid() )
                 {
                     const glm::vec2 depthStencil = RenderPassSpecification{}.ClearColor.DepthStencil;
                     pass.DepthTarget(
                          target.Depth,
                          RDG::LoadOp::ClearDepth( depthStencil.x, static_cast<uint32_t>( depthStencil.y ) ) );
                 }
                 DeclareResolves( pass, target.Resolves );
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
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* expand = UNIQUE_GET_AS( System::DepthExpandRenderer, m_RenderSystems["DepthExpandSystem"] );
        DeferredFrameNodes::AddDepthToScene(
             graph, m_TargetFramebuffer->GetSpecification().Samples, sourceRef, targetRef,
             [source, target]( RDG::PassContext& ) -> Common::BoolResultStr
             { return Renderer::GetInstance().CopyDepthImage( source.get(), target.get() ); },
             [expand]( RDG::PassBuilder& pass, RDG::TextureRef depth )
             {
                 if ( expand != nullptr )
                     expand->DeclareBindings( pass, depth );
             },
             [expand]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 if ( expand == nullptr )
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
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* resolve = UNIQUE_GET_AS( System::SceneDepthResolveRenderer, found->second );
        return resolve != nullptr && resolve->IsReady() ? resolve->GetFramebuffer()->GetDepthAttachmentImage()
                                                        : nullptr;
    }

    void SceneRenderer::AddFrameSceneDepthResolve( RDG::Builder& graph, FrameTextures& textures )
    {
        if ( !m_TargetFramebuffer || m_TargetFramebuffer->GetDepthAttachmentCount() == 0 )
            return;
        auto* resolve =
             // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
             UNIQUE_GET_AS( System::SceneDepthResolveRenderer, m_RenderSystems["SceneDepthResolveSystem"] );
        if ( resolve == nullptr || !resolve->IsReady() )
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

    void SceneRenderer::AddFrameGraphColorResolves( RDG::Builder& graph, FrameTextures& textures )
    {
        const std::vector<GraphColor>* colors = textures.GraphColorsOf( m_TargetFramebuffer.get() );
        if ( colors == nullptr )
            return;
        const auto                         found   = m_RenderSystems.find( "GraphColorResolveSystem" );
        System::GraphColorResolveRenderer* resolve = nullptr;
        if ( found != m_RenderSystems.end() )
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
            resolve = UNIQUE_GET_AS( System::GraphColorResolveRenderer, found->second );
        AddGraphColorResolves(
             graph, *colors,
             [resolve]( RDG::PassBuilder& pass, const GraphColor& color )
             {
                 if ( resolve != nullptr )
                     resolve->DeclareBindings( pass, color.Multisample );
             },
             [resolve, &graph]( const GraphColor& color )
             {
                 const auto                                      desc = graph.GetTextureDesc( color.Color );
                 const std::optional<Core::Formats::ImageFormat> format =
                      desc.IsSuccess() ? std::optional( desc.GetValue().Format ) : std::nullopt;
                 return [resolve, format]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     if ( resolve == nullptr || !format )
                         return Common::MakeError( "GraphColorResolve: no system or no format for the colour" );
                     return resolve->Record( context, *format );
                 };
             } );
    }

    void SceneRenderer::AddFrameSSAO( RDG::Builder& graph, FrameTextures& textures,
                                      const std::vector<RDG::TextureRef>& gbuffer, const ViewFrame& frame )
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
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* ssao = UNIQUE_GET_AS( System::SSAORenderer, m_RenderSystems["SSAOSystem"] );
        if ( ssao == nullptr )
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
        const RDG::TextureRef depth    = textures.Depth( m_GBuffer, "GBuffer" ); // world position reconstructed
        const RDG::TextureRef normal   = gbuffer[1]; // GBufferB
        textures.Transients.SSAO       = ao;         // -> Deferred: Composite (u_SSAO)
        graph.AddPass(
             "Deferred: SSAO", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ssao->DeclareBindings( pass, depth, normal );
                 pass.ColorTarget( 0, ao, EngineClearColor() ); // AO is fully recomputed each frame
             },
             // The G-buffer was rasterised with the jittered matrix: SSAO projects its samples through it and
             // reconstructs positions with its inverse (both from the ViewFrame, one copy).
             [ssao, viewProj = frame.JitteredViewProjection, invViewProj = frame.InvJitteredViewProjection,
              cameraPos = glm::vec4( frame.CameraPosition, 1.0f ),
              samples   = m_SSAOSamples]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 return ssao->Record( context, viewProj, invViewProj, cameraPos, kSSAORadius, kSSAOBias,
                                      /*power*/ 1.5f, samples );
             } );
    }

    RDG::TextureRef SceneRenderer::AddFrameGIResolve( RDG::Builder& graph, FrameTextures& textures,
                                                      const std::vector<RDG::TextureRef>& gbuffer,
                                                      const std::vector<RDG::TextureRef>& rsm,
                                                      System::MeshRenderer* meshRenderer, const ViewFrame& frame,
                                                      const glm::vec4& lightColor )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* gi = UNIQUE_GET_AS( System::GIResolveRenderer, m_RenderSystems["GISystem"] );
        if ( gi == nullptr || !gi->Prepare() )
            return {};
        if ( gbuffer.size() < 3 || rsm.size() < 3 )
        {
            LOG_ERROR(
                 "[SceneRenderer] Deferred: GIResolve needs 3 G-buffer and 3 RSM colours, the graph has {} and {}",
                 gbuffer.size(), rsm.size() );
            return {};
        }
        // The raw gather lives within this graph (UE: a transient from the scene textures' desc); the denoiser
        // keeps its own history.
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
        textures.Transients.GIResolve = gather;
        const RDG::TextureRef        gbufferDepth = textures.Depth( m_GBuffer, "GBuffer" );
        const System::GIGatherInputs inputs{ .GBufferNormal = gbuffer[1],
                                             .GBufferDepth  = gbufferDepth,
                                             .RSMAlbedo     = rsm[0],
                                             .RSMNormal     = rsm[1],
                                             .RSMDepth      = textures.Depth( m_RSMBuffer, "RSM" ) };
        const float                  giIntensity = m_GIIntensity;
        const int                    giSamples   = m_GISamples;
        graph.AddPass(
             "Deferred: GIResolve", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 gi->DeclareGatherBindings( pass, inputs );
                 pass.ColorTarget( 0, gather, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [gi, meshRenderer, lightColor, giIntensity, giSamples,
              invViewProj = frame.InvJitteredViewProjection]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 // Read when the node runs: the RSM node before it is what sets this frame's light matrix.
                 return gi->RecordGather( context, meshRenderer->GetRSMViewProj(), invViewProj, lightColor,
                                          giIntensity, giSamples );
             } );
        const RDG::TextureRef velocity = textures.Transients.Velocity;
        if ( !velocity.IsValid() )
        {
            LOG_ERROR(
                 "[SceneRenderer] Deferred: GI needs the view's velocity (the denoiser reprojects its history "
                 "through it), the frame has none" );
            return {};
        }
        return gi->Denoise( graph, gather,
                            System::DenoiserSceneTextures{ .GBufferNormal = gbuffer[1],
                                                           .GBufferDepth  = gbufferDepth,
                                                           .Velocity      = velocity,
                                                           .Black         = textures.GraphRefs().System.Black },
                            frame );
    }

    void SceneRenderer::AddFrameLightGrid( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame )
    {
        auto* const deferred =
             dynamic_cast<System::DeferredLightingRenderer*>( m_RenderSystems["DeferredLightingSystem"].get() );
        if ( deferred == nullptr )
        {
            LOG_ERROR( "[SceneRenderer] LightGrid: no DeferredLightingSystem, the view has no light grid" );
            return;
        }
        // The grid is cut over the view's render extent (the scene target and the G-buffer are both drawn at it),
        // with the matrices the geometry is rasterised with (the jittered projection: a pixel's cell is the one
        // its depth sample fell in), on BOTH paths: the forward meshes read it as the deferred composite does.
        const LightGridParamsPayload grid = MakeLightGridParams(
             frame.View, frame.JitteredProjection, frame.NearPlane, frame.Split.Render.Width,
             frame.Split.Render.Height, GetPointLights(), GetSpotLights(), m_LightGridQuality );
        // "Upload: Lights.Point" / "Upload: Lights.Spot" / "Upload: Lights.ShadowViews" /
        // "Upload: Lights.GridParams", ordered before the nodes that read them.
        const System::DeferredCompositeLights lights = System::DeferredLightingRenderer::UploadLights(
             graph, GetPointLights(), GetSpotLights(),
             // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
             UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->GetLocalShadowRows(), grid );
        textures.Transients.Lights = lights;

        // UE LightGridInjection (ComputeLightGrid, before the base pass): every froxel lists the lights that reach
        // it, before any lit node reads the lists.
        graph.AddPass(
             "LightGrid", RDG::PassFlags::Compute,
             [deferred, lights]( RDG::PassBuilder& pass ) { deferred->DeclareGridBindings( pass, lights ); },
             [deferred, cells = glm::uvec3( grid.Size )]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return deferred->RecordGrid( context, cells ); } );
    }

    void SceneRenderer::AddFrameComposite( RDG::Builder& graph, FrameTextures& textures,
                                           const std::vector<RDG::TextureRef>& gbuffer, RDG::TextureRef giAccum,
                                           RDG::TextureRef ssr, System::MeshRenderer* meshRenderer,
                                           const glm::vec4& lightDir, const glm::vec4& lightColor,
                                           const ViewFrame& frame )
    {
        // w: whether u_SSR holds this frame's traced reflection (DeferredLighting.shader DeferredUB).
        const glm::vec4 cameraPos( frame.CameraPosition, ssr.IsValid() ? 1.0f : 0.0f );
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
        const std::vector<RDG::LoadOp>  loads  = textures.ColorLoads( *targets, RDG::LoadOp::Load() );

        // Every input by name; an input not produced this frame is the system texture neutral for it (UE
        // GSystemTextures), so the lighting is drawn in every mode (SSAO off, GI not RSM, fewer cascades).
        const FrameGraphRefs            refs = textures.GraphRefs();
        System::DeferredCompositeInputs inputs;
        inputs.GBufferA        = gbuffer[0];
        inputs.GBufferB        = gbuffer[1];
        inputs.GBufferShadingWord = gbuffer[2];
        inputs.GBufferDepth       = textures.Depth( m_GBuffer, "GBuffer" );
        // The opaque depth is final here: the forward meshes and the glass added after this node march it for
        // their contact shadows (SceneViewInputs::SceneDepth) and read the hit texel's caster bit from the shading
        // word (SceneViewInputs::SceneShadingWord). They draw on the scene target, not the G-buffer.
        textures.Transients.SceneDepth       = inputs.GBufferDepth;
        textures.Transients.SceneShadingWord = inputs.GBufferShadingWord;
        inputs.GBufferEmissive = gbuffer[3];
        inputs.SSAO            = refs.Transients.SSAO.IsValid() ? refs.Transients.SSAO : refs.System.White;
        inputs.GI              = giAccum.IsValid() ? giAccum : refs.System.Black;
        inputs.SSR             = ssr.IsValid() ? ssr : refs.System.Black;
        inputs.View            = SceneViewInputsOf( refs );
        // CPU state of this frame, gathered at graph build: the material is filled in the node's setup, where the
        // pass validation reads its route fill (lead decision B), never in the exec.
        DeferredShadowInput shadow;
        if ( meshRenderer != nullptr )
        {
            shadow.CascadeVP            = meshRenderer->GetCascadeViewProj();
            shadow.Count                = meshRenderer->GetValidCascadeCount();
            shadow.Enabled              = meshRenderer->AreShadowsEnabled();
            shadow.CascadeWorldPerTexel = meshRenderer->GetCascadeWorldPerTexel();
            shadow.CascadeDepthRange    = meshRenderer->GetCascadeDepthRange();
            shadow.SourceTanHalf        = meshRenderer->GetSunSourceTanHalf();
        }
        // The composite marches the G-buffer depth, so its view always has one (GetContactShadowSettings).
        shadow.Contact                       = GetContactShadowSettings( frame, true );
        const CloudShadowInput   cloudShadow = GetCloudShadowInput();
        DeferredEnvironmentInput environment;
        {
            auto* imageService = Runtime::ResourceRegistry::GetImageService();
            if ( const auto& env = GetEnvironment(); env.has_value() )
            {
                environment.Look = env->Look;
                if ( env->IrradianceMap.IsValid() )
                    environment.Irradiance =
                         dynamic_cast<ImageCube*>( imageService->Resolve( env->IrradianceMap ) );
                if ( env->PreFilteredMap.IsValid() )
                    environment.Prefiltered =
                         dynamic_cast<ImageCube*>( imageService->Resolve( env->PreFilteredMap ) );
            }
            if ( const auto& brdf = Renderer::GetInstance().GetBRDFTexture();
                 brdf && brdf->GetImageHandle().IsValid() )
                environment.BrdfLut = dynamic_cast<Image2D*>( imageService->Resolve( brdf->GetImageHandle() ) );
        }
        // The composite's GI branch (DeferredLighting.shader u_Params.w): 0 none, 1 the screen-space gather inside
        // this pass, 2 the RSM resolve's buffer. Spelled here, not cast from the enum, whose numbering is the
        // method ladder's (Common/Settings/RenderMethods.hpp).
        using Common::Scalability::GIMethod;
        const GIMethod gi            = m_RenderMethods.GlobalIllumination.Effective;
        const int      compositeGI   = gi == GIMethod::ScreenSpace ? 1 : gi == GIMethod::ReflectiveShadowMaps ? 2 : 0;
        const float    giIntensity   = ( gi == GIMethod::ScreenSpace ) ? m_GIIntensity : 0.0f;

        auto* const deferred =
             dynamic_cast<System::DeferredLightingRenderer*>( m_RenderSystems["DeferredLightingSystem"].get() );
        // The lights and the light grid were made by AddFrameLightGrid, before any lit node.
        const ViewLightBuffers& lights = refs.Transients.Lights;
        if ( !lights.GridCells.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: Composite: the view's light grid was not built this frame" );
            return;
        }
        const auto pointCount = static_cast<uint32_t>( GetPointLights().PointLights.size() );
        const auto spotCount  = static_cast<uint32_t>( GetSpotLights().SpotLights.size() );

        graph.AddPass(
             "Deferred: Composite", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 deferred->FillMaterial( lightDir, lightColor, cameraPos, frame.InvJitteredViewProjection,
                                         static_cast<int>( m_DebugView.DeferredDebug ), pointCount, spotCount,
                                         shadow, giIntensity, m_EnableSSAO, compositeGI, cloudShadow, environment,
                                         frame.JitteredViewProjection );
                 deferred->DeclareCompositeBindings( pass, inputs, lights );
                 DeferredFrameNodes::LoadTarget( pass, target, loads );
             },
             [deferred]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return deferred->Record( context ); } );
    }

#if DESERT_DEV_INSTRUMENTS
    void SceneRenderer::AddFrameVelocityView( RDG::Builder& graph, FrameTextures& textures,
                                              const RDG::TextureRef target )
    {
        // Like the G-buffer view modes, Deferred only: on Forward the mode draws the lit frame.
        if ( m_DebugView.DeferredDebug != DeferredDebugMode::Velocity ||
             m_RenderPath != Core::RenderPath::Deferred )
            return;
        const RDG::TextureRef velocity = textures.Transients.Velocity;
        const auto            it       = m_RenderSystems.find( "VelocityViewSystem" );
        auto* const           view =
             it != m_RenderSystems.end() ? UNIQUE_GET_AS( System::VelocityViewRenderer, it->second ) : nullptr;
        if ( view == nullptr || !target.IsValid() || !velocity.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] Debug: Velocity not drawn: {}",
                       view == nullptr ? "no VelocityViewSystem" : "the frame has no velocity or no post input" );
            return;
        }
        const auto desc = graph.GetTextureDesc( target );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] Debug: Velocity: {}", desc.GetError() );
            return;
        }
        if ( const auto prepared = view->Prepare( desc.GetValue().Format, desc.GetValue().Samples ); !prepared )
        {
            LOG_ERROR( "[SceneRenderer] Debug: Velocity: {}", prepared.GetError() );
            return;
        }
        graph.AddPass(
             "Debug: Velocity", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 view->DeclareBindings( pass, velocity );
                 pass.ColorTarget( 0, target, EngineClearColor() );
             },
             [view]( RDG::PassContext& context ) -> Common::BoolResultStr { return view->Record( context ); } );
    }

    void SceneRenderer::AddFrameGlobalDistanceFieldView( RDG::Builder& graph, FrameTextures& textures,
                                                         const ViewFrame& frame, const RDG::TextureRef target )
    {
        // Like the G-buffer view modes, Deferred only: on Forward the mode draws the lit frame.
        if ( m_DebugView.DeferredDebug != DeferredDebugMode::GlobalDistanceField ||
             m_RenderPath != Core::RenderPath::Deferred )
            return;
        const auto  it           = m_RenderSystems.find( "MeshSystem" );
        auto* const meshRenderer = it != m_RenderSystems.end() ? UNIQUE_GET_AS( System::MeshRenderer, it->second ) : nullptr;
        if ( meshRenderer == nullptr || !target.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] Debug: Global Distance Field not drawn: {}",
                       meshRenderer == nullptr ? "no MeshSystem" : "the frame has no post input" );
            return;
        }
        if ( !m_GlobalDistanceField )
        {
            auto field = std::make_unique<DistanceField::GlobalDistanceField>();
            if ( const auto initialized = field->Initialize(); !initialized )
            {
                LOG_ERROR( "[SceneRenderer] Debug: Global Distance Field: {}", initialized.GetError() );
                return;
            }
            m_GlobalDistanceField = std::move( field );
        }

        // Every static and instanced mesh of the frame that carries a field, whole - the field holds what is
        // off-screen too (a trace leaves the frustum).
        std::vector<DistanceField::GlobalDistanceFieldObject> objects;
        // One object per volume of the mesh's field, each owning the whole field (an aliasing pointer: the
        // volume lives as long as the field it belongs to). A mesh whose field is still being derived (the
        // asynchronous queue) is not in it yet.
        const auto place = [&objects]( const std::shared_ptr<const Assets::MeshDistanceFieldData>& field,
                                       const glm::mat4&                                            world )
        {
            for ( const Assets::MeshDistanceFieldVolume& volume : field->Volumes )
                objects.push_back(
                     { std::shared_ptr<const Assets::MeshDistanceFieldVolume>( field, &volume ), world } );
        };
        for ( const auto& draw : meshRenderer->GetStaticQueue() )
            if ( draw.Mesh != nullptr )
                if ( const auto field = draw.Mesh->GetDistanceField() )
                    place( field, draw.Transform );
        for ( const auto& ism : meshRenderer->GetInstancedQueue() )
            if ( ism.Mesh != nullptr && ism.Transforms )
                if ( const auto field = ism.Mesh->GetDistanceField() )
                    for ( const glm::mat4& world : *ism.Transforms )
                        place( field, world );

        const auto import = [&textures]( const std::shared_ptr<Image>& image, const std::string_view name )
        { return textures.Import( image, name ); };
        // The view's matrices, unjittered (ViewFrame, as every overlay reads them; SceneRenderer.hpp: a pass that
        // reads a matrix or a camera position takes the ViewFrame) - not m_SceneInfo.ActiveCamera's, which framed
        // the field from a different camera than the frame it is drawn over.
        const auto field = m_GlobalDistanceField->AddUpdatePasses( graph, import, frame.CameraPosition, objects,
                                                                   m_DistanceFieldPoolMiB );
        if ( !field )
        {
            LOG_ERROR( "[SceneRenderer] Debug: Global Distance Field: {}", field.GetError() );
            return;
        }
        if ( const auto drawn = m_GlobalDistanceField->AddDebugViewPass(
                  graph, field.GetValue(), target, EngineClearColor(), frame.InvViewProjection );
             !drawn )
            LOG_ERROR( "[SceneRenderer] Debug: Global Distance Field: {}", drawn.GetError() );
    }
#endif // DESERT_DEV_INSTRUMENTS

    RDG::TextureRef SceneRenderer::AddFrameSceneCopy( RDG::Builder& graph, FrameTextures& textures,
                                                      const std::vector<RDG::TextureRef>& sceneColor,
                                                      System::CopyRenderer* copy, RDG::TextureRef into,
                                                      std::function<void()> recorded )
    {
        if ( copy == nullptr || sceneColor.empty() )
            return {};
        // The snapshot lives within this graph (UE: a transient from the scene colour's desc): the scene
        // colour's size, the copy format, one mip, one layer, one sample (the scene colour ref is the
        // attachment 0 the copy always read).
        const auto desc = graph.GetTextureDesc( sceneColor.front() );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] Scene: ColorCopy: {}", desc.GetError() );
            return {};
        }
        RDG::TextureDesc copyDesc          = desc.GetValue();
        copyDesc.Format                    = ViewTargetFormats::kSceneColorCopy;
        copyDesc.Mips                      = 1;
        copyDesc.Layers                    = 1;
        copyDesc.Samples                   = 1;
        const RDG::TextureRef sceneCopy    = into.IsValid() ? into : graph.CreateTexture( copyDesc, "SceneCopy" );
        const RDG::TextureRef source       = sceneColor.front();
        // -> Scene: Glass, Water; and, as SSR's scene colour history, next frame's Deferred: SSR
        // (u_SceneColor).
        textures.Transients.SceneColorCopy = sceneCopy;
        graph.AddPass(
             "Scene: ColorCopy", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 // The copy samples attachment 0; the other scene-colour refs stay plain reads of the node.
                 copy->DeclareBindings( pass, source );
                 ReadAll( pass, { sceneColor.begin() + 1, sceneColor.end() }, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, sceneCopy, EngineClearColor() );
             },
             [copy, recorded = std::move( recorded )]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const Common::BoolResultStr result = copy->Record( context );
                 if ( result.IsSuccess() && recorded )
                     recorded();
                 return result;
             } );
        return sceneCopy;
    }

    RDG::TextureRef SceneRenderer::AddFrameHZB( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* hzb = UNIQUE_GET_AS( System::HZBRenderer, m_RenderSystems["HZBSystem"] );
        if ( hzb == nullptr )
            return {};
        const RDG::TextureRef depth = textures.Depth( m_GBuffer, "GBuffer" );
        const auto            desc  = graph.GetTextureDesc( depth );
        if ( !desc )
        {
            LOG_ERROR( "[SceneRenderer] HZB: {}", desc.GetError() );
            return {};
        }
        return hzb->AddPasses( graph, depth, desc.GetValue().Size.Width, desc.GetValue().Size.Height );
    }

    SceneRenderer::FrameSSR SceneRenderer::AddFrameSSR( RDG::Builder& graph, FrameTextures& textures,
                                                        System::SSRRenderer&                ssr,
                                                        const std::vector<RDG::TextureRef>& gbuffer,
                                                        const ViewFrame&                    frame )
    {
        if ( gbuffer.size() < 3 )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: SSR needs the G-buffer albedo and normal, the graph has {}",
                       gbuffer.size() );
            return {};
        }
        const std::optional<System::SSRRenderer::TraceTargets> traceTargets = ssr.Prepare();
        if ( !traceTargets )
            return {};
        const RDG::TextureRef velocity = textures.Transients.Velocity;
        if ( !velocity.IsValid() )
        {
            LOG_ERROR( "[SceneRenderer] Deferred: SSR needs the view's velocity (where a hit was last frame), the "
                       "frame has none" );
            return {};
        }
        const RDG::TextureRef hzb = AddFrameHZB( graph, textures );
        if ( !hzb.IsValid() )
            return {};
        // Transients of this graph, sized from this frame's view: written by the trace, read by the tiled resolve.
        const RDG::TextureRef trace    = graph.CreateTexture( traceTargets->Trace, "SSR.Trace" );
        const RDG::TextureRef traceRay = graph.CreateTexture( traceTargets->TraceRay, "SSR.TraceRay" );
        const RDG::TextureRef tiles    = graph.CreateTexture( traceTargets->TileMask, "SSR.TileMask" );
        // A lost trace reflects nothing; a lost tile mask marks no tile (RDG-FAULT1). A lost ray (a zero
        // direction) is skipped by the reconstruction's estimator.
        graph.SetFaultDefault( trace, RDG::FaultDefault::Black );
        graph.SetFaultDefault( traceRay, RDG::FaultDefault::Black );
        graph.SetFaultDefault( tiles, RDG::FaultDefault::Black );
        const RDG::TextureRef sceneHistory =
             textures.Import( ssr.GetSceneColorHistoryImage(), "SSR.SceneColorHistory" );
        // Sampled by name in every SSR pass (no G-buffer image crosses into a pass exec).
        // World positions come from the G-buffer depth (Common/ReconstructPosition.glslh).
        const System::SSRRenderer::GBufferInputs inputs{ gbuffer[0], gbuffer[1],
                                                         textures.Depth( m_GBuffer, "GBuffer" ) };
        const RDG::BufferRef                     traceUniforms = System::SSRRenderer::UploadTraceUniforms(
             graph, frame.InvJitteredViewProjection, frame.JitteredViewProjection );
        // Build time, after Prepare: the exec runs after the frame was built and must not consult a stamp a later
        // frame may have written.
        const bool                         sceneHistoryReadable = ssr.SceneColorHistoryReadableIn( frame );
        const uint32_t                     seed                 = ssr.NextTraceSeed();
        System::SSRRenderer::TraceSettings settings;
        settings.RaysPerPixel = m_SSRRaysPerPixel;
        settings.Iterations   = m_SSRMaxSteps;
        settings.MaxDistance  = m_SSRMaxDistance;
        settings.Intensity    = m_SSRIntensity;
        settings.Thickness    = Common::Units::Metres( 0.5f ); // literature: 0.5 m
        graph.AddPass(
             "Deferred: SSR", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 ssr.DeclareTraceBindings( pass, trace, traceRay, tiles, inputs, velocity, sceneHistory, hzb,
                                           traceUniforms );
                 // The G-buffer colours the trace does not sample stay declared as before.
                 ReadAll( pass, { gbuffer[0] }, RDG::Access::SampledCompute );
                 ReadAll( pass, { gbuffer.begin() + 2, gbuffer.end() }, RDG::Access::SampledCompute );
             },
             [ssrPtr = &ssr, cameraPos = glm::vec4( frame.CameraPosition, 1.0f ), settings, sceneHistoryReadable,
              seed]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return ssrPtr->RecordTrace( context, cameraPos, settings, sceneHistoryReadable, seed ); } );
        const RDG::TextureRef resolved =
             ssr.Denoise( graph,
                          System::ReflectionsDenoiserInputs{
                               .Trace = trace, .TraceRay = traceRay, .TileMask = tiles, .TraceSeed = seed },
                          System::DenoiserSceneTextures{ .GBufferNormal = gbuffer[1],
                                                         .GBufferDepth  = inputs[2],
                                                         .Velocity      = velocity,
                                                         .Black         = textures.GraphRefs().System.Black },
                          frame );
        return FrameSSR{ .Resolved = resolved, .SceneHistory = sceneHistory };
    }
} // namespace Desert::Graphic

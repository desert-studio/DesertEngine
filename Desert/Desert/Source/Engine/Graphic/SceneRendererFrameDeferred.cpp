#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
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

        // Every attachment of @p target LOAD/STORE, the depth tested and written: the graph opens the render
        // pass a pipeline built for that framebuffer draws in, and leaves the depth in the attachment layout.
        void LoadTarget( RDG::PassBuilder& pass, const RDG::ImportedFramebuffer& target )
        {
            for ( uint32_t i = 0; i < target.Colors.size(); ++i )
                pass.ColorTarget( i, target.Colors[i], RDG::LoadOp::Load() );
            if ( target.Depth.IsValid() )
                pass.DepthTarget( target.Depth, RDG::LoadOp::Load(), /*write*/ true );
        }

        void ReadAll( RDG::PassBuilder& pass, const std::vector<RDG::TextureRef>& refs, RDG::Access access )
        {
            for ( const RDG::TextureRef ref : refs )
                pass.Read( ref, access );
        }
    } // namespace

    void SceneRenderer::AddFrameClearMainFramebuffer( RDG::Builder& graph, LegacyFrameTextures& textures )
    {
        const RDG::ImportedFramebuffer target = textures.ImportFramebuffer( m_TargetFramebuffer, "SceneColor" );
        if ( target.Colors.empty() && !target.Depth.IsValid() )
            return;
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
             },
             []( RDG::PassContext& ) -> Common::BoolResultStr { return BOOLSUCCESS; } );
    }

    void SceneRenderer::AddFrameDepthResolve( RDG::Builder& graph, LegacyFrameTextures& textures )
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
        const RDG::TextureRef          sourceRef = textures.Import( source, "GBuffer.Depth", RDG::Access::DepthWrite );
        const RDG::TextureRef          targetRef = textures.ImportFramebuffer( m_TargetFramebuffer, "SceneColor" ).Depth;
        graph.AddPass(
             "Deferred: DepthResolve", RDG::PassFlags::Copy,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( sourceRef, RDG::Access::CopySrc );
                 pass.Write( targetRef, RDG::Access::CopyDst );
             },
             [source, target]( RDG::PassContext& ) -> Common::BoolResultStr
             { return Renderer::GetInstance().CopyDepthImage( source.get(), target.get() ); } );
    }

    void SceneRenderer::AddFrameSSAO( RDG::Builder& graph, LegacyFrameTextures& textures,
                                      const std::vector<RDG::TextureRef>& gbuffer, const glm::mat4& viewProj,
                                      const glm::vec4& cameraPos, const std::shared_ptr<LegacyFrameValues>& values,
                                      std::vector<RDG::TextureRef>& compositeReads )
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
        if ( !ssao || !ssao->GetAOImage() )
            return;
        const std::shared_ptr<Image2D> aoImage = ssao->GetAOImage();
        const RDG::TextureRef          ao      = textures.Import( aoImage, "SSAO" );
        compositeReads.push_back( ao );
        graph.AddPass(
             "Deferred: SSAO", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ReadAll( pass, gbuffer, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, ao, EngineClearColor() ); // AO is fully recomputed each frame
             },
             [this, ssao, aoImage, viewProj, cameraPos, values]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 ssao->Execute( m_GBuffer, viewProj, cameraPos, kSSAORadius, kSSAOBias, /*power*/ 1.5f,
                                /*samples*/ 16 );
                 values->AoImage = aoImage;
                 return BOOLSUCCESS;
             } );
    }

    void SceneRenderer::AddFrameGIResolve( RDG::Builder& graph, LegacyFrameTextures& textures,
                                           const std::vector<RDG::TextureRef>& gbuffer,
                                           const std::vector<RDG::TextureRef>& rsm,
                                           System::MeshRenderer* meshRenderer, const glm::mat4& viewProj,
                                           const glm::vec4&                          lightColor,
                                           const std::shared_ptr<LegacyFrameValues>& values,
                                           std::vector<RDG::TextureRef>&             compositeReads )
    {
        auto* gi = UNIQUE_GET_AS( System::GIResolveRenderer, m_RenderSystems["GISystem"] );
        if ( !gi || !gi->Prepare() )
            return;
        const std::shared_ptr<Image2D> accumImage = gi->GetAccumImage();
        const RDG::TextureRef          gather     = textures.Import( gi->GetGatherImage(), "GI.Gather" );
        const RDG::TextureRef          accum      = textures.Import( accumImage, "GI" );
        const RDG::TextureRef          history    = textures.Import( gi->GetHistoryImage(), "GI.History" );
        compositeReads.push_back( accum );
        graph.AddPass(
             "Deferred: GIResolve", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ReadAll( pass, gbuffer, RDG::Access::SampledGraphics );
                 ReadAll( pass, rsm, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, gather, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [this, gi, meshRenderer, lightColor]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 gi->RecordGather( m_GBuffer, m_RSMBuffer->GetColorAttachmentImage( 0 ),
                                   m_RSMBuffer->GetColorAttachmentImage( 1 ), m_RSMBuffer->GetColorAttachmentImage( 2 ),
                                   meshRenderer->GetRSMViewProj(), lightColor, m_GIIntensity );
                 return BOOLSUCCESS;
             } );
        graph.AddPass(
             "Deferred: GITemporal", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ReadAll( pass, gbuffer, RDG::Access::SampledGraphics );
                 pass.Read( gather, RDG::Access::SampledGraphics );
                 pass.Read( history, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, accum, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [this, gi, viewProj, accumImage, values]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 gi->RecordTemporal( m_GBuffer, viewProj );
                 values->GiImage = accumImage;
                 return BOOLSUCCESS;
             } );
    }

    void SceneRenderer::AddFrameComposite( RDG::Builder& graph, LegacyFrameTextures& textures,
                                           const std::vector<RDG::TextureRef>& compositeReads,
                                           System::MeshRenderer* meshRenderer, const glm::vec4& lightDir,
                                           const glm::vec4& lightColor, const glm::vec4& cameraPos,
                                           const std::shared_ptr<LegacyFrameValues>& values )
    {
        // LOAD/STORE on every scene-target attachment. The depth is declared written because the passes after
        // this one that are not graph nodes yet (forward meshes, glass, fog, clouds, overlays) begin their own
        // render passes on it in the depth-attachment layout, and DepthResolve left it a transfer destination.
        const RDG::ImportedFramebuffer target = textures.ImportFramebuffer( m_TargetFramebuffer, "SceneColor" );
        graph.AddPass(
             "Deferred: Composite", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ReadAll( pass, compositeReads, RDG::Access::SampledGraphics );
                 LoadTarget( pass, target );
             },
             [this, meshRenderer, lightDir, lightColor, cameraPos, values]( RDG::PassContext& ) -> Common::BoolResultStr
                   {
                       DeferredShadowInput shadow;
                       if ( meshRenderer )
                       {
                           shadow.CascadeVP            = meshRenderer->GetCascadeViewProj();
                           shadow.Count                = meshRenderer->GetValidCascadeCount();
                           shadow.Bias                 = meshRenderer->GetShadowBias();
                           shadow.Enabled              = meshRenderer->AreShadowsEnabled();
                           shadow.CascadeWorldPerTexel = meshRenderer->GetCascadeWorldPerTexel();
                           for ( uint32_t c = 0; c < shadow.Count && c < 4u; ++c )
                           {
                               const auto img        = meshRenderer->GetCascadeShadowImage( c );
                               shadow.CascadeMaps[c] = img ? img.get() : nullptr;
                           }
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
                       UNIQUE_GET_AS( System::DeferredLightingRenderer, m_RenderSystems["DeferredLightingSystem"] )
                            ->Execute( m_GBuffer, lightDir, lightColor, cameraPos,
                                       static_cast<int>( m_DebugView.DeferredDebug ), GetPointLights(),
                                       GetSpotLights(), shadow, values->AoImage, giIntensity, m_EnableSSAO,
                                       static_cast<int>( m_GIMode ), values->GiImage, cloudShadow, environment );
                       return BOOLSUCCESS;
             } );
    }

    void SceneRenderer::AddFrameSceneCopy( RDG::Builder& graph, LegacyFrameTextures& textures,
                                           const std::vector<RDG::TextureRef>&       sceneColor,
                                           System::CopyRenderer*                     copy,
                                           const std::shared_ptr<LegacyFrameValues>& values,
                                           std::vector<RDG::TextureRef>&             copyReads )
    {
        if ( !copy || !copy->GetImage() )
            return;
        const std::shared_ptr<Image2D> image = copy->GetImage();
        copyReads                            = { textures.Import( image, "SceneCopy" ) };
        graph.AddPass(
             "Deferred: SceneCopy", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ReadAll( pass, sceneColor, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, copyReads.front(), EngineClearColor() );
             },
             [this, copy, image, values]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 copy->Execute( m_TargetFramebuffer->GetColorAttachmentImage( 0 ) );
                 values->SceneCopy = image;
                 return BOOLSUCCESS;
             } );
    }

    void SceneRenderer::AddFrameSSR( RDG::Builder& graph, LegacyFrameTextures& textures,
                                     const std::vector<RDG::TextureRef>& gbuffer,
                                     const std::vector<RDG::TextureRef>& copyReads, const glm::mat4& viewProj,
                                     const glm::vec4& cameraPos, const std::shared_ptr<LegacyFrameValues>& values )
    {
        auto* ssr = UNIQUE_GET_AS( System::SSRRenderer, m_RenderSystems["SSRSystem"] );
        if ( !ssr || copyReads.empty() || !ssr->Prepare() )
            return;
        const RDG::TextureRef          trace   = textures.Import( ssr->GetTraceImage(), "SSR.Trace" );
        const RDG::TextureRef          tiles   = textures.Import( ssr->GetTileMask(), "SSR.TileMask" );
        const RDG::TextureRef          accum   = textures.Import( ssr->GetAccumImage(), "SSR" );
        const RDG::TextureRef          history = textures.Import( ssr->GetHistoryImage(), "SSR.History" );
        const RDG::ImportedFramebuffer target  = textures.ImportFramebuffer( m_TargetFramebuffer, "SceneColor" );
        graph.AddPass(
             "Deferred: SSR", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 ReadAll( pass, gbuffer, RDG::Access::SampledCompute );
                 ReadAll( pass, copyReads, RDG::Access::SampledCompute );
                 pass.Write( trace, RDG::Access::StorageWrite );
                 pass.Write( tiles, RDG::Access::StorageWrite );
             },
             [this, ssr, viewProj, cameraPos, values]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 if ( !values->SceneCopy )
                     return Common::MakeError( "Deferred: SSR runs after a SceneCopy pass that left no copy" );
                 constexpr float kSSRThickness = Common::Units::Metres( 0.5f ); // literature: 0.5 m
                 ssr->RecordTrace( m_GBuffer, values->SceneCopy, viewProj, cameraPos, /*maxSteps*/ 32,
                                   m_SSRMaxDistance, m_SSRIntensity, kSSRThickness );
                 return BOOLSUCCESS;
             } );
        graph.AddPass(
             "Deferred: SSRResolve", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ReadAll( pass, gbuffer, RDG::Access::SampledGraphics );
                 ReadAll( pass, { trace, tiles, history }, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, accum, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             },
             [this, ssr]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 ssr->RecordResolve( m_GBuffer );
                 return BOOLSUCCESS;
             } );
        graph.AddPass(
             "Deferred: SSRComposite", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 ReadAll( pass, gbuffer, RDG::Access::SampledGraphics );
                 ReadAll( pass, { accum, tiles }, RDG::Access::SampledGraphics );
                 LoadTarget( pass, target ); // blend over the scene
             },
             [this, ssr, viewProj]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 ssr->RecordComposite( m_GBuffer, viewProj );
                 return BOOLSUCCESS;
             } );
    }
} // namespace Desert::Graphic

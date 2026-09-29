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
    void SceneRenderer::AddFrameClearMainFramebuffer( RDG::Builder& graph, LegacyFrameTextures& textures )
    {
        AddLegacy( graph, "ClearMainFramebuffer", {}, textures.Colors( m_TargetFramebuffer, "SceneColor" ),
                   [this]() { ClearMainFramebuffer(); } );
    }

    void SceneRenderer::AddFrameDepthResolve( RDG::Builder& graph )
    {
        // Resolve the G-buffer depth (static opaque geometry) into the scene target depth. The deferred
        // composite writes only colour, so without this depth-tested overlays (grid, colliders) never get
        // occluded by static meshes. Outside any render pass, before the forward-over-composite draws.
        if ( m_TargetFramebuffer && m_TargetFramebuffer->GetDepthAttachmentCount() > 0 &&
             m_GBuffer->GetDepthAttachmentCount() > 0 )
        {
            AddLegacy( graph, "Deferred: DepthResolve", {}, {},
                       [this]()
                       {
                           Renderer::GetInstance().CopyDepthImage(
                                m_GBuffer->GetDepthAttachmentImage().get(),
                                m_TargetFramebuffer->GetDepthAttachmentImage().get() );
                       } );
        }
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

        if ( m_EnableSSAO )
            if ( auto* ssao = UNIQUE_GET_AS( System::SSAORenderer, m_RenderSystems["SSAOSystem"] ) )
            {
                const std::vector<RDG::TextureRef> ao = textures.Refs( { { ssao->GetAOImage(), "SSAO" } } );
                compositeReads.insert( compositeReads.end(), ao.begin(), ao.end() );
                AddLegacy( graph, "Deferred: SSAO", gbuffer, ao,
                           [this, ssao, viewProj, cameraPos, values]()
                           {
                               ssao->Execute( m_GBuffer, viewProj, cameraPos, kSSAORadius, kSSAOBias,
                                              /*power*/ 1.5f, /*samples*/ 16 );
                               values->AoImage = ssao->GetAOImage();
                           } );
            }
    }

    void SceneRenderer::AddFrameGIResolve( RDG::Builder& graph, LegacyFrameTextures& textures,
                                           const std::vector<RDG::TextureRef>& gbuffer,
                                           const std::vector<RDG::TextureRef>& rsm,
                                           System::MeshRenderer* meshRenderer, const glm::mat4& viewProj,
                                           const glm::vec4&                          lightColor,
                                           const std::shared_ptr<LegacyFrameValues>& values,
                                           std::vector<RDG::TextureRef>&             compositeReads )
    {
        if ( auto* gi = UNIQUE_GET_AS( System::GIResolveRenderer, m_RenderSystems["GISystem"] ) )
        {
            std::vector<RDG::TextureRef> giReads = gbuffer;
            giReads.insert( giReads.end(), rsm.begin(), rsm.end() );
            const std::vector<RDG::TextureRef> giOut = textures.Refs( { { gi->GetGIImage(), "GI" } } );
            compositeReads.insert( compositeReads.end(), giOut.begin(), giOut.end() );
            AddLegacy( graph, "Deferred: GIResolve", giReads, giOut,
                       [this, gi, meshRenderer, viewProj, lightColor, values]()
                       {
                           gi->Execute( m_GBuffer, m_RSMBuffer->GetColorAttachmentImage( 0 ),
                                        m_RSMBuffer->GetColorAttachmentImage( 1 ),
                                        m_RSMBuffer->GetColorAttachmentImage( 2 ), meshRenderer->GetRSMViewProj(),
                                        viewProj, lightColor, m_GIIntensity );
                           values->GiImage = gi->GetGIImage();
                       } );
        }
    }

    void SceneRenderer::AddFrameComposite( RDG::Builder& graph, const std::vector<RDG::TextureRef>& compositeReads,
                                           const std::vector<RDG::TextureRef>& sceneColor,
                                           System::MeshRenderer* meshRenderer, const glm::vec4& lightDir,
                                           const glm::vec4& lightColor, const glm::vec4& cameraPos,
                                           const std::shared_ptr<LegacyFrameValues>& values )
    {
        AddLegacy( graph, "Deferred: Composite", compositeReads, sceneColor,
                   [this, meshRenderer, lightDir, lightColor, cameraPos, values]()
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
                   } );
    }

    void SceneRenderer::AddFrameSceneCopy( RDG::Builder& graph, LegacyFrameTextures& textures,
                                           const std::vector<RDG::TextureRef>&       sceneColor,
                                           System::CopyRenderer*                     copy,
                                           const std::shared_ptr<LegacyFrameValues>& values,
                                           std::vector<RDG::TextureRef>&             copyReads )
    {
        if ( copy )
        {
            copyReads = textures.Refs( { { copy->GetImage(), "SceneCopy" } } );
            AddLegacy( graph, "Deferred: SceneCopy", sceneColor, copyReads,
                       [this, copy, values]()
                       {
                           copy->Execute( m_TargetFramebuffer->GetColorAttachmentImage( 0 ) );
                           values->SceneCopy = copy->GetImage();
                       } );
        }
    }

    void SceneRenderer::AddFrameSSR( RDG::Builder& graph, const std::vector<RDG::TextureRef>& gbuffer,
                                     const std::vector<RDG::TextureRef>& copyReads,
                                     const std::vector<RDG::TextureRef>& sceneColor, const glm::mat4& viewProj,
                                     const glm::vec4& cameraPos, const std::shared_ptr<LegacyFrameValues>& values )
    {
        if ( auto* ssr = UNIQUE_GET_AS( System::SSRRenderer, m_RenderSystems["SSRSystem"] ) )
        {
            std::vector<RDG::TextureRef> ssrReads = gbuffer;
            ssrReads.insert( ssrReads.end(), copyReads.begin(), copyReads.end() );
            AddLegacy( graph, "Deferred: SSR", ssrReads, sceneColor,
                       [this, ssr, viewProj, cameraPos, values]()
                       {
                           if ( !values->SceneCopy )
                               return;
                           constexpr float kSSRThickness = Common::Units::Metres( 0.5f ); // literature: 0.5 m
                           ssr->Execute( m_GBuffer, values->SceneCopy, viewProj, cameraPos, /*maxSteps*/ 32,
                                         m_SSRMaxDistance, m_SSRIntensity, kSSRThickness );
                       } );
        }
    }
} // namespace Desert::Graphic

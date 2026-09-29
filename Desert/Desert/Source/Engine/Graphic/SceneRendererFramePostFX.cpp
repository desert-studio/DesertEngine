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
    void SceneRenderer::AddFrameJumpFlood( RDG::Builder& graph )
    {
        AddLegacy( graph, "PostFX: JumpFlood", {}, {},
                   [this]()
                   {
                       const auto& jfa =
                            UNIQUE_GET_AS( System::JumpFloodOutlineRenderer, m_RenderSystems["JumpFloodSystem"] );
                       jfa->SetOutlineActive(
                            UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] )->HasOutline() );
                       jfa->Execute();
                   } );
    }

    void SceneRenderer::AddFrameAutoExposure( RDG::Builder& graph, const std::vector<RDG::TextureRef>& sceneColor )
    {
        AddLegacy( graph, "PostFX: AutoExposure", sceneColor, {},
                   [this]()
                   {
                       const auto& autoExp =
                            UNIQUE_GET_AS( System::AutoExposureRenderer, m_RenderSystems["AutoExposureSystem"] );
                       autoExp->Execute();
                       UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )
                            ->SetAutoExposureImage( autoExp->GetAdaptedLuminanceImage() );
                   } );
    }

    void SceneRenderer::AddFrameBloom( RDG::Builder& graph, const std::vector<RDG::TextureRef>& sceneColor )
    {
        AddLegacy( graph, "PostFX: Bloom", sceneColor, {}, [this]()
                   { UNIQUE_GET_AS( System::BloomRenderer, m_RenderSystems["BloomSystem"] )->Execute(); } );
    }

    void SceneRenderer::AddFrameLightShafts( RDG::Builder& graph, const std::vector<RDG::TextureRef>& sceneColor,
                                             const std::shared_ptr<LegacyFrameValues>& values )
    {
        AddLegacy( graph, "PostFX: LightShafts", sceneColor, {},
                   [this, values]()
                   {
                       const AtmosphereEnv& atmosphere = GetAtmosphere();
                       if ( m_SceneInfo.ActiveCamera && atmosphere.Valid )
                       {
                           const glm::mat4 viewProjection = m_SceneInfo.ActiveCamera->GetProjectionMatrix() *
                                                            m_SceneInfo.ActiveCamera->GetViewMatrix();
                           values->Sun = ComputeSunScreen( viewProjection, atmosphere.SunDirection );
                       }
                       const SunScreen& sun = values->Sun;

                       const auto& shafts =
                            UNIQUE_GET_AS( System::LightShaftRenderer, m_RenderSystems["LightShaftSystem"] );
                       const auto& tonemap =
                            UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );

                       shafts->SetParams( System::LightShaftRenderer::Params{
                            .Enabled       = m_SunLightFx.LightShaftBloom,
                            .BloomScale    = m_SunLightFx.BloomScale,
                            .Threshold     = m_SunLightFx.BloomThreshold,
                            .MaxBrightness = m_SunLightFx.BloomMaxBrightness,
                            .BloomTint     = m_SunLightFx.BloomTint,
                       } );
                       shafts->Execute( sun.Uv, sun.Fade );

                       const float intensity =
                            m_SunLightFx.LightShaftBloom ? m_SunLightFx.BloomScale * sun.Fade : 0.0f;
                       tonemap->SetLightShaftImage( shafts->GetShaftImage() );
                       tonemap->SetLightShafts( intensity, m_SunLightFx.BloomTint );
                   } );
    }

    void SceneRenderer::AddFrameLensFlare( RDG::Builder& graph, const std::vector<RDG::TextureRef>& sceneColor,
                                           const std::shared_ptr<LegacyFrameValues>& values )
    {
        AddLegacy( graph, "PostFX: LensFlare", sceneColor, {},
                   [this, values]()
                   {
                       const SunScreen& sun = values->Sun;
                       const auto&      flare =
                            UNIQUE_GET_AS( System::LensFlareRenderer, m_RenderSystems["LensFlareSystem"] );
                       const auto& tonemap =
                            UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] );

                       flare->SetParams( m_LensFlare );
                       flare->Execute( sun.Uv, sun.Fade );

                       // Derived HERE, from the same two numbers that decided whether the dispatches ran, so a
                       // zero intensity and a skipped dispatch can never disagree — the bloom image's contract.
                       const float intensity =
                            m_LensFlare.Enabled ? LensFlareStrength( sun.Fade, m_LensFlare.Intensity ) : 0.0f;
                       tonemap->SetLensFlareImage( flare->GetFlareImage() );
                       tonemap->SetLensFlare( intensity, m_LensFlareTint );
                   } );
    }

    void SceneRenderer::AddFrameTonemap( RDG::Builder& graph, const std::vector<RDG::TextureRef>& sceneColor )
    {
        AddLegacy( graph, "PostFX: Tonemap", sceneColor, {}, [this]()
                   { UNIQUE_GET_AS( System::TonemapRenderer, m_RenderSystems["TonemapSystem"] )->Execute(); } );
    }

    void SceneRenderer::AddFrameFXAA( RDG::Builder& graph )
    {
        AddLegacy( graph, "PostFX: FXAA", {}, {},
                   [this]() { UNIQUE_GET_AS( System::FXAARenderer, m_RenderSystems["FXAASystem"] )->Execute(); } );
    }

    void SceneRenderer::AddFrameSMAA( RDG::Builder& graph )
    {
        AddLegacy( graph, "PostFX: SMAA", {}, {},
                   [this]() { UNIQUE_GET_AS( System::SMAARenderer, m_RenderSystems["SMAASystem"] )->Execute(); } );
    }

    void SceneRenderer::AddFrameBackdropBlur( RDG::Builder& graph, LegacyFrameTextures& textures,
                                              const std::vector<RDG::TextureRef>& sceneColor )
    {
        if ( auto* backdrop =
                  UNIQUE_GET_AS( System::BackdropBlurRenderer, m_RenderSystems["BackdropBlurSystem"] ) )
            AddLegacy( graph, "UI: BackdropBlur", sceneColor,
                       textures.Refs( { { backdrop->GetImage(), "BackdropBlur" } } ),
                       [backdrop]() { backdrop->Execute(); } );
    }
} // namespace Desert::Graphic

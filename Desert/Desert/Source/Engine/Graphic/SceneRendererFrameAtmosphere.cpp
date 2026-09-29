#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
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
    void SceneRenderer::AddFrameParticlesSimulate( RDG::Builder& graph, const UpdateInfo& sceneRenderInfo )
    {
        // Particle simulation compute (outside any render pass) BEFORE the graph records the billboard draw,
        // so the freshly-integrated particle buffer is ready + visible to the vertex stage.
        // The graph executes before OnUpdate returns, so the frame's UpdateInfo outlives this pass.
        AddLegacy( graph, "Particles: SimulateInFrame", {}, {},
                   [this, &sceneRenderInfo]()
                   {
                       UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] )
                            ->SimulateInFrame( sceneRenderInfo.Timestep.GetSeconds() );
                   } );
    }

    void SceneRenderer::AddFrameCloudShadowMap( RDG::Builder& graph )
    {
        // The cloud layer's shadow on the world. HERE, and not beside the cloud march at the other end of
        // the frame, because its consumer is the DEFERRED LIGHTING pass. It reads no scene depth, no
        // G-buffer and no atmosphere LUT, and it is an in-frame compute dispatch, so it must be outside any
        // open render pass, which this point is.
        AddLegacy( graph, "CloudShadowMap", {}, {}, [this]() { ExecuteCloudShadowMap(); } );
    }

    void SceneRenderer::AddFrameSkyAtmosphereLuts( RDG::Builder& graph )
    {
        AddLegacy(
             graph, "SkyAtmosphereLuts", {}, {},
             [this]() {
                 UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] )->ExecuteAtmosphereLuts();
             } );
    }

    void SceneRenderer::AddFrameAtmosphericFog( RDG::Builder&                       graph,
                                                const std::vector<RDG::TextureRef>& sceneColor )
    {
        AddLegacy( graph, "AtmosphericFog", {}, sceneColor, [this]() { ExecuteAtmosphericFog(); } );
    }

    void SceneRenderer::AddFrameVolumetricClouds( RDG::Builder&                       graph,
                                                  const std::vector<RDG::TextureRef>& sceneColor )
    {
        AddLegacy( graph, "VolumetricClouds", {}, sceneColor, [this]() { ExecuteVolumetricClouds(); } );
    }
} // namespace Desert::Graphic

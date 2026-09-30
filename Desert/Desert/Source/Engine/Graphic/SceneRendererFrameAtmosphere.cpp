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
    void SceneRenderer::AddFrameParticlesSimulate( RDG::Builder& graph, const UpdateInfo& sceneRenderInfo )
    {
        // Particle simulation: one compute dispatch per emitter, outside any render pass, BEFORE the billboard
        // draw reads the integrated particle buffer in its vertex stage. A Compute node, NeverCull because it
        // declares no graph resource: what it writes are the emitters' storage buffers
        // (ShaderResources::StorageBuffer), and the renderer can import an engine Image2D into the graph
        // (Renderer::ImportImage) but has no import for an engine buffer. So the graph cannot place the
        // compute-write -> vertex-read barrier on them, and DispatchComputeCull keeps it (ParticleRenderer.cpp,
        // SimulateInFrame). The graph executes before OnUpdate returns, so the frame's UpdateInfo outlives this
        // pass.
        auto* particles = UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] );
        if ( !particles )
            return;
        const float seconds = static_cast<float>( sceneRenderInfo.Timestep.GetSeconds() );
        graph.AddPass(
             "Particles: Simulate", RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
             []( RDG::PassBuilder& ) {},
             [particles, seconds]( RDG::PassContext& ) -> Common::BoolResultStr
             {
                 particles->SimulateInFrame( seconds );
                 return BOOLSUCCESS;
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

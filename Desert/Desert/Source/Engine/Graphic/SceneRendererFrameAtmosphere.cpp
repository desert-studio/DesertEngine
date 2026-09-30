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
        // draw reads the integrated particle buffer in its vertex stage. A Compute node that imports every
        // emitter's state and spawn-counter buffers (ParticleRenderer::ImportSimulationBuffers, through
        // Renderer::ImportBuffer) and declares StorageWrite on them, so the graph places the barrier against the
        // previous graph's write of the persistent state. The simulation reads its buffers only through that
        // read-modify-write, so StorageWrite is every access it makes. The billboard draw that reads the result
        // is in the Transparency bridge and declares nothing yet: L5 makes ParticlePass a node with
        // Read(particles, StorageRead), and removes DispatchComputeCull's own compute -> vertex barrier then.
        // NeverCull stays: the node also advances the particle clock on a frame with no emitter, a write the
        // graph cannot see. The graph executes before OnUpdate returns, so the frame's UpdateInfo, and the
        // imports held by the renderer's frame emitters, outlive this pass.
        auto* particles = UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] );
        if ( !particles )
            return;
        const float                       seconds = static_cast<float>( sceneRenderInfo.Timestep.GetSeconds() );
        const std::vector<RDG::BufferRef> written = particles->ImportSimulationBuffers( graph );
        graph.AddPass(
             "Particles: Simulate", RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
             [&written]( RDG::PassBuilder& pass )
             {
                 for ( const RDG::BufferRef buffer : written )
                     pass.Write( buffer, RDG::Access::StorageWrite );
             },
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

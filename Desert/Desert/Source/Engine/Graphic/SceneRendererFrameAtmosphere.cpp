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
        // (ParticlePass, a Transparency raster node) declares Read(particles, StorageRead), so the graph places
        // the compute -> vertex barrier; DispatchComputeCull records none of its own. NeverCull stays: the node
        // also advances the particle clock on a frame with no emitter, a write the graph cannot see. The graph
        // executes before OnUpdate returns, so the frame's UpdateInfo, and the imports held by the renderer's
        // frame emitters, outlive this pass.
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

    void SceneRenderer::AddFrameCloudShadowMap( RDG::Builder& graph, FrameTextures& textures )
    {
        // The cloud layer's shadow on the world. HERE, and not beside the cloud march at the other end of the
        // frame, because its readers are the lit passes (the deferred Composite, the forward meshes and terrain),
        // which declare it through DeclareShadowReads. It reads no scene depth, no G-buffer and no atmosphere LUT.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* clouds = UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] );
        if ( clouds )
            AddComputeNodes( graph, textures, clouds->DeclareShadowMapNodes() );
    }

    void SceneRenderer::AddFrameSkyAtmosphereLuts( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* sky = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] );
        if ( sky )
            AddComputeNodes( graph, textures, sky->DeclareAtmosphereLutNodes() );
    }

    void SceneRenderer::AddFrameAtmosphericFog( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* fog = UNIQUE_GET_AS( System::HeightFogRenderer, m_RenderSystems["HeightFogSystem"] );
        if ( fog )
            AddComputeNodes( graph, textures, fog->DeclareFrameNodes() );
    }

    void SceneRenderer::AddFrameVolumetricClouds( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* clouds = UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] );
        if ( clouds )
            AddComputeNodes( graph, textures, clouds->DeclareFrameNodes() );
    }
} // namespace Desert::Graphic

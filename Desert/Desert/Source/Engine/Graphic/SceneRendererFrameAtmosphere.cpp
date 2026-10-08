#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
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

namespace Desert::Graphic
{
    void SceneRenderer::AddFrameParticlesSimulate( RDG::Builder& graph )
    {
        // Particle simulation: the frame's fixed VFX steps (the scene's VFXWorld, ticked by the scene update;
        // no timestep is read here), one Compute node per step, outside any render pass, BEFORE the billboard
        // draw reads the integrated particle buffer in its vertex stage. Every emitter's state and step-table
        // buffers are imported once (ParticleRenderer::ImportSimulationBuffers, through Renderer::ImportBuffer);
        // each step node declares StorageWrite on the buffers of the emitters that run that step (one binding
        // block per emitter, ParticleRenderer::DeclareSimulateBindings), so the graph places the barrier against
        // the previous step's write - step s+1 reads what step s wrote - and, for the first step, against the
        // previous graph's write of the persistent state. The billboard draw that reads the result (ParticlePass,
        // a translucency raster node) declares it StorageRead in its blocks, so the graph places the compute ->
        // vertex barrier after the last step; DispatchCompute records none of its own. NeverCull: the persistent
        // state advances even on a frame nothing draws it. The graph executes before OnUpdate returns, so the
        // imports held by the renderer's frame emitters outlive these passes.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* particles = UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] );
        if ( particles == nullptr )
            return;
        particles->ImportSimulationBuffers( graph );
        const uint32_t steps = particles->SimulationStepCount();
        for ( uint32_t step = 0; step < steps; ++step )
            graph.AddPass(
                 std::format( "Particles: Simulate {}", step ),
                 RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
                 [particles, step]( RDG::PassBuilder& pass ) { particles->DeclareSimulateBindings( pass, step ); },
                 [particles, step]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->Simulate( context, step ); } );
    }

    void SceneRenderer::AddFrameCloudShadowMap( RDG::Builder& graph, FrameTextures& textures )
    {
        // The cloud layer's shadow on the world. HERE, and not beside the cloud march at the other end of the
        // frame, because its readers are the lit passes (the deferred Composite, the forward meshes and terrain),
        // which take it as a scene view input (SceneViewInputsOf). It reads no scene depth, no G-buffer and no
        // atmosphere LUT. NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this
        // exact type
        auto* clouds = UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] );
        if ( clouds == nullptr )
            return;
        clouds->SettleShadowMapNodes( AddComputeNodes( graph, textures, clouds->DeclareShadowMapNodes() ) );
        // Its readers take this graph ref through SceneViewInputsOf (CloudShadowMapOrWhite): one registration
        // per engine image.
        if ( clouds->HasShadowMap() )
            textures.Transients.CloudShadowMap = textures.Import( clouds->GetShadowMap(), "Clouds.ShadowMap" );
    }

    void SceneRenderer::AddFrameSkyAtmosphereLuts( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* sky = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] );
        if ( sky != nullptr )
            sky->SettleAtmosphereLutNodes( AddComputeNodes( graph, textures, sky->DeclareAtmosphereLutNodes() ) );
    }

    void SceneRenderer::AddFrameAtmosphericFog( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* fog = UNIQUE_GET_AS( System::HeightFogRenderer, m_RenderSystems["HeightFogSystem"] );
        if ( fog != nullptr )
            AddComputeNodes( graph, textures, fog->DeclareFrameNodes( graph, textures.Transients ) );
    }

    void SceneRenderer::AddFrameVolumetricClouds( RDG::Builder& graph, FrameTextures& textures,
                                                  const ViewFrame& frame )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* clouds = UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] );
        if ( clouds == nullptr )
            return;
        clouds->SettleFrameNodes( AddComputeNodes( graph, textures, clouds->DeclareFrameNodes( graph, frame ) ) );
        // The composite (a translucency pass, added after this by AddFrameTranslucency) samples the pair by graph
        // ref.
        const System::VolumetricCloudRenderer::FrameResult result = clouds->GetFrameResult();
        textures.Transients.CloudScatter =
             textures.Import( result.Scatter, std::format( "Clouds.History{}", result.Slot ) );
        textures.Transients.CloudGuide =
             textures.Import( result.Guide, std::format( "Clouds.HistoryGuide{}", result.Slot ) );
        // The pair is next frame's history: if a fault removes the resolve that writes it, the history restarts.
        for ( const RDG::TextureRef history :
              { textures.Transients.CloudScatter, textures.Transients.CloudGuide } )
            if ( history.IsValid() )
                textures.MarkHistory( history, [clouds]() { clouds->OnTemporalHistoryReset(); } );
    }
} // namespace Desert::Graphic

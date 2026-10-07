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
        // Renderer::ImportBuffer) and declares StorageWrite on them (one binding block per emitter,
        // ParticleRenderer::DeclareSimulateBindings), so the graph places the barrier against the
        // previous graph's write of the persistent state. The simulation reads its buffers only through that
        // read-modify-write, so StorageWrite is every access it makes. The billboard draw that reads the result
        // (ParticlePass, a Transparency raster node) declares it StorageRead in its blocks, so the graph places
        // the compute -> vertex barrier; DispatchCompute records none of its own. NeverCull stays: the node
        // also advances the particle clock on a frame with no emitter, a write the graph cannot see. The graph
        // executes before OnUpdate returns, so the frame's UpdateInfo, and the imports held by the renderer's
        // frame emitters, outlive this pass.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* particles = UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] );
        if ( particles == nullptr )
            return;
        const float seconds = sceneRenderInfo.Timestep.GetSeconds();
        particles->ImportSimulationBuffers( graph );
        graph.AddPass(
             "Particles: Simulate", RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
             [particles]( RDG::PassBuilder& pass ) { particles->DeclareSimulateBindings( pass ); },
             [particles, seconds]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return particles->Simulate( context, seconds ); } );
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
        // The composite (a Transparency phase pass, declared after this) samples the pair by graph ref.
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

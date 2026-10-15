#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
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
    // The claiming view's simulation nodes (Compact 0, then per step Dispatch Args / Spawn+Update / Compact).
    static void AddParticleSimulationSteps( RDG::Builder& graph, System::ParticleRenderer& renderer );

    void SceneRenderer::AddFrameParticlesSimulate( RDG::Builder& graph )
    {
        // The world's particle pool (VFX-07): the frame's fixed VFX steps (the scene's VFXWorld; no timestep is
        // read here) as Compute nodes outside any render pass, BEFORE the billboard draw. "Particles: Compact 0"
        // builds every emitter's free / alive lists and draw slot from the pool; each step s then adds
        // "Particles: Spawn+Update s" (reads the lists, writes the pool) and "Particles: Compact s+1" (reads the
        // pool, writes the lists and the slot). Every node declares its buffers per emitter (ParticleRenderer
        // Declare*Bindings), so the graph orders them and places the barriers; the draw (ParticlePass, a
        // translucency raster node) reads the alive list StorageRead and the last slot IndirectArgs. Not
        // NeverCull: a node is live because it writes the imported pool; with no emitter it declares nothing and
        // the graph culls it. The graph executes before OnUpdate returns, so the imports held by the renderer
        // outlive these passes.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* particles = UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] );
        if ( particles == nullptr )
            return;
        // VFX-07b: the pool is the scene's (ParticleWorldGpu). Every view imports it for its draw; only the view
        // that claimed the VFXWorld tick adds the simulation, so two views of one scene simulate once and draw
        // twice. Spawn+Update s and compact s+1 are dispatched INDIRECT from what "Dispatch Args s" wrote from
        // compact s's GPU counts.
        particles->ImportFrameBuffers( graph );
        if ( particles->ClaimsSimulation() )
            AddParticleSimulationSteps( graph, *particles );
        // VFX-08: every view sorts its own translucent / additive emitters back to front (the key is its view
        // depth); the graph orders the sort after the last Compact and before ParticlePass from the buffers.
        particles->AddSortPasses( graph );
    }

    static void AddParticleSimulationSteps( RDG::Builder& graph, System::ParticleRenderer& renderer )
    {
        System::ParticleRenderer* particles = &renderer;
        const uint32_t            steps     = particles->SimulationStepCount();
        graph.AddPass(
             "Particles: Compact 0", RDG::PassFlags::Compute,
             [particles]( RDG::PassBuilder& pass ) { particles->DeclareCompactBindings( pass, 0 ); },
             [particles]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return particles->Compact( context, 0 ); } );
        for ( uint32_t step = 0; step < steps; ++step )
        {
            graph.AddPass(
                 std::format( "Particles: Dispatch Args {}", step ), RDG::PassFlags::Compute,
                 [particles, step]( RDG::PassBuilder& pass )
                 { particles->DeclareDispatchArgsBindings( pass, step ); },
                 [particles, step]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->BuildDispatchArgs( context, step ); } );
            graph.AddPass(
                 std::format( "Particles: Spawn+Update {}", step ), RDG::PassFlags::Compute,
                 [particles, step]( RDG::PassBuilder& pass ) { particles->DeclareSimulateBindings( pass, step ); },
                 [particles, step]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->Simulate( context, step ); } );
            graph.AddPass(
                 std::format( "Particles: Compact {}", step + 1 ), RDG::PassFlags::Compute,
                 [particles, step]( RDG::PassBuilder& pass )
                 { particles->DeclareCompactBindings( pass, step + 1 ); },
                 [particles, step]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->Compact( context, step + 1 ); } );
        }
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

    void SceneRenderer::AddFrameVolumetricFog( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* volume = UNIQUE_GET_AS( System::VolumetricFogRenderer, m_RenderSystems["VolumetricFogSystem"] );
        if ( volume == nullptr )
            return;

        // The sun exactly as the surfaces receive it: DirLights[0] after the atmosphere's transmittance
        // (BeginScene), times its VolumetricScatteringIntensity, shadowed by the same cascades.
        System::VolumetricFogSunInput sun;
        if ( const auto& lights = m_DirectionLights.DirectionLights; !lights.empty() )
        {
            sun.Present   = true;
            sun.TowardSun = glm::normalize( -glm::vec3( lights[0].Direction ) );
            sun.Radiance  = glm::vec3( lights[0].ColorIntensity ) * lights[0].ColorIntensity.w *
                           glm::max( m_SunVolumetricScatteringIntensity, 0.0f );
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( const auto* mesh = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] );
             mesh != nullptr && mesh->AreShadowsEnabled() && mesh->GetCascadeViewProj() != nullptr )
        {
            sun.CascadeCount = std::min<uint32_t>( mesh->GetValidCascadeCount(), 4u );
            for ( uint32_t c = 0; c < sun.CascadeCount; ++c )
                sun.CascadeViewProj[c] = mesh->GetCascadeViewProj()[c];
            sun.CascadeWorldPerTexel = mesh->GetCascadeWorldPerTexel();
            sun.CascadeDepthRange    = mesh->GetCascadeDepthRange();
        }
        // Inactive (fog off, VolumetricFog off, machine ceiling 0): no node, and the frame result says so.
        AddComputeNodes( graph, textures, volume->DeclareFrameNodes( textures.GraphRefs(), frame, sun ) );
    }

    void SceneRenderer::AddFrameOpaqueFog( RDG::Builder& graph, FrameTextures& textures, const ViewFrame& frame )
    {
        AddFrameSceneDepthResolve( graph, textures );
        AddFrameSkyAtmosphereLuts( graph, textures );
        AddFrameVolumetricFog( graph, textures, frame );
        AddFrameAtmosphericFog( graph, textures );
    }

    void SceneRenderer::AddFrameAtmosphericFog( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* fog = UNIQUE_GET_AS( System::HeightFogRenderer, m_RenderSystems["HeightFogSystem"] );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* volume = UNIQUE_GET_AS( System::VolumetricFogRenderer, m_RenderSystems["VolumetricFogSystem"] );
        const System::VolumetricFogFrameResult volumeResult =
             volume != nullptr ? volume->GetFrameResult() : System::VolumetricFogFrameResult{};
        if ( fog == nullptr )
            return;
        // The evaluation of the OPAQUE depth, then its apply over the scene target - the opaque composite only.
        AddComputeNodes( graph, textures, fog->DeclareFrameNodes( graph, textures.Transients, volumeResult ) );
        // No fog image this frame (neither fog nor aerial perspective): no apply node at all.
        if ( textures.Transients.HeightFog.IsValid() )
            AddSystemRaster( graph, textures, fog->ApplyPass() );

        // What every translucent surface after this point fogs itself with, at its own depth (Common/ViewFog.glslh,
        // bound by name through SceneViewInputs): DeclareFrameNodes published the parameter block (always - gates
        // 0 without fog); here the three images it reads, each the engine's fallback when absent (the block's
        // gates say it is not sampled), so a translucent cell never has an unbound slot.
        const AtmosphereEnv&         atmosphere = GetAtmosphere();
        const std::shared_ptr<Image> fallback3D =
             FallbackTextures::Get().GetFallbackTexture3D( Core::Formats::ImageFormat::RGBA8F );
        FrameTransients& t = textures.Transients;
        if ( atmosphere.AerialPerspectiveVolume )
            t.FogAerialPerspective = textures.Import( std::shared_ptr<Image>( atmosphere.AerialPerspectiveVolume ),
                                                      "Sky.AerialPerspectiveLut" );
        else
            t.FogAerialPerspective = textures.Import( fallback3D, "HeightFog.AerialPerspectiveFallback" );
        if ( atmosphere.DistantSkyLight )
            t.FogDistantSkyLight =
                 textures.Import( std::shared_ptr<Image>( atmosphere.DistantSkyLight ), "Sky.DistantLight" );
        else
            t.FogDistantSkyLight = textures.Import(
                 FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F ),
                 "HeightFog.DistantSkyLightFallback" );
        if ( volumeResult.Integrated != nullptr )
            t.FogVolume = textures.Import( volumeResult.Integrated, "VolumetricFog.Integrated" );
        else
            t.FogVolume = textures.Import( fallback3D, "HeightFog.VolumetricFogFallback" );
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

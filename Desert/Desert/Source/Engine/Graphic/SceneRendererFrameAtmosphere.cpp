#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ViewSettings.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
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
    static void AddParticleSimulationSteps( RDG::Builder& graph, System::ParticleRenderer& renderer,
                                            VFX::VFXTickStage stage );

    void SceneRenderer::AddFrameParticlesSimulate( RDG::Builder& graph, FrameTextures& textures )
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
        // VFX-PARAMS: the scene depth engine:Collision reads (the single-sample one the height fog and the clouds
        // sample, "SceneDepth.Compute"). Imported here, before the opaque passes; only the PostOpaque chain
        // (AddFrameParticlesPostOpaque) reads it for a collision, after the opaque passes wrote it this frame - the
        // graph orders that read after those writes (one graph texture per image, FrameTextures::Import). A frame
        // without one hands the 2D fallback, never an empty slot (an
        // unbound sampler is an invalid descriptor set), and says so: nothing collides then.
        const std::shared_ptr<Image2D> sceneDepth = GetComputeSceneDepth();
        RDG::TextureRef                depthRef =
             sceneDepth ? textures.Import( sceneDepth, "SceneDepth.Compute" ) : RDG::TextureRef{};
        const bool hasDepth = depthRef.IsValid();
        if ( !hasDepth )
            depthRef = textures.Import(
                 FallbackTextures::Get().GetFallbackTexture2D( Core::Formats::ImageFormat::RGBA8F ),
                 "Particles.NoSceneDepth" );
        particles->ImportFrameBuffers( graph, depthRef, hasDepth );
        // The PreOpaque emitters (UE ENiagaraGpuComputeTickStage::PreInitViews): their opaque rows draw this
        // frame's particles in the G-buffer, the base pass and the shadow views.
        if ( particles->ClaimsSimulation() )
            AddParticleSimulationSteps( graph, *particles, VFX::VFXTickStage::PreOpaque );
        // VFX-RENDER: the mesh rows' indexed-indirect commands of every node before the PostOpaque chain.
        particles->AddMeshDrawArgsPasses( graph, VFX::VFXTickStage::PreOpaque );
    }

    void SceneRenderer::AddFrameParticlesPostOpaque( RDG::Builder& graph )
    {
        // The PostOpaque emitters (UE ENiagaraGpuComputeTickStage::PostOpaqueRender): a stack that reads the scene
        // (VFX::kVFXSceneReadHostFunctions, engine:Collision) simulates here, after every opaque pass and the
        // depth resolve, against THIS frame's depth; then the sorts and the blended mesh commands ParticlePass
        // reads. Their opaque rows were drawn above from the previous tick (ParticleRenderer::DrawSource).
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* particles = UNIQUE_GET_AS( System::ParticleRenderer, m_RenderSystems["ParticleSystem"] );
        if ( particles == nullptr )
            return;
        if ( particles->ClaimsSimulation() )
            AddParticleSimulationSteps( graph, *particles, VFX::VFXTickStage::PostOpaque );
        // VFX-08: every view sorts its own translucent / additive emitters back to front (the key is its view
        // depth); after both chains' last Compact and before ParticlePass.
        particles->AddSortPasses( graph );
        particles->AddMeshDrawArgsPasses( graph, VFX::VFXTickStage::PostOpaque );
    }

    static void AddParticleSimulationSteps( RDG::Builder& graph, System::ParticleRenderer& renderer,
                                            const VFX::VFXTickStage stage )
    {
        System::ParticleRenderer* particles = &renderer;
        const uint32_t            steps     = particles->SimulationStepCount( stage );
        const char* const         phase     = stage == VFX::VFXTickStage::PreOpaque ? "PreOpaque" : "PostOpaque";
        graph.AddPass(
             std::format( "Particles {}: Compact 0", phase ), RDG::PassFlags::Compute,
             [particles, stage]( RDG::PassBuilder& pass ) { particles->DeclareCompactBindings( pass, 0, stage ); },
             [particles, stage]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return particles->Compact( context, 0, stage ); } );
        for ( uint32_t step = 0; step < steps; ++step )
        {
            graph.AddPass(
                 std::format( "Particles {}: Dispatch Args {}", phase, step ), RDG::PassFlags::Compute,
                 [particles, step, stage]( RDG::PassBuilder& pass )
                 { particles->DeclareDispatchArgsBindings( pass, step, stage ); },
                 [particles, step, stage]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->BuildDispatchArgs( context, step, stage ); } );
            graph.AddPass(
                 std::format( "Particles {}: Spawn+Update {}", phase, step ), RDG::PassFlags::Compute,
                 [particles, step, stage]( RDG::PassBuilder& pass )
                 { particles->DeclareSimulateBindings( pass, step, stage ); },
                 [particles, step, stage]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->Simulate( context, step, stage ); } );
            graph.AddPass(
                 std::format( "Particles {}: Compact {}", phase, step + 1 ), RDG::PassFlags::Compute,
                 [particles, step, stage]( RDG::PassBuilder& pass )
                 { particles->DeclareCompactBindings( pass, step + 1, stage ); },
                 [particles, step, stage]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->Compact( context, step + 1, stage ); } );
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

#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
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
        // VFX-09: Mesh renderer rows as GPU instances of the mesh renderer (lit by the GBuffer, cast into the
        // cascades) - submitted here, before the cascades and the base pass build their draws.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        if ( auto* meshes = UNIQUE_GET_AS( System::MeshRenderer, m_RenderSystems["MeshSystem"] ) )
            particles->AddMeshInstances( graph, *meshes );
    }

    static void AddParticleSimulationSteps( RDG::Builder& graph, System::ParticleRenderer& renderer )
    {
        // ASYNC COMPUTE (UE Niagara GPU simulation on the async pipe): every node reads and writes the particle pool and
        // the emitters' host-written step tables only; the first graphics reader is the particle draw (translucency)
        // or, for Mesh rows, the cascades. The sort and mesh-instance nodes after them run async as well, so the run
        // is not joined by its own next node. Fork: the pool imports' first touch; join: that first reader.
        const RDG::PassFlags kSimulationFlags = RDG::PassFlags::Compute | RDG::PassFlags::AsyncCompute;
        System::ParticleRenderer* particles = &renderer;
        const uint32_t            steps     = particles->SimulationStepCount();
        graph.AddPass(
             "Particles: Compact 0", kSimulationFlags,
             [particles]( RDG::PassBuilder& pass ) { particles->DeclareCompactBindings( pass, 0 ); },
             [particles]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return particles->Compact( context, 0 ); } );
        for ( uint32_t step = 0; step < steps; ++step )
        {
            graph.AddPass(
                 std::format( "Particles: Dispatch Args {}", step ), kSimulationFlags,
                 [particles, step]( RDG::PassBuilder& pass )
                 { particles->DeclareDispatchArgsBindings( pass, step ); },
                 [particles, step]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->BuildDispatchArgs( context, step ); } );
            graph.AddPass(
                 std::format( "Particles: Spawn+Update {}", step ), kSimulationFlags,
                 [particles, step]( RDG::PassBuilder& pass ) { particles->DeclareSimulateBindings( pass, step ); },
                 [particles, step]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->Simulate( context, step ); } );
            graph.AddPass(
                 std::format( "Particles: Compact {}", step + 1 ), kSimulationFlags,
                 [particles, step]( RDG::PassBuilder& pass )
                 { particles->DeclareCompactBindings( pass, step + 1 ); },
                 [particles, step]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->Compact( context, step + 1 ); } );
        }
        // VFX-16 (UE Niagara Debugger, dev only): the GPU counts and one attribute page, after the last compact (the
        // graph orders it from the buffers), only while an editor debugger asks.
        if ( particles->DebugReadbackRequested() )
            graph.AddPass(
                 "Particles: Debug Readback", kSimulationFlags,
                 [particles]( RDG::PassBuilder& pass ) { particles->DeclareDebugReadbackBindings( pass ); },
                 [particles]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->DebugReadback( context ); } );
        // VFX-LIGHT (UE Niagara Light Renderer): the Light rows' particles, after the last compact, for the
        // scene's particle point lights FramesInFlight frames later (ParticleWorldGpu::ReadLightReadbacks).
        if ( particles->LightReadbackRequested() )
            graph.AddPass(
                 "Particles: Light Readback", kSimulationFlags,
                 [particles]( RDG::PassBuilder& pass ) { particles->DeclareLightReadbackBindings( pass ); },
                 [particles]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->LightReadback( context ); } );
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
        std::vector<ComputeNodeDeclaration> nodes = clouds->DeclareShadowMapNodes( graph );
        // ASYNC COMPUTE (UE r.VolumetricCloud.ShadowMap on the async pipe): every input is the cloud field (host
        // payloads, the uploads just below) and its first reader is a lit pass after the shadow depths, the light
        // grids and the G-buffer, so the march overlaps them. Fork: after the input uploads; join: the first lit
        // reader of Clouds.ShadowMap.
        for ( ComputeNodeDeclaration& node : nodes )
            node.AsyncCompute = true;
        clouds->AddInputUploads( graph, textures ); // what the declaration just baked, before its readers
        clouds->SettleShadowMapNodes( AddComputeNodes( graph, textures, std::move( nodes ) ) );
        // A transient of this graph; its readers take the ref through SceneViewInputsOf (CloudShadowMapOrWhite).
        textures.Transients.CloudShadowMap = clouds->GetShadowMap();
    }

    void SceneRenderer::AddFrameCloudSkyOcclusion( RDG::Builder& graph, FrameTextures& textures )
    {
        // A path-traced frame draws no screen clouds, but the layer's sky-occlusion volume is an input of the sky
        // capture that marches the clouds into the environment the paths read (PathTracerSky "Prepare"): without
        // it the capture waits on an undecided volume and the path-traced sky has no clouds.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* clouds = UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] );
        if ( clouds == nullptr )
            return;
        std::vector<ComputeNodeDeclaration> nodes = clouds->DeclareSkyOcclusionNodes();
        clouds->AddInputUploads( graph, textures ); // what the declaration just baked, before its readers
        clouds->SettleSkyOcclusionNodes( AddComputeNodes( graph, textures, std::move( nodes ) ) );
    }

    void SceneRenderer::AddFrameSkyLight( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* sky = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] );
        if ( sky == nullptr )
            return;
        // The cloud inputs the environment bake made this update (OnUpdate, before this graph) reach their images
        // here, ahead of the capture that marches them.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        if ( auto* clouds =
                  UNIQUE_GET_AS( System::VolumetricCloudRenderer, m_RenderSystems["VolumetricCloudSystem"] ) )
            clouds->AddInputUploads( graph, textures );
        sky->SettleAtmosphereCachedLutNodes(
             AddComputeNodes( graph, textures, sky->DeclareAtmosphereCachedLutNodes() ) );
        sky->AddSkyCapturePasses( graph, textures );
    }

    void SceneRenderer::AddFrameSkyAtmosphereLuts( RDG::Builder& graph, FrameTextures& textures )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key names this exact type
        auto* sky = UNIQUE_GET_AS( System::SkyboxRenderer, m_RenderSystems["SkyboxSystem"] );
        if ( sky != nullptr )
            sky->SettleAtmosphereLutNodes( AddComputeNodes( graph, textures, sky->DeclareAtmosphereLutNodes() ) );
    }

    void SceneRenderer::AddFrameVolumetricFog( RDG::Builder& graph, FrameTextures& textures,
                                               const ViewFrame& frame )
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
        // The froxels read the local lights' shadows by the opaque lighting's rule (atlas, or virtual maps).
        AddComputeNodes( graph, textures,
                         volume->DeclareFrameNodes( textures.GraphRefs(), frame, sun, LocalShadowsAreVirtual() ) );
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

        // What every translucent surface after this point fogs itself with, at its own depth
        // (Common/ViewFog.glslh, bound by name through SceneViewInputs): DeclareFrameNodes published the parameter
        // block (always - gates 0 without fog); here the three images it reads, each the engine's fallback when
        // absent (the block's gates say it is not sampled), so a translucent cell never has an unbound slot.
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
        VolumetricCloudHistory&             history = m_ViewState.Clouds();
        std::vector<ComputeNodeDeclaration> nodes   = clouds->DeclareFrameNodes( graph, frame, history );
        clouds->AddInputUploads( graph, textures ); // what the declaration just baked, before its readers
        // The resolve is the last node: its AddPass index is the writer whose loss the view's EndFrame checks.
        const uint32_t resolvePass = graph.GetPassCount() + static_cast<uint32_t>( nodes.size() ) - 1u;
        const bool     hasNodes    = !nodes.empty();
        const bool     accepted    = AddComputeNodes( graph, textures, std::move( nodes ) );
        clouds->SettleFrameNodes( accepted );
        // The composite (a translucency pass, added after this by AddFrameTranslucency) samples this frame's pair
        // by graph ref: transients the view extracts as its history (SceneViewState::Clouds).
        const System::VolumetricCloudRenderer::FrameResult result = clouds->GetFrameResult();
        if ( accepted && hasNodes && result.Scatter.IsValid() )
            history.SetWriterPass( resolvePass );
        textures.Transients.CloudScatter = result.Scatter;
        textures.Transients.CloudGuide   = result.Guide;
    }
} // namespace Desert::Graphic

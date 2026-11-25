#include <VFX/Public/ParticleRenderer.hpp>

#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <VFX/Public/VFXStackCompiler.hpp>
#include <VFX/Public/VFXWorld.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>

// THE PARTICLES' SIDE OF THE FRAME (UE FFXSystemInterface as Niagara implements it): what the renderer calls at
// BeginScene (PrepareView), before the base pass (AddPreOpaque) and after the scene depth resolve (AddPostOpaque).

namespace Desert::Graphic::System
{
    namespace
    {
        // The claiming view's simulation nodes of one tick stage's chain (Open, Compact 0, then per step Dispatch
        // Args / Spawn+Update / Compact), every node with @p flags.
        void AddParticleSimulationSteps( RDG::Builder& graph, ParticleRenderer& renderer,
                                         const VFX::VFXTickStage stage, const RDG::PassFlags flags )
        {
            // The pool and the emitters' host-written step tables only; the first graphics reader is the particle
            // draw, which the graph orders after the last compact from the buffers.
            ParticleRenderer*      particles = &renderer;
            const uint32_t         steps     = particles->SimulationStepCount( stage );
            const std::string_view name      = stage == VFX::VFXTickStage::PreOpaque ? "PreOpaque" : "PostOpaque";
            graph.AddPass(
                 std::format( "Particles {}: Open", name ), flags,
                 [particles, stage]( RDG::PassBuilder& pass ) { particles->DeclareOpenBindings( pass, stage ); },
                 [particles, stage]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->OpenDrawSlots( context, stage ); } );
            graph.AddPass(
                 std::format( "Particles {}: Compact 0", name ), flags,
                 [particles, stage]( RDG::PassBuilder& pass )
                 { particles->DeclareCompactBindings( pass, 0, stage ); },
                 [particles, stage]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return particles->Compact( context, 0, stage ); } );
            for ( uint32_t step = 0; step < steps; ++step )
            {
                graph.AddPass(
                     std::format( "Particles {}: Dispatch Args {}", name, step ), flags,
                     [particles, step, stage]( RDG::PassBuilder& pass )
                     { particles->DeclareDispatchArgsBindings( pass, step, stage ); },
                     [particles, step, stage]( RDG::PassContext& context ) -> Common::BoolResultStr
                     { return particles->BuildDispatchArgs( context, step, stage ); } );
                graph.AddPass(
                     std::format( "Particles {}: Spawn+Update {}", name, step ), flags,
                     [particles, step, stage]( RDG::PassBuilder& pass )
                     { particles->DeclareSimulateBindings( pass, step, stage ); },
                     [particles, step, stage]( RDG::PassContext& context ) -> Common::BoolResultStr
                     { return particles->Simulate( context, step, stage ); } );
                graph.AddPass(
                     std::format( "Particles {}: Compact {}", name, step + 1 ), flags,
                     [particles, step, stage]( RDG::PassBuilder& pass )
                     { particles->DeclareCompactBindings( pass, step + 1, stage ); },
                     [particles, step, stage]( RDG::PassContext& context ) -> Common::BoolResultStr
                     { return particles->Compact( context, step + 1, stage ); } );
            }
        }

        // The claiming view's Debug / Light readbacks of the finished tick: after BOTH chains' last compacts (the
        // graph orders them from the buffers), on the graphics queue.
        void AddParticleReadbacks( RDG::Builder& graph, ParticleRenderer& renderer )
        {
            const RDG::PassFlags kSimulationFlags = RDG::PassFlags::Compute;
            ParticleRenderer*    particles        = &renderer;
            // VFX-16 (UE Niagara Debugger, dev only): the GPU counts and one attribute page, only while an editor
            // debugger asks.
            if ( particles->DebugReadbackRequested() )
                graph.AddPass(
                     "Particles: Debug Readback", kSimulationFlags,
                     [particles]( RDG::PassBuilder& pass ) { particles->DeclareDebugReadbackBindings( pass ); },
                     [particles]( RDG::PassContext& context ) -> Common::BoolResultStr
                     { return particles->DebugReadback( context ); } );
            // VFX-LIGHT (UE Niagara Light Renderer): the Light rows' particles, for the scene's particle point
            // lights FramesInFlight frames later (ParticleWorldGpu::ReadLightReadbacks).
            if ( particles->LightReadbackRequested() )
                graph.AddPass(
                     "Particles: Light Readback", kSimulationFlags,
                     [particles]( RDG::PassBuilder& pass ) { particles->DeclareLightReadbackBindings( pass ); },
                     [particles]( RDG::PassContext& context ) -> Common::BoolResultStr
                     { return particles->LightReadback( context ); } );
        }
    } // namespace

    std::span<const ShaderProtocols::PointLightPayload>
    ParticleRenderer::PrepareView( const Core::Scene& scene, const Core::Camera* camera,
                                   const Common::Scalability::ResolvedQuality& quality )
    {
        using Common::Scalability::Parameter;
        // VFX-11: this view's origin and Effects particle rows go to the world's next scalability decision
        // (VFXWorld::Tick) - the world decides once for all views, not here.
        if ( camera != nullptr )
        {
            VFX::VFXViewReport report{
                 .Origin          = glm::vec3( camera->GetPosition() ),
                 .SpawnCountScale = quality.As<double>( Parameter::EffectsSpawnCountScalePercent ) / 100.0,
                 .EffectsLevel    = quality.As<int>( Parameter::EffectsQualityLevel ),
                 .MaxWorldParticles =
                      static_cast<std::uint32_t>( quality.As<int>( Parameter::EffectsMaxWorldParticles ) ) };
            // The view's frustum (Gribb-Hartmann on clip = P * V, depth 0..1): six inward planes, normalized; a
            // degenerate plane (an infinite far plane) culls nothing.
            const glm::mat4 clip =
                 glm::mat4( camera->GetProjectionMatrix() ) * glm::mat4( camera->GetViewMatrix() );
            const auto row = [&clip]( const int r )
            { return glm::vec4( clip[0][r], clip[1][r], clip[2][r], clip[3][r] ); };
            const std::array<glm::vec4, 6> planes = {
                 row( 3 ) + row( 0 ), row( 3 ) - row( 0 ), row( 3 ) + row( 1 ), row( 3 ) - row( 1 ), row( 2 ),
                 row( 3 ) - row( 2 ) };
            for ( std::size_t i = 0; i < planes.size(); ++i )
            {
                const float length = glm::length( glm::vec3( planes[i] ) );
                report.FrustumPlanes[i] =
                     length > 1.0e-6f ? planes[i] / length : glm::vec4( 0.0f, 0.0f, 0.0f, 1.0f );
            }
            report.HasFrustum = true;
            VFX::VFXWorld::Of( scene ).SubmitView( report );
        }
        PrepareFrame( scene );
        return ParticleWorldGpu::Of( VFX::VFXWorld::Of( scene ) ).ParticleLights();
    }

    void ParticleRenderer::AddPreOpaque( RDG::Builder& graph, MeshRenderer* meshes )
    {
        // The world's particle pool (VFX-07): the frame's fixed VFX steps (the scene's VFXWorld; no timestep is
        // read here) as Compute nodes outside any render pass, BEFORE the billboard draw. Every node declares its
        // buffers per emitter (Declare*Bindings), so the graph orders them and places the barriers; with no
        // emitter a node declares nothing and the graph culls it.
        // VFX-07b: the pool is the scene's (ParticleWorldGpu). Every view imports it for its draw; only the view
        // that claimed the VFXWorld tick adds the simulation, so two views of one scene simulate once and draw
        // twice.
        ImportFrameBuffers( graph );
        // PORT-VFX-MODULES (UE Niagara tick stages): the PreOpaque chain here, before the base pass, on the async
        // pipe; the PostOpaque chain (a stack reading the scene depth) in AddPostOpaque.
        if ( ClaimsSimulation() && SimulatesStage( VFX::VFXTickStage::PreOpaque ) )
            AddParticleSimulationSteps( graph, *this, VFX::VFXTickStage::PreOpaque,
                                        RDG::PassFlags::Compute | RDG::PassFlags::AsyncCompute );
        // VFX-08: every view sorts its own translucent / additive emitters back to front (DrawSortPhase).
        AddSortPasses( graph, VFX::VFXTickStage::PreOpaque );
        // VFX-09: Mesh renderer rows as GPU instances of the mesh renderer (lit by the GBuffer, cast into the
        // cascades) - submitted here, before the cascades and the base pass build their draws.
        if ( meshes != nullptr )
            AddMeshInstances( graph, *meshes );
    }

    void ParticleRenderer::AddPostOpaque( RDG::Builder& graph, const RDG::TextureRef sceneDepth )
    {
        // UE Niagara PostRenderOpaque: the emitters whose stack reads the scene (depth collision) simulate after
        // the opaque scene's depth is resolved, then this view's sort of their forward draws (current half).
        if ( ClaimsSimulation() )
        {
            // False (logged inside): the PostOpaque emitters sit this frame out, their draws skip (HasDrawnHalf).
            (void)ImportCollisionInputs( graph, sceneDepth );
            if ( SimulatesStage( VFX::VFXTickStage::PostOpaque ) )
                AddParticleSimulationSteps( graph, *this, VFX::VFXTickStage::PostOpaque, RDG::PassFlags::Compute );
        }
        AddSortPasses( graph, VFX::VFXTickStage::PostOpaque );
        if ( ClaimsSimulation() )
            AddParticleReadbacks( graph, *this );
    }
} // namespace Desert::Graphic::System

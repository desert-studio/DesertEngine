#include "ParticleRenderer.hpp"

#include "ParticleGpuLayout.hpp"

#include <Engine/Graphic/Materials/Particles/MaterialParticleBillboard.hpp>
#include <Engine/Graphic/RenderPhase.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdint>

namespace Desert::Graphic::System
{
    ParticleRenderer::~ParticleRenderer() = default;

    Common::BoolResultStr ParticleRenderer::Initialize()
    {
        if ( !CreatePipelines() )
            return Common::MakeError( "ParticleRenderer: failed to create pipelines (missing shaders?)" );
        return BOOLSUCCESS;
    }

    bool ParticleRenderer::CreatePipelines()
    {
        auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return false;

        auto simShader  = shaderService->GetByName( "ParticleSimulate" );
        auto billShader = shaderService->GetByName( "ParticleBillboard" );
        if ( !simShader || !billShader )
        {
            LOG_ERROR( "ParticleRenderer: missing Particle shaders" );
            return false;
        }

        const auto sim = ComputePipeline::Create( { .Shader = simShader, .DebugName = "ParticleSimulate" } );
        if ( !sim )
        {
            LOG_ERROR( "ParticleRenderer: {}", sim.GetError() );
            return false;
        }
        m_SimPipeline = sim.GetValue();

        const auto& target = m_TargetFramebuffer.lock();
        if ( !target )
            return false;

        GraphicsPipelineSpecification base;
        base.Shader      = billShader;
        base.Framebuffer = target;
        // Additive FX read as "always visible": depth-testing billboards against the scene made them vanish
        // when the camera looked DOWN at particles sitting near a surface (the surface occluded them), while
        // they showed when looking up (nothing behind). Draw them without a depth test (never write depth
        // either), in the Transparency phase — the common default for glow/fire/sparks. (A per-emitter
        // "occlude" toggle can bring depth testing back for smoke/dust that should hide behind walls.)
        base.DepthTestEnabled  = false;
        base.DepthWriteEnabled = false;
        base.CullMode          = CullMode::None;
        base.Topology          = PrimitiveTopology::Triangles;
        base.BlendEnable       = true;

        GraphicsPipelineSpecification addSpec = base;
        addSpec.DebugName                     = "ParticleAdd";
        addSpec.SrcColorBlendFactor           = BlendFactor::SrcAlpha;
        addSpec.DstColorBlendFactor           = BlendFactor::One; // additive glow
        const auto add                        = GraphicsPipeline::Create( addSpec );
        if ( !add )
        {
            LOG_ERROR( "ParticleRenderer: {}", add.GetError() );
            return false;
        }
        m_AddPipeline = add.GetValue();

        GraphicsPipelineSpecification alphaSpec = base;
        alphaSpec.DebugName                     = "ParticleAlpha";
        alphaSpec.SrcColorBlendFactor           = BlendFactor::SrcAlpha;
        alphaSpec.DstColorBlendFactor           = BlendFactor::OneMinusSrcAlpha; // soft over
        const auto alpha                        = GraphicsPipeline::Create( alphaSpec );
        if ( !alpha )
        {
            LOG_ERROR( "ParticleRenderer: {}", alpha.GetError() );
            return false;
        }
        m_AlphaPipeline = alpha.GetValue();

        // No shared billboard material here — each emitter owns one (created in GetOrCreate). The
        // particle SSBO is a descriptor, and one material can hold exactly one per frame.

        // `return m_SimPipeline && m_AddPipeline && m_AlphaPipeline;` stood here and could not be false:
        // every one of the three was assigned from a factory that could not fail. Each is refused above,
        // at the point that knows which one it was.
        return true;
    }

    ParticleRenderer::EmitterGpu& ParticleRenderer::GetOrCreate( uint32_t entityId, int maxParticles )
    {
        auto&     e   = m_Emitters[entityId];
        const int cap = std::max( 1, maxParticles );
        if ( e.MaxParticles != cap || !e.Particles )
        {
            e.MaxParticles = cap;
            // Binding 1: the graphics descriptor write uses the buffer's OWN binding (VulkanMaterialBackend),
            // and the billboard shader reads it at ReadBuffer(1). The compute pass binds it at 0 via the
            // explicit SetStorageBuffer(0, ...) arg (compute uses the arg, not the buffer's binding), matching
            // ParticleSimulate's Buffer(0). Mismatching this (buffer binding 0) aliased the camera UB at
            // binding 0 -> VUID-VkWriteDescriptorSet-descriptorType-00319.
            e.Particles = ShaderResources::StorageBuffer::Create(
                 "ParticleState", static_cast<uint32_t>( cap ) * kParticleStride, 1, /*persistent=*/true );
            e.Counter    = ShaderResources::StorageBuffer::Create( "ParticleSpawn", sizeof( uint32_t ), 1 );
            e.SpawnAccum = 0.0f;

            // THIS emitter's material, holding THIS emitter's buffer in its descriptors. Created with
            // the buffer (and kept across a capacity change — Update rebinds the new buffer) so the
            // draw pass never routes two emitters through one descriptor set; see EmitterGpu::Material.
            if ( !e.Material )
                e.Material = std::make_unique<MaterialParticleBillboard>();

            // All particles start dead (VelLife.w = 0, Color.a = 0): a zeroed buffer, so compute respawns them.
            //
            // AND IF IT DOES NOT ZERO, THE EMITTER MUST NOT RUN. This is the one buffer in the engine
            // created `persistent = true`: nothing rewrites it per frame, the compute pass reads what is
            // there and writes back. So an initialisation that silently did nothing does not produce a
            // stale frame, it produces a simulation seeded from whatever VMA handed back — particles with
            // NaN lifetimes and positions, for as long as the emitter exists. Dropping the buffer makes
            // GetOrCreate try again next frame instead of running on garbage.
            std::vector<uint8_t> zeros( static_cast<size_t>( cap ) * kParticleStride, 0 );
            const auto cleared = e.Particles->SetData( zeros.data(), static_cast<uint32_t>( zeros.size() ) );
            if ( !cleared.IsSuccess() )
            {
                LOG_ERROR( "[Particles] emitter {} could not be initialised, so it does not run: {}", entityId,
                           cleared.GetError() );
                e.Particles = nullptr;
            }
        }
        return e;
    }

    void ParticleRenderer::OnSceneReplaced()
    {
        if ( m_Emitters.empty() )
            return;

        // m_FrameEmitters holds raw pointers INTO m_Emitters, so it goes first. Nothing will consume it
        // before the next PrepareFrame refills it — SimulateInFrame and the draw pass both run later in a
        // frame than this, and this runs between frames.
        m_FrameEmitters.clear();

        LOG_INFO( "[Particles] Released {} cached emitter(s) belonging to the previous scene.",
                  m_Emitters.size() );
        m_Emitters.clear();
    }

    void ParticleRenderer::PrepareFrame( const ::Desert::Core::Scene& scene )
    {
        m_FrameEmitters.clear();

        auto& reg  = const_cast<entt::registry&>( scene.GetRegistry() );
        auto  view = reg.view<ECS::ParticleEmitterComponent, ECS::TransformComponent>();
        view.each(
             [&]( entt::entity entity, ECS::ParticleEmitterComponent& emitter, ECS::TransformComponent& transform )
             {
                 const auto& d = emitter.Data;

                 // One-shot Restart from the editor transport. Handled BEFORE the enabled check so a
                 // paused emitter also comes back empty, and by ZEROING the state rather than dropping
                 // the buffer — the GPU may still be reading it this frame.
                 if ( emitter.RequestRestart )
                 {
                     emitter.RequestRestart = false;
                     const auto it          = m_Emitters.find( static_cast<uint32_t>( entity ) );
                     if ( it != m_Emitters.end() && it->second.Particles )
                     {
                         // A Restart that wrote nothing left the emitter running with the OLD state
                         // while the editor's transport reported it restarted. The report is the log
                         // here: the transport flag has already been consumed and there is no second
                         // place to put a failure, but "the button did nothing" must at least be
                         // findable.
                         const auto restarted = ClearEmitterState( it->second );
                         if ( !restarted.IsSuccess() )
                             LOG_ERROR( "[Particles] Restart on emitter {} did not clear the state: {}",
                                        static_cast<uint32_t>( entity ), restarted.GetError() );
                     }
                 }

                 if ( !d.Enabled || d.MaxParticles <= 0 )
                     return;

                 EmitterGpu& gpu = GetOrCreate( static_cast<uint32_t>( entity ), d.MaxParticles );

                 // GetOrCreate refuses by leaving the state buffer null when it could not be zeroed
                 // (see there). It has already said why; this emitter simply does not take part in the
                 // frame, and the next frame tries to create it again.
                 if ( !gpu.Particles || !gpu.Counter )
                     return;

                 // Zero the spawn counter for this frame. A counter that did not reset holds LAST
                 // frame's atomic total, so the compute pass would spawn from an index past the end of
                 // the live range — the emitter must sit this frame out rather than simulate from it.
                 const uint32_t zero    = 0;
                 const auto     counter = gpu.Counter->SetData( &zero, sizeof( zero ) );
                 if ( !counter.IsSuccess() )
                 {
                     LOG_ERROR( "[Particles] emitter {} sits out this frame, its spawn counter did not "
                                "reset: {}",
                                static_cast<uint32_t>( entity ), counter.GetError() );
                     return;
                 }

                 const glm::vec3 worldPos = glm::vec3( transform.GetTransform()[3] );
                 glm::vec3       dir      = d.Direction;
                 if ( glm::dot( dir, dir ) < 1e-6f )
                     dir = glm::vec3( 0.0f, 1.0f, 0.0f );
                 dir = glm::normalize( dir );

                 FrameEmitter fe;
                 fe.Gpu             = &gpu;
                 fe.Additive        = ( d.Blend == ECS::ParticleBlendMode::Additive );
                 fe.SpawnRate       = d.SpawnRate;
                 fe.Looping         = d.Looping;
                 // .w of both (dt, simulated time) and the spawn budget are the frame's time: SimulateInFrame.
                 fe.Push.EmitterPos = glm::vec4( worldPos, 0.0f );
                 fe.Push.Gravity    = glm::vec4( d.Gravity, 0.0f );
                 fe.Push.Direction  = glm::vec4( dir, glm::radians( d.ConeAngle ) );
                 fe.Push.Params     = glm::vec4( d.StartSpeed, d.SpeedVariance, d.Lifetime, d.LifetimeVariance );
                 fe.Push.StartColor = glm::vec4( d.StartColor, d.StartAlpha );
                 fe.Push.EndColor   = glm::vec4( d.EndColor, d.EndAlpha );
                 fe.Push.Sizes      = glm::vec4( d.StartSize, d.EndSize, d.SizeCurvePower, 0.0f );
                 // Counts.w = local-space simulation (WorldSpace off): the sim keeps each particle's
                 // offset FROM the emitter and rebases it on the current emitter position every frame, so
                 // the whole system rides a moving emitter instead of trailing behind it. The particle
                 // buffer still holds world positions either way — the billboard pass needs no per-emitter
                 // uniform and does not change. (Only the TRANSLATION rides; the emitter's rotation is not
                 // applied to the cloud.)
                 fe.Push.Counts =
                      glm::uvec4( static_cast<uint32_t>( gpu.MaxParticles ), 0u, 1u, d.WorldSpace ? 0u : 1u );

                 m_FrameEmitters.push_back( fe );
             } );
    }

    Common::BoolResultStr ParticleRenderer::ClearEmitterState( EmitterGpu& gpu )
    {
        gpu.SpawnAccum = 0.0f;
        const std::vector<uint8_t> zeros( static_cast<size_t>( gpu.MaxParticles ) * kParticleStride, 0 );
        auto written = gpu.Particles->SetData( zeros.data(), static_cast<uint32_t>( zeros.size() ) );
        return written;
    }

    void ParticleRenderer::OnTemporalHistoryReset()
    {
        m_SimSeconds = 0.0;
        for ( auto& [entityId, gpu] : m_Emitters )
        {
            if ( !gpu.Particles )
                continue;
            // An emitter that kept its state carries the frames before the cut into the ones after it -
            // exactly what the caller asked to be rid of - so the failure is reported, not swallowed.
            if ( const auto cleared = ClearEmitterState( gpu ); !cleared.IsSuccess() )
                LOG_ERROR( "[Particles] temporal reset: emitter {} kept its state: {}", entityId,
                           cleared.GetError() );
        }
    }

    void ParticleRenderer::SimulateInFrame( const float frameSeconds )
    {
        // The frame's step, clamped so a stall (a hitch, a breakpoint) is not one huge integration step.
        // Advanced even on a frame with nothing to simulate, so the seed stays the simulated time and not
        // "the time spent with emitters present".
        const float dt = std::clamp( frameSeconds, 0.0f, 0.1f );
        m_SimSeconds += dt;
        const float time = static_cast<float>( m_SimSeconds );

        if ( !m_SimPipeline || m_FrameEmitters.empty() )
            return;

        for ( auto& fe : m_FrameEmitters )
        {
            // Spawn budget for this frame (fractional carry so low rates still emit).
            fe.Gpu->SpawnAccum += fe.SpawnRate * dt;
            uint32_t budget = static_cast<uint32_t>( fe.Gpu->SpawnAccum );
            fe.Gpu->SpawnAccum -= static_cast<float>( budget );
            if ( !fe.Looping )
                budget = 0; // one-shot bursts are a follow-up; looping emits continuously
            fe.Push.Counts.y     = budget;
            fe.Push.EmitterPos.w = dt;
            fe.Push.Gravity.w    = time;
        }

        auto& renderer = Renderer::GetInstance();
        for ( auto& fe : m_FrameEmitters )
        {
            m_SimPipeline->SetStorageBuffer( 0, fe.Gpu->Particles.get() );
            m_SimPipeline->SetStorageBuffer( 1, fe.Gpu->Counter.get() );
            m_SimPipeline->SetPushConstants( &fe.Push, sizeof( fe.Push ) );

            const uint32_t groups =
                 ( static_cast<uint32_t>( fe.Gpu->MaxParticles ) + kParticleLocalSize - 1 ) / kParticleLocalSize;
            // DispatchComputeCull (not InFrame): its barrier makes the writes visible to the VERTEX stage that
            // the billboard shader reads the particle buffer from.
            renderer.DispatchComputeCull( m_SimPipeline.get(), groups, 1, 1 );
        }
    }

    void ParticleRenderer::RegisterPasses( RenderGraphBuilder& builder )
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb || !m_AddPipeline )
            return;

        builder.AddPass(
             "ParticlePass", RenderPhase::Transparency,
             [this]()
             {
                 if ( m_FrameEmitters.empty() )
                     return;
                 const auto camera = m_SceneRenderer->GetMainCamera();
                 if ( !camera )
                     return;

                 auto& renderer = Renderer::GetInstance();
                 for ( auto& fe : m_FrameEmitters )
                 {
                     if ( !fe.Gpu || !fe.Gpu->Particles || !fe.Gpu->Material )
                         continue;
                     // Each emitter updates and draws ITS OWN material: a shared one here routed every
                     // emitter through one descriptor set, which is written at most once per frame — so
                     // every emitter after the first drew the first one's buffer.
                     fe.Gpu->Material->Update( camera, fe.Gpu->Particles );
                     auto* pipeline = fe.Additive ? m_AddPipeline.get() : m_AlphaPipeline.get();
                     renderer.SubmitVertices( pipeline, static_cast<uint32_t>( fe.Gpu->MaxParticles ) * 6u,
                                              fe.Gpu->Material->GetMaterialExecutor() );
                 }
             },
             m_AddPipeline->GetSpecification(), targetFb, { RenderPassDependency( RenderPhase::Geometry ) } );
    }
} // namespace Desert::Graphic::System

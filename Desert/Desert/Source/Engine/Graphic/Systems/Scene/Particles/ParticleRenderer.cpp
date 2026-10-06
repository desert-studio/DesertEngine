#include "ParticleRenderer.hpp"

#include "ParticleGpuLayout.hpp"

#include <Engine/Graphic/Materials/Particles/MaterialParticleBillboard.hpp>
#include <Engine/Graphic/RenderPhase.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/VFX/VFXWorld.hpp>

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

    ParticleRenderer::EmitterGpu& ParticleRenderer::GetOrCreate( uint32_t entityId, int maxParticles, uint32_t stepCapacity )
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
            e.Generation = 0; // freshly zeroed below: no generation to clear

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
        if ( e.StepCapacity != stepCapacity || !e.Steps )
        {
            // Per-frame-in-flight (not persistent): the CPU rewrites the table every frame. Binding 1 is
            // ParticleSimulate's Buffer(1); compute binds by the explicit SetStorageBuffer argument.
            e.StepCapacity = stepCapacity;
            e.Steps        = ShaderResources::StorageBuffer::Create(
                 "ParticleSteps", std::max( 1u, stepCapacity ) * kParticleStepStride, 1 );
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

        // The time, the steps and the randomness all come from the scene's VFXWorld, ticked once per
        // scene update; this view only turns them into dispatches. No clock is read here.
        const VFX::VFXWorld& world        = scene.GetVFXWorld();
        const auto&          clock        = world.GetClock().GetSettings();
        const float          stepSeconds  = static_cast<float>( clock.StepSeconds );
        const uint32_t       stepCapacity = std::max( clock.MaxStepsPerTick, clock.MaxSeekStepsPerTick );

        const auto& reg  = scene.GetRegistry();
        auto        view = reg.view<const ECS::ParticleEmitterComponent, const ECS::TransformComponent,
                                    const ECS::UUIDComponent>();
        view.each(
             [&]( entt::entity entity, const ECS::ParticleEmitterComponent& emitter,
                  const ECS::TransformComponent& transform, const ECS::UUIDComponent& id )
             {
                 const auto& d = emitter.Data;
                 if ( !d.Enabled || d.MaxParticles <= 0 )
                     return;

                 // An emitter added after this update's VFX tick joins on the next one.
                 const VFX::EmitterInstance* instance = world.FindEmitter( static_cast<uint64_t>( id.UUID ) );
                 if ( !instance )
                     return;

                 const auto  entityId = static_cast<uint32_t>( entity );
                 EmitterGpu& gpu      = GetOrCreate( entityId, d.MaxParticles, stepCapacity );

                 // GetOrCreate refuses by leaving a buffer null when it could not be created or zeroed
                 // (see there). It has already said why; this emitter sits the frame out and the next
                 // frame tries again.
                 if ( !gpu.Particles || !gpu.Steps )
                     return;

                 // The world threw this instance's state away (Restart, reset, backwards seek): zero it
                 // — every particle dead — by ZEROING rather than dropping the buffer, which the GPU may
                 // still be reading. Fresh state (generation 0) is already zero.
                 if ( gpu.Generation != instance->Generation )
                 {
                     if ( gpu.Generation != 0 )
                     {
                         const std::vector<uint8_t> zeros( static_cast<size_t>( gpu.MaxParticles ) *
                                                                kParticleStride,
                                                           0 );
                         const auto cleared =
                              gpu.Particles->SetData( zeros.data(), static_cast<uint32_t>( zeros.size() ) );
                         if ( !cleared.IsSuccess() )
                         {
                             // Running the new generation's steps on the old state would be a simulation
                             // that is neither: the emitter sits out until the state can be cleared.
                             LOG_ERROR( "[Particles] emitter {} sits out this frame, its state did not "
                                        "reset: {}",
                                        entityId, cleared.GetError() );
                             return;
                         }
                     }
                     gpu.Generation = instance->Generation;
                 }

                 // This frame's step table. A table that did not upload must not run: the steps would
                 // read last frame's counters and id bases. The emitter still draws its current state.
                 uint32_t stepCount =
                      std::min( static_cast<uint32_t>( instance->Steps.size() ), gpu.StepCapacity );
                 if ( stepCount > 0 )
                 {
                     std::vector<StepGpu> table( stepCount );
                     for ( uint32_t s = 0; s < stepCount; ++s )
                         table[s] = { 0u, instance->Steps[s].IdBase, instance->Seed, instance->Steps[s].Budget };
                     const auto uploaded =
                          gpu.Steps->SetData( table.data(), stepCount * static_cast<uint32_t>( sizeof( StepGpu ) ) );
                     if ( !uploaded.IsSuccess() )
                     {
                         LOG_ERROR( "[Particles] emitter {} does not simulate this frame, its step table did "
                                    "not upload: {}",
                                    entityId, uploaded.GetError() );
                         stepCount = 0;
                     }
                 }

                 const glm::vec3 worldPos = glm::vec3( transform.GetTransform()[3] );
                 glm::vec3       dir      = d.Direction;
                 if ( glm::dot( dir, dir ) < 1e-6f )
                     dir = glm::vec3( 0.0f, 1.0f, 0.0f );
                 dir = glm::normalize( dir );

                 FrameEmitter fe;
                 fe.Gpu             = &gpu;
                 fe.Additive        = ( d.Blend == ECS::ParticleBlendMode::Additive );
                 fe.StepCount       = stepCount;
                 fe.Push.EmitterPos = glm::vec4( worldPos, stepSeconds );
                 fe.Push.Gravity    = glm::vec4( d.Gravity, 0.0f );
                 fe.Push.Direction  = glm::vec4( dir, glm::radians( d.ConeAngle ) );
                 fe.Push.Params     = glm::vec4( d.StartSpeed, d.SpeedVariance, d.Lifetime, d.LifetimeVariance );
                 fe.Push.StartColor = glm::vec4( d.StartColor, d.StartAlpha );
                 fe.Push.EndColor   = glm::vec4( d.EndColor, d.EndAlpha );
                 fe.Push.Sizes      = glm::vec4( d.StartSize, d.EndSize, d.SizeCurvePower, 0.0f );
                 // Counts.w = local-space simulation (WorldSpace off): the sim keeps each particle's
                 // offset FROM the emitter and rebases it on the current emitter position every step, so
                 // the whole system rides a moving emitter instead of trailing behind it. (Only the
                 // TRANSLATION rides; the emitter's rotation is not applied to the cloud.) Counts.y, the
                 // step, is set per dispatch.
                 fe.Push.Counts = glm::uvec4( static_cast<uint32_t>( gpu.MaxParticles ), 0u, 0u, d.WorldSpace ? 0u : 1u );

                 m_FrameEmitters.push_back( fe );
             } );
    }

    void ParticleRenderer::SimulateInFrame()
    {
        if ( !m_SimPipeline || m_FrameEmitters.empty() )
            return;

        uint32_t maxSteps   = 0;
        uint32_t dispatches = 0;
        for ( const auto& fe : m_FrameEmitters )
        {
            maxSteps = std::max( maxSteps, fe.StepCount );
            dispatches += fe.StepCount;
        }

        // Steps in order, each step over every emitter: step s+1 reads what step s wrote, which the
        // compute-to-compute barrier of DispatchComputeInFrame makes visible. The LAST dispatch of the
        // frame goes through DispatchComputeCull instead, whose barrier makes every write so far visible
        // to the billboard VERTEX stage that reads the particle buffers.
        auto& renderer = Renderer::GetInstance();
        for ( uint32_t step = 0; step < maxSteps; ++step )
        {
            for ( auto& fe : m_FrameEmitters )
            {
                if ( step >= fe.StepCount )
                    continue;
                SimPush push  = fe.Push;
                push.Counts.y = step;
                m_SimPipeline->SetStorageBuffer( 0, fe.Gpu->Particles.get() );
                m_SimPipeline->SetStorageBuffer( 1, fe.Gpu->Steps.get() );
                m_SimPipeline->SetPushConstants( &push, sizeof( push ) );

                const uint32_t groups =
                     ( static_cast<uint32_t>( fe.Gpu->MaxParticles ) + kParticleLocalSize - 1 ) / kParticleLocalSize;
                if ( --dispatches == 0 )
                    renderer.DispatchComputeCull( m_SimPipeline.get(), groups, 1, 1 );
                else
                    renderer.DispatchComputeInFrame( m_SimPipeline.get(), groups, 1, 1 );
            }
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

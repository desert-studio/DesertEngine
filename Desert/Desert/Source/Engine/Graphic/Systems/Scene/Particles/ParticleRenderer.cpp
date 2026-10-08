#include "ParticleRenderer.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include "ParticleEmitterRetire.hpp"
#include "ParticleGpuLayout.hpp"

#include <Engine/Graphic/Materials/Particles/MaterialParticleBillboard.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/VFX/VFXWorld.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <tuple>

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

        auto compactShader = shaderService->GetByName( "ParticleCompact" );
        if ( !compactShader )
        {
            LOG_ERROR( "ParticleRenderer: missing ParticleCompact shader" );
            return false;
        }
        const auto compact =
             ComputePipeline::Create( { .Shader = compactShader, .DebugName = "ParticleCompact" } );
        if ( !compact )
        {
            LOG_ERROR( "ParticleRenderer: {}", compact.GetError() );
            return false;
        }
        m_CompactPipeline = compact.GetValue();

        const auto& target = m_TargetFramebuffer.lock();
        if ( !target )
            return false;

        GraphicsPipelineSpecification base;
        base.Shader      = billShader;
        base.TargetLayout = SceneTargetLayout();
        // Additive FX read as "always visible": depth-testing billboards against the scene made them vanish
        // when the camera looked DOWN at particles sitting near a surface (the surface occluded them), while
        // they showed when looking up (nothing behind). Draw them without a depth test (never write depth
        // either), in the translucency (AddFrameTranslucency) — the common default for glow/fire/sparks. (A
        // per-emitter "occlude" toggle can bring depth testing back for smoke/dust that should hide behind walls.)
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

    ParticleRenderer::EmitterGpu& ParticleRenderer::GetOrCreate( const uint32_t entityId,
                                                                 const uint32_t stepCapacity )
    {
        auto& e = m_Emitters[entityId];
        if ( !e.Material )
            e.Material = std::make_unique<MaterialParticleBillboard>();
        if ( !e.Counters )
            e.Counters = ShaderResources::StorageBuffer::Create( "ParticleCounters",
                                                                 kParticleDrawSlots * kParticleDrawSlotStride, 1 );
        if ( e.StepCapacity != stepCapacity || !e.Steps )
        {
            e.StepCapacity = stepCapacity;
            e.Steps        = ShaderResources::StorageBuffer::Create(
                 "ParticleSteps", std::max( 1u, stepCapacity ) * kParticleStepStride, 1 );
        }
        return e;
    }

    bool ParticleRenderer::EnsurePoolCapacity( const uint32_t particles )
    {
        if ( particles <= m_Pool.Capacity && m_Pool.Particles )
            return true;
        // Grown to at least double, so a scene adding emitters one by one recreates the pool a logarithmic number
        // of times. The contents are not carried over: every emitter restarts (compact 0 resets its range).
        const uint32_t capacity = std::max( particles, m_Pool.Capacity * 2u );
        m_Pool                  = {};
        m_Pool.Particles = ShaderResources::StorageBuffer::Create( "ParticlePool", capacity * kParticleStride, 1,
                                                                   /*persistent=*/true );
        m_Pool.FreeList  = ShaderResources::StorageBuffer::Create( "ParticleFreeList", capacity * 4u, 2, true );
        m_Pool.AliveList = ShaderResources::StorageBuffer::Create( "ParticleAliveList", capacity * 4u, 2, true );
        if ( !m_Pool.Particles || !m_Pool.FreeList || !m_Pool.AliveList )
        {
            LOG_ERROR( "[Particles] the world pool of {} particles could not be created: no emitter runs",
                       capacity );
            m_Pool = {};
            return false;
        }
        m_Pool.Capacity = capacity;
        for ( auto& [key, gpu] : m_Emitters )
            gpu.NeedsReset = true;
        return true;
    }

    void ParticleRenderer::OnSceneReplaced()
    {
        if ( m_Emitters.empty() )
            return;

        m_FrameEmitters.clear();

        LOG_INFO( "[Particles] Released {} cached emitter(s) belonging to the previous scene.",
                  m_Emitters.size() );
        m_Emitters.clear();
        m_Ranges.Clear(); // the pool keeps its capacity; the next scene's emitters reset their ranges
    }

    void ParticleRenderer::PrepareFrame( const ::Desert::Core::Scene& scene )
    {
        m_FrameEmitters.clear();

        const VFX::VFXWorld& world        = scene.GetVFXWorld();
        const auto&          clock        = world.GetClock().GetSettings();
        const auto           stepSeconds  = static_cast<float>( clock.StepSeconds );
        const uint32_t       stepCapacity = std::max( clock.MaxStepsPerTick, clock.MaxSeekStepsPerTick );

        const auto& reg = scene.GetRegistry();

        const std::size_t retired = RetireDestroyedEmitters( m_Emitters, reg );
        if ( retired > 0 )
            LOG_INFO( "[Particles] Released {} emitter(s) whose entity is gone.", retired );
        m_Ranges.ReleaseUnless( [this]( const uint32_t key ) { return m_Emitters.contains( key ); } );

        auto view = reg.view<const ECS::ParticleEmitterComponent, const ECS::TransformComponent,
                             const ECS::UUIDComponent>();
        view.each(
             [&]( entt::entity entity, const ECS::ParticleEmitterComponent& emitter,
                  const ECS::TransformComponent& transform, const ECS::UUIDComponent& id )
             {
                 const auto& d = emitter.Data;
                 if ( !d.Enabled || d.MaxParticles <= 0 )
                     return;

                 const VFX::EmitterInstance* instance = world.FindEmitter( static_cast<uint64_t>( id.UUID ) );
                 if ( instance == nullptr )
                     return;

                 const auto  entityId = static_cast<uint32_t>( entity );
                 EmitterGpu& gpu      = GetOrCreate( entityId, stepCapacity );
                 if ( !gpu.Steps || !gpu.Counters )
                     return;

                 // The emitter's range of the world pool; a new or moved range starts dead.
                 const ParticlePoolRange range =
                      m_Ranges.Acquire( entityId, static_cast<uint32_t>( d.MaxParticles ) );
                 if ( range.Base != gpu.Range.Base || range.Count != gpu.Range.Count )
                     gpu.NeedsReset = true;
                 gpu.Range = range;
                 // A state whose VFXWorld generation was moved past (seek, restart) restarts.
                 if ( gpu.Generation != instance->Generation )
                 {
                     gpu.NeedsReset = true;
                     gpu.Generation = instance->Generation;
                 }

                 uint32_t stepCount =
                      std::min( static_cast<uint32_t>( instance->Steps.size() ), gpu.StepCapacity );
                 if ( stepCount > 0 )
                 {
                     std::vector<StepGpu> table( stepCount );
                     for ( uint32_t s = 0; s < stepCount; ++s )
                         table[s] = { instance->Steps[s].IdBase, instance->Seed, instance->Steps[s].Budget };
                     const auto uploaded = gpu.Steps->SetData(
                          table.data(), stepCount * static_cast<uint32_t>( sizeof( StepGpu ) ) );
                     if ( !uploaded.IsSuccess() )
                     {
                         LOG_ERROR( "[Particles] emitter {} does not simulate this frame, its step table did "
                                    "not upload: {}",
                                    entityId, uploaded.GetError() );
                         stepCount = 0;
                     }
                 }

                 // Both draw slots start empty: compact 0 fills slot 0 from the pool, so the counters need no
                 // history (ParticleCompact).
                 DrawSlotGpu slots[kParticleDrawSlots];
                 for ( DrawSlotGpu& slot : slots )
                     slot.FirstVertex = range.Base * 6u;
                 const auto counted = gpu.Counters->SetData( slots, static_cast<uint32_t>( sizeof( slots ) ) );
                 if ( !counted.IsSuccess() )
                 {
                     LOG_ERROR( "[Particles] emitter {} sits out this frame, its counters did not upload: {}",
                                entityId, counted.GetError() );
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
                 fe.StepCount       = stepCount;
                 fe.Push.EmitterPos = glm::vec4( worldPos, stepSeconds );
                 fe.Push.Gravity    = glm::vec4( d.Gravity, 0.0f );
                 fe.Push.Direction  = glm::vec4( dir, glm::radians( d.ConeAngle ) );
                 fe.Push.Params     = glm::vec4( d.StartSpeed, d.SpeedVariance, d.Lifetime, d.LifetimeVariance );
                 fe.Push.StartColor = glm::vec4( d.StartColor, d.StartAlpha );
                 fe.Push.EndColor   = glm::vec4( d.EndColor, d.EndAlpha );
                 fe.Push.Sizes      = glm::vec4( d.StartSize, d.EndSize, d.SizeCurvePower, 0.0f );
                 fe.Push.Counts     = glm::uvec4( range.Count, 0u, range.Base, d.WorldSpace ? 0u : 1u );

                 m_FrameEmitters.push_back( fe );
             } );

        if ( !m_FrameEmitters.empty() && !EnsurePoolCapacity( m_Ranges.End() ) )
            m_FrameEmitters.clear();
    }

    uint32_t ParticleRenderer::SimulationStepCount() const
    {
        if ( !m_SimPipeline || !m_CompactPipeline )
            return 0; // the nodes dispatch nothing either
        uint32_t steps = 0;
        for ( const FrameEmitter& fe : m_FrameEmitters )
            if ( fe.Declared )
                steps = std::max( steps, fe.StepCount );
        return steps;
    }

    bool ParticleRenderer::RunsStep( const FrameEmitter& fe, const uint32_t step )
    {
        return fe.Declared && step < fe.StepCount;
    }

    bool ParticleRenderer::RunsCompact( const FrameEmitter& fe, const uint32_t compact )
    {
        return fe.Declared && compact <= fe.StepCount;
    }

    Common::BoolResultStr ParticleRenderer::Compact( const RDG::PassContext& context, const uint32_t compact )
    {
        if ( !m_CompactPipeline )
            return BOOLSUCCESS;

        auto&    renderer = Renderer::GetInstance();
        uint32_t block    = 0;
        for ( const FrameEmitter& fe : m_FrameEmitters )
        {
            if ( !RunsCompact( fe, compact ) )
                continue;
            const bool        reset = compact == 0 && fe.Gpu->NeedsReset;
            const CompactPush push{
                 glm::uvec4( fe.Gpu->Range.Base, fe.Gpu->Range.Count, compact & 1u, reset ? 1u : 0u ) };
            RDG::PassBindings bindings( context, context.GetBindingBlock( block++ ) );
            bindings.PushConstants( &push, sizeof( push ) );
            const uint32_t groups = ( fe.Gpu->Range.Count + kParticleLocalSize - 1 ) / kParticleLocalSize;
            const Common::BoolResultStr dispatched =
                 renderer.DispatchCompute( bindings, *m_CompactPipeline, groups, 1, 1 );
            if ( !dispatched )
                return dispatched;
            if ( reset )
                fe.Gpu->NeedsReset = false;
        }
        return BOOLSUCCESS;
    }

    void ParticleRenderer::DeclareCompactBindings( RDG::PassBuilder& pass, const uint32_t compact ) const
    {
        if ( !m_CompactPipeline )
            return; // Compact dispatches nothing either
        const auto& layout = m_CompactLayout.Get( m_CompactPipeline->GetSpecification().Shader );
        for ( const FrameEmitter& fe : m_FrameEmitters )
        {
            if ( !RunsCompact( fe, compact ) )
                continue; // Compact skips it the same way
            pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_CompactPipeline ) )
                 .Storage( "Particles", m_Pool.ParticlesRef, RDG::Access::StorageWrite )
                 .Storage( "FreeList", m_Pool.FreeRef, RDG::Access::StorageWrite )
                 .Storage( "AliveList", m_Pool.AliveRef, RDG::Access::StorageWrite )
                 .Storage( "Counters", fe.CountersRef, RDG::Access::StorageWrite )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( CompactPush ) ) );
        }
    }

    Common::BoolResultStr ParticleRenderer::Simulate( const RDG::PassContext& context, const uint32_t step )
    {
        if ( !m_SimPipeline )
            return BOOLSUCCESS;

        auto&    renderer = Renderer::GetInstance();
        uint32_t block    = 0;
        for ( const FrameEmitter& fe : m_FrameEmitters )
        {
            if ( !RunsStep( fe, step ) )
                continue;
            SimPush push  = fe.Push;
            push.Counts.y = step;
            RDG::PassBindings bindings( context, context.GetBindingBlock( block++ ) );
            bindings.PushConstants( &push, sizeof( push ) );

            const uint32_t groups = ( fe.Gpu->Range.Count + kParticleLocalSize - 1 ) / kParticleLocalSize;
            const Common::BoolResultStr dispatched =
                 renderer.DispatchCompute( bindings, *m_SimPipeline, groups, 1, 1 );
            if ( !dispatched )
                return dispatched;
        }
        return BOOLSUCCESS;
    }

    void ParticleRenderer::ImportSimulationBuffers( RDG::Builder& graph )
    {
        m_Pool.Declared = false;
        for ( FrameEmitter& fe : m_FrameEmitters )
            fe.Declared = false;
        if ( m_FrameEmitters.empty() || !m_Pool.Particles )
            return;
        for ( const auto& [buffer, import, name] :
              { std::tuple{ &m_Pool.Particles, &m_Pool.ParticlesImport, "ParticlePool" },
                std::tuple{ &m_Pool.FreeList, &m_Pool.FreeImport, "ParticleFreeList" },
                std::tuple{ &m_Pool.AliveList, &m_Pool.AliveImport, "ParticleAliveList" } } )
        {
            const Common::BoolResultStr imported = Renderer::ImportBuffer( *buffer, *import );
            if ( !imported )
            {
                LOG_ERROR( "[Particles] no emitter runs this frame, {} is not in the frame graph: {}", name,
                           imported.GetError() );
                return;
            }
        }
        m_Pool.ParticlesRef = graph.RegisterExternal( m_Pool.ParticlesImport, "ParticlePool" );
        m_Pool.FreeRef      = graph.RegisterExternal( m_Pool.FreeImport, "ParticleFreeList" );
        m_Pool.AliveRef     = graph.RegisterExternal( m_Pool.AliveImport, "ParticleAliveList" );
        m_Pool.Declared     = true;

        for ( size_t i = 0; i < m_FrameEmitters.size(); ++i )
        {
            FrameEmitter&               fe    = m_FrameEmitters[i];
            const Common::BoolResultStr steps = Renderer::ImportBuffer( fe.Gpu->Steps, fe.StepsImport );
            if ( !steps )
            {
                LOG_ERROR( "[Particles] emitter {} sits out this frame, its step table is not in the frame "
                           "graph: {}",
                           i, steps.GetError() );
                continue;
            }
            const Common::BoolResultStr counters = Renderer::ImportBuffer( fe.Gpu->Counters, fe.CountersImport );
            if ( !counters )
            {
                LOG_ERROR(
                     "[Particles] emitter {} sits out this frame, its counters are not in the frame graph: {}", i,
                     counters.GetError() );
                continue;
            }
            fe.StepsRef    = graph.RegisterExternal( fe.StepsImport, std::format( "ParticleSteps{}", i ) );
            fe.CountersRef = graph.RegisterExternal( fe.CountersImport, std::format( "ParticleCounters{}", i ) );
            fe.Declared    = true;
        }
    }

    void ParticleRenderer::DeclareSimulateBindings( RDG::PassBuilder& pass, const uint32_t step ) const
    {
        if ( !m_SimPipeline )
            return; // Simulate dispatches nothing either
        const auto& layout = m_SimLayout.Get( m_SimPipeline->GetSpecification().Shader );
        for ( const FrameEmitter& fe : m_FrameEmitters )
        {
            if ( !RunsStep( fe, step ) )
                continue; // Simulate skips it the same way
            pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_SimPipeline ) )
                 .Storage( "Particles", m_Pool.ParticlesRef, RDG::Access::StorageWrite )
                 .Storage( "StepTable", fe.StepsRef, RDG::Access::StorageRead )
                 .Storage( "FreeList", m_Pool.FreeRef, RDG::Access::StorageRead )
                 .Storage( "AliveList", m_Pool.AliveRef, RDG::Access::StorageRead )
                 .Storage( "Counters", fe.CountersRef, RDG::Access::StorageRead )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( SimPush ) ) );
        }
    }

    bool ParticleRenderer::IsDrawn( const FrameEmitter& fe )
    {
        return fe.Declared && fe.Gpu != nullptr && fe.Gpu->Material;
    }

    GraphicsPipeline* ParticleRenderer::BillboardPipeline( const FrameEmitter& fe ) const
    {
        return fe.Additive ? m_AddPipeline.get() : m_AlphaPipeline.get();
    }

    SystemRasterPass ParticleRenderer::DrawPass()
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb || !m_AddPipeline )
            return {};

        SystemRasterPass pass{
             .Name        = "ParticlePass",
             .ExecuteFunc = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
             {
                 if ( m_FrameEmitters.empty() )
                     return BOOLSUCCESS;
                 if ( m_SceneRenderer->GetMainCamera() == nullptr || m_SceneRenderer->GetViewFrame() == nullptr )
                     return BOOLSUCCESS;

                 uint32_t block = 0;
                 for ( auto& fe : m_FrameEmitters )
                 {
                     if ( !IsDrawn( fe ) )
                         continue;
                     GraphicsPipeline* pipeline = BillboardPipeline( fe );
                     if ( pipeline == nullptr )
                         return Common::MakeError( "ParticlePass: no pipeline for the emitter's blend" );
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( block++ ) );
                     // The last compact (index StepCount) filled slot StepCount & 1: six vertices per alive
                     // particle from vertex 6 x the emitter's pool base.
                     const uint64_t slot = ( fe.StepCount & 1u ) * kParticleDrawSlotStride;
                     if ( auto drawn = Renderer::DrawProceduralIndirect( bindings, *pipeline,
                                                                         fe.Gpu->Material->GetMaterialExecutor(),
                                                                         fe.CountersRef, slot );
                          !drawn.IsSuccess() )
                         return drawn;
                 }
                 return BOOLSUCCESS;
             },
             .TargetFramebuffer = targetFb };
        pass.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& )
        {
            const ViewFrame* view = m_SceneRenderer->GetViewFrame();
            if ( m_SceneRenderer->GetMainCamera() == nullptr || view == nullptr )
                return; // the exec draws nothing either
            for ( const FrameEmitter& fe : m_FrameEmitters )
            {
                if ( !IsDrawn( fe ) )
                    continue;
                GraphicsPipeline* pipeline = BillboardPipeline( fe );
                if ( pipeline == nullptr )
                    continue; // the exec refuses the emitter by name before it opens a block
                fe.Gpu->Material->Update( *view );
                ShaderBindingLayoutCache& layout = fe.Additive ? m_AddLayout : m_AlphaLayout;
                declared
                     .Bindings( layout.Get( pipeline->GetSpecification().Shader ),
                                fe.Gpu->Material->GetMaterialExecutor()->GetRouteFill() )
                     .Storage( "Particles", m_Pool.ParticlesRef, RDG::Access::StorageRead )
                     .Storage( "AliveList", m_Pool.AliveRef, RDG::Access::StorageRead );
                declared.Read( fe.CountersRef, RDG::Access::IndirectArgs );
            }
        };
        return pass;
    }
} // namespace Desert::Graphic::System

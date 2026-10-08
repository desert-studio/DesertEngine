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

#include <Common/Core/Logger.hpp>

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

        auto billShader = shaderService->GetByName( "ParticleBillboard" );
        if ( !billShader )
        {
            LOG_ERROR( "ParticleRenderer: missing ParticleBillboard shader" );
            return false;
        }

        // The three compute programs of the simulation: Spawn+Update, Compact and Dispatch Args.
        for ( const auto& [name, into] : { std::pair{ "ParticleSimulate", &m_SimPipeline },
                                           std::pair{ "ParticleCompact", &m_CompactPipeline },
                                           std::pair{ "ParticleDispatchArgs", &m_ArgsPipeline } } )
        {
            auto shader = shaderService->GetByName( name );
            if ( !shader )
            {
                LOG_ERROR( "ParticleRenderer: missing {} shader", name );
                return false;
            }
            const auto made = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !made )
            {
                LOG_ERROR( "ParticleRenderer: {}", made.GetError() );
                return false;
            }
            *into = made.GetValue();
        }

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

    void ParticleRenderer::OnSceneReplaced()
    {
        m_ViewEmitters.clear();
        m_World     = nullptr;
        m_Simulates = false;
        if ( m_Materials.empty() )
            return;
        LOG_INFO( "[Particles] Released {} emitter material(s) belonging to the previous scene.",
                  m_Materials.size() );
        m_Materials.clear();
    }

    void ParticleRenderer::PrepareFrame( const ::Desert::Core::Scene& scene )
    {
        m_ViewEmitters.clear();

        ParticleWorldGpu& world = ParticleWorldGpu::Of( scene.GetVFXWorld() );
        m_World                 = &world;
        m_Simulates             = world.PrepareTick( scene );

        // This view's materials follow the scene's emitters: a destroyed emitter's goes through the allocator's
        // deletion ring with it, so the frame still reading it finishes first.
        RetireDestroyedEmitters( m_Materials, scene.GetRegistry() );
        m_ViewEmitters.reserve( world.FrameEmitters().size() );
        for ( const ParticleFrameEmitter& fe : world.FrameEmitters() )
        {
            std::unique_ptr<MaterialParticleBillboard>& material = m_Materials[fe.EntityId];
            if ( !material )
                material = std::make_unique<MaterialParticleBillboard>();
            ViewEmitter ve;
            ve.Frame    = &fe;
            ve.Material = material.get();
            m_ViewEmitters.push_back( ve );
        }
    }

    uint32_t ParticleRenderer::SimulationStepCount() const
    {
        if ( !m_Simulates || !m_SimPipeline || !m_CompactPipeline || !m_ArgsPipeline )
            return 0; // the nodes dispatch nothing either
        uint32_t steps = 0;
        for ( const ViewEmitter& ve : m_ViewEmitters )
            if ( ve.Declared )
                steps = std::max( steps, ve.Frame->StepCount );
        return steps;
    }

    bool ParticleRenderer::RunsStep( const ViewEmitter& ve, const uint32_t step ) const
    {
        return m_Simulates && ve.Declared && step < ve.Frame->StepCount;
    }

    bool ParticleRenderer::RunsCompact( const ViewEmitter& ve, const uint32_t compact ) const
    {
        return m_Simulates && ve.Declared && compact <= ve.Frame->StepCount;
    }

    Common::BoolResultStr ParticleRenderer::Compact( const RDG::PassContext& context, const uint32_t compact )
    {
        if ( !m_CompactPipeline )
            return BOOLSUCCESS;

        auto&    renderer = Renderer::GetInstance();
        uint32_t block    = 0;
        for ( const ViewEmitter& ve : m_ViewEmitters )
        {
            if ( !RunsCompact( ve, compact ) )
                continue;
            ParticleEmitterGpu& gpu   = *ve.Frame->Gpu;
            const bool          reset = compact == 0 && gpu.NeedsReset;
            const uint32_t      flags =
                 ( compact == 0 ? kParticleCompactFullScan : 0u ) | ( reset ? kParticleCompactReset : 0u );
            const ParticleCompactPush push{ glm::uvec4( gpu.Range.Base, gpu.Range.Count, compact & 1u, flags ) };
            RDG::PassBindings         bindings( context, context.GetBindingBlock( block++ ) );
            bindings.PushConstants( &push, sizeof( push ) );
            // Compact 0 rebuilds the lists from the whole range; a later one scans the touched particles only,
            // as many groups as the step's Dispatch Args wrote.
            const Common::BoolResultStr dispatched =
                 compact == 0 ? renderer.DispatchCompute(
                                     bindings, *m_CompactPipeline,
                                     ( gpu.Range.Count + kParticleLocalSize - 1 ) / kParticleLocalSize, 1, 1 )
                              : Renderer::DispatchComputeIndirect( bindings, *m_CompactPipeline, ve.ArgsRef,
                                                                   kParticleCompactArgsOffset );
            if ( !dispatched )
                return dispatched;
            if ( reset )
                gpu.NeedsReset = false;
        }
        return BOOLSUCCESS;
    }

    void ParticleRenderer::DeclareCompactBindings( RDG::PassBuilder& pass, const uint32_t compact ) const
    {
        if ( !m_CompactPipeline )
            return; // Compact dispatches nothing either
        const auto& layout = m_CompactLayout.Get( m_CompactPipeline->GetSpecification().Shader );
        for ( const ViewEmitter& ve : m_ViewEmitters )
        {
            if ( !RunsCompact( ve, compact ) )
                continue; // Compact skips it the same way
            pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_CompactPipeline ) )
                 .Storage( "Particles", m_Pool.ParticlesRef, RDG::Access::StorageWrite )
                 .Storage( "FreeList", m_Pool.FreeRef, RDG::Access::StorageWrite )
                 .Storage( "AliveList", m_Pool.AliveRef, RDG::Access::StorageWrite )
                 .Storage( "Counters", ve.CountersRef, RDG::Access::StorageWrite )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( ParticleCompactPush ) ) );
            if ( compact > 0 )
                pass.Read( ve.ArgsRef, RDG::Access::IndirectArgs );
        }
    }

    Common::BoolResultStr ParticleRenderer::BuildDispatchArgs( const RDG::PassContext& context,
                                                               const uint32_t          step )
    {
        if ( !m_ArgsPipeline )
            return BOOLSUCCESS;

        auto&    renderer = Renderer::GetInstance();
        uint32_t block    = 0;
        for ( const ViewEmitter& ve : m_ViewEmitters )
        {
            if ( !RunsStep( ve, step ) )
                continue;
            const ParticleArgsPush push{ glm::uvec4( step, 0u, 0u, 0u ) };
            RDG::PassBindings      bindings( context, context.GetBindingBlock( block++ ) );
            bindings.PushConstants( &push, sizeof( push ) );
            const Common::BoolResultStr dispatched =
                 renderer.DispatchCompute( bindings, *m_ArgsPipeline, 1, 1, 1 );
            if ( !dispatched )
                return dispatched;
        }
        return BOOLSUCCESS;
    }

    void ParticleRenderer::DeclareDispatchArgsBindings( RDG::PassBuilder& pass, const uint32_t step ) const
    {
        if ( !m_ArgsPipeline )
            return; // BuildDispatchArgs dispatches nothing either
        const auto& layout = m_ArgsLayout.Get( m_ArgsPipeline->GetSpecification().Shader );
        for ( const ViewEmitter& ve : m_ViewEmitters )
        {
            if ( !RunsStep( ve, step ) )
                continue; // BuildDispatchArgs skips it the same way
            pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_ArgsPipeline ) )
                 .Storage( "StepTable", ve.StepsRef, RDG::Access::StorageRead )
                 .Storage( "Counters", ve.CountersRef, RDG::Access::StorageWrite )
                 .Storage( "DispatchArgs", ve.ArgsRef, RDG::Access::StorageWrite )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( ParticleArgsPush ) ) );
        }
    }

    Common::BoolResultStr ParticleRenderer::Simulate( const RDG::PassContext& context, const uint32_t step )
    {
        if ( !m_SimPipeline )
            return BOOLSUCCESS;

        uint32_t block = 0;
        for ( const ViewEmitter& ve : m_ViewEmitters )
        {
            if ( !RunsStep( ve, step ) )
                continue;
            ParticleSimPush push = ve.Frame->Push;
            push.Counts.y        = step;
            RDG::PassBindings bindings( context, context.GetBindingBlock( block++ ) );
            bindings.PushConstants( &push, sizeof( push ) );
            // As many groups as the step's Dispatch Args wrote: max(alive, spawned) threads.
            const Common::BoolResultStr dispatched = Renderer::DispatchComputeIndirect(
                 bindings, *m_SimPipeline, ve.ArgsRef, kParticleSimulateArgsOffset );
            if ( !dispatched )
                return dispatched;
        }
        return BOOLSUCCESS;
    }

    void ParticleRenderer::DeclareSimulateBindings( RDG::PassBuilder& pass, const uint32_t step ) const
    {
        if ( !m_SimPipeline )
            return; // Simulate dispatches nothing either
        const auto& layout = m_SimLayout.Get( m_SimPipeline->GetSpecification().Shader );
        for ( const ViewEmitter& ve : m_ViewEmitters )
        {
            if ( !RunsStep( ve, step ) )
                continue; // Simulate skips it the same way
            pass.Bindings( layout, Renderer::GetPipelineRouteFill( *m_SimPipeline ) )
                 .Storage( "Particles", m_Pool.ParticlesRef, RDG::Access::StorageWrite )
                 .Storage( "StepTable", ve.StepsRef, RDG::Access::StorageRead )
                 .Storage( "FreeList", m_Pool.FreeRef, RDG::Access::StorageRead )
                 .Storage( "AliveList", m_Pool.AliveRef, RDG::Access::StorageWrite )
                 .Storage( "Counters", ve.CountersRef, RDG::Access::StorageRead )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( ParticleSimPush ) ) );
            pass.Read( ve.ArgsRef, RDG::Access::IndirectArgs );
        }
    }

    void ParticleRenderer::ImportFrameBuffers( RDG::Builder& graph )
    {
        m_Pool.Declared = false;
        for ( ViewEmitter& ve : m_ViewEmitters )
            ve.Declared = false;
        if ( m_World == nullptr || m_ViewEmitters.empty() || !m_World->Pool().Particles )
            return;
        const ParticlePoolBuffers& pool = m_World->Pool();
        for ( const auto& [buffer, import, name] :
              { std::tuple{ &pool.Particles, &m_Pool.ParticlesImport, "ParticlePool" },
                std::tuple{ &pool.FreeList, &m_Pool.FreeImport, "ParticleFreeList" },
                std::tuple{ &pool.AliveList, &m_Pool.AliveImport, "ParticleAliveList" } } )
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

        for ( size_t i = 0; i < m_ViewEmitters.size(); ++i )
        {
            ViewEmitter&                ve       = m_ViewEmitters[i];
            const ParticleEmitterGpu&   gpu      = *ve.Frame->Gpu;
            const Common::BoolResultStr counters = Renderer::ImportBuffer( gpu.Counters, ve.CountersImport );
            if ( !counters )
            {
                LOG_ERROR(
                     "[Particles] emitter {} sits out this frame, its counters are not in the frame graph: {}", i,
                     counters.GetError() );
                continue;
            }
            // The step table and the dispatch arguments are read only by the simulation nodes.
            if ( m_Simulates )
            {
                const Common::BoolResultStr steps = Renderer::ImportBuffer( gpu.Steps, ve.StepsImport );
                const Common::BoolResultStr args  = Renderer::ImportBuffer( gpu.DispatchArgs, ve.ArgsImport );
                if ( !steps || !args )
                {
                    LOG_ERROR( "[Particles] emitter {} sits out this frame, its step table or dispatch arguments "
                               "are not in the frame graph: {}",
                               i, !steps ? steps.GetError() : args.GetError() );
                    continue;
                }
                ve.StepsRef = graph.RegisterExternal( ve.StepsImport, std::format( "ParticleSteps{}", i ) );
                ve.ArgsRef  = graph.RegisterExternal( ve.ArgsImport, std::format( "ParticleDispatchArgs{}", i ) );
            }
            ve.CountersRef = graph.RegisterExternal( ve.CountersImport, std::format( "ParticleCounters{}", i ) );
            ve.Declared    = true;
        }
    }

    bool ParticleRenderer::IsDrawn( const ViewEmitter& ve )
    {
        return ve.Declared && ve.Material != nullptr;
    }

    // INTERIM until VFX-08 step R (REMAINDER-VFX-08.md) draws each emitter through its material's
    // ParticleSprite.Forward cell and reads the blend off the template: the v42 migration put exactly the
    // formerly Additive emitters on a material (M_ParticleAdditive) and left the AlphaBlend ones empty, so
    // "names a material" is "was Additive" for every scene of the corpus.
    bool ParticleRenderer::DrawsAdditive( const ViewEmitter& ve )
    {
        return ve.Frame->Material != Common::AssetHandle::Null();
    }

    GraphicsPipeline* ParticleRenderer::BillboardPipeline( const ViewEmitter& ve ) const
    {
        return DrawsAdditive( ve ) ? m_AddPipeline.get() : m_AlphaPipeline.get();
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
                 if ( m_ViewEmitters.empty() )
                     return BOOLSUCCESS;
                 if ( m_SceneRenderer->GetMainCamera() == nullptr || m_SceneRenderer->GetViewFrame() == nullptr )
                     return BOOLSUCCESS;

                 uint32_t block = 0;
                 for ( const ViewEmitter& ve : m_ViewEmitters )
                 {
                     if ( !IsDrawn( ve ) )
                         continue;
                     GraphicsPipeline* pipeline = BillboardPipeline( ve );
                     if ( pipeline == nullptr )
                         return Common::MakeError( "ParticlePass: no pipeline for the emitter's blend" );
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( block++ ) );
                     // The last compact (index StepCount) filled slot StepCount & 1: six vertices per alive
                     // particle of alive half StepCount & 1 (the slot's FirstVertex).
                     const uint64_t slot = ( ve.Frame->StepCount & 1u ) * kParticleDrawSlotStride;
                     if ( auto drawn = Renderer::DrawProceduralIndirect(
                               bindings, *pipeline, ve.Material->GetMaterialExecutor(), ve.CountersRef, slot );
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
            for ( const ViewEmitter& ve : m_ViewEmitters )
            {
                if ( !IsDrawn( ve ) )
                    continue;
                GraphicsPipeline* pipeline = BillboardPipeline( ve );
                if ( pipeline == nullptr )
                    continue; // the exec refuses the emitter by name before it opens a block
                ve.Material->Update( *view );
                ShaderBindingLayoutCache& layout = DrawsAdditive( ve ) ? m_AddLayout : m_AlphaLayout;
                declared
                     .Bindings( layout.Get( pipeline->GetSpecification().Shader ),
                                ve.Material->GetMaterialExecutor()->GetRouteFill() )
                     .Storage( "Particles", m_Pool.ParticlesRef, RDG::Access::StorageRead )
                     .Storage( "AliveList", m_Pool.AliveRef, RDG::Access::StorageRead );
                declared.Read( ve.CountersRef, RDG::Access::IndirectArgs );
            }
        };
        return pass;
    }
} // namespace Desert::Graphic::System

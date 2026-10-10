#include "ParticleRenderer.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>

#include "ParticleEmitterRetire.hpp"
#include "ParticleGpuLayout.hpp"
#include "ParticleSortGraph.hpp"

#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>
#include <Engine/Graphic/Materials/SurfaceBlendPipeline.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRendererInternal.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
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
    namespace
    {
        // ParticleSort.shader's PushConstants, field for field.
        struct ParticleSortPush
        {
            glm::uvec4 Range; // x = alive offset, y = draw slot, z = key base, w = N
            glm::uvec4 Step;  // x = stage, y = k, z = j
            glm::vec4  ViewOrigin;
            glm::vec4  ViewForward;
        };
        static_assert( sizeof( ParticleSortPush ) == kParticleSortPushBytes );
    } // namespace

    ParticleRenderer::~ParticleRenderer() = default;

    Common::BoolResultStr ParticleRenderer::Initialize()
    {
        if ( !CreatePipelines() )
            return Common::MakeError( "ParticleRenderer: failed to create pipelines (missing shaders?)" );
        // The cell every emitter without a drawable material falls back to: no default, no particles.
        const auto cell = TemplateCellShader(
             "ParticleSpriteDefault", MeshVertexPath::ParticleSprite, MeshPass::Forward,
             []( std::string_view name ) {
                 return Runtime::ResourceRegistry::GetShaderService()->GetByName( std::string( name ) ) != nullptr;
             } );
        if ( !cell )
            return Common::MakeError( "ParticleRenderer: the ParticleSpriteDefault template has no registered "
                                      "ParticleSprite.Forward cell" );
        m_DefaultCell = *cell;
        return BOOLSUCCESS;
    }

    bool ParticleRenderer::CreatePipelines()
    {
        auto shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return false;

        // The compute programs of the simulation every emitter shares (Compact, Dispatch Args) and the per-view
        // translucent sort.
        // Spawn+Update is the emitter's own stack host program (ParticleWorldGpu::ProgramFor).
        for ( const auto& [name, into] : { std::pair{ "ParticleCompact", &m_CompactPipeline },
                                           std::pair{ "ParticleDispatchArgs", &m_ArgsPipeline },
                                           std::pair{ "ParticleSort", &m_SortPipeline } } )
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

        // The sprite pipelines are per material cell, built on first use (SpriteDrawFor).
        return true;
    }

    void ParticleRenderer::OnSceneReplaced()
    {
        m_ViewEmitters.clear();
        m_World     = nullptr;
        m_Simulates = false;
        if ( m_Materials.empty() )
            return;
        LOG_INFO( "[Particles] Released {} emitter material instance(s) belonging to the previous scene.",
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
            EmitterMaterial& material = m_Materials[fe.EntityId];
            ViewEmitter      ve;
            ve.Frame    = &fe;
            ve.Sprite   = ResolveSprite( fe.EntityId, fe.Material, material );
            ve.Instance = ve.Sprite != nullptr ? material.Instance.get() : nullptr;
            m_ViewEmitters.push_back( ve );
        }
    }

    uint32_t ParticleRenderer::SimulationStepCount() const
    {
        if ( !m_Simulates || !m_CompactPipeline || !m_ArgsPipeline )
            return 0; // the nodes dispatch nothing either
        uint32_t steps = 0;
        for ( const ViewEmitter& ve : m_ViewEmitters )
            if ( ve.Declared )
                steps = std::max( steps, ve.Frame->StepCount );
        return steps;
    }

    bool ParticleRenderer::RunsStep( const ViewEmitter& ve, const uint32_t step ) const
    {
        return m_Simulates && ve.Declared && ve.Frame->Pipeline != nullptr && step < ve.Frame->StepCount;
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
                 bindings, *ve.Frame->Pipeline, ve.ArgsRef, kParticleSimulateArgsOffset );
            if ( !dispatched )
                return dispatched;
        }
        return BOOLSUCCESS;
    }

    void ParticleRenderer::DeclareSimulateBindings( RDG::PassBuilder& pass, const uint32_t step ) const
    {
        for ( const ViewEmitter& ve : m_ViewEmitters )
        {
            if ( !RunsStep( ve, step ) )
                continue; // Simulate skips it the same way
            const ComputePipeline& program = *ve.Frame->Pipeline;
            const auto&            layout  = m_SimLayouts[&program].Get( program.GetSpecification().Shader );
            pass.Bindings( layout, Renderer::GetPipelineRouteFill( program ) )
                 .Storage( "Particles", m_Pool.ParticlesRef, RDG::Access::StorageWrite )
                 .Storage( "StepTable", ve.StepsRef, RDG::Access::StorageRead )
                 .Storage( "FreeList", m_Pool.FreeRef, RDG::Access::StorageRead )
                 .Storage( "AliveList", m_Pool.AliveRef, RDG::Access::StorageWrite )
                 .Storage( "Counters", ve.CountersRef, RDG::Access::StorageRead )
                 .Storage( "ChannelSpawns", ve.ChannelRef, RDG::Access::StorageRead )
                 .Storage( "AttributeFloats", ve.FloatsRef, RDG::Access::StorageWrite )
                 .Storage( "AttributeInts", ve.IntsRef, RDG::Access::StorageWrite )
                 .Storage( "StackParams", ve.ParamsRef, RDG::Access::StorageRead )
                 .Storage( "StackCurves", ve.CurvesRef, RDG::Access::StorageRead )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( ParticleSimPush ) ) );
            pass.Read( ve.ArgsRef, RDG::Access::IndirectArgs );
        }
    }

    void ParticleRenderer::ImportFrameBuffers( RDG::Builder& graph )
    {
        m_Pool.Declared = false;
        for ( ViewEmitter& ve : m_ViewEmitters )
        {
            ve.Declared = false;
            ve.Sorted   = false;
        }
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
                const Common::BoolResultStr chan  = Renderer::ImportBuffer( gpu.ChannelSpawns, ve.ChannelImport );
                const Common::BoolResultStr stack =
                     Renderer::ImportBuffer( gpu.AttributeFloats, ve.FloatsImport ) &&
                               Renderer::ImportBuffer( gpu.AttributeInts, ve.IntsImport ) &&
                               Renderer::ImportBuffer( gpu.Params, ve.ParamsImport ) &&
                               Renderer::ImportBuffer( gpu.Curves, ve.CurvesImport )
                          ? BOOLSUCCESS
                          : Common::MakeError( "its stack buffers (attributes, parameters, curves) are not" );
                if ( !steps || !args || !chan || !stack )
                {
                    LOG_ERROR( "[Particles] emitter {} sits out this frame, its step table, dispatch arguments or "
                               "channel spawns are not in the frame graph: {}",
                               i,
                               !steps ? steps.GetError()
                                      : ( !args ? args.GetError() : ( !chan ? chan.GetError() : stack.GetError() ) ) );
                    continue;
                }
                ve.ChannelRef =
                     graph.RegisterExternal( ve.ChannelImport, std::format( "ParticleChannelSpawns{}", i ) );
                ve.StepsRef = graph.RegisterExternal( ve.StepsImport, std::format( "ParticleSteps{}", i ) );
                ve.ArgsRef  = graph.RegisterExternal( ve.ArgsImport, std::format( "ParticleDispatchArgs{}", i ) );
                ve.FloatsRef =
                     graph.RegisterExternal( ve.FloatsImport, std::format( "VFXAttributeFloats{}", i ) );
                ve.IntsRef   = graph.RegisterExternal( ve.IntsImport, std::format( "VFXAttributeInts{}", i ) );
                ve.ParamsRef = graph.RegisterExternal( ve.ParamsImport, std::format( "VFXStackParams{}", i ) );
                ve.CurvesRef = graph.RegisterExternal( ve.CurvesImport, std::format( "VFXStackCurves{}", i ) );
            }
            ve.CountersRef = graph.RegisterExternal( ve.CountersImport, std::format( "ParticleCounters{}", i ) );
            ve.Declared    = true;
        }
    }

    void ParticleRenderer::AddSortPasses( RDG::Builder& graph )
    {
        if ( !m_Pool.Declared || !m_SortPipeline || m_World == nullptr || m_SceneRenderer == nullptr )
            return;
        const ViewFrame* view = m_SceneRenderer->GetViewFrame();
        if ( view == nullptr )
            return; // ParticlePass draws nothing either

        // The drawn half is the one the last compact (index StepCount) filled: alive half h = StepCount & 1 at
        // [2 Base + h Count, 2 Base + (h + 1) Count), draw slot h.
        std::vector<ParticleSortEmitter> emitters;
        for ( uint32_t i = 0; i < static_cast<uint32_t>( m_ViewEmitters.size() ); ++i )
        {
            const ViewEmitter& ve = m_ViewEmitters[i];
            if ( !IsDrawn( ve ) )
                continue;
            const ParticleEmitterGpu& gpu  = *ve.Frame->Gpu;
            const uint32_t            half = ve.Frame->StepCount & 1u;
            emitters.push_back(
                 { i, ve.Sprite->Blend, gpu.Range.Count, 2u * gpu.Range.Base + half * gpu.Range.Count, half } );
        }
        const ParticleSortPlan plan = PlanParticleSort( emitters );
        if ( plan.Ranges.empty() )
            return;

        const uint64_t aliveBytes              = uint64_t{ 2 } * m_World->Pool().Capacity * sizeof( uint32_t );
        std::tie( m_SortKeysRef, m_SortedRef ) = CreateParticleSortBuffers( graph, plan, aliveBytes );
        // The view depth is along the camera's forward axis from its position (cm): ParticleSortViewOf.
        const ParticleSortView sortView = ParticleSortViewOf( view->InvView );
        const glm::vec4        origin( sortView.Origin, 0.0f );
        const glm::vec4        forward( sortView.Forward, 0.0f );
        for ( const ParticleSortRange& range : plan.Ranges )
        {
            ViewEmitter& ve = m_ViewEmitters[range.Emitter];
            ve.Sorted       = true;
            const ParticleSortBuffers buffers{ m_Pool.ParticlesRef, m_Pool.AliveRef, ve.CountersRef, m_SortKeysRef,
                                               m_SortedRef };
            const std::vector<ParticleSortStage> stages = ParticleSortStages( range.Length );
            for ( uint32_t s = 0; s < static_cast<uint32_t>( stages.size() ); ++s )
            {
                const ParticleSortStage stage = stages[s];
                const ParticleSortPush  push{
                     glm::uvec4( range.AliveOffset, range.Slot, range.KeyBase, range.Length ),
                     glm::uvec4( static_cast<uint32_t>( stage.Kind ), stage.K, stage.J, 0u ), origin, forward };
                graph.AddPass(
                     ParticleSortPassName( range.Emitter, s ), RDG::PassFlags::Compute,
                     [this, buffers, stage]( RDG::PassBuilder& pass )
                     {
                         DeclareParticleSortStage(
                              pass, m_SortLayout.Get( m_SortPipeline->GetSpecification().Shader ),
                              Renderer::GetPipelineRouteFill( *m_SortPipeline ), buffers, stage.Kind );
                     },
                     [this, push, stage]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( &push, sizeof( push ) );
                         return Renderer::GetInstance().DispatchCompute( bindings, *m_SortPipeline, stage.Groups,
                                                                         1, 1 );
                     } );
            }
        }
    }

    bool ParticleRenderer::IsDrawn( const ViewEmitter& ve )
    {
        return ve.Declared && ve.Sprite != nullptr;
    }

    ParticleRenderer::SpriteDraw* ParticleRenderer::SpriteDrawFor( const std::string& cellShader )
    {
        if ( const auto found = m_Sprites.find( cellShader ); found != m_Sprites.end() )
            return found->second.get(); // null = refused before (said once, below)
        auto& slot = m_Sprites[cellShader];

        auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( cellShader );
        if ( !shader || m_SceneRenderer == nullptr )
        {
            LOG_ERROR( "[Particles] sprite cell '{}' will not draw: {}", cellShader,
                       !shader ? "no such shader is registered" : "the view has no scene renderer" );
            return nullptr;
        }
        auto draw      = std::make_unique<SpriteDraw>();
        draw->Material = std::make_shared<DataDrivenMaterial>( cellShader );
        draw->Instance = draw->Material->CreateInstance();
        if ( !draw->Instance )
        {
            LOG_ERROR( "[Particles] sprite cell '{}' will not draw: its material has no instance", cellShader );
            return nullptr;
        }
        draw->Blend = draw->Material->GetSchema().Blend;

        // Depth-TESTED against the scene (read-only attachment, DrawPass), never written by a blended sprite;
        // the cell's template decides over / added / opaque - the one rule the translucent meshes use too.
        GraphicsPipelineSpecification spec;
        spec.DebugName        = cellShader;
        spec.Shader           = shader;
        spec.TargetLayout     = SceneTargetLayout();
        spec.DepthTestEnabled = true;
        spec.DepthCompareOp   = DepthCompare::CloserOrEqual;
        spec.CullMode         = CullMode::None;
        spec.Topology         = PrimitiveTopology::Triangles;
        ApplySurfaceBlendMode( spec, draw->Blend );
        // The pass holds the depth READ-ONLY (SystemRasterPass::DepthReadOnly): no cell writes it.
        spec.DepthWriteEnabled = false;

        const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
        {
            LOG_ERROR( "[Particles] sprite cell '{}' will not draw: {}", cellShader, pipeline.GetError() );
            return nullptr;
        }
        draw->Pipeline = pipeline.GetValue();
        slot           = std::move( draw );
        return slot.get();
    }

    ParticleRenderer::SpriteDraw* ParticleRenderer::ResolveSprite( const uint32_t             entityId,
                                                                   const Common::AssetHandle& handle,
                                                                   EmitterMaterial&           material )
    {
        // A named material draws through its own ParticleSprite.Forward cell (UE: the sprite renderer's material).
        if ( handle != Common::AssetHandle::Null() )
        {
            auto*       service = Runtime::ResourceRegistry::GetMaterialService();
            const auto* cell    = dynamic_cast<const DataDrivenMaterial*>(
                 service->Get( handle, MeshVertexPath::ParticleSprite, MeshPass::Forward ) );
            SpriteDraw* sprite = cell != nullptr ? SpriteDrawFor( cell->GetShaderName() ) : nullptr;
            if ( sprite != nullptr )
            {
                if ( !material.Instance || material.Handle != handle )
                {
                    material.Handle   = handle;
                    material.Instance = service->CreateRuntimeInstance( handle, MeshVertexPath::ParticleSprite );
                }
                return sprite;
            }
            if ( !material.HasRefused || material.Refused != handle )
            {
                LOG_ERROR( "[Particles] emitter {} draws with ParticleSpriteDefault: its material {} {}", entityId,
                           static_cast<uint64_t>( handle ),
                           cell == nullptr ? "is missing or its template has no `Usage ParticleSprites` cell"
                                           : "has a ParticleSprite cell that cannot draw (logged above)" );
                material.Refused    = handle;
                material.HasRefused = true;
            }
        }
        material.Handle   = Common::AssetHandle::Null();
        material.Instance = nullptr; // the default cell's own defaults
        return SpriteDrawFor( m_DefaultCell );
    }

    SystemRasterPass ParticleRenderer::DrawPass()
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
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
                     SpriteDraw& sprite = *ve.Sprite;
                     // Still in the driver (PSO1): not drawn this frame and not a fault — an engine pipeline is
                     // counted by PipelineBuilds, so no gated frame is shown without it. Failed still faults.
                     if ( sprite.Pipeline->GetReadiness() == PipelineReadiness::Compiling )
                         continue;
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( block++ ) );
                     sprite.Material->SetMaterialIndex( ve.Row );
                     sprite.Material->Bind( sprite.Instance.get() );
                     // The last compact (index StepCount) filled slot StepCount & 1: six vertices per alive
                     // particle of alive half StepCount & 1 (the slot's FirstVertex).
                     const uint64_t slot = ( ve.Frame->StepCount & 1u ) * kParticleDrawSlotStride;
                     if ( auto drawn = Renderer::DrawProceduralIndirect( bindings, *sprite.Pipeline,
                                                                         sprite.Material->GetMaterialExecutor(),
                                                                         ve.CountersRef, slot );
                          !drawn.IsSuccess() )
                         return drawn;
                 }
                 return BOOLSUCCESS;
             },
             .TargetFramebuffer = targetFb };
        // Depth-tested, never written, and sampled for the depth fade in the same pass: the node holds the scene
        // depth READ-ONLY (UE: FExclusiveDepthStencil::DepthRead with the SceneDepth SRV).
        pass.DepthReadOnly = true;
        pass.Declare       = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& )
        {
            const ViewFrame* view = m_SceneRenderer->GetViewFrame();
            if ( m_SceneRenderer->GetMainCamera() == nullptr || view == nullptr )
                return; // the exec draws nothing either

            // Per cell, once per frame and BEFORE its blocks are declared: the rows of the emitters it draws and
            // the view's camera block, so the route fill is what the draws read.
            for ( auto& [cell, sprite] : m_Sprites )
                if ( sprite )
                    sprite->Rows.clear();
            for ( ViewEmitter& ve : m_ViewEmitters )
            {
                if ( !IsDrawn( ve ) )
                    continue;
                MaterialInstance* instance = ve.Instance != nullptr ? ve.Instance : ve.Sprite->Instance.get();
                ve.Row                     = MeshRendererDetail::AppendRow(
                     ve.Sprite->Rows, MeshRendererDetail::EffectiveRow( ve.Sprite->Material.get(), instance ) );
            }
            for ( auto& [cell, sprite] : m_Sprites )
            {
                if ( !sprite || sprite->Rows.empty() )
                    continue;
                if ( auto* sb = sprite->Material->Get<StorageBufferProperty>( "Materials" ) )
                    sb->SetRawData( sprite->Rows.data(),
                                    static_cast<uint32_t>( sprite->Rows.size() * sizeof( glm::vec4 ) ) );
                SceneCameraBind( sprite->Material.get(), *view );
            }

            // The scene depth the cells fade against: the single-sample depth (the attachment itself at MSAA 1,
            // read-only in this node), bound only where the cell's layout samples it.
            const std::shared_ptr<Image2D> depth = m_SceneRenderer->GetComputeSceneDepth();
            for ( const ViewEmitter& ve : m_ViewEmitters )
            {
                if ( !IsDrawn( ve ) )
                    continue;
                const auto& layout = ve.Sprite->Layout.Get( ve.Sprite->Pipeline->GetSpecification().Shader );
                // A translucent / additive emitter reads its alive half back to front from this view's SortedAlive
                // (AddSortPasses), at the same offset; an opaque / masked one reads AliveList unsorted.
                auto block =
                     declared.Bindings( layout, ve.Sprite->Material->GetMaterialExecutor()->GetRouteFill() )
                          .Storage( "Particles", m_Pool.ParticlesRef, RDG::Access::StorageRead )
                          .Storage( "AliveList", ve.Sorted ? m_SortedRef : m_Pool.AliveRef,
                                    RDG::Access::StorageRead );
                if ( SceneViewDetail::LayoutSamples( *layout, "u_SceneDepth" ) && depth )
                    block.Sampled( "u_SceneDepth", depth, RDG::Access::SampledGraphics,
                                   RDG::SamplerDesc::PointClamp(), "SceneDepth.Compute" );
                declared.Read( ve.CountersRef, RDG::Access::IndirectArgs );
            }
        };
        return pass;
    }
} // namespace Desert::Graphic::System

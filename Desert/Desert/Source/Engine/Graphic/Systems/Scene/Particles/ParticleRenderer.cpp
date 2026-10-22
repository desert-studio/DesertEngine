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
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <Engine/Core/Scene.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <format>
#include <tuple>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ParticleCompactShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, PoolSlots );
        DESERT_SHADER_PARAMETER( StorageBuffer, FreeList );
        DESERT_SHADER_PARAMETER( StorageBuffer, AliveList );
        DESERT_SHADER_PARAMETER( StorageBuffer, Counters );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ParticleDispatchArgsShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, StepTable );
        DESERT_SHADER_PARAMETER( StorageBuffer, Counters );
        DESERT_SHADER_PARAMETER( StorageBuffer, DispatchArgs );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ParticleSimulateShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, PoolSlots );
        DESERT_SHADER_PARAMETER( StorageBuffer, StepTable );
        DESERT_SHADER_PARAMETER( StorageBuffer, FreeList );
        DESERT_SHADER_PARAMETER( StorageBuffer, AliveList );
        DESERT_SHADER_PARAMETER( StorageBuffer, Counters );
        DESERT_SHADER_PARAMETER( StorageBuffer, ChannelSpawns );
        DESERT_SHADER_PARAMETER( StorageBuffer, AttributeFloats );
        DESERT_SHADER_PARAMETER( StorageBuffer, AttributeInts );
        DESERT_SHADER_PARAMETER( StorageBuffer, StackParams );
        DESERT_SHADER_PARAMETER( StorageBuffer, StackCurves );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ParticleMeshInstancesShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, AttributeFloats );
        DESERT_SHADER_PARAMETER( StorageBuffer, DrawLayout );
        DESERT_SHADER_PARAMETER( StorageBuffer, AliveList );
        DESERT_SHADER_PARAMETER( StorageBuffer, Counters );
        DESERT_SHADER_PARAMETER( StorageBuffer, Instances );
        DESERT_SHADER_PARAMETER( StorageBuffer, Args );
        DESERT_SHADER_PARAMETER( StorageBuffer, History );
        DESERT_SHADER_PARAMETER( StorageBuffer, PoolSlots );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ParticleShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, Materials );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ParticleSpriteShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, AttributeFloats );
        DESERT_SHADER_PARAMETER( StorageBuffer, DrawLayout );
        DESERT_SHADER_PARAMETER( StorageBuffer, AliveList );
        DESERT_SHADER_PARAMETER( StorageBuffer, RibbonOrder );
        DESERT_SHADER_PARAMETER( Texture, SceneDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
    };
} // namespace

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
        const auto ribbon = TemplateCellShader(
             "ParticleRibbonDefault", MeshVertexPath::ParticleRibbon, MeshPass::Forward,
             []( std::string_view name ) {
                 return Runtime::ResourceRegistry::GetShaderService()->GetByName( std::string( name ) ) != nullptr;
             } );
        if ( !ribbon )
            return Common::MakeError( "ParticleRenderer: the ParticleRibbonDefault template has no registered "
                                      "ParticleRibbon.Forward cell" );
        m_DefaultRibbonCell = *ribbon;
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
                                           std::pair{ "ParticleSort", &m_SortPipeline },
                                           std::pair{ "ParticleMeshInstances", &m_MeshPipeline } } )
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
        std::size_t rows = 0;
        for ( const auto& [entity, materials] : m_Materials )
            rows += materials.size();
        m_Materials.clear();
        if ( rows == 0 )
            return;
        LOG_INFO( "[Particles] Released {} renderer row material instance(s) belonging to the previous scene.",
                  rows );
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
            EmitterMaterials& materials = m_Materials[fe.EntityId];
            ViewEmitter       ve;
            ve.Frame = &fe;
            // One draw per enabled renderer row, in `.dfx` order (UE: every renderer of the emitter draws), each
            // with its own material instance keyed by (emitter, row).
            ve.Draws.reserve( fe.Renderers.size() );
            for ( const ParticleFrameRenderer& row : fe.Renderers )
            {
                EmitterMaterial& material = materials[RendererKey{ fe.Emitter, row.Row }];
                if ( row.Kind == Assets::Serialization::VFXRendererKind::Mesh )
                {
                    // A Mesh row draws its static mesh per particle through the mesh renderer's instanced cells
                    // (AddMeshInstances); a mesh still loading or refused sits the frame out.
                    if ( row.Mesh != nullptr && ResolveMesh( fe, row, material ) )
                        ve.Meshes.push_back( { &material.Meshes, row.Mesh->Scale, row.Row } );
                    continue;
                }
                ViewDraw draw;
                draw.Sprite   = ResolveSprite( fe, row, material );
                draw.Instance = draw.Sprite != nullptr ? material.Instance.get() : nullptr;
                if ( draw.Sprite != nullptr )
                    ve.Draws.push_back( draw );
            }
            m_ViewEmitters.push_back( std::move( ve ) );
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
                 .Storage( ParticleCompactShaderParameters::PoolSlots, m_Pool.SlotsRef, RDG::Access::StorageWrite )
                 .Storage( ParticleCompactShaderParameters::FreeList, m_Pool.FreeRef, RDG::Access::StorageWrite )
                 .Storage( ParticleCompactShaderParameters::AliveList, m_Pool.AliveRef, RDG::Access::StorageWrite )
                 .Storage( ParticleCompactShaderParameters::Counters, ve.CountersRef, RDG::Access::StorageWrite )
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
                 .Storage( ParticleDispatchArgsShaderParameters::StepTable, ve.StepsRef, RDG::Access::StorageRead )
                 .Storage( ParticleDispatchArgsShaderParameters::Counters, ve.CountersRef,
                           RDG::Access::StorageWrite )
                 .Storage( ParticleDispatchArgsShaderParameters::DispatchArgs, ve.ArgsRef,
                           RDG::Access::StorageWrite )
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
                 .Storage( ParticleSimulateShaderParameters::PoolSlots, m_Pool.SlotsRef,
                           RDG::Access::StorageWrite )
                 .Storage( ParticleSimulateShaderParameters::StepTable, ve.StepsRef, RDG::Access::StorageRead )
                 .Storage( ParticleSimulateShaderParameters::FreeList, m_Pool.FreeRef, RDG::Access::StorageRead )
                 .Storage( ParticleSimulateShaderParameters::AliveList, m_Pool.AliveRef,
                           RDG::Access::StorageWrite )
                 .Storage( ParticleSimulateShaderParameters::Counters, ve.CountersRef, RDG::Access::StorageRead )
                 .Storage( ParticleSimulateShaderParameters::ChannelSpawns, ve.ChannelRef,
                           RDG::Access::StorageRead )
                 .Storage( ParticleSimulateShaderParameters::AttributeFloats, ve.FloatsRef,
                           RDG::Access::StorageWrite )
                 .Storage( ParticleSimulateShaderParameters::AttributeInts, ve.IntsRef, RDG::Access::StorageWrite )
                 .Storage( ParticleSimulateShaderParameters::StackParams, ve.ParamsRef, RDG::Access::StorageRead )
                 .Storage( ParticleSimulateShaderParameters::StackCurves, ve.CurvesRef, RDG::Access::StorageRead )
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
            ve.Ribboned = false;
        }
        if ( m_World == nullptr || m_ViewEmitters.empty() || !m_World->Pool().Slots )
            return;
        const ParticlePoolBuffers& pool = m_World->Pool();
        for ( const auto& [buffer, import, name] :
              { std::tuple{ &pool.Slots, &m_Pool.SlotsImport, "ParticleSlots" },
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
        m_Pool.SlotsRef = graph.RegisterExternal( m_Pool.SlotsImport, "ParticleSlots" );
        m_Pool.FreeRef  = graph.RegisterExternal( m_Pool.FreeImport, "ParticleFreeList" );
        m_Pool.AliveRef = graph.RegisterExternal( m_Pool.AliveImport, "ParticleAliveList" );
        m_Pool.Declared = true;

        for ( size_t i = 0; i < m_ViewEmitters.size(); ++i )
        {
            ViewEmitter&                ve       = m_ViewEmitters[i];
            const ParticleEmitterGpu&   gpu      = *ve.Frame->Gpu;
            // Every view reads the counters (the indirect draw), the attribute columns and the draw layout (the
            // sort and the sprite draw read the particles where they live).
            Common::BoolResultStr drawn = Renderer::ImportBuffer( gpu.Counters, ve.CountersImport );
            if ( drawn )
                drawn = Renderer::ImportBuffer( gpu.AttributeFloats, ve.FloatsImport );
            if ( drawn ) // the ribbon sort reads the RibbonIdBinding's int column
                drawn = Renderer::ImportBuffer( gpu.AttributeInts, ve.IntsImport );
            if ( drawn )
                drawn = Renderer::ImportBuffer( gpu.DrawLayout, ve.DrawLayoutImport );
            if ( !drawn )
            {
                LOG_ERROR( "[Particles] emitter {} sits out this frame, its counters, attribute columns or draw "
                           "layout are not in the frame graph: {}",
                           i, drawn.GetError() );
                continue;
            }
            // The step table and the dispatch arguments are read only by the simulation nodes.
            if ( m_Simulates )
            {
                const Common::BoolResultStr steps = Renderer::ImportBuffer( gpu.Steps, ve.StepsImport );
                const Common::BoolResultStr args  = Renderer::ImportBuffer( gpu.DispatchArgs, ve.ArgsImport );
                const Common::BoolResultStr chan  = Renderer::ImportBuffer( gpu.ChannelSpawns, ve.ChannelImport );
                Common::BoolResultStr       stack = Renderer::ImportBuffer( gpu.Params, ve.ParamsImport );
                if ( stack )
                    stack = Renderer::ImportBuffer( gpu.Curves, ve.CurvesImport );
                if ( !steps || !args || !chan || !stack )
                {
                    LOG_ERROR(
                         "[Particles] emitter {} sits out this frame, its step table, dispatch arguments or "
                         "channel spawns are not in the frame graph: {}",
                         i,
                         !steps ? steps.GetError()
                                : ( !args ? args.GetError() : ( !chan ? chan.GetError() : stack.GetError() ) ) );
                    continue;
                }
                ve.ChannelRef =
                     graph.RegisterExternal( ve.ChannelImport, std::format( "ParticleChannelSpawns{}", i ) );
                ve.StepsRef  = graph.RegisterExternal( ve.StepsImport, std::format( "ParticleSteps{}", i ) );
                ve.ArgsRef   = graph.RegisterExternal( ve.ArgsImport, std::format( "ParticleDispatchArgs{}", i ) );
                ve.ParamsRef = graph.RegisterExternal( ve.ParamsImport, std::format( "VFXStackParams{}", i ) );
                ve.CurvesRef = graph.RegisterExternal( ve.CurvesImport, std::format( "VFXStackCurves{}", i ) );
            }
            ve.CountersRef = graph.RegisterExternal( ve.CountersImport, std::format( "ParticleCounters{}", i ) );
            ve.FloatsRef   = graph.RegisterExternal( ve.FloatsImport, std::format( "VFXAttributeFloats{}", i ) );
            ve.IntsRef     = graph.RegisterExternal( ve.IntsImport, std::format( "VFXAttributeInts{}", i ) );
            ve.DrawLayoutRef =
                 graph.RegisterExternal( ve.DrawLayoutImport, std::format( "ParticleDrawLayout{}", i ) );
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
            // The emitter's alive half is sorted once for all its draws: when any of them blends (an opaque draw
            // reads the sorted order as well as the unsorted one).
            const auto blends = std::find_if( ve.Draws.begin(), ve.Draws.end(), []( const ViewDraw& d )
                                              { return ParticleSpritesSort( d.Sprite->Blend ); } );
            const Core::Formats::SurfaceBlendMode blend =
                 blends != ve.Draws.end() ? blends->Sprite->Blend : ve.Draws.front().Sprite->Blend;
            emitters.push_back(
                 { i, blend, gpu.Range.Count, 2u * gpu.Range.Base + half * gpu.Range.Count, half } );
            // A Ribbon row links the particles by (RibbonId, RibbonLinkOrder): its own sort, into RibbonOrder.
            const bool ribbon = std::any_of( ve.Frame->Renderers.begin(), ve.Frame->Renderers.end(),
                                             []( const ParticleFrameRenderer& row ) {
                                                 return row.Kind == Assets::Serialization::VFXRendererKind::Ribbon;
                                             } );
            if ( ribbon )
                emitters.push_back(
                     { i, blend, gpu.Range.Count, 2u * gpu.Range.Base + half * gpu.Range.Count, half, true } );
        }
        const ParticleSortPlan plan = PlanParticleSort( emitters );
        if ( plan.Ranges.empty() )
            return;

        const uint64_t aliveBytes              = uint64_t{ 2 } * m_World->Pool().Capacity * sizeof( uint32_t );
        std::tie( m_SortKeysRef, m_SortedRef ) = CreateParticleSortBuffers( graph, plan, aliveBytes );
        m_RibbonRef =
             graph.CreateBuffer( RDG::BufferDesc{ aliveBytes * 4u }, "ParticleRibbonOrder" ); // uvec4 each
        // The view depth is along the camera's forward axis from its position (cm): ParticleSortViewOf.
        const ParticleSortView sortView = ParticleSortViewOf( view->InvView );
        const glm::vec4        origin( sortView.Origin, 0.0f );
        const glm::vec4        forward( sortView.Forward, 0.0f );
        for ( const ParticleSortRange& range : plan.Ranges )
        {
            ViewEmitter& ve = m_ViewEmitters[range.Emitter];
            ( range.Ribbon ? ve.Ribboned : ve.Sorted ) = true;
            const ParticleSortBuffers buffers{ ve.FloatsRef,    ve.IntsRef,    ve.DrawLayoutRef,
                                               m_Pool.AliveRef, ve.CountersRef, m_SortKeysRef,
                                               m_SortedRef,     m_RibbonRef };
            const std::vector<ParticleSortStage> stages = ParticleSortStages( range.Length, range.Ribbon );
            for ( uint32_t s = 0; s < static_cast<uint32_t>( stages.size() ); ++s )
            {
                const ParticleSortStage stage = stages[s];
                const ParticleSortPush  push{
                     glm::uvec4( range.AliveOffset, range.Slot, range.KeyBase, range.Length ),
                     glm::uvec4( static_cast<uint32_t>( stage.Kind ), stage.K, stage.J, range.Ribbon ? 1u : 0u ),
                     origin, forward };
                graph.AddPass(
                     ParticleSortPassName( range.Emitter, s, range.Ribbon ), RDG::PassFlags::Compute,
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
        return ve.Declared && !ve.Draws.empty();
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

    ParticleRenderer::SpriteDraw* ParticleRenderer::ResolveSprite( const ParticleFrameEmitter&  fe,
                                                                   const ParticleFrameRenderer& row,
                                                                   EmitterMaterial&             material )
    {
        const Common::AssetHandle& handle = row.Material;
        // The row's vertex factory (UE: the sprite / ribbon renderer's vertex factory) and its engine material.
        const bool           ribbon = row.Kind == Assets::Serialization::VFXRendererKind::Ribbon;
        const MeshVertexPath path   = ribbon ? MeshVertexPath::ParticleRibbon : MeshVertexPath::ParticleSprite;
        const std::string&   engine = ribbon ? m_DefaultRibbonCell : m_DefaultCell;
        // A named material draws through its own cell of the row's path (UE: the renderer's material).
        if ( handle != Common::AssetHandle::Null() )
        {
            auto*       service = Runtime::ResourceRegistry::GetMaterialService();
            const auto* cell =
                 dynamic_cast<const DataDrivenMaterial*>( service->Get( handle, path, MeshPass::Forward ) );
            SpriteDraw* sprite = cell != nullptr ? SpriteDrawFor( cell->GetShaderName() ) : nullptr;
            if ( sprite != nullptr )
            {
                if ( !material.Instance || material.Handle != handle )
                {
                    material.Handle   = handle;
                    material.Instance = service->CreateRuntimeInstance( handle, path );
                }
                return sprite;
            }
            if ( !material.HasRefused || material.Refused != handle )
            {
                LOG_ERROR( "[Particles] emitter {}.{} renderer row {} draws with {}: its material {} {}",
                           fe.EntityId, fe.Emitter, row.Row,
                           ribbon ? "ParticleRibbonDefault" : "ParticleSpriteDefault",
                           static_cast<uint64_t>( handle ),
                           cell == nullptr ? std::format( "is missing or its template has no `Usage {}` cell",
                                                          ribbon ? "ParticleRibbons" : "ParticleSprites" )
                                           : std::format( "has a {} cell that cannot draw (logged above)",
                                                          MeshVertexPathName( path ) ) );
                material.Refused    = handle;
                material.HasRefused = true;
            }
        }
        material.Handle   = Common::AssetHandle::Null();
        material.Instance = nullptr; // the default cell's own defaults
        return SpriteDrawFor( engine );
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
                     for ( const ViewDraw& draw : ve.Draws )
                     {
                         // The n-th declared block is the n-th draw, drawn or not.
                         const uint32_t drawBlock = block++;
                         SpriteDraw&    sprite    = *draw.Sprite;
                         // Still in the driver (PSO1): not drawn this frame and not a fault — an engine pipeline
                         // is counted by PipelineBuilds, so no gated frame is shown without it. Failed still
                         // faults.
                         if ( sprite.Pipeline->GetReadiness() == PipelineReadiness::Compiling )
                             continue;
                         const RDG::PassBindings bindings( context, context.GetBindingBlock( drawBlock ) );
                         sprite.Material->SetMaterialIndex( draw.Row );
                         sprite.Material->Bind( sprite.Instance.get() );
                         // The last compact (index StepCount) filled slot StepCount & 1: six vertices per alive
                         // particle of alive half StepCount & 1 (the slot's FirstVertex).
                         const uint64_t slot = ( ve.Frame->StepCount & 1u ) * kParticleDrawSlotStride;
                         if ( auto drawn = Renderer::DrawProceduralIndirect(
                                   bindings, *sprite.Pipeline, sprite.Material->GetMaterialExecutor(),
                                   ve.CountersRef, slot );
                              !drawn.IsSuccess() )
                             return drawn;
                     }
                 }
                 return BOOLSUCCESS;
             },
             .TargetFramebuffer = targetFb };
        // Depth-tested, never written, and sampled for the depth fade in the same pass: the node holds the scene
        // depth READ-ONLY (UE: FExclusiveDepthStencil::DepthRead with the SceneDepth SRV).
        pass.DepthReadOnly = true;
        pass.Declare       = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& refs )
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
                for ( ViewDraw& draw : ve.Draws )
                {
                    MaterialInstance* instance =
                         draw.Instance != nullptr ? draw.Instance : draw.Sprite->Instance.get();
                    draw.Row = MeshRendererDetail::AppendRow(
                         draw.Sprite->Rows,
                         MeshRendererDetail::EffectiveRow( draw.Sprite->Material.get(), instance ) );
                }
            }
            for ( auto& [cell, sprite] : m_Sprites )
            {
                if ( !sprite || sprite->Rows.empty() )
                    continue;
                // Find: a sprite surface template with no parameters (or every row field dead) declares no row
                // block.
                if ( auto* sb =
                          sprite->Material->Find<StorageBufferProperty>( ParticleShaderParameters::Materials ) )
                    sb->SetRawData( sprite->Rows.data(),
                                    static_cast<uint32_t>( sprite->Rows.size() * sizeof( glm::vec4 ) ) );
                SceneCameraBind( sprite->Material.get(), *view );
            }

            // The scene depth the cells fade against: the single-sample depth (the attachment itself at MSAA 1,
            // read-only in this node), on BOTH paths (SceneViewInputs::SceneDepth is the G-buffer's, absent on the
            // forward path). Every other scene/view input a cell samples comes from THE one list, as the forward
            // meshes and the glass take them — the shading word the lit cell's contact shadow tests, the cascades,
            // the environment, the light grid: a slot the pass leaves unfilled faults the node
            // (ValidatePassBindings), so a sprite cell that gains a scene read must not need this pass to name it.
            const std::shared_ptr<Image2D> depth  = m_SceneRenderer->GetComputeSceneDepth();
            const SceneViewInputs          inputs = SceneViewInputsOf( refs );
            for ( const ViewEmitter& ve : m_ViewEmitters )
            {
                if ( !IsDrawn( ve ) )
                    continue;
                for ( const ViewDraw& draw : ve.Draws )
                {
                    const SpriteDraw& sprite = *draw.Sprite;
                    const auto&       layout = sprite.Layout.Get( sprite.Pipeline->GetSpecification().Shader );
                    // A translucent / additive emitter reads its alive half back to front from this view's
                    // SortedAlive (AddSortPasses), at the same offset; an opaque / masked one reads AliveList
                    // unsorted.
                    auto block =
                         declared.Bindings( layout, sprite.Material->GetMaterialExecutor()->GetRouteFill() )
                              .Storage( ParticleSpriteShaderParameters::AttributeFloats, ve.FloatsRef, RDG::Access::StorageRead )
                              .Storage( ParticleSpriteShaderParameters::DrawLayout, ve.DrawLayoutRef, RDG::Access::StorageRead );
                    // A sprite cell reads the alive list; a ribbon cell reads the emitter's link order instead
                    // (Vertex_ParticleRibbon.glslh RibbonOrder, AddSortPasses), at the same offsets.
                    const bool links =
                         std::any_of( layout->Slots.begin(), layout->Slots.end(),
                                      []( const RDG::ShaderSlot& slot ) { return slot.Name == "RibbonOrder"; } );
                    if ( links && ve.Ribboned )
                        block.Storage( ParticleSpriteShaderParameters::RibbonOrder, m_RibbonRef, RDG::Access::StorageRead );
                    else
                        block.Storage( ParticleSpriteShaderParameters::AliveList, ve.Sorted ? m_SortedRef : m_Pool.AliveRef,
                                       RDG::Access::StorageRead );
                    const bool fades = SceneViewDetail::LayoutSamples( *layout, kSceneViewSceneDepthName );
                    if ( fades && depth )
                        block.Texture( kSceneViewSceneDepth, depth, RDG::Access::SampledGraphics,
                                       "SceneDepth.Compute" )
                             .Sampler( ParticleSpriteShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() );
                    // The rest of the list, minus the depth above and minus what the material route already fills
                    // (a slot filled by both faults the node as surely as one filled by neither).
                    const RDG::OtherRouteFill& route  = sprite.Material->GetMaterialExecutor()->GetRouteFill();
                    RDG::ShaderBindingLayout   others = *layout;
                    std::erase_if( others.Slots,
                                   [&route]( const RDG::ShaderSlot& slot )
                                   {
                                       return slot.Name == kSceneViewSceneDepthName ||
                                              slot.Name == "RibbonOrder" ||
                                              std::find( route.Slots.begin(), route.Slots.end(), slot.Name ) !=
                                                   route.Slots.end();
                                   } );
                    BindSceneViewInputs( block, inputs, others );
                }
                declared.Read( ve.CountersRef, RDG::Access::IndirectArgs );
            }
        };
        return pass;
    }

    bool ParticleRenderer::ResolveMesh( const ParticleFrameEmitter& fe, const ParticleFrameRenderer& row,
                                        EmitterMaterial& material )
    {
        auto* meshes    = Runtime::ResourceRegistry::GetMeshService();
        auto* materials = Runtime::ResourceRegistry::GetMaterialService();
        const auto refuse = [&]( const std::string& why )
        {
            if ( !material.MeshRefused )
                LOG_ERROR( "[Particles] emitter {}.{} Mesh renderer row {} ('{}') does not draw: {}", fe.EntityId,
                           fe.Emitter, row.Row, row.Mesh->Mesh.Path, why );
            material.MeshRefused = true;
            return false;
        };
        if ( meshes == nullptr || materials == nullptr )
            return refuse( "the mesh or material service is not running" );
        const auto guid = Common::Content::AssetGuidFromText( row.Mesh->Mesh.Guid );
        if ( !guid )
            return refuse( guid.GetError() );
        const Assets::AssetHandle handle = Common::Content::HandleForGuid( guid.GetValue() );
        Desert::Mesh*             mesh   = meshes->Get( handle );
        const Assets::MeshAsset*  asset  = meshes->GetAsset( handle );
        if ( mesh == nullptr || asset == nullptr )
            return false; // still loading, or MeshService said why it failed
        const auto& submeshes = mesh->GetSubmeshes();
        if ( submeshes.empty() )
            return refuse( "the mesh has no submeshes" );

        MeshInstances&     state    = material.Meshes;
        const uint32_t     capacity = std::max( fe.Gpu->Range.Count, 1u );
        const auto         count    = static_cast<uint32_t>( std::min<size_t>( submeshes.size(), 64 ) );
        const uint64_t     all      = count >= 64 ? ~0ull : ( 1ull << count ) - 1ull;
        if ( state.Mesh != mesh || state.Material != row.Material || state.Version != materials->GetInvalidationVersion() )
        {
            state.Mesh     = nullptr;
            state.Material = row.Material;
            state.Version  = materials->GetInvalidationVersion();
            state.Sections.clear();
            // The row's Material overrides every submesh (UE: the mesh renderer's override materials); without
            // one each material slot of the mesh draws its own sections, as the mesh component does.
            if ( row.Material != Common::AssetHandle::Null() )
            {
                if ( auto instance = materials->CreateRuntimeInstance( row.Material ) )
                    state.Sections.push_back( { std::move( instance ), 0 } );
                else
                    return refuse( "its override material does not resolve" );
            }
            else
            {
                const auto& slots = asset->GetMaterialHandles();
                for ( size_t slot = 0; slot < slots.size(); ++slot )
                {
                    uint64_t hidden = 0;
                    for ( uint32_t si = 0; si < count; ++si )
                        if ( SectionMaterialSlot( si, slots.size() ) != slot )
                            hidden |= 1ull << si;
                    if ( hidden == all )
                        continue;
                    auto instance =
                         materials->CreateRuntimeInstance( materials->GetAssetHandleByExternal( slots[slot] ) );
                    if ( !instance )
                        return refuse( std::format( "its material slot {} does not resolve", slot ) );
                    state.Sections.push_back( { std::move( instance ), hidden } );
                }
                if ( state.Sections.empty() )
                    return refuse( "the mesh has no material slot" );
            }
            state.Mesh = mesh;
        }
        if ( state.Instances == nullptr || state.Capacity != capacity || state.Submeshes != count ||
             state.ArgsMesh != mesh )
        {
            if ( state.Key == 0 )
                state.Key = ++m_NextMeshKey;
            state.Capacity  = capacity;
            state.Submeshes = count;
            state.Instances = ShaderResources::StorageBuffer::Create(
                 "ParticleMeshInstances", 2u * capacity * sizeof( glm::mat4 ), 3, true );
            // Zeroed: frame stamp 0 is no frame's (the first frame stamped is 2), so no particle reads a stale
            // row.
            constexpr uint32_t kHistoryRow = sizeof( glm::mat4 ) + sizeof( glm::uvec4 );
            state.History =
                 ShaderResources::StorageBuffer::Create( "ParticleMeshHistory", capacity * kHistoryRow, 6, true );
            const std::vector<uint8_t> zeros( capacity * kHistoryRow, uint8_t{ 0 } );
            if ( const auto cleared = state.History->SetData( zeros.data(), capacity * kHistoryRow );
                 !cleared.IsSuccess() )
            {
                state.Instances = nullptr;
                return refuse( std::format( "its transform history was not cleared: {}", cleared.GetError() ) );
            }
            // The mesh's levels as the mesh component draws them (Geometry::SelectLODFromBounds): every level a
            // submesh carries, each with its screen size; a mesh without a LOD chain has one.
            const std::vector<float>& sizes = mesh->GetLODScreenSizes();
            state.Levels                    = std::max( 1u, std::min( Geometry::MaxAvailableLOD( submeshes ) + 1u,
                                                                      static_cast<uint32_t>( sizes.size() ) ) );
            state.LodTable                  = static_cast<uint32_t>( GpuInstanceArgsOffset( count ) / 4u );
            const auto local                = Geometry::LocalBounds( submeshes );
            state.LocalSphere =
                 glm::vec4( 0.5f * ( local.Min + local.Max ), 0.5f * glm::length( local.Max - local.Min ) );
            const uint32_t argWords = state.LodTable + state.Levels * count * 2u + state.Levels + 1u;
            const uint32_t argBytes = argWords * 4u;
            state.Args = ShaderResources::StorageBuffer::Create( "ParticleMeshArgs", argBytes, 4, true );
            // The records' static half: each submesh's level-0 index range (ParticleMeshInstances' LOD pass
            // rewrites count and first index from the level table each frame); instanceCount and firstInstance
            // are ParticleMeshInstances' (one bucket: the whole range).
            std::vector<uint32_t> args( argWords, 0u );
            args[0] = 1u;
            args[1] = capacity;
            args[2] = count;
            for ( uint32_t level = 0; level < state.Levels; ++level )
                for ( uint32_t si = 0; si < count; ++si )
                {
                    // A submesh with a shorter chain draws its coarsest level; one without a chain its base range.
                    const auto& sm    = submeshes[si];
                    uint32_t*   entry = &args[state.LodTable + ( level * count + si ) * 2u];
                    if ( sm.LODs.empty() )
                    {
                        entry[0] = sm.IndexCount;
                        entry[1] = sm.IndexOffset;
                    }
                    else
                    {
                        const auto& lod = sm.LODs[std::min<size_t>( level, sm.LODs.size() - 1 )];
                        entry[0]        = lod.IndexCount;
                        entry[1]        = lod.IndexOffset;
                    }
                }
            for ( uint32_t level = 0; level < state.Levels; ++level )
                args[state.LodTable + state.Levels * count * 2u + level] = std::bit_cast<uint32_t>( sizes[level] );
            args[argWords - 1u] = state.Levels - 1u; // the reduction starts at the coarsest level
            for ( uint32_t si = 0; si < count; ++si )
            {
                uint32_t* record = &args[GpuInstanceArgsOffset( si ) / 4u];
                record[0]        = args[state.LodTable + si * 2u];
                record[2]        = args[state.LodTable + si * 2u + 1u];
                record[3]        = submeshes[si].VertexOffset;
            }
            if ( const auto wrote = state.Args->SetData( args.data(), argBytes ); !wrote.IsSuccess() )
            {
                state.Instances = nullptr;
                return refuse( std::format( "its argument records were not written: {}", wrote.GetError() ) );
            }
            state.ArgsMesh = mesh;
        }
        material.MeshRefused = false;
        return true;
    }

    bool ParticleRenderer::HasMeshRows() const
    {
        return std::any_of( m_ViewEmitters.begin(), m_ViewEmitters.end(),
                            []( const ViewEmitter& ve ) { return !ve.Meshes.empty(); } );
    }

    void ParticleRenderer::AddMeshInstances( RDG::Builder& graph, MeshRenderer& meshes )
    {
        if ( !m_Pool.Declared || !m_MeshPipeline )
            return;
        // One view frame: a history row stamped with the previous value is the particle's last drawn transform.
        const uint32_t frame = ++m_MeshFrame;
        // The view the level is chosen from (UE ComputeBoundsScreenRadiusSquared through
        // Geometry::BoundsScreenRadiusSquared): the eye, the lens' focal scale, and whether it is perspective.
        const auto& camera = m_SceneRenderer->GetMainCamera();
        if ( camera == nullptr )
            return;
        const glm::mat4& projection = camera->GetProjectionMatrix();
        const glm::vec4  lens( camera->GetPosition(), std::max( 0.5f * std::abs( projection[0][0] ),
                                                                0.5f * std::abs( projection[1][1] ) ) );
        const float      perspective = std::abs( projection[2][3] );
        for ( uint32_t i = 0; i < static_cast<uint32_t>( m_ViewEmitters.size() ); ++i )
        {
            const ViewEmitter& ve = m_ViewEmitters[i];
            if ( !ve.Declared )
                continue;
            const ParticleEmitterGpu& gpu  = *ve.Frame->Gpu;
            const uint32_t            half = ve.Frame->StepCount & 1u;
            for ( const ViewMesh& vm : ve.Meshes )
            {
                MeshInstances& state = *vm.State;
                Common::BoolResultStr imported = Renderer::ImportBuffer( state.Instances, state.InstancesImport );
                if ( imported )
                    imported = Renderer::ImportBuffer( state.Args, state.ArgsImport );
                if ( imported )
                    imported = Renderer::ImportBuffer( state.History, state.HistoryImport );
                if ( !imported )
                {
                    LOG_ERROR( "[Particles] emitter {} Mesh row {} sits out this frame: {}", i, vm.Row,
                               imported.GetError() );
                    continue;
                }
                state.Refs.Instances = graph.RegisterExternal( state.InstancesImport,
                                                               std::format( "ParticleMeshInstances{}.{}", i, vm.Row ) );
                state.Refs.Args =
                     graph.RegisterExternal( state.ArgsImport, std::format( "ParticleMeshArgs{}.{}", i, vm.Row ) );
                const RDG::BufferRef history = graph.RegisterExternal(
                     state.HistoryImport, std::format( "ParticleMeshHistory{}.{}", i, vm.Row ) );

                struct MeshPush
                {
                    glm::uvec4 Range;
                    glm::vec4  Scale;
                    glm::uvec4 Frame;
                    glm::vec4  Sphere;
                    glm::vec4  Lens;
                    glm::uvec4 Lod;
                };
                MeshPush push{
                     glm::uvec4( 2u * gpu.Range.Base + half * gpu.Range.Count, half, state.Submeshes,
                                 state.Capacity ),
                     glm::vec4( vm.Scale, 0.0f ),
                     glm::uvec4( frame, 0u, 0u, 0u ),
                     state.LocalSphere,
                     lens,
                     glm::uvec4( state.Levels, state.LodTable, 0u, std::bit_cast<uint32_t>( perspective ) ) };
                const uint32_t       groups = ( state.Capacity + 63u ) / 64u;
                const RDG::BufferRef floats = ve.FloatsRef, layout = ve.DrawLayoutRef, counters = ve.CountersRef;
                const GpuInstanceRefs refs  = state.Refs;
                graph.AddPass(
                     std::format( "Particles: Mesh Instances {}.{}", i, vm.Row ), RDG::PassFlags::Compute,
                     [this, floats, layout, counters, refs, history]( RDG::PassBuilder& pass )
                     {
                         // PoolSlots: the slot's Id tells a respawned particle from the one the history row holds.
                         pass.Bindings( m_MeshLayout.Get( m_MeshPipeline->GetSpecification().Shader ),
                                        Renderer::GetPipelineRouteFill( *m_MeshPipeline ) )
                              .Storage( ParticleMeshInstancesShaderParameters::AttributeFloats, floats,
                                        RDG::Access::StorageRead )
                              .Storage( ParticleMeshInstancesShaderParameters::DrawLayout, layout,
                                        RDG::Access::StorageRead )
                              .Storage( ParticleMeshInstancesShaderParameters::AliveList, m_Pool.AliveRef,
                                        RDG::Access::StorageRead )
                              .Storage( ParticleMeshInstancesShaderParameters::Counters, counters,
                                        RDG::Access::StorageRead )
                              .Storage( ParticleMeshInstancesShaderParameters::Instances, refs.Instances,
                                        RDG::Access::StorageWrite )
                              .Storage( ParticleMeshInstancesShaderParameters::Args, refs.Args,
                                        RDG::Access::StorageWrite )
                              .Storage( ParticleMeshInstancesShaderParameters::History, history,
                                        RDG::Access::StorageWrite )
                              .Storage( ParticleMeshInstancesShaderParameters::PoolSlots, m_Pool.SlotsRef,
                                        RDG::Access::StorageRead )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( MeshPush ) ) );
                     },
                     [this, push, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( &push, sizeof( push ) );
                         return Renderer::GetInstance().DispatchCompute( bindings, *m_MeshPipeline, groups, 1, 1 );
                     } );
                // The emitter's level (UE Niagara Mesh Renderer, LOD by screen size): the instances pass reduced
                // the finest level any particle's bounds asks for; one thread writes that level's index ranges
                // into every record, so the GBuffer and the cascades draw it, and re-arms the reduction.
                push.Lod.z = 1u;
                graph.AddPass(
                     std::format( "Particles: Mesh LOD {}.{}", i, vm.Row ), RDG::PassFlags::Compute,
                     [this, floats, layout, counters, refs, history]( RDG::PassBuilder& pass )
                     {
                         pass.Bindings( m_MeshLayout.Get( m_MeshPipeline->GetSpecification().Shader ),
                                        Renderer::GetPipelineRouteFill( *m_MeshPipeline ) )
                              .Storage( ParticleMeshInstancesShaderParameters::AttributeFloats, floats,
                                        RDG::Access::StorageRead )
                              .Storage( ParticleMeshInstancesShaderParameters::DrawLayout, layout,
                                        RDG::Access::StorageRead )
                              .Storage( ParticleMeshInstancesShaderParameters::AliveList, m_Pool.AliveRef,
                                        RDG::Access::StorageRead )
                              .Storage( ParticleMeshInstancesShaderParameters::Counters, counters,
                                        RDG::Access::StorageRead )
                              .Storage( ParticleMeshInstancesShaderParameters::Instances, refs.Instances,
                                        RDG::Access::StorageWrite )
                              .Storage( ParticleMeshInstancesShaderParameters::Args, refs.Args,
                                        RDG::Access::StorageWrite )
                              .Storage( ParticleMeshInstancesShaderParameters::History, history,
                                        RDG::Access::StorageWrite )
                              .Storage( ParticleMeshInstancesShaderParameters::PoolSlots, m_Pool.SlotsRef,
                                        RDG::Access::StorageRead )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( MeshPush ) ) );
                     },
                     [this, push]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( &push, sizeof( push ) );
                         return Renderer::GetInstance().DispatchCompute( bindings, *m_MeshPipeline, 1, 1, 1 );
                     } );

                // Each material section drawn by the GBuffer and the cascades through its Instanced cell; the
                // view-variant keys sit above the grass's (Key * 64 + section, kept below 2^31 / kMaxCascades).
                for ( uint32_t s = 0; s < static_cast<uint32_t>( state.Sections.size() ); ++s )
                    meshes.SubmitGpuInstanced( { .Mesh               = state.Mesh,
                                                 .Path               = MeshVertexPath::Instanced,
                                                 .Material           = state.Sections[s].Material,
                                                 .HiddenSubmeshes    = state.Sections[s].HiddenSubmeshes,
                                                 .CastShadows        = true,
                                                 .Instances          = state.Instances,
                                                 .Refs               = &state.Refs,
                                                 .Key                = 0x08000000u + state.Key * 64u + s,
                                                 .PrevInstanceOffset = state.Capacity } );
            }
        }
    }
} // namespace Desert::Graphic::System

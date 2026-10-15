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
#include <Engine/Graphic/Materials/Mesh/MeshVertexLayout.hpp>
#include <Engine/Graphic/Materials/Mesh/MaterialShadow.hpp>
#include <Engine/Graphic/PipelineCache.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRendererInternal.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Core/Frustum.hpp>
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

        // ParticleMeshDrawArgs.shader's PushConstants: the section (index count, first index, vertex offset, draw
        // slot) and where its command goes (in uints).
        struct ParticleMeshArgsPush
        {
            glm::uvec4 Section;
            glm::uvec4 Out;
        };

        // One VkDrawIndexedIndirectCommand (five uints), the stride of the view's MeshDrawArgs.
        constexpr uint32_t kMeshDrawCommandBytes = 5u * sizeof( uint32_t );

        using Assets::Serialization::VFXRendererKind;

        // The vertex path, default template and material usage of each renderer kind (UE: the renderer
        // properties' vertex factory, default material and the material usage flag it needs).
        MeshVertexPath PathOf( const VFXRendererKind kind )
        {
            switch ( kind )
            {
                case VFXRendererKind::Mesh:
                    return MeshVertexPath::ParticleMesh;
                case VFXRendererKind::Ribbon:
                    return MeshVertexPath::ParticleRibbon;
                case VFXRendererKind::Sprite:
                    break;
            }
            return MeshVertexPath::ParticleSprite;
        }

        std::string_view DefaultTemplateOf( const VFXRendererKind kind )
        {
            switch ( kind )
            {
                case VFXRendererKind::Mesh:
                    return "ParticleMeshDefault";
                case VFXRendererKind::Ribbon:
                    return "ParticleRibbonDefault";
                case VFXRendererKind::Sprite:
                    break;
            }
            return "ParticleSpriteDefault";
        }

        std::string_view UsageOf( const VFXRendererKind kind )
        {
            switch ( kind )
            {
                case VFXRendererKind::Mesh:
                    return "ParticleMeshes";
                case VFXRendererKind::Ribbon:
                    return "ParticleRibbons";
                case VFXRendererKind::Sprite:
                    break;
            }
            return "ParticleSprites";
        }

        // A row's key among its entity's draws: the emitter index and the row's index in that emitter's Renderers.
        uint64_t DrawKeyOf( const uint32_t emitter, const uint32_t row )
        {
            return ( uint64_t{ emitter } << 32u ) | row;
        }

        // A cell's Materials rows of this frame, into its storage block (nothing to write for a cell without rows
        // or, an opaque shadow-depth cell, without the block).
        void UploadRows( DataDrivenMaterial& material, const std::vector<glm::vec4>& rows )
        {
            if ( rows.empty() )
                return;
            if ( auto* sb = material.Get<StorageBufferProperty>( "Materials" ) )
                sb->SetRawData( rows.data(), static_cast<uint32_t>( rows.size() * sizeof( glm::vec4 ) ) );
        }

        bool IsRegisteredShader( const std::string_view name )
        {
            return Runtime::ResourceRegistry::GetShaderService()->GetByName( std::string( name ) ) != nullptr;
        }
    } // namespace

    ParticleRenderer::~ParticleRenderer() = default;

    Common::BoolResultStr ParticleRenderer::Initialize()
    {
        if ( !CreatePipelines() )
            return Common::MakeError( "ParticleRenderer: failed to create pipelines (missing shaders?)" );
        // The cells every row without a drawable material falls back to, one per renderer kind: no default, no
        // particles of that kind.
        for ( const VFXRendererKind kind :
              { VFXRendererKind::Sprite, VFXRendererKind::Ribbon, VFXRendererKind::Mesh } )
        {
            const auto cell = TemplateCellShader( DefaultTemplateOf( kind ), PathOf( kind ), MeshPass::Forward,
                                                  IsRegisteredShader );
            if ( !cell )
                return Common::MakeFormattedError<bool>(
                     "ParticleRenderer: the {} template has no registered {}.Forward cell",
                     DefaultTemplateOf( kind ), MeshVertexPathName( PathOf( kind ) ) );
            m_DefaultCells[static_cast<std::size_t>( kind )] = *cell;
        }
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
        for ( const auto& [name, into] :
              { std::pair{ "ParticleCompact", &m_CompactPipeline },
                std::pair{ "ParticleDispatchArgs", &m_ArgsPipeline }, std::pair{ "ParticleSort", &m_SortPipeline },
                std::pair{ "ParticleMeshDrawArgs", &m_MeshArgsPipeline } } )
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

        // The draw pipelines are per material cell, built on first use (CellDrawFor).
        return true;
    }

    void ParticleRenderer::OnSceneReplaced()
    {
        m_ViewEmitters.clear();
        m_ViewDraws.clear();
        m_World     = nullptr;
        m_Simulates = false;
        if ( m_Materials.empty() )
            return;
        std::size_t rows = 0;
        for ( const auto& [entity, draws] : m_Materials )
            rows += draws.size();
        LOG_INFO( "[Particles] Released {} renderer row material instance(s) of {} emitter(s) belonging to the "
                  "previous scene.",
                  rows, m_Materials.size() );
        m_Materials.clear();
    }

    void ParticleRenderer::PrepareFrame( const ::Desert::Core::Scene& scene )
    {
        m_ViewEmitters.clear();
        m_ViewDraws.clear();

        ParticleWorldGpu& world = ParticleWorldGpu::Of( scene.GetVFXWorld() );
        m_World                 = &world;
        m_Simulates             = world.PrepareTick( scene );

        // This view's materials follow the scene's emitters: a destroyed emitter's goes through the allocator's
        // deletion ring with it, so the frame still reading it finishes first.
        RetireDestroyedEmitters( m_Materials, scene.GetRegistry() );
        m_ViewEmitters.reserve( world.FrameEmitters().size() );
        auto* meshes = Runtime::ResourceRegistry::GetMeshService();
        // This view culls each emitter by its fixed bounds (UE: a GPU emitter's FixedBounds against the view). No
        // camera: nothing to cull against, and nothing is drawn either (the passes need one).
        std::optional<Core::Frustum> viewFrustum;
        if ( m_SceneRenderer != nullptr && m_SceneRenderer->GetMainCamera() != nullptr )
            viewFrustum = m_SceneRenderer->GetMainCamera()->GetFrustum();
        for ( const ParticleFrameEmitter& fe : world.FrameEmitters() )
        {
            const auto  emitter = static_cast<uint32_t>( m_ViewEmitters.size() );
            ViewEmitter ve;
            ve.Frame  = &fe;
            ve.InView = !viewFrustum || viewFrustum->Intersects( fe.WorldBounds );
            m_ViewEmitters.push_back( ve );
            // Every enabled renderer row is one draw (UE FNiagaraRenderer), its material kept per row.
            auto& materials = m_Materials[fe.EntityId];
            for ( uint32_t r = 0; r < static_cast<uint32_t>( fe.Gpu->Renderers.size() ); ++r )
            {
                const ParticleRendererGpu& renderer = fe.Gpu->Renderers[r];
                DrawMaterial&              material = materials[DrawKeyOf( fe.Gpu->Index, renderer.Row )];
                ViewDraw                   draw;
                draw.Emitter       = emitter;
                draw.RendererIndex = r;
                draw.Renderer      = &renderer;
                draw.Cell          = ResolveCell( fe.EntityId, renderer, material );
                draw.Instance      = draw.Cell != nullptr ? material.Instance.get() : nullptr;
                if ( renderer.Kind == VFXRendererKind::Mesh && meshes != nullptr )
                {
                    // Streamed (MeshService::Get is null until resident): the row draws from the frame its mesh
                    // is. A mesh that never will be (its read or build failed, or no such mesh) is said once by
                    // name.
                    draw.StaticMesh = meshes->Get( renderer.Mesh );
                    if ( draw.StaticMesh == nullptr )
                    {
                        const auto residency = meshes->GetResidency( renderer.Mesh );
                        if ( residency != Runtime::MeshService::Residency::Pending &&
                             m_MissingMeshes.emplace( static_cast<uint64_t>( renderer.Mesh ), true ).second )
                        {
                            LOG_ERROR(
                                 "[Particles] emitter {} renderer row {} (Mesh) draws nothing: its mesh '{}' ({}) "
                                 "{}",
                                 fe.EntityId, renderer.Row, renderer.MeshPath,
                                 static_cast<uint64_t>( renderer.Mesh ),
                                 residency == Runtime::MeshService::Residency::Failed
                                      ? "failed to load (MeshService said why above)"
                                      : "is no mesh the content registry knows" );
                        }
                    }
                    if ( draw.StaticMesh != nullptr )
                        for ( const auto& sub : draw.StaticMesh->GetSubmeshes() )
                        {
                            const bool lod = !sub.LODs.empty();
                            draw.Sections.push_back( { lod ? sub.LODs[0].IndexCount : sub.IndexCount,
                                                       lod ? sub.LODs[0].IndexOffset : sub.IndexOffset,
                                                       sub.VertexOffset, sub.Transform } );
                        }
                }
                ResolveOpaqueCells( draw );
                m_ViewDraws.push_back( std::move( draw ) );
            }
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
                 .Storage( "PoolSlots", m_Pool.SlotsRef, RDG::Access::StorageWrite )
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
                 .Storage( "PoolSlots", m_Pool.SlotsRef, RDG::Access::StorageWrite )
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
            ve.Declared     = false;
            ve.ViewSorted   = false;
            ve.RibbonSorted = false;
        }
        for ( ViewDraw& draw : m_ViewDraws )
            draw.Declared = false;
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
            if ( drawn )
                drawn = Renderer::ImportBuffer( gpu.DrawLayout, ve.DrawLayoutImport );
            if ( drawn )
                drawn = Renderer::ImportBuffer( gpu.AttributeInts, ve.IntsImport );
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
            ve.DrawLayoutRef =
                 graph.RegisterExternal( ve.DrawLayoutImport, std::format( "ParticleDrawLayout{}", i ) );
            ve.IntsRef  = graph.RegisterExternal( ve.IntsImport, std::format( "VFXAttributeInts{}", i ) );
            ve.Declared = true;
        }

        // Each row's settings (every particle vertex path reads them: facing, UV tiling, the tick's seconds).
        for ( std::size_t d = 0; d < m_ViewDraws.size(); ++d )
        {
            ViewDraw& draw = m_ViewDraws[d];
            if ( !m_ViewEmitters[draw.Emitter].Declared )
                continue;
            const ParticleEmitterGpu& gpu = *m_ViewEmitters[draw.Emitter].Frame->Gpu;
            if ( const Common::BoolResultStr imported =
                      Renderer::ImportBuffer( gpu.RendererParams[draw.RendererIndex], draw.RendererParamsImport );
                 !imported )
            {
                LOG_ERROR(
                     "[Particles] emitter {} renderer row {} is not drawn this frame, its settings are not in "
                     "the frame graph: {}",
                     draw.Emitter, draw.Renderer->Row, imported.GetError() );
                continue;
            }
            draw.RendererParamsRef =
                 graph.RegisterExternal( draw.RendererParamsImport, std::format( "ParticleRendererParams{}", d ) );
            draw.Declared = true;
        }
    }

    void ParticleRenderer::AddSortPasses( RDG::Builder& graph )
    {
        m_SortKeysRef    = {};
        m_SortedRef      = {};
        m_RibbonOrderRef = {};
        if ( !m_Pool.Declared || !m_SortPipeline || m_World == nullptr || m_SceneRenderer == nullptr )
            return;
        const ViewFrame* view = m_SceneRenderer->GetViewFrame();
        if ( view == nullptr )
            return; // ParticlePass draws nothing either

        // Per emitter, the orders its drawable rows read: a blended sprite / mesh row the view order (one per
        // emitter, shared by its blended rows), a ribbon row the ribbon order.
        std::vector<Core::Formats::SurfaceBlendMode> viewBlend( m_ViewEmitters.size(),
                                                                Core::Formats::SurfaceBlendMode::Opaque );
        std::vector<uint8_t>                         ribbon( m_ViewEmitters.size(), 0u );
        for ( const ViewDraw& draw : m_ViewDraws )
        {
            if ( !IsDrawable( draw ) )
                continue;
            // A ribbon's order is the view's nodes' and the light views' alike (RibbonID, then age): planned when
            // the view sees the emitter or the row casts (an opaque ribbon); the back-to-front order is the view's
            // alone.
            const bool inView = m_ViewEmitters[draw.Emitter].InView;
            if ( draw.Renderer->Kind == VFXRendererKind::Ribbon )
            {
                if ( inView || !draw.ShadowShader.empty() )
                    ribbon[draw.Emitter] = 1u;
            }
            else if ( inView && ParticleSpritesSort( draw.Cell->Blend ) )
                viewBlend[draw.Emitter] = draw.Cell->Blend;
        }

        // The drawn half is the one the last compact (index StepCount) filled: alive half h = StepCount & 1 at
        // [2 Base + h Count, 2 Base + (h + 1) Count), draw slot h.
        std::vector<ParticleSortEmitter> emitters;
        for ( uint32_t i = 0; i < static_cast<uint32_t>( m_ViewEmitters.size() ); ++i )
        {
            const ViewEmitter& ve = m_ViewEmitters[i];
            if ( !ve.Declared )
                continue;
            const ParticleEmitterGpu& gpu   = *ve.Frame->Gpu;
            const uint32_t            half  = ve.Frame->StepCount & 1u;
            const uint32_t            alive = 2u * gpu.Range.Base + half * gpu.Range.Count;
            if ( ParticleSpritesSort( viewBlend[i] ) )
                emitters.push_back( { i, viewBlend[i], gpu.Range.Count, alive, half, ParticleSortKind::View } );
            if ( ribbon[i] != 0u )
                emitters.push_back( { i, Core::Formats::SurfaceBlendMode::Opaque, gpu.Range.Count, alive, half,
                                      ParticleSortKind::Ribbon } );
        }
        const ParticleSortPlan plan = PlanParticleSort( emitters );
        if ( plan.Ranges.empty() )
            return;

        const uint64_t aliveBytes              = uint64_t{ 2 } * m_World->Pool().Capacity * sizeof( uint32_t );
        std::tie( m_SortKeysRef, m_SortedRef ) = CreateParticleSortBuffers( graph, plan, aliveBytes );
        if ( std::ranges::any_of( plan.Ranges, []( const ParticleSortRange& r )
                                  { return r.Kind == ParticleSortKind::Ribbon; } ) )
            m_RibbonOrderRef = CreateParticleRibbonOrderBuffer( graph, aliveBytes );
        // The view depth is along the camera's forward axis from its position (cm): ParticleSortViewOf.
        const ParticleSortView sortView = ParticleSortViewOf( view->InvView );
        const glm::vec4        origin( sortView.Origin, 0.0f );
        const glm::vec4        forward( sortView.Forward, 0.0f );
        for ( const ParticleSortRange& range : plan.Ranges )
        {
            ViewEmitter& ve                               = m_ViewEmitters[range.Emitter];
            const bool   ribbons                          = range.Kind == ParticleSortKind::Ribbon;
            ( ribbons ? ve.RibbonSorted : ve.ViewSorted ) = true;
            const ParticleSortBuffers            buffers{ ve.FloatsRef,    ve.DrawLayoutRef,
                                               m_Pool.AliveRef, ve.CountersRef,
                                               m_SortKeysRef,   ribbons ? m_RibbonOrderRef : m_SortedRef,
                                               ve.IntsRef };
            const std::vector<ParticleSortStage> stages = ParticleSortStages( range.Length );
            for ( uint32_t s = 0; s < static_cast<uint32_t>( stages.size() ); ++s )
            {
                const ParticleSortStage stage = stages[s];
                const ParticleSortPush  push{
                     glm::uvec4( range.AliveOffset, range.Slot, range.KeyBase, range.Length ),
                     glm::uvec4( static_cast<uint32_t>( stage.Kind ), stage.K, stage.J,
                                  static_cast<uint32_t>( range.Kind ) ),
                     origin, forward };
                graph.AddPass(
                     ParticleSortPassName( range.Emitter, s, range.Kind ), RDG::PassFlags::Compute,
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

    void ParticleRenderer::AddMeshDrawArgsPasses( RDG::Builder& graph )
    {
        m_MeshArgsRef = {};
        if ( !m_Pool.Declared || !m_MeshArgsPipeline )
            return;
        uint32_t commands = 0;
        for ( ViewDraw& draw : m_ViewDraws )
        {
            if ( draw.Renderer->Kind != VFXRendererKind::Mesh || !IsDrawable( draw ) )
                continue;
            draw.ArgsBase = commands;
            commands += static_cast<uint32_t>( draw.Sections.size() );
        }
        if ( commands == 0 )
            return;
        m_MeshArgsRef = graph.CreateBuffer( RDG::BufferDesc{ uint64_t{ commands } * kMeshDrawCommandBytes },
                                            "ParticleMeshDrawArgs" );
        for ( uint32_t d = 0; d < static_cast<uint32_t>( m_ViewDraws.size() ); ++d )
        {
            const ViewDraw& draw = m_ViewDraws[d];
            if ( draw.Renderer->Kind != VFXRendererKind::Mesh || !IsDrawable( draw ) )
                continue;
            const ViewEmitter&                ve   = m_ViewEmitters[draw.Emitter];
            const uint32_t                    slot = ve.Frame->StepCount & 1u;
            std::vector<ParticleMeshArgsPush> pushes;
            for ( uint32_t s = 0; s < static_cast<uint32_t>( draw.Sections.size() ); ++s )
            {
                const MeshSection& section = draw.Sections[s];
                pushes.push_back(
                     { glm::uvec4( section.IndexCount, section.FirstIndex, section.VertexOffset, slot ),
                       glm::uvec4( ( draw.ArgsBase + s ) * ( kMeshDrawCommandBytes / sizeof( uint32_t ) ), 0u, 0u,
                                   0u ) } );
            }
            graph.AddPass(
                 std::format( "Particles: Mesh Args {}", d ), RDG::PassFlags::Compute,
                 [this, counters = ve.CountersRef]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_MeshArgsLayout.Get( m_MeshArgsPipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_MeshArgsPipeline ) )
                          .Storage( "Counters", counters, RDG::Access::StorageRead )
                          .Storage( "MeshDrawArgs", m_MeshArgsRef, RDG::Access::StorageWrite )
                          .PushConstantBytes( sizeof( ParticleMeshArgsPush ) );
                 },
                 [this, pushes = std::move( pushes )]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     for ( const ParticleMeshArgsPush& push : pushes )
                     {
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( &push, sizeof( push ) );
                         if ( auto ran = Renderer::GetInstance().DispatchCompute( bindings, *m_MeshArgsPipeline, 1,
                                                                                  1, 1 );
                              !ran )
                             return ran;
                     }
                     return BOOLSUCCESS;
                 } );
        }
    }

    bool ParticleRenderer::IsDrawable( const ViewDraw& draw ) const
    {
        if ( !draw.Declared || draw.Cell == nullptr || !m_ViewEmitters[draw.Emitter].Declared )
            return false;
        if ( draw.Renderer->Kind == VFXRendererKind::Mesh )
            return draw.StaticMesh != nullptr && !draw.Sections.empty();
        return true;
    }

    bool ParticleRenderer::HasDrawArgs( const ViewDraw& draw ) const
    {
        switch ( draw.Renderer->Kind )
        {
            case VFXRendererKind::Ribbon:
                return m_ViewEmitters[draw.Emitter].RibbonSorted;
            case VFXRendererKind::Mesh:
                return m_MeshArgsRef.IsValid();
            case VFXRendererKind::Sprite:
                break;
        }
        return true;
    }

    bool ParticleRenderer::IsDrawn( const ViewDraw& draw ) const
    {
        // An opaque / masked row is the base passes' and the shadow views' (ResolveOpaqueCells), never this one's.
        return IsDrawable( draw ) && !draw.Opaque && m_ViewEmitters[draw.Emitter].InView && HasDrawArgs( draw );
    }

    bool ParticleRenderer::IsDrawnOpaque( const ViewDraw& draw ) const
    {
        return draw.GBufferCell != nullptr && IsDrawable( draw ) && m_ViewEmitters[draw.Emitter].InView &&
               HasDrawArgs( draw );
    }

    bool ParticleRenderer::IsDrawnBase( const ViewDraw& draw ) const
    {
        return draw.BaseCell != nullptr && IsDrawable( draw ) && m_ViewEmitters[draw.Emitter].InView &&
               HasDrawArgs( draw );
    }

    bool ParticleRenderer::CastsShadow( const ViewDraw& draw ) const
    {
        return !draw.ShadowShader.empty() && IsDrawable( draw ) && HasDrawArgs( draw );
    }

    bool ParticleRenderer::CanCast( const ViewDraw& draw ) const
    {
        if ( draw.ShadowShader.empty() || draw.Cell == nullptr )
            return false;
        return draw.Renderer->Kind != VFXRendererKind::Mesh ||
               ( draw.StaticMesh != nullptr && !draw.Sections.empty() );
    }

    bool ParticleRenderer::InFrustum( const ViewDraw& draw, const Core::Frustum& frustum ) const
    {
        return frustum.Intersects( m_ViewEmitters[draw.Emitter].Frame->WorldBounds );
    }

    LocalShadowReach ParticleRenderer::LocalShadowReachOf( const glm::vec3& centre, const float radius ) const
    {
        // Asked after PrepareFrame, before the graph imports anything. A GPU emitter's fixed bounds are all the
        // CPU knows of where its particles are (UE culls a GPU emitter's shadow by them): a light whose sphere
        // reaches the bounds of an emitter with a casting row casts it, and redraws every frame - the particles
        // move. A light no such emitter reaches keeps its cached views.
        LocalShadowReach reach;
        for ( const ViewDraw& draw : m_ViewDraws )
        {
            if ( !CanCast( draw ) )
                continue;
            const Common::Math::AABB& box     = m_ViewEmitters[draw.Emitter].Frame->WorldBounds;
            const glm::vec3           nearest = glm::clamp( centre, box.Min, box.Max );
            const glm::vec3           apart   = nearest - centre;
            if ( glm::dot( apart, apart ) > radius * radius )
                continue;
            reach.Casts  = true;
            reach.Moving = true;
            break;
        }
        return reach;
    }

    void ParticleRenderer::ResolveOpaqueCells( ViewDraw& draw )
    {
        draw.Opaque      = false;
        draw.GBufferCell = nullptr;
        draw.BaseCell    = nullptr;
        draw.RSMCell     = nullptr;
        draw.ShadowShader.clear();
        if ( draw.Cell == nullptr || Core::Formats::IsTranslucentBlend( draw.Cell->Blend ) ||
             m_SceneRenderer == nullptr )
            return;
        // Opaque or masked, whatever the kind: a base pass and shadow draw, never the particle pass's (its Forward
        // cell is the lit one, DShaderParser kSurfaceParticleOpaquePasses; UE draws an opaque sprite, ribbon or
        // mesh particle in the base pass and the shadow depths alike). Whatever is refused below leaves the row
        // undrawn, said.
        draw.Opaque               = true;
        const MeshVertexPath path = PathOf( draw.Renderer->Kind );
        const bool           deferred =
             m_SceneRenderer->GetRenderPath() == Core::RenderPath::Deferred && m_SceneRenderer->GetGBuffer();
        // The same template as the row's Forward cell, another cell of it (MeshShaderFor's rule).
        const std::string_view forward      = draw.Cell->Shader;
        const std::string_view templateName = forward.substr( 0, forward.rfind( '/' ) );
        const auto shadow  = TemplateCellShader( templateName, path, MeshPass::ShadowDepth, IsRegisteredShader );
        const auto gbuffer = deferred
                                  ? TemplateCellShader( templateName, path, MeshPass::GBuffer, IsRegisteredShader )
                                  : std::optional<std::string>{};
        if ( !shadow || ( deferred && !gbuffer ) )
        {
            const MeshPass missing = shadow ? MeshPass::GBuffer : MeshPass::ShadowDepth;
            if ( m_RefusedOpaque.emplace( std::string( templateName ), true ).second )
            {
                LOG_ERROR(
                     "[Particles] the opaque particle template '{}' has no {} cell: its {} rows are not drawn",
                     templateName, MeshCellFor( path, missing ), MeshVertexPathName( path ) );
            }
            return;
        }
        draw.ShadowShader = *shadow;
        // Null (a refused pipeline, said by CellDrawFor) leaves the row undrawn.
        if ( deferred )
        {
            draw.GBufferCell = CellDrawFor( *gbuffer, path, CellNode::GBuffer );
            return;
        }
        // The forward path lights the row with the mesh renderer's frame state (BasePass): without one it cannot.
        if ( m_MeshRenderer.expired() )
        {
            if ( m_RefusedOpaque.emplace( std::format( "{}#forward", templateName ), true ).second )
                LOG_ERROR(
                     "[Particles] the opaque particle rows of template '{}' are not drawn on the forward path: "
                     "the view has no mesh renderer to light them with",
                     templateName );
            return;
        }
        draw.BaseCell = CellDrawFor( draw.Cell->Shader, path, CellNode::Base );
    }

    template <typename Declaration>
    auto ParticleRenderer::DeclareDrawBlock( Declaration& declared, const ViewDraw& draw,
                                             const CellDraw& cell ) const
    {
        const ViewEmitter&    ve     = m_ViewEmitters[draw.Emitter];
        const VFXRendererKind kind   = draw.Renderer->Kind;
        const auto&           layout = cell.Layout.Get( cell.Pipeline->GetSpecification().Shader );
        auto block = declared.Bindings( layout, cell.Material->GetMaterialExecutor()->GetRouteFill() )
                          .Storage( "AttributeFloats", ve.FloatsRef, RDG::Access::StorageRead )
                          .Storage( "DrawLayout", ve.DrawLayoutRef, RDG::Access::StorageRead )
                          .Storage( "AliveList", ListFor( draw, cell ), RDG::Access::StorageRead )
                          .Storage( "RendererParams", draw.RendererParamsRef, RDG::Access::StorageRead );
        if ( kind == VFXRendererKind::Ribbon )
            block.Storage( "AttributeInts", ve.IntsRef, RDG::Access::StorageRead )
                 .Storage( "Counters", ve.CountersRef, RDG::Access::StorageRead );
        // Named, not a conditional inline in the by-value argument (the MSVC 19.44 hang of 2026-10-07).
        const RDG::BufferRef args = kind == VFXRendererKind::Mesh ? m_MeshArgsRef : ve.CountersRef;
        declared.Read( args, RDG::Access::IndirectArgs );
        return block;
    }

    RDG::BufferRef ParticleRenderer::ListFor( const ViewDraw& draw, const CellDraw& cell ) const
    {
        // A blended sprite / mesh row reads its alive half back to front from this view's SortedAlive, a ribbon
        // row (blended or not) the view's RibbonOrder (AddSortPasses), both at the alive list's offsets; an opaque
        // / masked sprite or mesh row reads AliveList unsorted.
        if ( draw.Renderer->Kind == VFXRendererKind::Ribbon )
            return m_RibbonOrderRef;
        return m_ViewEmitters[draw.Emitter].ViewSorted && ParticleSpritesSort( cell.Blend ) ? m_SortedRef
                                                                                            : m_Pool.AliveRef;
    }

    Common::BoolResultStr ParticleRenderer::RecordDraw( const RDG::PassBindings& bindings, const ViewDraw& draw,
                                                        const CellDraw& cell ) const
    {
        if ( draw.Renderer->Kind == VFXRendererKind::Mesh )
            return RecordMeshDraws( bindings, draw, cell );
        // Sprite and ribbon: six vertices per alive particle of alive half StepCount & 1, from the slot the last
        // compact (index StepCount) filled (the slot's FirstVertex). They face the VIEW's camera in every node,
        // its camera-to-world in the push block's Transform (Vertex_ParticleSprite.glslh): a light view draws a
        // sprite as the camera sees it, so its shadow is the silhouette on screen (UE: the shadow's dependent
        // view).
        const ViewFrame* view = m_SceneRenderer->GetViewFrame();
        if ( view == nullptr )
            return BOOLSUCCESS; // no view: the Declares declared nothing either
        cell.Material->SetPushMatrix( view->InvView );
        const ViewEmitter& ve   = m_ViewEmitters[draw.Emitter];
        const uint64_t     slot = ( ve.Frame->StepCount & 1u ) * kParticleDrawSlotStride;
        return Renderer::DrawProceduralIndirect( bindings, *cell.Pipeline, cell.Material->GetMaterialExecutor(),
                                                 ve.CountersRef, slot );
    }

    Common::BoolResultStr ParticleRenderer::RecordMeshDraws( const RDG::PassBindings& bindings,
                                                             const ViewDraw& draw, const CellDraw& cell ) const
    {
        // One indexed-indirect draw per section: instances = the alive particles, written by
        // AddMeshDrawArgsPasses; the section's own transform in the push block.
        for ( uint32_t s = 0; s < static_cast<uint32_t>( draw.Sections.size() ); ++s )
        {
            cell.Material->SetPushMatrix( draw.Sections[s].Transform );
            if ( auto drawn = Renderer::DrawMeshIndirect(
                      bindings, *cell.Pipeline, cell.Material->GetMaterialExecutor(), *draw.StaticMesh,
                      m_MeshArgsRef, uint64_t{ draw.ArgsBase + s } * kMeshDrawCommandBytes );
                 !drawn.IsSuccess() )
                return drawn;
        }
        return BOOLSUCCESS;
    }

    ParticleRenderer::CellDraw* ParticleRenderer::CellDrawFor( const std::string&   cellShader,
                                                               const MeshVertexPath path, const CellNode node,
                                                               const uint32_t cascade )
    {
        // The particle pass's state is keyed by the cell alone; a G-buffer / cascade state by node and cascade
        // too.
        std::string key = cellShader;
        if ( node != CellNode::Particles )
            key = std::format( "{}#{}#{}", cellShader, static_cast<uint32_t>( node ), cascade );
        if ( const auto found = m_Cells.find( key ); found != m_Cells.end() )
            return found->second.get(); // null = refused before (said once, below)
        auto& slot = m_Cells[key];

        // The RSM draws the G-buffer cell's DESERT_GBUFFER_RSM permutation (no shading-word output: the RSM's
        // colour slot 2 is unused), held by the pipeline; its material is the plain cell's, as MeshRenderer's RSM
        // material.
        auto* const shaders = Runtime::ResourceRegistry::GetShaderService();
        auto        shader =
             node == CellNode::RSM
                         ? shaders->AcquireVariant( cellShader, ShaderVariant{ .Defines = { "DESERT_GBUFFER_RSM" } } )
                         : shaders->GetByName( cellShader );
        if ( !shader || m_SceneRenderer == nullptr )
        {
            LOG_ERROR( "[Particles] particle cell '{}' will not draw: {}", cellShader,
                       !shader ? "no such shader is registered" : "the view has no scene renderer" );
            return nullptr;
        }
        auto draw      = std::make_unique<CellDraw>();
        draw->Shader   = cellShader;
        draw->Material = std::make_shared<DataDrivenMaterial>( cellShader );
        draw->Instance = draw->Material->CreateInstance();
        if ( !draw->Instance )
        {
            LOG_ERROR( "[Particles] particle cell '{}' will not draw: its material has no instance", cellShader );
            return nullptr;
        }
        draw->Blend = draw->Material->GetSchema().Blend;

        // The sprite and ribbon paths are vertexless; the mesh path reads the static mesh's streams.
        GraphicsPipelineSpecification spec;
        spec.DebugName        = std::format( "{} #{}.{}", cellShader, static_cast<uint32_t>( node ), cascade );
        spec.Shader           = shader;
        spec.DepthTestEnabled = true;
        spec.Topology         = PrimitiveTopology::Triangles;
        if ( path == MeshVertexPath::ParticleMesh )
            spec.Layout = MeshVertexLayout( MeshVertexPath::ParticleMesh );
        switch ( node )
        {
            case CellNode::Particles:
                // Depth-TESTED against the scene (read-only attachment, DrawPass), never written by a particle
                // cell; the cell's template decides over / added / opaque - the one rule the translucent meshes
                // use too.
                spec.TargetLayout   = SceneTargetLayout();
                spec.DepthCompareOp = DepthCompare::CloserOrEqual;
                spec.CullMode       = CullMode::None;
                ApplySurfaceBlendMode( spec, draw->Blend );
                // The pass holds the depth READ-ONLY (SystemRasterPass::DepthReadOnly): no cell writes it.
                spec.DepthWriteEnabled = false;
                break;
            case CellNode::GBuffer:
                // The meshes' G-buffer state (MeshRenderer::SetupGBufferPass): back faces culled unless the
                // template is two-sided (its cell's Cull), the depth tested and WRITTEN, no blend.
                spec.TargetLayout = GBufferLayout();
                spec.CullMode     = CullMode::Back;
                ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
                spec.DepthTestEnabled  = true;
                spec.DepthWriteEnabled = true;
                spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
                spec.BlendEnable       = false;
                break;
            case CellNode::Shadow:
                // The mesh casters' state (MeshRenderer::SetupShadowPass): every face, STANDARD-Z, against the
                // cascade target's formats.
                if ( !m_CascadeFramebuffer )
                {
                    LOG_ERROR( "[Particles] particle cell '{}' will not cast: no cascade target was set",
                               cellShader );
                    return nullptr;
                }
                spec.Framebuffer       = m_CascadeFramebuffer;
                spec.CullMode          = CullMode::None;
                spec.DepthWriteEnabled = true;
                spec.DepthCompareOp    = CompareOp::LessOrEqual;
                spec.BlendEnable       = false;
                break;
            case CellNode::Base:
                // The forward meshes' opaque state (MeshRenderer's static forward pipeline) on the scene target:
                // back faces culled unless the template is two-sided, the depth tested and WRITTEN, velocity at
                // slot 1.
                spec.TargetLayout = SceneTargetLayout();
                spec.CullMode     = CullMode::Back;
                ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
                spec.DepthTestEnabled  = true;
                spec.DepthWriteEnabled = true;
                spec.DepthCompareOp    = DepthCompare::CloserOrEqual;
                spec.BlendEnable       = false;
                break;
            case CellNode::RSM:
                // MeshRenderer's RSM pipeline: the G-buffer state against the RSM's own colour slots (slot 2
                // unused) and depth, STANDARD-Z (drawn through the sun's cascade matrix, cleared to 1).
                spec.TargetLayout = RenderTargetLayout{
                     .ColorFormats = std::vector<std::optional<Core::Formats::ImageFormat>>(
                          ViewTargetFormats::kRSMColourSlots.begin(), ViewTargetFormats::kRSMColourSlots.end() ),
                     .DepthFormat = ViewTargetFormats::kRSMDepth };
                spec.CullMode = CullMode::Back;
                ApplyShaderRenderState( spec, shader->GetProgramMeta().State );
                spec.DepthTestEnabled  = true;
                spec.DepthWriteEnabled = true;
                spec.DepthCompareOp    = CompareOp::LessOrEqual;
                spec.BlendEnable       = false;
                break;
        }
        // A sprite faces the camera and a ribbon turns along its path: neither has a back to cull, in any node.
        if ( path != MeshVertexPath::ParticleMesh )
            spec.CullMode = CullMode::None;

        const auto pipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !pipeline )
        {
            LOG_ERROR( "[Particles] particle cell '{}' will not draw: {}", cellShader, pipeline.GetError() );
            return nullptr;
        }
        draw->Pipeline = pipeline.GetValue();
        slot           = std::move( draw );
        return slot.get();
    }

    ParticleRenderer::CellDraw* ParticleRenderer::ResolveCell( const uint32_t             entityId,
                                                               const ParticleRendererGpu& renderer,
                                                               DrawMaterial&              material )
    {
        const MeshVertexPath       path   = PathOf( renderer.Kind );
        const Common::AssetHandle& handle = renderer.Material;
        // A named material draws through its own cell of the row's kind (UE: the renderer's material).
        if ( handle != Common::AssetHandle::Null() )
        {
            auto*       service = Runtime::ResourceRegistry::GetMaterialService();
            const auto* cell =
                 dynamic_cast<const DataDrivenMaterial*>( service->Get( handle, path, MeshPass::Forward ) );
            CellDraw* draw = cell != nullptr ? CellDrawFor( cell->GetShaderName(), path ) : nullptr;
            if ( draw != nullptr )
            {
                if ( !material.Instance || material.Handle != handle )
                {
                    material.Handle   = handle;
                    material.Instance = service->CreateRuntimeInstance( handle, path );
                }
                return draw;
            }
            if ( !material.HasRefused || material.Refused != handle )
            {
                LOG_ERROR( "[Particles] emitter {} renderer row {} ({}) draws with {}: its material {} {}",
                           entityId, renderer.Row, MeshVertexPathName( path ), DefaultTemplateOf( renderer.Kind ),
                           static_cast<uint64_t>( handle ),
                           cell == nullptr ? std::format( "is missing or its template has no `Usage {}` cell",
                                                          UsageOf( renderer.Kind ) )
                                           : std::string( "has a cell that cannot draw (logged above)" ) );
                material.Refused    = handle;
                material.HasRefused = true;
            }
        }
        material.Handle   = Common::AssetHandle::Null();
        material.Instance = nullptr; // the default cell's own defaults
        return CellDrawFor( m_DefaultCells[static_cast<std::size_t>( renderer.Kind )], path );
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
                 if ( m_ViewDraws.empty() )
                     return BOOLSUCCESS;
                 if ( m_SceneRenderer->GetMainCamera() == nullptr || m_SceneRenderer->GetViewFrame() == nullptr )
                     return BOOLSUCCESS;

                 uint32_t block = 0;
                 for ( const ViewDraw& draw : m_ViewDraws )
                 {
                     if ( !IsDrawn( draw ) )
                         continue;
                     // The block is the draw's whether or not it draws: the n-th drawn row declared block n.
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( block++ ) );
                     CellDraw&               cell = *draw.Cell;
                     // Still in the driver (PSO1): not drawn this frame and not a fault — an engine pipeline is
                     // counted by PipelineBuilds, so no gated frame is shown without it. Failed still faults.
                     if ( cell.Pipeline->GetReadiness() == PipelineReadiness::Compiling )
                         continue;
                     cell.Material->SetMaterialIndex( draw.Row );
                     cell.Material->Bind( cell.Instance.get() );
                     if ( auto drawn = RecordDraw( bindings, draw, cell ); !drawn.IsSuccess() )
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

            // Per cell, once per frame and BEFORE its blocks are declared: the rows of the draws through it and
            // the view's camera block, so the route fill is what the draws read.
            for ( auto& [name, cell] : m_Cells )
                if ( cell )
                    cell->Rows.clear();
            for ( ViewDraw& draw : m_ViewDraws )
            {
                if ( !IsDrawn( draw ) )
                    continue;
                MaterialInstance* instance = draw.Instance != nullptr ? draw.Instance : draw.Cell->Instance.get();
                draw.Row                   = MeshRendererDetail::AppendRow(
                     draw.Cell->Rows, MeshRendererDetail::EffectiveRow( draw.Cell->Material.get(), instance ) );
            }
            for ( auto& [name, cell] : m_Cells )
            {
                if ( !cell || cell->Rows.empty() )
                    continue;
                UploadRows( *cell->Material, cell->Rows );
                SceneCameraBind( cell->Material.get(), *view );
            }

            // The scene depth the cells fade against: the single-sample depth (the attachment itself at MSAA 1,
            // read-only in this node), bound only where the cell's layout samples it.
            const std::shared_ptr<Image2D> depth = m_SceneRenderer->GetComputeSceneDepth();
            for ( const ViewDraw& draw : m_ViewDraws )
            {
                if ( !IsDrawn( draw ) )
                    continue;
                const CellDraw& cell   = *draw.Cell;
                const auto&     layout = cell.Layout.Get( cell.Pipeline->GetSpecification().Shader );
                auto            block  = DeclareDrawBlock( declared, draw, cell );
                if ( SceneViewDetail::LayoutSamples( *layout, "u_SceneDepth" ) && depth )
                    block.Sampled( "u_SceneDepth", depth, RDG::Access::SampledGraphics,
                                   RDG::SamplerDesc::PointClamp(), "SceneDepth.Compute" );
            }
        };
        return pass;
    }

    SystemRasterPass ParticleRenderer::GBufferPass()
    {
        if ( m_SceneRenderer == nullptr || !m_SceneRenderer->GetGBuffer() )
            return {};

        SystemRasterPass pass{
             .Name        = "Particles: GBuffer",
             .ExecuteFunc = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
             {
                 if ( m_SceneRenderer->GetMainCamera() == nullptr || m_SceneRenderer->GetViewFrame() == nullptr )
                     return BOOLSUCCESS; // the Declare declared nothing either
                 uint32_t block = 0;
                 for ( const ViewDraw& draw : m_ViewDraws )
                 {
                     if ( !IsDrawnOpaque( draw ) )
                         continue;
                     // The block is the draw's whether or not it draws: the n-th drawn row declared block n.
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( block++ ) );
                     const CellDraw&         cell = *draw.GBufferCell;
                     if ( cell.Pipeline->GetReadiness() == PipelineReadiness::Compiling )
                         continue; // PSO1, as in ParticlePass
                     cell.Material->SetMaterialIndex( draw.GBufferRow );
                     cell.Material->Bind( cell.Instance.get() );
                     if ( auto drawn = RecordDraw( bindings, draw, cell ); !drawn.IsSuccess() )
                         return drawn;
                 }
                 return BOOLSUCCESS;
             },
             .TargetFramebuffer = m_SceneRenderer->GetGBuffer() };
        pass.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& )
        {
            const ViewFrame* view = m_SceneRenderer->GetViewFrame();
            if ( m_SceneRenderer->GetMainCamera() == nullptr || view == nullptr )
                return;
            // Per G-buffer cell, BEFORE its blocks: this frame's rows of the rows drawn through it, and the view's
            // camera block (the G-buffer cell is a view pass: its velocity reads the view's previous matrices).
            std::vector<CellDraw*> cells;
            for ( ViewDraw& draw : m_ViewDraws )
            {
                if ( !IsDrawnOpaque( draw ) )
                    continue;
                CellDraw* cell = draw.GBufferCell;
                if ( std::ranges::find( cells, cell ) == cells.end() )
                {
                    cell->Rows.clear();
                    cells.push_back( cell );
                }
                MaterialInstance* instance = draw.Instance != nullptr ? draw.Instance : cell->Instance.get();
                draw.GBufferRow            = MeshRendererDetail::AppendRow(
                     cell->Rows, MeshRendererDetail::EffectiveRow( cell->Material.get(), instance ) );
            }
            for ( CellDraw* cell : cells )
            {
                UploadRows( *cell->Material, cell->Rows );
                SceneCameraBind( cell->Material.get(), *view );
            }
            // Opaque: the alive list unsorted (a ribbon its order), ListFor.
            for ( const ViewDraw& draw : m_ViewDraws )
            {
                if ( !IsDrawnOpaque( draw ) )
                    continue;
                (void)DeclareDrawBlock( declared, draw, *draw.GBufferCell );
            }
        };
        return pass;
    }

    SystemRasterPass ParticleRenderer::BasePass()
    {
        auto targetFb = m_TargetFramebuffer.lock();
        if ( !targetFb )
            return {};

        SystemRasterPass pass{
             .Name        = "Particles: Base",
             .ExecuteFunc = [this]( RDG::PassContext& context, const FrameGraphRefs& ) -> Common::BoolResultStr
             {
                 if ( m_SceneRenderer->GetMainCamera() == nullptr || m_SceneRenderer->GetViewFrame() == nullptr ||
                      m_MeshRenderer.expired() )
                     return BOOLSUCCESS; // the Declare declared nothing either
                 uint32_t block = 0;
                 for ( const ViewDraw& draw : m_ViewDraws )
                 {
                     if ( !IsDrawnBase( draw ) )
                         continue;
                     // The block is the draw's whether or not it draws: the n-th drawn row declared block n.
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( block++ ) );
                     const CellDraw&         cell = *draw.BaseCell;
                     if ( cell.Pipeline->GetReadiness() == PipelineReadiness::Compiling )
                         continue; // PSO1, as in ParticlePass
                     cell.Material->SetMaterialIndex( draw.BaseRow );
                     cell.Material->Bind( cell.Instance.get() );
                     if ( auto drawn = RecordDraw( bindings, draw, cell ); !drawn.IsSuccess() )
                         return drawn;
                 }
                 return BOOLSUCCESS;
             },
             .TargetFramebuffer = targetFb };
        pass.Declare = [this]( RenderPassDeclaration& declared, const FrameGraphRefs& refs )
        {
            const ViewFrame* view = m_SceneRenderer->GetViewFrame();
            const auto       mesh = m_MeshRenderer.lock();
            if ( m_SceneRenderer->GetMainCamera() == nullptr || view == nullptr || !mesh )
                return;
            // Per lit cell, BEFORE its blocks: this frame's rows of the rows drawn through it, and the frame state
            // a forward mesh cell is lit with - camera, lights, cascades, environment, cloud shadow, motion rows -
            // from the one snapshot the mesh renderer's own forward draws take (SceneFrameBinding).
            std::vector<CellDraw*> cells;
            for ( ViewDraw& draw : m_ViewDraws )
            {
                if ( !IsDrawnBase( draw ) )
                    continue;
                CellDraw* cell = draw.BaseCell;
                if ( std::ranges::find( cells, cell ) == cells.end() )
                {
                    cell->Rows.clear();
                    cells.push_back( cell );
                }
                MaterialInstance* instance = draw.Instance != nullptr ? draw.Instance : cell->Instance.get();
                draw.BaseRow               = MeshRendererDetail::AppendRow(
                     cell->Rows, MeshRendererDetail::EffectiveRow( cell->Material.get(), instance ) );
            }
            if ( cells.empty() )
                return;
            const SceneFrameBinding frame = mesh->CaptureFrameState( view );
            for ( CellDraw* cell : cells )
            {
                UploadRows( *cell->Material, cell->Rows );
                frame.ApplyTo( cell->Material.get() );
            }
            // The view's scene inputs (the cascades, the sky cubes, the BRDF LUT, the cloud shadow) where the cell
            // samples them: pass parameters, as for the forward meshes (MeshDrawList::DeclareBlocks).
            const SceneViewInputs inputs = SceneViewInputsOf( refs );
            for ( const ViewDraw& draw : m_ViewDraws )
            {
                if ( !IsDrawnBase( draw ) )
                    continue;
                const CellDraw& cell   = *draw.BaseCell;
                const auto&     layout = cell.Layout.Get( cell.Pipeline->GetSpecification().Shader );
                auto            block  = DeclareDrawBlock( declared, draw, cell );
                if ( SamplesSceneViewInputs( *layout ) )
                    BindSceneViewInputs( block, inputs, *layout );
            }
        };
        return pass;
    }

    void ParticleRenderer::DeclareRSMDraws( RDG::PassBuilder& pass )
    {
        for ( ViewDraw& draw : m_ViewDraws )
            draw.RSMCell = nullptr;
        const auto       mesh = m_MeshRenderer.lock();
        const ViewFrame* view = m_SceneRenderer != nullptr ? m_SceneRenderer->GetViewFrame() : nullptr;
        if ( !mesh || view == nullptr || !m_Pool.Declared )
            return;
        // Per RSM cell, BEFORE its blocks: the rows of the rows drawn through it (their albedo IS the bounce's
        // flux colour) and the sun's camera - the matrix the mesh renderer's RSM draws project with, as a still
        // view. The sun's view culls by its own frustum, not the camera's: an emitter off screen still bounces
        // light.
        const Core::Frustum    sunFrustum( mesh->GetRSMViewProj(), glm::mat4( 1.0f ) );
        std::vector<CellDraw*> cells;
        for ( ViewDraw& draw : m_ViewDraws )
        {
            if ( draw.GBufferCell == nullptr || !IsDrawable( draw ) || !HasDrawArgs( draw ) ||
                 !InFrustum( draw, sunFrustum ) )
                continue;
            CellDraw* cell = CellDrawFor( draw.GBufferCell->Shader, PathOf( draw.Renderer->Kind ), CellNode::RSM );
            if ( cell == nullptr )
                continue; // refused, said once by CellDrawFor; RecordRSM skips it alike
            draw.RSMCell = cell;
            if ( std::ranges::find( cells, cell ) == cells.end() )
            {
                cell->Rows.clear();
                cells.push_back( cell );
            }
            MaterialInstance* instance = draw.Instance != nullptr ? draw.Instance : cell->Instance.get();
            draw.RSMRow                = MeshRendererDetail::AppendRow(
                 cell->Rows, MeshRendererDetail::EffectiveRow( cell->Material.get(), instance ) );
        }
        if ( cells.empty() )
            return;
        const ViewFrame sun =
             MakeStillViewFrame( glm::mat4( 1.0f ), mesh->GetRSMViewProj(), mesh->GetRSMEye(), view->TimeSeconds );
        for ( CellDraw* cell : cells )
        {
            UploadRows( *cell->Material, cell->Rows );
            SceneCameraBind( cell->Material.get(), sun );
        }
        for ( const ViewDraw& draw : m_ViewDraws )
        {
            if ( draw.RSMCell == nullptr )
                continue;
            (void)DeclareDrawBlock( pass, draw, *draw.RSMCell );
        }
    }

    Common::BoolResultStr ParticleRenderer::RecordRSM( const RDG::PassContext& context, const uint32_t firstBlock )
    {
        uint32_t block = firstBlock;
        for ( const ViewDraw& draw : m_ViewDraws )
        {
            if ( draw.RSMCell == nullptr )
                continue;
            const RDG::PassBindings bindings( context, context.GetBindingBlock( block++ ) );
            const CellDraw&         cell = *draw.RSMCell;
            if ( cell.Pipeline->GetReadiness() == PipelineReadiness::Compiling )
                continue;
            cell.Material->SetMaterialIndex( draw.RSMRow );
            cell.Material->Bind( cell.Instance.get() );
            if ( auto drawn = RecordDraw( bindings, draw, cell ); !drawn.IsSuccess() )
                return drawn;
        }
        return BOOLSUCCESS;
    }

    uint32_t ParticleRenderer::DeclareShadowCascade( RenderPassDeclaration& declared, const uint32_t cascade )
    {
        const auto mesh = m_MeshRenderer.lock();
        if ( !m_CascadeFramebuffer || !m_Pool.Declared || !mesh )
            return 0;
        // The view's casters' frustum (the mesh casters' cull): a row whose emitter's bounds it does not hold
        // casts nothing there. RecordShadowCascade walks the rows by the same test.
        const Core::Frustum casterFrustum( mesh->GetShadowCasterViewProj( cascade ), glm::mat4( 1.0f ) );
        // Per shadow cell of this cascade, BEFORE its blocks: the rows of the rows cast through it (a masked cell
        // clips by them; an opaque one has no Materials block). Its camera block is the cascade's light, written
        // by RecordShadowCascade, which is handed the cascade's matrix.
        std::vector<CellDraw*> cells;
        for ( ViewDraw& draw : m_ViewDraws )
        {
            if ( !CastsShadow( draw ) || !InFrustum( draw, casterFrustum ) )
                continue;
            CellDraw* cell =
                 CellDrawFor( draw.ShadowShader, PathOf( draw.Renderer->Kind ), CellNode::Shadow, cascade );
            if ( cell == nullptr )
                continue; // refused, said once by CellDrawFor; RecordShadowCascade skips it alike
            if ( std::ranges::find( cells, cell ) == cells.end() )
            {
                cell->Rows.clear();
                cells.push_back( cell );
            }
            if ( draw.ShadowRows.size() <= cascade )
                draw.ShadowRows.resize( cascade + 1u, 0u );
            MaterialInstance* instance = draw.Instance != nullptr ? draw.Instance : cell->Instance.get();
            draw.ShadowRows[cascade]   = MeshRendererDetail::AppendRow(
                 cell->Rows, MeshRendererDetail::EffectiveRow( cell->Material.get(), instance ) );
        }
        for ( CellDraw* cell : cells )
            UploadRows( *cell->Material, cell->Rows );

        uint32_t blocks = 0;
        for ( const ViewDraw& draw : m_ViewDraws )
        {
            if ( !CastsShadow( draw ) || !InFrustum( draw, casterFrustum ) )
                continue;
            const CellDraw* cell =
                 CellDrawFor( draw.ShadowShader, PathOf( draw.Renderer->Kind ), CellNode::Shadow, cascade );
            if ( cell == nullptr )
                continue;
            (void)DeclareDrawBlock( declared, draw, *cell );
            ++blocks;
        }
        return blocks;
    }

    Common::BoolResultStr ParticleRenderer::RecordShadowCascade( const RDG::PassContext& context,
                                                                 const uint32_t cascade, const uint32_t firstBlock,
                                                                 const glm::mat4&         cascadeViewProj,
                                                                 const CascadeCasterBias& bias )
    {
        const auto mesh = m_MeshRenderer.lock();
        if ( !m_CascadeFramebuffer || !m_Pool.Declared || !mesh )
            return BOOLSUCCESS;
        const Core::Frustum casterFrustum( mesh->GetShadowCasterViewProj( cascade ), glm::mat4( 1.0f ) );
        // The frame's clock, as the mesh casters' (WriteLightCamera): a masked mask reading Time clips its shadow
        // at the instant it clips itself.
        const ViewFrame*             view  = m_SceneRenderer->GetViewFrame();
        const double                 clock = view != nullptr ? view->TimeSeconds : 0.0;
        std::vector<const CellDraw*> lit;
        uint32_t                     block = firstBlock;
        for ( const ViewDraw& draw : m_ViewDraws )
        {
            if ( !CastsShadow( draw ) || !InFrustum( draw, casterFrustum ) )
                continue;
            const CellDraw* cell =
                 CellDrawFor( draw.ShadowShader, PathOf( draw.Renderer->Kind ), CellNode::Shadow, cascade );
            if ( cell == nullptr )
                continue;
            const RDG::PassBindings bindings( context, context.GetBindingBlock( block++ ) );
            if ( cell->Pipeline->GetReadiness() == PipelineReadiness::Compiling )
                continue;
            if ( std::ranges::find( lit, cell ) == lit.end() )
            {
                // The cell is this cascade's alone (CellNode::Shadow, cascade): its camera is the cascade's light.
                WriteLightCamera( *cell->Material, glm::mat4( 1.0f ), cascadeViewProj, clock, bias );
                lit.push_back( cell );
            }
            cell->Material->SetMaterialIndex( draw.ShadowRows[cascade] );
            cell->Material->Bind( cell->Instance.get() );
            if ( auto drawn = RecordDraw( bindings, draw, *cell ); !drawn.IsSuccess() )
                return drawn;
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System

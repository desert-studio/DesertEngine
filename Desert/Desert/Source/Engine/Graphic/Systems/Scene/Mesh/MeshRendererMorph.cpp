// MORPH TARGETS BEFORE SKINNING (UE FSkeletalMeshObjectGPUSkin + GPUMorphUpdateCS,
// SkeletalRenderGPUSkin.cpp:211-305). A skinned component drawn with non-zero MorphComponent weights skins from
// its own morphed twin of the mesh (SkinnedMesh::MakeMorphedTwin): the same index buffer, streams and skeleton,
// its own vertices, which the "Mesh: Morph" compute node blends from the base vertices and the mesh's morph
// deltas. The blend runs only when the component's revision moved (UE FMorphVertexBufferPool revision number) - a
// still face costs nothing - and once more the frame after, so the previous positions catch up with the shape.
//
// VELOCITY (UE FMorphVertexBufferPool double buffer, PreviousMorph): the twin's PreviousVertices (MeshGeometry, the
// skinned format's binding 3) hold the positions it was drawn with last frame; the blend moves the output's
// positions there before overwriting them (header w = 1) when the output IS last frame's shape - the twin was
// drawn last frame and holds a completed blend - and writes the new shape into both otherwise (the first-frame /
// teleport rule of the motion rows). The vertex stage that skins without a skin-cache entry reads them; with one,
// the cache rotates its own previous positions from the same output (RayTracingSkinCache.hpp).
#include "MeshRenderer.hpp"

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <span>
#include <vector>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct MeshMorphShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, MorphBaseVertices );
        DESERT_SHADER_PARAMETER( StorageBuffer, MorphDeltas );
        DESERT_SHADER_PARAMETER( StorageBuffer, MorphWeights );
        DESERT_SHADER_PARAMETER( StorageBuffer, MorphOutput );
        DESERT_SHADER_PARAMETER( StorageBuffer, MorphPrevious );
    };
} // namespace

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr const char* kMorphShader    = "MorphTargets";
        constexpr uint32_t    kMorphGroupSize = 64; // LocalSize of Programs/Compute/MorphTargets.shader

        // The MorphWeights buffer of the shader: a uvec4 header (w = the output holds last frame's shape), then
        // (target, weight bits) per active target.
        std::vector<uint32_t> PackActiveWeights( const std::vector<float>& weights, uint32_t targetCount,
                                                 uint32_t vertexCount, const bool history )
        {
            const uint32_t        historyWord = history ? 1u : 0u;
            std::vector<uint32_t> packed      = { vertexCount, 0u, targetCount, historyWord };
            const auto            count  = std::min<size_t>( weights.size(), targetCount );
            for ( size_t k = 0; k < count; ++k )
            {
                if ( weights[k] == 0.0f )
                    continue;
                packed.push_back( static_cast<uint32_t>( k ) );
                packed.push_back( std::bit_cast<uint32_t>( weights[k] ) );
                ++packed[1];
            }
            return packed;
        }

        bool AnyWeight( const std::vector<float>& weights, uint32_t targetCount )
        {
            const auto count = std::min<size_t>( weights.size(), targetCount );
            return std::any_of( weights.begin(), weights.begin() + static_cast<std::ptrdiff_t>( count ),
                                []( float w ) { return w != 0.0f; } );
        }
    } // namespace

    SkinnedMesh* MeshRenderer::MorphedMeshFor( const MeshRenderData& data, SkinnedMesh* base )
    {
        // UE draws a component with no active morph target from the base vertices; so does this.
        if ( base == nullptr || base->GetMorphTargetCount() == 0 || !base->GetMorphDeltas() ||
             !AnyWeight( data.MorphWeights, base->GetMorphTargetCount() ) )
            return base;

        MorphTwin& twin = m_MorphTwins[data.Entity];
        // The base's content revision and geometry span are the twin's identity (the pool's buffers are shared
        // by every mesh, so the buffer alone says nothing).
        if ( !twin.Twin || twin.BaseRevision != base->GetRenderRevision() ||
             twin.BaseGeometry.MeshBinding() != base->GetGeometry().MeshBinding() )
        {
            const auto bytes = static_cast<uint32_t>( base->GetVertexCount() * sizeof( SkinnedVertex ) );
            // The output and last frame's positions: same vertex format and stride (binding 3 reads the previous
            // positions at the skinned stride), only the position words of the second are written.
            std::shared_ptr<VertexBuffer> vertices = VertexBuffer::Create( bytes, BufferUsage::GpuWritten );
            std::shared_ptr<VertexBuffer> previous = VertexBuffer::Create( bytes, BufferUsage::GpuWritten );
            for ( const std::shared_ptr<VertexBuffer>* buffer : { &vertices, &previous } )
            {
                if ( const auto built = ( *buffer )->RT_Invalidate(); !built.IsSuccess() )
                {
                    LOG_ERROR( "[MeshRenderer] entity {}: its morphed vertices could not be allocated, it draws "
                               "unmorphed: {}",
                               data.Entity, built.GetError() );
                    m_MorphTwins.erase( data.Entity );
                    return base;
                }
            }
            twin              = MorphTwin{};
            twin.Base         = base;
            twin.BaseRevision = base->GetRenderRevision();
            twin.BaseGeometry = base->GetGeometry();
            twin.Vertices     = vertices;
            twin.Previous     = previous;
            twin.Twin         = SkinnedMesh::MakeMorphedTwin( *base, std::move( vertices ), std::move( previous ) );
            // New: no frame drew it before this one.
            twin.LastDrawn = m_MorphFrame;
        }

        twin.Base = base; // this frame's draw of it: valid until the frame's passes have run
        // The first draw of the frame decides whether last frame drew it (a second record of the entity this
        // frame finds LastDrawn already moved and keeps that answer).
        if ( twin.LastDrawn != m_MorphFrame )
        {
            twin.DrawnLastFrame = twin.LastDrawn + 1u == m_MorphFrame;
            twin.LastDrawn      = m_MorphFrame;
        }
        const bool changed = !twin.Blended || twin.Revision != data.MorphRevision;
        // A blend is owed when the shape changed, and once more the frame after a blend that moved it: the
        // previous positions still hold the shape before that blend and must catch up, or a still face would
        // keep the last change's velocity.
        if ( changed || twin.PreviousBehind )
        {
            twin.Pending         = true;
            twin.PendingWeights  = data.MorphWeights;
            twin.PendingRevision = data.MorphRevision;
            twin.PendingHistory  = twin.Blended && twin.DrawnLastFrame;
            twin.PendingMoves    = changed && twin.PendingHistory;
        }
        return twin.Twin.get();
    }

    RDG::ExternalBuffer* MeshRenderer::FindMorphOutput( const uint32_t entity, const SkinnedMesh* const mesh )
    {
        const auto found = m_MorphTwins.find( entity );
        if ( found == m_MorphTwins.end() || found->second.Twin.get() != mesh || !found->second.Blended )
            return nullptr;
        return &found->second.OutputImport;
    }

    void MeshRenderer::UpdateMorphTargets()
    {
        // Every twin this frame's queue skins from has a blend, or is put back to its base mesh here: a twin
        // never reaches a draw with vertices nothing wrote.
        const auto unmorph = [this]( MorphTwin& twin, std::string_view why )
        {
            LOG_ERROR( "[MeshRenderer] a morphed skinned mesh draws unmorphed this frame: {}", why );
            for ( SkinnedMeshRenderData& draw : m_SkinnedQueue )
                if ( draw.Mesh == twin.Twin.get() )
                    draw.Mesh = twin.Base;
            // The entity draws its base this frame: neither buffer is the shape it was drawn with, so the next
            // blend starts the history over.
            twin.Blended        = false;
            twin.PreviousBehind = false;
            twin.Pending        = false;
        };

        std::vector<MorphTwin*> pending;
        for ( auto& [entity, twin] : m_MorphTwins )
            if ( twin.Pending )
                pending.push_back( &twin );

        if ( !pending.empty() )
        {
            if ( !m_MorphPipeline )
            {
                Runtime::ShaderService*       service = Runtime::ResourceRegistry::GetShaderService();
                const std::shared_ptr<Shader> shader  = service ? service->GetByName( kMorphShader ) : nullptr;
                if ( shader )
                {
                    if ( const auto built =
                              ComputePipeline::Create( { .Shader = shader, .DebugName = kMorphShader } ) )
                        m_MorphPipeline = built.GetValue();
                    else
                        LOG_ERROR( "[MeshRenderer] the morph pipeline was not built: {}", built.GetError() );
                }
                else
                    LOG_ERROR( "[MeshRenderer] the shader '{}' did not compile - expected "
                               "Editor/Resources/Shaders/Programs/Compute/MorphTargets.shader",
                               kMorphShader );
            }

            if ( !m_MorphPipeline )
            {
                for ( MorphTwin* twin : pending )
                    unmorph( *twin, "no morph pipeline" );
            }
            else
            {
                RDG::Builder                           graph( "MorphTargets" );
                const std::shared_ptr<ComputePipeline> pipeline = m_MorphPipeline;
                const auto&               layout = m_MorphLayout.Get( pipeline->GetSpecification().Shader );
                const std::shared_ptr<const RDG::OtherRouteFill> fill = Renderer::GetPipelineRouteFill( *pipeline );

                std::vector<MorphTwin*> declared;
                for ( MorphTwin* twin : pending )
                {
                    const SkinnedMesh& base = *twin->Twin;
                    if ( const auto in = Renderer::ImportMeshVertices( twin->BaseGeometry, twin->BaseImport );
                         !in )
                    {
                        unmorph( *twin, in.GetError() );
                        continue;
                    }
                    if ( const auto in = Renderer::ImportBuffer( base.GetMorphDeltas(), twin->DeltasImport ); !in )
                    {
                        unmorph( *twin, in.GetError() );
                        continue;
                    }
                    if ( const auto in = Renderer::ImportVertexBuffer( twin->Vertices, twin->OutputImport ); !in )
                    {
                        unmorph( *twin, in.GetError() );
                        continue;
                    }
                    if ( const auto in = Renderer::ImportVertexBuffer( twin->Previous, twin->PreviousImport ); !in )
                    {
                        unmorph( *twin, in.GetError() );
                        continue;
                    }

                    const std::vector<uint32_t> weights =
                         PackActiveWeights( twin->PendingWeights, base.GetMorphTargetCount(), base.GetVertexCount(),
                                            twin->PendingHistory );
                    const RDG::BufferRef baseRef    = graph.RegisterExternal( twin->BaseImport, "Morph.Base" );
                    const RDG::BufferRef deltasRef  = graph.RegisterExternal( twin->DeltasImport, "Morph.Deltas" );
                    const RDG::BufferRef outRef     = graph.RegisterExternal( twin->OutputImport, "Morph.Output" );
                    const RDG::BufferRef prevRef = graph.RegisterExternal( twin->PreviousImport, "Morph.Previous" );
                    const RDG::BufferRef weightsRef = graph.CreateBuffer(
                         RDG::BufferDesc{ weights.size() * sizeof( uint32_t ) }, "Morph.Weights" );
                    graph.QueueBufferUpload( weightsRef, std::as_bytes( std::span( weights ) ) );

                    const uint32_t groups = ( base.GetVertexCount() + kMorphGroupSize - 1 ) / kMorphGroupSize;
                    graph.AddPass(
                         "Mesh: Morph", RDG::PassFlags::Compute,
                         [&]( RDG::PassBuilder& pass )
                         {
                             pass.Bindings( layout, fill )
                                  .Storage( MeshMorphShaderParameters::MorphBaseVertices, baseRef,
                                            RDG::Access::StorageRead )
                                  .Storage( MeshMorphShaderParameters::MorphDeltas, deltasRef,
                                            RDG::Access::StorageRead )
                                  .Storage( MeshMorphShaderParameters::MorphWeights, weightsRef,
                                            RDG::Access::StorageRead )
                                  .Storage( MeshMorphShaderParameters::MorphOutput, outRef,
                                            RDG::Access::StorageWrite )
                                  .Storage( MeshMorphShaderParameters::MorphPrevious, prevRef,
                                            RDG::Access::StorageWrite );
                         },
                         [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                         {
                             const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                             return Renderer::DispatchCompute( bindings, *pipeline, groups, 1, 1 );
                         } );
                    // Handed to vertex input at the graph's end: the view graph's passes bind the twin's vertices
                    // as their vertex stream without declaring them.
                    graph.Extract( outRef, twin->OutputImport, RDG::Access::VertexIndex );
                    // ...and so is the previous-position binding the skinned view cells read (binding 3).
                    graph.Extract( prevRef, twin->PreviousImport, RDG::Access::VertexIndex );
                    declared.push_back( twin );
                }

                if ( !declared.empty() )
                {
                    if ( const auto executed = Renderer::ExecuteGraph( graph ); !executed )
                    {
                        for ( MorphTwin* twin : declared )
                            unmorph( *twin, executed.GetError() );
                    }
                    else
                    {
                        for ( MorphTwin* twin : declared )
                        {
                            twin->Blended        = true;
                            twin->Revision       = twin->PendingRevision;
                            twin->PreviousBehind = twin->PendingMoves;
                            twin->Pending        = false;
                            twin->PendingWeights.clear();
                        }
                    }
                }
            }
        }

        // A twin no draw has asked for in a while is released (its buffer's destruction is deferred past the
        // frames in flight by the allocator).
        std::erase_if( m_MorphTwins, [this]( const auto& entry )
                       { return entry.second.LastDrawn + kMorphTwinIdleFrames < m_MorphFrame; } );
        ++m_MorphFrame;
    }
} // namespace Desert::Graphic::System

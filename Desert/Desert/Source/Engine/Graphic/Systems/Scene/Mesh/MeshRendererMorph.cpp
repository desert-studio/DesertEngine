// MORPH TARGETS BEFORE SKINNING (UE FSkeletalMeshObjectGPUSkin + GPUMorphUpdateCS,
// SkeletalRenderGPUSkin.cpp:211-305). A skinned component drawn with non-zero MorphComponent weights skins from
// its own morphed twin of the mesh (SkinnedMesh::MakeMorphedTwin): the same index buffer, streams and skeleton,
// its own vertices, which the "Mesh: Morph" compute node blends from the base vertices and the mesh's morph
// deltas. The blend runs only when the component's revision moved (UE FMorphVertexBufferPool revision number) - a
// still face costs nothing.
#include "MeshRenderer.hpp"

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <span>
#include <vector>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr const char* kMorphShader    = "MorphTargets";
        constexpr uint32_t    kMorphGroupSize = 64; // LocalSize of Programs/Compute/MorphTargets.shader

        // The MorphWeights buffer of the shader: a uvec4 header, then (target, weight bits) per active target.
        std::vector<uint32_t> PackActiveWeights( const std::vector<float>& weights, uint32_t targetCount,
                                                 uint32_t vertexCount )
        {
            std::vector<uint32_t> packed = { vertexCount, 0u, targetCount, 0u };
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
        // The base's vertex buffer is the twin's identity: held by the twin, it cannot be reused by another mesh.
        if ( !twin.Twins[0] || twin.BaseVertices != base->GetVertexBuffer() )
        {
            const auto bytes = static_cast<uint32_t>( base->GetVertexCount() * sizeof( SkinnedVertex ) );
            std::array<std::shared_ptr<VertexBuffer>, 2> outputs;
            for ( std::shared_ptr<VertexBuffer>& output : outputs )
            {
                output = VertexBuffer::Create( bytes, BufferUsage::GpuWritten );
                if ( const auto built = output->RT_Invalidate(); !built.IsSuccess() )
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
            twin.BaseVertices = base->GetVertexBuffer();
            for ( size_t i = 0; i < outputs.size(); ++i )
            {
                twin.Vertices[i] = outputs[i];
                twin.Twins[i]    = SkinnedMesh::MakeMorphedTwin( *base, std::move( outputs[i] ) );
            }
            // New: no frame of it was drawn before this one.
            twin.LastDrawn = m_MorphFrame;
        }
        else if ( twin.LastDrawn != m_MorphFrame )
        {
            twin.DrawnLastFrame = twin.LastDrawn + 1 == m_MorphFrame;
            twin.LastDrawn      = m_MorphFrame;
        }

        twin.Base = base; // this frame's draw of it: valid until the frame's passes have run
        if ( twin.Blended && twin.Revision == data.MorphRevision )
        {
            // The shape did not change since the blend: the current output is this frame's and last frame's.
            SkinnedMesh& drawn = *twin.Twins[twin.Current];
            drawn.SetPreviousPositionBuffer( nullptr );
            return &drawn;
        }

        // A blend is owed: it writes the other output, and the current one - the shape drawn last frame, when
        // it was drawn and holds a completed blend - is this frame's previous positions.
        twin.Pending         = true;
        twin.PendingWeights  = data.MorphWeights;
        twin.PendingRevision = data.MorphRevision;
        const uint32_t next  = twin.Current ^ 1u;
        SkinnedMesh&   drawn = *twin.Twins[next];
        drawn.SetPreviousPositionBuffer( twin.Blended && twin.DrawnLastFrame ? twin.Vertices[twin.Current]
                                                                             : nullptr );
        return &drawn;
    }

    void MeshRenderer::UpdateMorphTargets()
    {
        // Every twin this frame's queue skins from has a blend, or is put back to its base mesh here: a twin
        // never reaches a draw with vertices nothing wrote.
        const auto unmorph = [this]( MorphTwin& twin, std::string_view why )
        {
            LOG_ERROR( "[MeshRenderer] a morphed skinned mesh draws unmorphed this frame: {}", why );
            for ( SkinnedMeshRenderData& draw : m_SkinnedQueue )
                if ( draw.Mesh == twin.Twins[0].get() || draw.Mesh == twin.Twins[1].get() )
                    draw.Mesh = twin.Base;
            // The entity draws its base this frame: neither output is the shape it was drawn with, so the next
            // blend starts the history over.
            twin.Blended = false;
            twin.Pending = false;
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
                const RDG::OtherRouteFill fill   = Renderer::GetPipelineRouteFill( *pipeline );

                std::vector<MorphTwin*> declared;
                for ( MorphTwin* twin : pending )
                {
                    const uint32_t     next = twin->Current ^ 1u;
                    const SkinnedMesh& base = *twin->Twins[next];
                    if ( const auto in = Renderer::ImportVertexBuffer( twin->BaseVertices, twin->BaseImport );
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
                    if ( const auto in = Renderer::ImportVertexBuffer( twin->Vertices[next], twin->OutputImports[next] );
                         !in )
                    {
                        unmorph( *twin, in.GetError() );
                        continue;
                    }

                    const std::vector<uint32_t> weights = PackActiveWeights(
                         twin->PendingWeights, base.GetMorphTargetCount(), base.GetVertexCount() );
                    const RDG::BufferRef baseRef    = graph.RegisterExternal( twin->BaseImport, "Morph.Base" );
                    const RDG::BufferRef deltasRef  = graph.RegisterExternal( twin->DeltasImport, "Morph.Deltas" );
                    const RDG::BufferRef outRef     = graph.RegisterExternal( twin->OutputImports[next], "Morph.Output" );
                    const RDG::BufferRef weightsRef = graph.CreateBuffer(
                         RDG::BufferDesc{ weights.size() * sizeof( uint32_t ) }, "Morph.Weights" );
                    graph.QueueBufferUpload( weightsRef, std::as_bytes( std::span( weights ) ) );

                    const uint32_t groups = ( base.GetVertexCount() + kMorphGroupSize - 1 ) / kMorphGroupSize;
                    graph.AddPass(
                         "Mesh: Morph", RDG::PassFlags::Compute,
                         [&]( RDG::PassBuilder& pass )
                         {
                             pass.Bindings( layout, fill )
                                  .Storage( "MorphBaseVertices", baseRef, RDG::Access::StorageRead )
                                  .Storage( "MorphDeltas", deltasRef, RDG::Access::StorageRead )
                                  .Storage( "MorphWeights", weightsRef, RDG::Access::StorageRead )
                                  .Storage( "MorphOutput", outRef, RDG::Access::StorageWrite );
                         },
                         [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                         {
                             const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                             return Renderer::DispatchCompute( bindings, *pipeline, groups, 1, 1 );
                         } );
                    // Handed to vertex input at the graph's end: the view graph's passes bind the twin's vertices
                    // as their vertex stream without declaring them.
                    // The output drawn from as previous positions next frame is read the same undeclared way.
                    graph.Extract( outRef, twin->OutputImports[next], RDG::Access::VertexIndex );
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
                            twin->Current ^= 1u; // the output this frame draws
                            twin->Blended  = true;
                            twin->Revision = twin->PendingRevision;
                            twin->Pending  = false;
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

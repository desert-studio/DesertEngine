// SKINNED MESH DISPLACEMENT (CONTRACT-DISPLACE §9.8): the DisplacementCache's skinned entries. The surface is
// tessellated and its heights cooked over the REST pose exactly as a static mesh's (same levels, same canonical
// points, same Heights program), once per (mesh, topology, levels, materials); the deformation is not shared, so
// every frame the skin cache's graph moves that tessellation with the posed base vertices ("Displacement: Pose"
// per weld, "Displacement: Skinned" per twin vertex) into the record's displaced streams, which the raster, the
// velocity pass and the BLAS read as any skinned pose. Baked does not apply: a deforming surface has no single
// baked pose.
#include <Engine/Graphic/Systems/Scene/Mesh/DisplacementCache.hpp>
#include <Common/Core/Core.hpp> // BOOLSUCCESS
#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Graphic/IndexBuffer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <unordered_set>

namespace Desert::Graphic::System
{
    namespace
    {
        // The skinned entry key: the skin cache's {Entity, n-th skinned record of the entity} (KeyRecords).
        uint64_t SkinnedEntryKey( const uint32_t entity, const uint32_t slot )
        {
            return ( static_cast<uint64_t>( entity ) << 32u ) | slot;
        }

        template <typename T>
        std::span<const std::byte> SkinnedBytesOf( const std::vector<T>& values )
        {
            return std::as_bytes( std::span( values ) );
        }

        float SkinnedMaxAxisScale( const glm::mat4& m )
        {
            return std::max( { glm::length( glm::vec3( m[0] ) ), glm::length( glm::vec3( m[1] ) ),
                               glm::length( glm::vec3( m[2] ) ) } );
        }

        // Words per rest SkinnedVertex (Desert::Vertex's 14, bone ids and weights) and per skinned record.
        constexpr uint32_t kSkinnedVertexWords = static_cast<uint32_t>( sizeof( SkinnedVertex ) / 4u );
        constexpr uint32_t kSkinRecordWords    = 7u;
        static_assert( sizeof( SkinnedVertex ) == 88u,
                       "DisplacementEmit.shader writes SkinnedVertex as 22 words" );

        uint64_t SkinnedGeometryBytes( const uint64_t vertices, const uint64_t indices, const bool streams )
        {
            return vertices * ( sizeof( SkinnedVertex ) + ( streams ? sizeof( MeshVertexStreams ) : 0u ) +
                                kSkinRecordWords * sizeof( uint32_t ) ) +
                   indices * sizeof( uint32_t );
        }

        Common::ResultStr<std::shared_ptr<VertexBuffer>> GpuWrittenBuffer( const uint64_t bytes, const char* what )
        {
            auto buffer = VertexBuffer::Create( static_cast<uint32_t>( std::max<uint64_t>( bytes, 16u ) ),
                                                BufferUsage::GpuWritten );
            if ( const auto built = buffer->RT_Invalidate(); !built.IsSuccess() )
                return Common::MakeFormattedError<std::shared_ptr<VertexBuffer>>(
                     "skinned displacement: the {} buffer ({} bytes) could not be allocated: {}", what, bytes,
                     built.GetError() );
            buffer->ClaimOwnership( ResourceOwner::SceneRenderer );
            return Common::MakeSuccess( std::move( buffer ) );
        }
    } // namespace

    bool DisplacementCache::EnsureSkinnedPipelines()
    {
        if ( m_PosePipeline && m_SkinnedPipeline )
            return true;
        Runtime::ShaderService* service = Runtime::ResourceRegistry::GetShaderService();
        for ( const auto& [name, pipeline] : { std::pair{ kDisplacementPoseShader, &m_PosePipeline },
                                               std::pair{ kDisplacementSkinnedShader, &m_SkinnedPipeline } } )
        {
            if ( *pipeline )
                continue;
            const std::shared_ptr<Shader> shader = service ? service->GetByName( std::string( name ) ) : nullptr;
            if ( !shader )
            {
                if ( !m_PipelineRefusalLogged )
                    LOG_ERROR(
                         "[Displacement] the shader '{}' did not compile (Programs/Compute/{}.shader): skinned "
                         "meshes draw their base surface",
                         name, name );
                m_PipelineRefusalLogged = true;
                return false;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = std::string( name ) } );
            if ( !built )
            {
                if ( !m_PipelineRefusalLogged )
                    LOG_ERROR( "[Displacement] the pipeline '{}' was not built: skinned meshes draw their base "
                               "surface: {}",
                               name, built.GetError() );
                m_PipelineRefusalLogged = true;
                return false;
            }
            *pipeline = built.GetValue();
        }
        return true;
    }

    void DisplacementCache::RequestSkinned( const uint32_t entity, const uint32_t slot, const SkinnedMesh& base,
                                            const glm::mat4& objectToWorld, const std::span<const glm::mat4> bones,
                                            std::span<const std::optional<DisplacementMaterialRecord>> materials,
                                            const Desert::Geometry::LODView& view, const float viewHeightPixels,
                                            const uint64_t frame )
    {
        if ( !m_Quality.Enabled || !base.GetDisplacementTopology() )
            return;
        const auto                                    topologyPtr = base.GetDisplacementTopology();
        const Desert::Geometry::DisplacementTopology& topology    = *topologyPtr;
        const std::vector<Submesh>&                   submeshes   = base.GetSubmeshes();

        SkinnedEntry& entry = m_SkinnedEntries[SkinnedEntryKey( entity, slot )];
        if ( entry.Mesh != &base || entry.Topology != topologyPtr )
        {
            if ( entry.Current )
                RetireSkinnedTwin( entry.Current->Twin );
            entry          = SkinnedEntry{}; // another mesh in this slot: nothing of the old entry applies
            entry.Mesh     = &base;
            entry.Topology = topologyPtr;
            // The bone boxes that can move each cluster, cut to the cluster: a blend of bone transforms of a
            // vertex lies in the hull of those transforms of it, so the union of each box under its bone's matrix
            // bounds the posed cluster (the SkinnedPoseBounds rule, per cluster).
            entry.ClusterBones.resize( topology.Clusters.size() );
            for ( uint32_t c = 0; c < topology.Clusters.size(); ++c )
            {
                const Desert::Geometry::DisplacementCluster& cluster = topology.Clusters[c];
                for ( const SkinnedMesh::BoneBounds& bone : base.GetBoneBounds() )
                {
                    if ( bone.Submesh != cluster.Submesh )
                        continue;
                    const glm::vec3 lo = glm::max( bone.Box.Min, cluster.BoundsMin );
                    const glm::vec3 hi = glm::min( bone.Box.Max, cluster.BoundsMax );
                    if ( lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z )
                        entry.ClusterBones[c].emplace_back( bone.Bone, Common::Math::AABB{ lo, hi } );
                }
            }
        }
        if ( entry.LastFrame != frame )
        {
            if ( !entry.Asked.empty() )
                entry.Using = std::move( entry.Asked );
            entry.Asked.assign( topology.Clusters.size(), 0 );
            entry.SmallestScreen = std::numeric_limits<float>::max();
            entry.LastFrame      = frame;
            entry.Materials.assign( submeshes.size(), DisplacementMaterialRecord{} );
            for ( size_t s = 0; s < submeshes.size() && s < materials.size(); ++s )
                if ( materials[s] )
                    entry.Materials[s] = *materials[s];
        }

        for ( uint32_t c = 0; c < topology.Clusters.size(); ++c )
        {
            const Desert::Geometry::DisplacementCluster& cluster = topology.Clusters[c];
            const glm::mat4 toObject = objectToWorld * submeshes[cluster.Submesh].Transform;
            glm::vec3       lo( std::numeric_limits<float>::max() );
            glm::vec3       hi( std::numeric_limits<float>::lowest() );
            float           scale = 0.0f;
            const auto grow = [&]( const glm::mat4& toWorld, const glm::vec3& boxMin, const glm::vec3& boxMax )
            {
                for ( uint32_t corner = 0; corner < 8; ++corner )
                {
                    const glm::vec3 local( ( corner & 1u ) ? boxMax.x : boxMin.x,
                                           ( corner & 2u ) ? boxMax.y : boxMin.y,
                                           ( corner & 4u ) ? boxMax.z : boxMin.z );
                    const glm::vec3 world = glm::vec3( toWorld * glm::vec4( local, 1.0f ) );
                    lo                    = glm::min( lo, world );
                    hi                    = glm::max( hi, world );
                }
                scale = std::max( scale, SkinnedMaxAxisScale( toWorld ) );
            };
            for ( const auto& [bone, box] : entry.ClusterBones[c] )
                if ( bone < bones.size() )
                    grow( toObject * bones[bone], box.Min, box.Max );
            // A cluster no bone box meets has no weighted vertex (it cannot move): its bind placement is its pose.
            if ( scale == 0.0f )
                grow( toObject, cluster.BoundsMin, cluster.BoundsMax );
            const glm::vec3 nearest           = glm::clamp( view.Origin, lo, hi );
            const float     screen            = Desert::Geometry::BoundsScreenRadiusSquared( nearest, 1.0f, view );
            const float     pixelsPerObjectCm = std::sqrt( screen ) * viewHeightPixels * scale;
            bool            clamped           = false;
            const uint8_t   level             = Desert::Geometry::DisplacementLevelForDensity(
                 topology, c, pixelsPerObjectCm, m_Quality.DicingRatePixels, &clamped );
            if ( clamped )
                ++m_Stats.LevelsClamped;
            entry.Asked[c]       = std::max( entry.Asked[c], level );
            entry.SmallestScreen = std::min( entry.SmallestScreen, screen );
        }
    }

    Common::ResultStr<DisplacementCache::SkinnedTopologyBuffers*>
    DisplacementCache::SkinnedTopology( const Desert::Geometry::DisplacementTopology& topology )
    {
        using Result = SkinnedTopologyBuffers*;
        if ( const auto found = m_SkinnedTopologies.find( &topology ); found != m_SkinnedTopologies.end() )
            return Common::MakeSuccess<Result>( &found->second );

        // Weld -> its DISTINCT copies (mesh vertices), ascending: the rest WeldDirection sums one normal per copy,
        // so the posed direction must too (in the bind pose both give the same vector).
        const auto            welds = static_cast<uint32_t>( topology.WeldPosition.size() );
        std::vector<uint32_t> words( welds + 1u, 0u );
        std::vector<uint32_t> copies;
        for ( uint32_t w = 0; w < welds; ++w )
        {
            words[w]               = static_cast<uint32_t>( copies.size() );
            const size_t firstCopy = copies.size();
            for ( uint32_t i = topology.WeldCornerOffset[w]; i < topology.WeldCornerOffset[w + 1u]; ++i )
                copies.push_back( topology.CornerVertex[topology.WeldCorners[i]] );
            std::sort( copies.begin() + static_cast<std::ptrdiff_t>( firstCopy ), copies.end() );
            copies.erase( std::unique( copies.begin() + static_cast<std::ptrdiff_t>( firstCopy ), copies.end() ),
                          copies.end() );
        }
        words[welds] = static_cast<uint32_t>( copies.size() );
        SkinnedTopologyBuffers buffers;
        buffers.CopyWord = static_cast<uint32_t>( words.size() );
        buffers.Welds    = welds;
        words.insert( words.end(), copies.begin(), copies.end() );

        const auto upload = [&]( const std::vector<uint32_t>& values, std::shared_ptr<VertexBuffer>& buffer,
                                 RDG::ExternalBuffer& import, const char* what ) -> Common::BoolResultStr
        {
            buffer = VertexBuffer::Create(
                 values.data(), static_cast<uint32_t>( values.size() * sizeof( uint32_t ) ), BufferUsage::Static );
            if ( const auto built = buffer->RT_Invalidate(); !built.IsSuccess() )
                return Common::MakeFormattedError<bool>( "skinned displacement: the {} ({} words) could not be "
                                                         "uploaded: {}",
                                                         what, values.size(), built.GetError() );
            buffer->ClaimOwnership( ResourceOwner::SceneRenderer );
            if ( const auto imported = Renderer::ImportVertexBuffer( buffer, import ); !imported )
                return Common::MakeError<bool>( imported.GetError() );
            buffers.Bytes += values.size() * sizeof( uint32_t );
            return BOOLSUCCESS;
        };
        if ( const auto done = upload( topology.CornerVertex, buffers.Corners, buffers.CornersImport, "corners" );
             !done )
            return Common::MakeError<Result>( done.GetError() );
        if ( const auto done = upload( words, buffers.WeldCopies, buffers.WeldCopiesImport, "weld copies" );
             !done )
            return Common::MakeError<Result>( done.GetError() );
        auto [it, added] = m_SkinnedTopologies.emplace( &topology, std::move( buffers ) );
        return Common::MakeSuccess<Result>( &it->second );
    }

    Common::ResultStr<std::shared_ptr<DisplacementCache::SkinnedGeometry>>
    DisplacementCache::EmitSkinned( const SkinnedKey& key, const Tessellation& t )
    {
        using Result                                           = std::shared_ptr<SkinnedGeometry>;
        const SkinnedMesh&                            base     = *key.Mesh;
        const Desert::Geometry::DisplacementTopology& topology = *key.Topology;
        const bool                                    streams  = base.GetGeometry().AuthoredStreams;

        // The tessellation as one dispatch pair (EmitPieces' layout, every cluster once, at its own offset).
        std::vector<Desert::Geometry::DisplacementPoint>  points;
        std::vector<Desert::Geometry::DisplacementSample> samples;
        std::vector<Desert::Geometry::DisplacementVertex> records;
        std::vector<uint32_t>                             indices;
        std::vector<Submesh>                              submeshes = base.GetSubmeshes();
        std::vector<uint32_t>                             firstIndex( submeshes.size(), 0 );
        std::vector<uint32_t>                             indexCount( submeshes.size(), 0 );
        for ( uint32_t c = 0; c < t.Clusters.size(); ++c )
        {
            const Desert::Geometry::TessellatedCluster& r          = *t.Clusters[c];
            const auto                                  pointBase  = static_cast<uint32_t>( points.size() );
            const auto                                  sampleBase = static_cast<uint32_t>( samples.size() );
            const auto                                  vertexBase = static_cast<uint32_t>( records.size() );
            for ( Desert::Geometry::DisplacementPoint p : r.Points )
            {
                p.SampleOffset += sampleBase;
                points.push_back( p );
            }
            samples.insert( samples.end(), r.Samples.begin(), r.Samples.end() );
            for ( Desert::Geometry::DisplacementVertex v : r.Vertices )
            {
                v.Point += pointBase;
                records.push_back( v );
            }
            // Clusters are cooked per submesh in order, so each submesh's indices are contiguous.
            const uint32_t submesh = topology.Clusters[c].Submesh;
            if ( indexCount[submesh] == 0 )
                firstIndex[submesh] = static_cast<uint32_t>( indices.size() );
            for ( const uint32_t index : r.Indices )
                indices.push_back( vertexBase + index );
            indexCount[submesh] += static_cast<uint32_t>( r.Indices.size() );
        }
        const auto vertexCount = static_cast<uint32_t>( records.size() );
        const auto pointCount  = static_cast<uint32_t>( points.size() );
        if ( vertexCount == 0 || indices.empty() )
            return Common::MakeError<Result>( "skinned displacement: the geometry draws no triangles" );

        auto geometry         = std::make_shared<SkinnedGeometry>();
        geometry->Key         = key;
        geometry->VertexCount = vertexCount;
        auto rest = GpuWrittenBuffer( uint64_t( vertexCount ) * sizeof( SkinnedVertex ), "rest vertex" );
        if ( !rest )
            return Common::MakeError<Result>( rest.GetError() );
        geometry->RestVertices = rest.ExtractValue();
        auto skinRecords =
             GpuWrittenBuffer( uint64_t( vertexCount ) * kSkinRecordWords * sizeof( uint32_t ), "skinned record" );
        if ( !skinRecords )
            return Common::MakeError<Result>( skinRecords.GetError() );
        geometry->SkinRecords = skinRecords.ExtractValue();
        if ( streams )
        {
            auto streamBuffer =
                 GpuWrittenBuffer( uint64_t( vertexCount ) * sizeof( MeshVertexStreams ), "stream" );
            if ( !streamBuffer )
                return Common::MakeError<Result>( streamBuffer.GetError() );
            geometry->Streams = streamBuffer.ExtractValue();
        }

        RDG::ExternalBuffer baseVertices, baseStreams, restVertices, restStreams;
        if ( const auto in = Renderer::ImportMeshVertices( base.GetGeometry(), baseVertices ); !in )
            return Common::MakeError<Result>( in.GetError() );
        if ( const auto in = Renderer::ImportVertexBuffer( geometry->RestVertices, restVertices ); !in )
            return Common::MakeError<Result>( in.GetError() );
        if ( const auto in = Renderer::ImportVertexBuffer( geometry->SkinRecords, geometry->SkinRecordsImport );
             !in )
            return Common::MakeError<Result>( in.GetError() );
        if ( streams )
        {
            if ( const auto in = Renderer::ImportMeshStreams( base.GetGeometry(), baseStreams ); !in )
                return Common::MakeError<Result>( in.GetError() );
            if ( const auto in = Renderer::ImportVertexBuffer( geometry->Streams, restStreams ); !in )
                return Common::MakeError<Result>( in.GetError() );
        }

        const std::vector<uint32_t>& topologyWords = TopologyWords( topology );
        const auto                   triangles     = static_cast<uint32_t>( topology.TriangleSubmesh.size() );
        const auto                   welds         = static_cast<uint32_t>( topology.WeldPosition.size() );
        std::vector<uint32_t>        materials;
        for ( const DisplacementMaterialRecord& m : key.Materials )
        {
            const auto* words = reinterpret_cast<const uint32_t*>( &m );
            materials.insert( materials.end(), words,
                              words + sizeof( DisplacementMaterialRecord ) / sizeof( uint32_t ) );
        }
        // Heights read the rest SkinnedVertex (22 words); Emit writes the twin's rest SkinnedVertex (layout 1) and
        // the skinned records the per-frame pass reads.
        const std::vector<uint32_t> heightsDispatch = { pointCount,
                                                        0,
                                                        kSkinnedVertexWords,
                                                        0,
                                                        3 * triangles,
                                                        4 * triangles,
                                                        4 * triangles + 3 * welds,
                                                        streams ? 1u : 0u,
                                                        std::bit_cast<uint32_t>( topology.BaseEdgeCm ),
                                                        0,
                                                        0,
                                                        0 };
        std::vector<uint32_t>       emitDispatch    = heightsDispatch;
        emitDispatch[0]                             = vertexCount;
        emitDispatch[3]                             = 1u;

        RDG::Builder         graph( "Displacement Skinned" );
        const RDG::BufferRef baseVerticesRef =
             graph.RegisterExternal( baseVertices, "Displacement.SourceVertices" );
        const RDG::BufferRef baseStreamsRef =
             streams ? graph.RegisterExternal( baseStreams, "Displacement.SourceStreams" ) : baseVerticesRef;
        const RDG::BufferRef outVerticesRef = graph.RegisterExternal( restVertices, "Displacement.RestVertices" );
        const RDG::BufferRef outRecordsRef =
             graph.RegisterExternal( geometry->SkinRecordsImport, "Displacement.SkinRecords" );
        const RDG::BufferRef outStreamsRef =
             streams ? graph.RegisterExternal( restStreams, "Displacement.Streams" )
                     : graph.CreateBuffer( RDG::BufferDesc{ 16 }, "Displacement.NoStreams" );
        const auto upload = [&]( const auto& values, const char* name )
        {
            const RDG::BufferRef ref =
                 graph.CreateBuffer( RDG::BufferDesc{ SkinnedBytesOf( values ).size() }, name );
            graph.QueueBufferUpload( ref, SkinnedBytesOf( values ) );
            return ref;
        };
        const RDG::BufferRef topologyRef  = upload( topologyWords, "Displacement.Topology" );
        const RDG::BufferRef materialsRef = upload( materials, "Displacement.Materials" );
        const RDG::BufferRef pointsRef    = upload( points, "Displacement.Points" );
        const RDG::BufferRef samplesRef   = upload( samples, "Displacement.Samples" );
        const RDG::BufferRef recordsRef   = upload( records, "Displacement.VertexRecords" );
        const RDG::BufferRef heightsArgs  = upload( heightsDispatch, "Displacement.HeightsDispatch" );
        const RDG::BufferRef emitArgs     = upload( emitDispatch, "Displacement.EmitDispatch" );
        const RDG::BufferRef positionsRef =
             graph.CreateBuffer( RDG::BufferDesc{ static_cast<uint64_t>( pointCount ) * 4u * sizeof( float ) },
                                 "Displacement.Positions" );
        const std::shared_ptr<ComputePipeline> heights = m_HeightsPipeline;
        const std::shared_ptr<ComputePipeline> emit    = m_EmitPipeline;
        const auto&               heightsLayout        = m_HeightsLayout.Get( heights->GetSpecification().Shader );
        const auto&               emitLayout           = m_EmitLayout.Get( emit->GetSpecification().Shader );
        const RDG::OtherRouteFill heightsFill          = Renderer::GetPipelineRouteFill( *heights );
        const RDG::OtherRouteFill emitFill             = Renderer::GetPipelineRouteFill( *emit );
        const uint32_t pointGroups  = ( pointCount + kDisplacementGroupSize - 1 ) / kDisplacementGroupSize;
        const uint32_t vertexGroups = ( vertexCount + kDisplacementGroupSize - 1 ) / kDisplacementGroupSize;
        graph.AddPass(
             kDisplacementHeightsPassName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( heightsLayout, heightsFill )
                      .Storage( kDisplacementSourceVerticesBlock, baseVerticesRef, RDG::Access::StorageRead )
                      .Storage( kDisplacementSourceStreamsBlock, baseStreamsRef, RDG::Access::StorageRead )
                      .Storage( kDisplacementTopologyBlock, topologyRef, RDG::Access::StorageRead )
                      .Storage( kDisplacementMaterialsBlock, materialsRef, RDG::Access::StorageRead )
                      .Storage( kDisplacementPointsBlock, pointsRef, RDG::Access::StorageRead )
                      .Storage( kDisplacementSamplesBlock, samplesRef, RDG::Access::StorageRead )
                      .Storage( kDisplacementDispatchBlock, heightsArgs, RDG::Access::StorageRead )
                      .Storage( kDisplacementPositionsBlock, positionsRef, RDG::Access::StorageWrite );
             },
             [heights, pointGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::DispatchCompute( bindings, *heights, pointGroups, 1, 1 );
             } );
        graph.AddPass(
             kDisplacementEmitPassName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( emitLayout, emitFill )
                      .Storage( kDisplacementSourceVerticesBlock, baseVerticesRef, RDG::Access::StorageRead )
                      .Storage( kDisplacementSourceStreamsBlock, baseStreamsRef, RDG::Access::StorageRead )
                      .Storage( kDisplacementTopologyBlock, topologyRef, RDG::Access::StorageRead )
                      .Storage( kDisplacementVerticesBlock, recordsRef, RDG::Access::StorageRead )
                      .Storage( kDisplacementPositionsBlock, positionsRef, RDG::Access::StorageRead )
                      .Storage( kDisplacementDispatchBlock, emitArgs, RDG::Access::StorageRead )
                      .Storage( kDisplacementOutputVerticesBlock, outVerticesRef, RDG::Access::StorageWrite )
                      .Storage( kDisplacementOutputStreamsBlock, outStreamsRef, RDG::Access::StorageWrite )
                      .Storage( kDisplacementOutputSkinRecordsBlock, outRecordsRef, RDG::Access::StorageWrite )
                      .Storage( kDisplacementPointsBlock, pointsRef, RDG::Access::StorageRead );
             },
             [emit, vertexGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::DispatchCompute( bindings, *emit, vertexGroups, 1, 1 );
             } );
        // The rest vertices are the twin's vertex stream (fallback, UVs); the records are read by every frame's
        // "Displacement: Skinned" through the same external, so its carried state barriers them.
        graph.Extract( outVerticesRef, restVertices, RDG::Access::VertexIndex );
        graph.Extract( outRecordsRef, geometry->SkinRecordsImport, RDG::Access::StorageRead );
        if ( streams )
            graph.Extract( outStreamsRef, restStreams, RDG::Access::VertexIndex );
        if ( const Common::BoolResultStr executed = Renderer::ExecuteGraph( graph ); !executed )
            return Common::MakeFormattedError<Result>(
                 "skinned displacement: the rest graph of {} vertices did not execute: {}", vertexCount,
                 executed.GetError() );

        auto indexBuffer = IndexBuffer::Create(
             indices.data(), static_cast<uint32_t>( indices.size() * sizeof( uint32_t ) ), BufferUsage::Static );
        if ( const auto built = indexBuffer->RT_Invalidate(); !built.IsSuccess() )
            return Common::MakeFormattedError<Result>(
                 "skinned displacement: {} indices could not be uploaded: {}", indices.size(), built.GetError() );
        indexBuffer->ClaimOwnership( ResourceOwner::SceneRenderer );
        // Bounds grow by the largest displacement any submesh can apply (IndexGeometry's rule).
        float growth = 0.0f;
        for ( const DisplacementMaterialRecord& m : key.Materials )
            if ( m.Texture != 0xffffffffu )
                growth = std::max( growth, std::abs( m.ScaleCm ) * std::max( m.Center, 1.0f - m.Center ) );
        for ( size_t s = 0; s < submeshes.size(); ++s )
        {
            Submesh& sm     = submeshes[s];
            sm.VertexOffset = 0;
            sm.VertexCount  = vertexCount;
            sm.IndexOffset  = firstIndex[s];
            sm.IndexCount   = indexCount[s];
            sm.LODs         = { { sm.IndexOffset, sm.IndexCount } };
            sm.BakedLODs.clear();
            sm.BoundingBox.Min -= glm::vec3( growth );
            sm.BoundingBox.Max += glm::vec3( growth );
        }
        geometry->Twin =
             SkinnedMesh::MakeDisplacedTwin( base, geometry->RestVertices, geometry->Streams, indexBuffer,
                                             std::move( submeshes ), vertexCount, growth );
        geometry->Bytes = SkinnedGeometryBytes( vertexCount, indices.size(), streams );
        ++m_Stats.Rebuilt;
        return Common::MakeSuccess( std::move( geometry ) );
    }

    void DisplacementCache::RetireSkinnedTwin( std::shared_ptr<SkinnedMesh> twin )
    {
        if ( twin )
            m_RetiringSkinnedTwins.Park( std::move( twin ),
                                         ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
    }

    Common::BoolResultStr DisplacementCache::DisplaceSkinned( const uint64_t staticLiveBytes )
    {
        if ( !EnsureSkinnedPipelines() )
            return BOOLSUCCESS; // refused by name in EnsureSkinnedPipelines: every skinned record draws its base

        uint64_t liveBytes = staticLiveBytes;
        {
            std::unordered_set<const SkinnedGeometry*> counted;
            for ( const auto& [key, entry] : m_SkinnedEntries )
                if ( entry.Current && counted.insert( entry.Current.get() ).second )
                    liveBytes += entry.Current->Bytes;
        }
        std::vector<std::string> errors;
        for ( auto& [entryKey, entry] : m_SkinnedEntries )
        {
            if ( entry.Using.empty() )
                entry.Using = entry.Asked; // the first frame: this frame's requests
            if ( entry.Using.empty() )
                continue;
            SkinnedKey wanted{ entry.Mesh, entry.Topology.get(), entry.Using, entry.Materials };
            for ( uint8_t& level : wanted.Levels )
                level = static_cast<uint8_t>( level > entry.Lowered ? level - entry.Lowered : 0 );

            // A lowering is undone once the pool holds what the higher levels measured (§5).
            if ( entry.Lowered > 0 && entry.RaiseBytes > 0 && !entry.Pending.valid() &&
                 ( m_Quality.PoolBytes == 0 ||
                   liveBytes - ( entry.Current ? entry.Current->Bytes : 0 ) + entry.RaiseBytes <=
                        m_Quality.PoolBytes ) )
            {
                --entry.Lowered;
                entry.RaiseBytes = 0;
                ++m_Stats.LevelsRaised;
                continue; // the raised key is wanted next frame
            }
            if ( entry.Current && entry.Current->Key == wanted )
                continue;

            if ( entry.Pending.valid() )
            {
                if ( entry.Pending.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
                    continue;
                Common::ResultStr<Tessellation> done = entry.Pending.get();
                const SkinnedKey                key  = std::move( *entry.PendingKey );
                entry.PendingKey.reset();
                if ( !done.IsSuccess() )
                {
                    if ( !entry.RefusalLogged )
                        errors.push_back(
                             std::format( "skinned entity {}: {}", entryKey >> 32u, done.GetError() ) );
                    entry.RefusalLogged = true;
                    continue;
                }
                const Tessellation& t     = done.GetValue();
                const uint64_t      bytes = SkinnedGeometryBytes( t.VertexCount(), t.IndexCount(),
                                                                  entry.Mesh->GetGeometry().AuthoredStreams );
                const uint64_t      held  = entry.Current ? entry.Current->Bytes : 0;
                if ( m_Quality.PoolBytes != 0 && liveBytes - held + bytes > m_Quality.PoolBytes )
                {
                    // Over the pool: one level off every cluster, counted (overlay, [ViewMemory]); remembered so
                    // the step is undone when the pool has room for these bytes again.
                    ++entry.Lowered;
                    entry.RaiseBytes = bytes;
                    ++m_Stats.LevelsLowered;
                    continue;
                }
                std::shared_ptr<SkinnedGeometry> geometry;
                for ( const std::weak_ptr<SkinnedGeometry>& weak : m_SkinnedShared )
                    if ( auto shared = weak.lock(); shared && shared->Key == key )
                        geometry = std::move( shared );
                if ( !geometry )
                {
                    auto emitted = EmitSkinned( key, t );
                    if ( !emitted.IsSuccess() )
                    {
                        if ( !entry.RefusalLogged )
                            errors.push_back(
                                 std::format( "skinned entity {}: {}", entryKey >> 32u, emitted.GetError() ) );
                        entry.RefusalLogged = true;
                        continue;
                    }
                    geometry = emitted.ExtractValue();
                    m_SkinnedShared.push_back( geometry );
                    liveBytes += geometry->Bytes;
                }
                if ( entry.Current )
                    RetireSkinnedTwin( entry.Current->Twin ); // records of frames in flight may still name it
                entry.Current       = std::move( geometry );
                entry.RefusalLogged = false;
                continue;
            }

            // An equal geometry another entry holds is adopted; otherwise the CPU tessellation is launched.
            std::shared_ptr<SkinnedGeometry> shared;
            for ( const std::weak_ptr<SkinnedGeometry>& weak : m_SkinnedShared )
                if ( auto candidate = weak.lock(); candidate && candidate->Key == wanted )
                    shared = std::move( candidate );
            if ( shared )
            {
                if ( entry.Current )
                    RetireSkinnedTwin( entry.Current->Twin );
                entry.Current = std::move( shared );
                continue;
            }
            std::shared_ptr<const Desert::Geometry::DisplacementTopology> topology = entry.Topology;
            std::vector<uint8_t>                                          levels   = wanted.Levels;
            entry.PendingKey                                                       = std::move( wanted );
            entry.Pending = Common::JobSystem::Get().Async(
                 [topology, levels = std::move( levels )]() -> Common::ResultStr<Tessellation>
                 {
                     Tessellation out;
                     out.Clusters.reserve( topology->Clusters.size() );
                     for ( uint32_t c = 0; c < topology->Clusters.size(); ++c )
                     {
                         auto cluster = Desert::Geometry::TessellateCluster( *topology, c, levels );
                         if ( !cluster.IsSuccess() )
                             return Common::MakeError<Tessellation>( cluster.GetError() );
                         out.Clusters.push_back( std::make_shared<const Desert::Geometry::TessellatedCluster>(
                              cluster.ExtractValue() ) );
                     }
                     return Common::MakeSuccess( std::move( out ) );
                 } );
        }
        if ( !errors.empty() )
        {
            std::string joined;
            for ( const std::string& e : errors )
                std::format_to( std::back_inserter( joined ), "{}{}", joined.empty() ? "" : "; ", e );
            return Common::MakeError( std::format(
                 "[Displacement] {} skinned primitive(s) keep their base surface: {}", errors.size(), joined ) );
        }
        return BOOLSUCCESS;
    }

    std::shared_ptr<SkinnedMesh> DisplacementCache::FindSkinned( const uint32_t entity, const uint32_t slot ) const
    {
        const auto found = m_SkinnedEntries.find( SkinnedEntryKey( entity, slot ) );
        if ( found == m_SkinnedEntries.end() || !found->second.Current )
            return nullptr;
        return found->second.Current->Twin;
    }

    Common::BoolResultStr
    DisplacementCache::AddSkinnedPasses( RDG::Builder& graph, const uint32_t entity, const uint32_t slot,
                                         const RDG::BufferRef baseStreams, const uint32_t baseVertexCount,
                                         const RDG::BufferRef displacedStreams, const bool history,
                                         std::unordered_map<RDG::ExternalBuffer*, RDG::BufferRef>& graphRefs )
    {
        const auto found = m_SkinnedEntries.find( SkinnedEntryKey( entity, slot ) );
        if ( found == m_SkinnedEntries.end() || !found->second.Current || !EnsureSkinnedPipelines() )
            return Common::MakeFormattedError<bool>(
                 "{}: skinned record {} of entity {} has no displaced geometry", kDisplacementSkinnedPassName,
                 slot, entity );
        SkinnedEntry&    entry    = found->second;
        SkinnedGeometry& geometry = *entry.Current;
        if ( baseVertexCount != entry.Mesh->GetVertexCount() )
            return Common::MakeFormattedError<bool>(
                 "{}: entity {} skinned {} base vertices, its displacement topology indexes {}",
                 kDisplacementSkinnedPassName, entity, baseVertexCount, entry.Mesh->GetVertexCount() );
        auto topology = SkinnedTopology( *entry.Topology );
        if ( !topology )
            return Common::MakeError<bool>( topology.GetError() );
        SkinnedTopologyBuffers& buffers = *topology.GetValue();

        const auto refOf = [&]( RDG::ExternalBuffer& external, const char* name )
        {
            auto [it, fresh] = graphRefs.try_emplace( &external );
            if ( fresh )
                it->second = graph.RegisterExternal( external, name );
            return it->second;
        };
        const RDG::BufferRef records = refOf( geometry.SkinRecordsImport, "Displacement.SkinRecords" );
        const RDG::BufferRef corners = refOf( buffers.CornersImport, "Displacement.Corners" );
        const RDG::BufferRef copies  = refOf( buffers.WeldCopiesImport, "Displacement.WeldCopies" );

        const auto upload = [&]( const std::array<uint32_t, 12>& words, const char* name )
        {
            const RDG::BufferRef ref = graph.CreateBuffer( RDG::BufferDesc{ sizeof( words ) }, name );
            graph.QueueBufferUpload( ref, std::as_bytes( std::span( words ) ) );
            return ref;
        };
        const RDG::BufferRef poseArgs =
             upload( { buffers.Welds, baseVertexCount, 0, 0, buffers.CopyWord, 0, 0, 0, 0, 0, 0, 0 },
                     "Displacement.PoseDispatch" );
        const RDG::BufferRef skinnedArgs =
             upload( { geometry.VertexCount, baseVertexCount, history ? 1u : 0u, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                     "Displacement.SkinnedDispatch" );
        const RDG::BufferRef posedWelds = graph.CreateBuffer(
             RDG::BufferDesc{ static_cast<uint64_t>( std::max( buffers.Welds, 1u ) ) * 6u * sizeof( float ) },
             "Displacement.PosedWelds" );

        const std::shared_ptr<ComputePipeline> pose       = m_PosePipeline;
        const std::shared_ptr<ComputePipeline> skinned    = m_SkinnedPipeline;
        const auto&                            poseLayout = m_PoseLayout.Get( pose->GetSpecification().Shader );
        const auto&               skinLayout = m_SkinnedLayout.Get( skinned->GetSpecification().Shader );
        const RDG::OtherRouteFill poseFill   = Renderer::GetPipelineRouteFill( *pose );
        const RDG::OtherRouteFill skinFill   = Renderer::GetPipelineRouteFill( *skinned );
        const uint32_t weldGroups = ( buffers.Welds + kDisplacementGroupSize - 1 ) / kDisplacementGroupSize;
        const uint32_t vertexGroups =
             ( geometry.VertexCount + kDisplacementGroupSize - 1 ) / kDisplacementGroupSize;
        graph.AddPass(
             kDisplacementPosePassName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( poseLayout, poseFill )
                      .Storage( kDisplacementBaseStreamsBlock, baseStreams, RDG::Access::StorageRead )
                      .Storage( kDisplacementWeldCopiesBlock, copies, RDG::Access::StorageRead )
                      .Storage( kDisplacementDispatchBlock, poseArgs, RDG::Access::StorageRead )
                      .Storage( kDisplacementPosedWeldsBlock, posedWelds, RDG::Access::StorageWrite );
             },
             [pose, weldGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::DispatchCompute( bindings, *pose, weldGroups, 1, 1 );
             } );
        graph.AddPass(
             kDisplacementSkinnedPassName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( skinLayout, skinFill )
                      .Storage( kDisplacementBaseStreamsBlock, baseStreams, RDG::Access::StorageRead )
                      .Storage( kDisplacementSkinRecordsBlock, records, RDG::Access::StorageRead )
                      .Storage( kDisplacementCornersBlock, corners, RDG::Access::StorageRead )
                      .Storage( kDisplacementPosedWeldsBlock, posedWelds, RDG::Access::StorageRead )
                      .Storage( kDisplacementDispatchBlock, skinnedArgs, RDG::Access::StorageRead )
                      .Storage( kDisplacementSkinnedStreamsBlock, displacedStreams, RDG::Access::StorageWrite );
             },
             [skinned, vertexGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::DispatchCompute( bindings, *skinned, vertexGroups, 1, 1 );
             } );
        return BOOLSUCCESS;
    }

    uint64_t DisplacementCache::EndSkinnedFrame( const uint64_t frame )
    {
        for ( const auto& [key, entry] : m_SkinnedEntries )
            if ( entry.LastFrame != frame && entry.Current )
                RetireSkinnedTwin( entry.Current->Twin ); // this frame's draws may still name it
        std::erase_if( m_SkinnedEntries, [frame]( const auto& item ) { return item.second.LastFrame != frame; } );
        std::erase_if( m_SkinnedShared,
                       []( const std::weak_ptr<SkinnedGeometry>& weak ) { return weak.expired(); } );

        uint64_t                                                          bytes = 0;
        std::unordered_set<const SkinnedGeometry*>                        geometries;
        std::unordered_set<const Desert::Geometry::DisplacementTopology*> topologies;
        for ( const auto& [key, entry] : m_SkinnedEntries )
        {
            m_Stats.Entries += 1;
            if ( entry.Pending.valid() )
                ++m_Stats.Pending;
            if ( !m_PipelineRefusalLogged &&
                 ( entry.Pending.valid() || ( !entry.Current && !entry.RefusalLogged && !entry.Using.empty() ) ) )
                ++m_Stats.Unsettled;
            if ( entry.Lowered > 0 )
                m_Stats.LevelsHeld += entry.Lowered;
            if ( entry.Current )
            {
                ++m_Stats.Displaced;
                if ( geometries.insert( entry.Current.get() ).second )
                    bytes += entry.Current->Bytes;
            }
            if ( entry.Topology )
                topologies.insert( entry.Topology.get() );
        }
        // The per-frame topology buffers of a topology no skinned entry reads are dropped (its address could be
        // reused by a new mesh); the frame ring retires the buffers' API objects.
        std::erase_if( m_SkinnedTopologies,
                       [&]( const auto& item ) { return !topologies.contains( item.first ); } );
        for ( const auto& [topology, buffers] : m_SkinnedTopologies )
            bytes += buffers.Bytes;
        return bytes;
    }

    void DisplacementCache::ClearSkinned()
    {
        for ( const auto& [key, entry] : m_SkinnedEntries )
            if ( entry.Current )
                RetireSkinnedTwin( entry.Current->Twin );
        m_SkinnedEntries.clear();
        m_SkinnedShared.clear();
        m_SkinnedTopologies.clear();
    }
} // namespace Desert::Graphic::System

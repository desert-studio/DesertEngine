#include <Engine/Graphic/Systems/Scene/Mesh/DisplacementCache.hpp>

#include <Common/Core/Core.hpp> // BOOLSUCCESS
#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/IndexBuffer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Graphic::System
{
    namespace
    {
        uint64_t EntryKey( uint32_t entity, uint32_t part )
        {
            return ( static_cast<uint64_t>( entity ) << 32u ) | part;
        }

        uint32_t Bits( float value )
        {
            return std::bit_cast<uint32_t>( value );
        }

        // The largest axis scale of a transform's linear part: a centimetre of the object covers at most this many
        // world centimetres, so the cluster asks for the density its most stretched direction needs.
        float MaxAxisScale( const glm::mat4& m )
        {
            return std::max( { glm::length( glm::vec3( m[0] ) ), glm::length( glm::vec3( m[1] ) ),
                               glm::length( glm::vec3( m[2] ) ) } );
        }

        template <typename T>
        std::span<const std::byte> BytesOf( const std::vector<T>& values )
        {
            return std::as_bytes( std::span( values ) );
        }
    } // namespace

    Common::ResultStr<std::optional<DisplacementMaterialRecord>>
    ResolveDisplacementMaterial( const Core::Formats::ShaderProgramMeta& schema,
                                 const Core::Formats::MaterialLayout&    layout,
                                 const Core::Formats::MaterialParamRow& row, const bool heightAssigned )
    {
        using Result = std::optional<DisplacementMaterialRecord>;
        if ( !schema.Height || !heightAssigned )
            return Common::MakeSuccess( Result{} );
        const Core::Formats::SurfaceHeight& height = *schema.Height;

        const auto slotOf = [&]( const std::string& name, const char* role ) -> Common::ResultStr<glm::vec4>
        {
            const Core::Formats::MaterialLayoutParam* param = layout.FindParam( name );
            if ( param == nullptr )
                return Common::MakeFormattedError<glm::vec4>(
                     "displacement: the Height line names {} '{}', which the material layout does not hold", role,
                     name );
            const uint32_t slot = param->Offset / Core::Formats::kMaterialParamSlotSize;
            if ( slot >= row.size() )
                return Common::MakeFormattedError<glm::vec4>(
                     "displacement: {} '{}' sits in row slot {} of a {}-slot row", role, name, slot, row.size() );
            return Common::MakeSuccess( row[slot] );
        };

        DisplacementMaterialRecord record;
        const auto                 texture = slotOf( height.Texture, "texture" );
        if ( !texture.IsSuccess() )
            return Common::MakeError<Result>( texture.GetError() );
        record.Texture = glm::floatBitsToUint( texture.GetValue().x );
        record.Sampler = glm::floatBitsToUint( texture.GetValue().y );
        record.UVSet   = height.UVSet;
        record.Channel = height.Channel;
        if ( !height.ChannelParam.empty() )
        {
            const auto channel = slotOf( height.ChannelParam, "channel" );
            if ( !channel.IsSuccess() )
                return Common::MakeError<Result>( channel.GetError() );
            record.Channel = static_cast<uint32_t>( std::clamp( static_cast<int>( channel.GetValue().x ), 0, 3 ) );
        }
        const auto scale = slotOf( height.ScaleParam, "scale" );
        if ( !scale.IsSuccess() )
            return Common::MakeError<Result>( scale.GetError() );
        record.ScaleCm = scale.GetValue().x;
        if ( !height.CenterParam.empty() )
        {
            const auto center = slotOf( height.CenterParam, "centre" );
            if ( !center.IsSuccess() )
                return Common::MakeError<Result>( center.GetError() );
            record.Center = std::clamp( center.GetValue().x, 0.0f, 1.0f );
        }
        if ( !height.TilingParam.empty() )
        {
            const auto tiling   = slotOf( height.TilingParam, "tiling" );
            const auto offset   = slotOf( height.OffsetParam, "offset" );
            const auto rotation = slotOf( height.RotationParam, "rotation" );
            for ( const auto* r : { &tiling, &offset, &rotation } )
                if ( !r->IsSuccess() )
                    return Common::MakeError<Result>( r->GetError() );
            record.Tiling       = glm::vec2( tiling.GetValue() );
            record.Offset       = glm::vec2( offset.GetValue() );
            record.Rotation     = rotation.GetValue().x;
            record.HasTransform = 1;
        }
        return Common::MakeSuccess( Result{ record } );
    }

    DisplacementCache::DisplacementCache()  = default;
    DisplacementCache::~DisplacementCache() = default;

    void DisplacementCache::SetQuality( const DisplacementQuality& quality )
    {
        if ( quality == m_Quality )
            return;
        const bool wasEnabled = m_Quality.Enabled;
        m_Quality             = quality;
        if ( !quality.Enabled )
        {
            // Parked, not dropped: the records of frames in flight name the twins by raw pointer (RetireTwin).
            for ( const auto& [key, entry] : m_Entries )
                if ( entry.Current )
                    RetireTwin( entry.Current->Twin );
            m_Entries.clear();
            m_Shared.clear();
            m_Pools.clear();
            m_TopologyWords.clear();
            if ( wasEnabled )
                LOG_INFO(
                     "[Displacement] off (Scalability ViewDistance.DisplacementDicingRate = 0, the Low level): "
                     "displaced surfaces draw their base mesh with parallax occlusion" );
        }
    }

    bool DisplacementCache::EnsurePipelines()
    {
        if ( m_HeightsPipeline && m_EmitPipeline )
            return true;
        Runtime::ShaderService* service = Runtime::ResourceRegistry::GetShaderService();
        for ( const auto& [name, pipeline] : { std::pair{ kDisplacementHeightsShader, &m_HeightsPipeline },
                                               std::pair{ kDisplacementEmitShader, &m_EmitPipeline } } )
        {
            if ( *pipeline )
                continue;
            const std::shared_ptr<Shader> shader = service ? service->GetByName( std::string( name ) ) : nullptr;
            if ( !shader )
            {
                if ( !m_PipelineRefusalLogged )
                    LOG_ERROR(
                         "[Displacement] the shader '{}' did not compile - expected "
                         "Editor/Resources/Shaders/Programs/Compute/{}.shader; displaced meshes draw their base",
                         name, name );
                m_PipelineRefusalLogged = true;
                return false;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = std::string( name ) } );
            if ( !built )
            {
                if ( !m_PipelineRefusalLogged )
                    LOG_ERROR(
                         "[Displacement] the pipeline '{}' was not built; displaced meshes draw their base: {}",
                         name, built.GetError() );
                m_PipelineRefusalLogged = true;
                return false;
            }
            *pipeline = built.GetValue();
        }
        return true;
    }

    void DisplacementCache::Request( const uint32_t entity, const uint32_t part,
                                     const StaticMesh& mesh, const glm::mat4& objectToWorld,
                                     std::span<const std::optional<DisplacementMaterialRecord>> materials,
                                     const Desert::Geometry::LODView& view, const float viewHeightPixels,
                                     const uint64_t frame )
    {
        if ( !m_Quality.Enabled || !mesh.GetDisplacementTopology() )
            return;
        const Desert::Geometry::DisplacementTopology& topology  = *mesh.GetDisplacementTopology();
        const std::vector<Submesh>&                   submeshes = mesh.GetSubmeshes();

        Entry& entry = m_Entries[EntryKey( entity, part )];
        if ( entry.Mesh != &mesh || entry.Topology != mesh.GetDisplacementTopology() )
            entry = Entry{}; // another mesh in this slot: nothing of the old entry applies
        if ( entry.LastFrame != frame )
        {
            // The first request of this frame: last frame's requests become the levels the next DisplaceFrame
            // uses.
            if ( !entry.Asked.empty() )
                entry.Using = std::move( entry.Asked );
            entry.Asked.assign( topology.Clusters.size(), 0 );
            entry.SmallestScreen = std::numeric_limits<float>::max();
            entry.LastFrame      = frame;
            entry.Mesh           = &mesh;
            entry.Topology = mesh.GetDisplacementTopology();
            entry.Materials.assign( submeshes.size(), DisplacementMaterialRecord{} );
            for ( size_t s = 0; s < submeshes.size() && s < materials.size(); ++s )
                if ( materials[s] )
                    entry.Materials[s] = *materials[s];
        }

        for ( uint32_t c = 0; c < topology.Clusters.size(); ++c )
        {
            const Desert::Geometry::DisplacementCluster& cluster = topology.Clusters[c];
            const glm::mat4 toWorld = objectToWorld * submeshes[cluster.Submesh].Transform;
            glm::vec3       lo( std::numeric_limits<float>::max() );
            glm::vec3       hi( std::numeric_limits<float>::lowest() );
            for ( uint32_t corner = 0; corner < 8; ++corner )
            {
                const glm::vec3 local( ( corner & 1u ) ? cluster.BoundsMax.x : cluster.BoundsMin.x,
                                       ( corner & 2u ) ? cluster.BoundsMax.y : cluster.BoundsMin.y,
                                       ( corner & 4u ) ? cluster.BoundsMax.z : cluster.BoundsMin.z );
                const glm::vec3 world = glm::vec3( toWorld * glm::vec4( local, 1.0f ) );
                lo                    = glm::min( lo, world );
                hi                    = glm::max( hi, world );
            }
            // The cluster's nearest point: its closest edge decides the density it needs.
            const glm::vec3 nearest           = glm::clamp( view.Origin, lo, hi );
            const float     screen            = Desert::Geometry::BoundsScreenRadiusSquared( nearest, 1.0f, view );
            const float     pixelsPerObjectCm = std::sqrt( screen ) * viewHeightPixels * MaxAxisScale( toWorld );
            bool            clamped           = false;
            const uint8_t   level             = Desert::Geometry::DisplacementLevelForDensity(
                 topology, c, pixelsPerObjectCm, m_Quality.DicingRatePixels, &clamped );
            if ( clamped )
                ++m_Stats.LevelsClamped;
            entry.Asked[c]       = std::max( entry.Asked[c], level );
            entry.SmallestScreen = std::min( entry.SmallestScreen, screen );
        }
    }

    uint64_t DisplacementCache::HashKey( const BuildKey& key )
    {
        uint64_t h     = 14695981039346656037ull;
        auto     bytes = [&h]( const void* data, size_t size )
        {
            const auto* p = static_cast<const uint8_t*>( data );
            for ( size_t i = 0; i < size; ++i )
            {
                h ^= p[i];
                h *= 1099511628211ull;
            }
        };
        bytes( &key.Mesh, sizeof( key.Mesh ) );
        bytes( &key.Topology, sizeof( key.Topology ) );
        bytes( key.Levels.data(), key.Levels.size() );
        // Field by field: the record has no padding (static_assert 48 B of 4-byte fields), but floats compare
        // by value in operator== and -0/+0 must hash alike.
        for ( const DisplacementMaterialRecord& m : key.Materials )
        {
            const std::array<float, 7> values{ m.ScaleCm, m.Center, m.Tiling.x, m.Tiling.y,
                                               m.Offset.x, m.Offset.y, m.Rotation };
            const std::array<uint32_t, 5> words{ m.Texture, m.Sampler, m.Channel, m.UVSet, m.HasTransform };
            bytes( words.data(), sizeof( words ) );
            for ( const float v : values )
            {
                const uint32_t bits = v == 0.0f ? 0u : std::bit_cast<uint32_t>( v );
                bytes( &bits, sizeof( bits ) );
            }
        }
        return h;
    }

    std::shared_ptr<DisplacementCache::SharedGeometry> DisplacementCache::FindShared( const BuildKey& key ) const
    {
        const auto [first, last] = m_Shared.equal_range( HashKey( key ) );
        for ( auto it = first; it != last; ++it )
            if ( const auto shared = it->second.lock(); shared && shared->Key == key )
                return shared;
        return nullptr;
    }

    const std::vector<uint32_t>&
    DisplacementCache::TopologyWords( const Desert::Geometry::DisplacementTopology& topology )
    {
        auto [it, added] = m_TopologyWords.try_emplace( &topology );
        if ( !added )
            return it->second;
        std::vector<uint32_t>& words = it->second;
        words.insert( words.end(), topology.CornerVertex.begin(), topology.CornerVertex.end() );
        words.insert( words.end(), topology.TriangleSubmesh.begin(), topology.TriangleSubmesh.end() );
        for ( const auto* field : { &topology.WeldPosition, &topology.WeldDirection } )
            for ( const glm::vec3& v : *field )
                for ( int k = 0; k < 3; ++k )
                    words.push_back( Bits( v[k] ) );
        return words;
    }

    namespace
    {
        // A piece = a cluster at its own level and the level each border with a neighbour dices at (the larger of
        // the two clusters' levels): exactly what TessellateCluster reads, so equal keys are equal tessellations.
        std::string PieceKeyOf( const Desert::Geometry::DisplacementTopology& topology, std::span<const uint8_t> levels,
                                const uint32_t cluster )
        {
            const auto& neighbours = topology.Clusters[cluster].Neighbours;
            std::string key( sizeof( uint32_t ) + 1 + neighbours.size(), '\0' );
            std::memcpy( key.data(), &cluster, sizeof( uint32_t ) );
            key[sizeof( uint32_t )] = static_cast<char>( levels[cluster] );
            for ( size_t n = 0; n < neighbours.size(); ++n )
                key[sizeof( uint32_t ) + 1 + n] =
                     static_cast<char>( std::max( levels[cluster], levels[neighbours[n]] ) );
            return key;
        }

        uint32_t PoolStride( const bool streams )
        {
            return static_cast<uint32_t>( sizeof( Vertex ) + ( streams ? sizeof( MeshVertexStreams ) : 0u ) );
        }
    } // namespace

    uint64_t DisplacementCache::Tessellation::VertexCount() const
    {
        uint64_t count = 0;
        for ( const auto& cluster : Clusters )
            count += cluster->Vertices.size();
        return count;
    }

    uint64_t DisplacementCache::Tessellation::IndexCount() const
    {
        uint64_t count = 0;
        for ( const auto& cluster : Clusters )
            count += cluster->Indices.size();
        return count;
    }

    std::shared_ptr<DisplacementCache::VertexPool> DisplacementCache::PoolFor( const BuildKey& key )
    {
        uint64_t hash = std::hash<const void*>{}( key.Mesh ) ^ ( std::hash<const void*>{}( key.Topology ) << 1u );
        for ( const DisplacementMaterialRecord& m : key.Materials )
            hash = hash * 1099511628211ull ^
                   ( static_cast<uint64_t>( m.Texture ) << 32u | std::bit_cast<uint32_t>( m.ScaleCm ) );
        const auto [first, last] = m_Pools.equal_range( hash );
        for ( auto it = first; it != last; ++it )
            if ( auto pool = it->second.lock() )
                if ( pool->Mesh == key.Mesh && pool->Topology == key.Topology && pool->Materials == key.Materials )
                    return pool;
        auto pool       = std::make_shared<VertexPool>();
        pool->Mesh      = key.Mesh;
        pool->Topology  = key.Topology;
        pool->Materials = key.Materials;
        m_Pools.emplace( hash, pool );
        return pool;
    }

    Common::BoolResultStr DisplacementCache::EmitPieces( VertexPool& pool, std::vector<PoolPiece*>& batch,
                                                         const uint32_t firstVertex )
    {
        if ( batch.empty() )
            return BOOLSUCCESS;
        const StaticMesh&                             base     = *pool.Mesh;
        const Desert::Geometry::DisplacementTopology& topology = *pool.Topology;
        const bool                                    streams  = pool.Streams != nullptr;

        // The batch as one dispatch pair: points, samples and vertex records concatenated, rebased per piece.
        std::vector<Desert::Geometry::DisplacementPoint>  points;
        std::vector<Desert::Geometry::DisplacementSample> samples;
        std::vector<Desert::Geometry::DisplacementVertex> records;
        uint32_t                                          next = firstVertex;
        for ( PoolPiece* piece : batch )
        {
            const Desert::Geometry::TessellatedCluster& r          = *piece->Source;
            const auto                                  pointBase  = static_cast<uint32_t>( points.size() );
            const auto                                  sampleBase = static_cast<uint32_t>( samples.size() );
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
            piece->First = next;
            piece->Count = static_cast<uint32_t>( r.Vertices.size() );
            next += piece->Count;
        }
        const auto vertexCount = static_cast<uint32_t>( records.size() );
        const auto pointCount  = static_cast<uint32_t>( points.size() );
        if ( firstVertex + vertexCount > pool.Capacity )
            return Common::MakeFormattedError<bool>(
                 "displacement: {} vertices from {} overflow the pool of {}", vertexCount, firstVertex, pool.Capacity );

        RDG::ExternalBuffer baseVertices, baseStreams, poolVertices, poolStreams;
        if ( const auto in = Renderer::ImportVertexBuffer( base.GetVertexBuffer(), baseVertices ); !in )
            return Common::MakeError<bool>( in.GetError() );
        if ( streams )
            if ( const auto in = Renderer::ImportVertexBuffer( base.GetStreamBuffer(), baseStreams ); !in )
                return Common::MakeError<bool>( in.GetError() );
        if ( const auto in = Renderer::ImportVertexBuffer( pool.Vertices, poolVertices ); !in )
            return Common::MakeError<bool>( in.GetError() );
        if ( streams )
            if ( const auto in = Renderer::ImportVertexBuffer( pool.Streams, poolStreams ); !in )
                return Common::MakeError<bool>( in.GetError() );

        const std::vector<uint32_t>& topologyWords = TopologyWords( topology );
        const auto                   triangles     = static_cast<uint32_t>( topology.TriangleSubmesh.size() );
        const auto                   welds         = static_cast<uint32_t>( topology.WeldPosition.size() );
        std::vector<uint32_t>        materials;
        for ( const DisplacementMaterialRecord& m : pool.Materials )
        {
            const auto* words = reinterpret_cast<const uint32_t*>( &m );
            materials.insert( materials.end(), words,
                              words + sizeof( DisplacementMaterialRecord ) / sizeof( uint32_t ) );
        }
        // Counts.y: the pool vertex the Emit pass writes the batch's first record to.
        const std::array<uint32_t, 12> heightsDispatch = { pointCount,
                                                           0,
                                                           0,
                                                           0,
                                                           3 * triangles,
                                                           4 * triangles,
                                                           4 * triangles + 3 * welds,
                                                           streams ? 1u : 0u,
                                                           Bits( topology.BaseEdgeCm ),
                                                           0,
                                                           0,
                                                           0 };
        std::array<uint32_t, 12>       emitDispatch    = heightsDispatch;
        emitDispatch[0]                                = vertexCount;
        emitDispatch[1]                                = firstVertex;

        RDG::Builder         graph( "Displacement" );
        const RDG::BufferRef baseVerticesRef =
             graph.RegisterExternal( baseVertices, "Displacement.SourceVertices" );
        const RDG::BufferRef baseStreamsRef =
             streams ? graph.RegisterExternal( baseStreams, "Displacement.SourceStreams" ) : baseVerticesRef;
        const RDG::BufferRef outVerticesRef = graph.RegisterExternal( poolVertices, "Displacement.Vertices" );
        const auto           upload         = [&]( const auto& values, const char* name )
        {
            const RDG::BufferRef ref = graph.CreateBuffer( RDG::BufferDesc{ BytesOf( values ).size() }, name );
            graph.QueueBufferUpload( ref, BytesOf( values ) );
            return ref;
        };
        const std::vector<uint32_t> heightsDispatchWords( heightsDispatch.begin(), heightsDispatch.end() );
        const std::vector<uint32_t> emitDispatchWords( emitDispatch.begin(), emitDispatch.end() );
        const RDG::BufferRef        topologyRef  = upload( topologyWords, "Displacement.Topology" );
        const RDG::BufferRef        materialsRef = upload( materials, "Displacement.Materials" );
        const RDG::BufferRef        pointsRef    = upload( points, "Displacement.Points" );
        const RDG::BufferRef        samplesRef   = upload( samples, "Displacement.Samples" );
        const RDG::BufferRef        recordsRef   = upload( records, "Displacement.VertexRecords" );
        const RDG::BufferRef        heightsArgs  = upload( heightsDispatchWords, "Displacement.HeightsDispatch" );
        const RDG::BufferRef        emitArgs     = upload( emitDispatchWords, "Displacement.EmitDispatch" );
        const RDG::BufferRef        positionsRef =
             graph.CreateBuffer( RDG::BufferDesc{ static_cast<uint64_t>( pointCount ) * 3u * sizeof( float ) },
                                 "Displacement.Positions" );
        const RDG::BufferRef outStreamsRef =
             streams ? graph.RegisterExternal( poolStreams, "Displacement.Streams" )
                     : graph.CreateBuffer( RDG::BufferDesc{ 16 }, "Displacement.NoStreams" );
        const std::shared_ptr<ComputePipeline> heights       = m_HeightsPipeline;
        const std::shared_ptr<ComputePipeline> emit          = m_EmitPipeline;
        const auto& heightsLayout = m_HeightsLayout.Get( heights->GetSpecification().Shader );
        const auto& emitLayout    = m_EmitLayout.Get( emit->GetSpecification().Shader );
        const RDG::OtherRouteFill              heightsFill   = Renderer::GetPipelineRouteFill( *heights );
        const RDG::OtherRouteFill              emitFill      = Renderer::GetPipelineRouteFill( *emit );
        const uint32_t pointGroups  = ( pointCount + kDisplacementGroupSize - 1 ) / kDisplacementGroupSize;
        const uint32_t vertexGroups = ( vertexCount + kDisplacementGroupSize - 1 ) / kDisplacementGroupSize;
        graph.AddPass(
             kDisplacementHeightsPassName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( heightsLayout, heightsFill )
                      .Storage( "DisplacementSourceVertices", baseVerticesRef, RDG::Access::StorageRead )
                      .Storage( "DisplacementSourceStreams", baseStreamsRef, RDG::Access::StorageRead )
                      .Storage( "DisplacementTopology", topologyRef, RDG::Access::StorageRead )
                      .Storage( "DisplacementMaterials", materialsRef, RDG::Access::StorageRead )
                      .Storage( "DisplacementPoints", pointsRef, RDG::Access::StorageRead )
                      .Storage( "DisplacementSamples", samplesRef, RDG::Access::StorageRead )
                      .Storage( "DisplacementDispatch", heightsArgs, RDG::Access::StorageRead )
                      .Storage( "DisplacementPositions", positionsRef, RDG::Access::StorageWrite );
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
                      .Storage( "DisplacementSourceVertices", baseVerticesRef, RDG::Access::StorageRead )
                      .Storage( "DisplacementSourceStreams", baseStreamsRef, RDG::Access::StorageRead )
                      .Storage( "DisplacementTopology", topologyRef, RDG::Access::StorageRead )
                      .Storage( "DisplacementVertices", recordsRef, RDG::Access::StorageRead )
                      .Storage( "DisplacementPositions", positionsRef, RDG::Access::StorageRead )
                      .Storage( "DisplacementDispatch", emitArgs, RDG::Access::StorageRead )
                      .Storage( "DisplacementOutputVertices", outVerticesRef, RDG::Access::StorageWrite )
                      .Storage( "DisplacementOutputStreams", outStreamsRef, RDG::Access::StorageWrite );
             },
             [emit, vertexGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::DispatchCompute( bindings, *emit, vertexGroups, 1, 1 );
             } );
        // Handed to vertex input at the graph's end: the view graphs bind the pool as their vertex stream (the
        // morph twin rule); the BLAS build declares its own AccelStructBuildInput read.
        graph.Extract( outVerticesRef, poolVertices, RDG::Access::VertexIndex );
        if ( streams )
            graph.Extract( outStreamsRef, poolStreams, RDG::Access::VertexIndex );
        if ( const Common::BoolResultStr executed = Renderer::ExecuteGraph( graph ); !executed )
            return Common::MakeFormattedError<bool>( "displacement: the graph of {} vertices did not execute: {}",
                                               vertexCount, executed.GetError() );
        pool.Used = std::max( pool.Used, next );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr DisplacementCache::RebuildPool( VertexPool& pool, const std::vector<std::string>& wanted,
                                                          const Tessellation& t )
    {
        // Live = what the pool's live geometries draw, plus the geometry being built.
        std::vector<std::shared_ptr<SharedGeometry>> geometries;
        std::unordered_set<std::string>              live( wanted.begin(), wanted.end() );
        for ( const auto& [hash, weak] : m_Shared )
            if ( auto geometry = weak.lock(); geometry && geometry->Pool.get() == &pool )
            {
                live.insert( geometry->Pieces.begin(), geometry->Pieces.end() );
                geometries.push_back( std::move( geometry ) );
            }
        std::unordered_map<std::string, PoolPiece> pieces;
        uint64_t                                   total = 0;
        for ( const std::string& key : live )
        {
            PoolPiece piece;
            if ( const auto found = pool.Pieces.find( key ); found != pool.Pieces.end() )
                piece = found->second;
            else
            {
                uint32_t cluster = 0;
                std::memcpy( &cluster, key.data(), sizeof( uint32_t ) );
                piece.Source  = t.Clusters[cluster];
                piece.Submesh = pool.Topology->Clusters[cluster].Submesh;
            }
            total += piece.Source->Vertices.size();
            pieces.emplace( key, std::move( piece ) );
        }
        const bool     streams  = pool.Mesh->GetStreamBuffer() != nullptr;
        const uint64_t capacity = std::max<uint64_t>( 2 * total, 4096 );
        if ( capacity > std::numeric_limits<uint32_t>::max() / PoolStride( streams ) )
            return Common::MakeFormattedError<bool>( "displacement: a vertex pool of {} vertices exceeds a buffer",
                                               capacity );
        auto vertices = VertexBuffer::Create( static_cast<uint32_t>( capacity * sizeof( Vertex ) ),
                                              BufferUsage::GpuWritten );
        if ( const auto built = vertices->RT_Invalidate(); !built.IsSuccess() )
            return Common::MakeFormattedError<bool>( "displacement: a pool of {} vertices could not be allocated: {}",
                                               capacity, built.GetError() );
        vertices->ClaimOwnership( ResourceOwner::SceneRenderer );
        std::shared_ptr<VertexBuffer> streamBuffer;
        if ( streams )
        {
            streamBuffer = VertexBuffer::Create( static_cast<uint32_t>( capacity * sizeof( MeshVertexStreams ) ),
                                                 BufferUsage::GpuWritten );
            if ( const auto built = streamBuffer->RT_Invalidate(); !built.IsSuccess() )
                return Common::MakeFormattedError<bool>(
                     "displacement: a pool of {} stream vertices could not be allocated: {}", capacity,
                     built.GetError() );
            streamBuffer->ClaimOwnership( ResourceOwner::SceneRenderer );
        }
        pool.Vertices = std::move( vertices );
        pool.Streams  = std::move( streamBuffer );
        pool.Capacity = static_cast<uint32_t>( capacity );
        pool.Used     = 0;
        pool.Pieces   = std::move( pieces );
        // Re-emitted in key order: a deterministic layout.
        std::vector<std::pair<const std::string*, PoolPiece*>> order;
        for ( auto& [key, piece] : pool.Pieces )
            order.emplace_back( &key, &piece );
        std::sort( order.begin(), order.end(), []( const auto& a, const auto& b ) { return *a.first < *b.first; } );
        std::vector<PoolPiece*> batch;
        for ( const auto& [key, piece] : order )
            batch.push_back( piece );
        if ( const Common::BoolResultStr emitted = EmitPieces( pool, batch, 0 ); !emitted )
            return emitted;
        for ( const auto& geometry : geometries )
            if ( const Common::BoolResultStr indexed = IndexGeometry( *geometry ); !indexed )
                return indexed;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr DisplacementCache::IndexGeometry( SharedGeometry& geometry )
    {
        const VertexPool&                             pool     = *geometry.Pool;
        const Desert::Geometry::DisplacementTopology& topology = *pool.Topology;
        const size_t                                  submeshes = pool.Mesh->GetSubmeshes().size();
        // Clusters are cooked per submesh in order, so each submesh's indices are contiguous.
        std::vector<StaticMesh::DisplacedRange> ranges( submeshes );
        std::vector<uint32_t>                   indices;
        for ( uint32_t c = 0; c < geometry.Pieces.size(); ++c )
        {
            const auto found = pool.Pieces.find( geometry.Pieces[c] );
            if ( found == pool.Pieces.end() )
                return Common::MakeFormattedError<bool>( "displacement: cluster {} has no piece in its pool", c );
            const PoolPiece& piece   = found->second;
            const uint32_t   submesh = topology.Clusters[c].Submesh;
            if ( ranges[submesh].IndexCount == 0 )
                ranges[submesh].FirstIndex = static_cast<uint32_t>( indices.size() );
            for ( const uint32_t index : piece.Source->Indices )
                indices.push_back( piece.First + index );
            ranges[submesh].IndexCount += static_cast<uint32_t>( piece.Source->Indices.size() );
        }
        if ( indices.empty() )
            return Common::MakeError<bool>( "displacement: the geometry draws no triangles" );
        auto indexBuffer = IndexBuffer::Create(
             indices.data(), static_cast<uint32_t>( indices.size() * sizeof( uint32_t ) ), BufferUsage::Static );
        if ( const auto built = indexBuffer->RT_Invalidate(); !built.IsSuccess() )
            return Common::MakeFormattedError<bool>( "displacement: {} indices could not be uploaded: {}",
                                                     indices.size(), built.GetError() );
        indexBuffer->ClaimOwnership( ResourceOwner::SceneRenderer );
        // The bounds grow by the largest displacement any submesh can apply: |ScaleCm| times the farther of the
        // centre's two distances to the height range ends.
        float growth = 0.0f;
        for ( const DisplacementMaterialRecord& m : pool.Materials )
            if ( m.Texture != 0xffffffffu )
                growth = std::max( growth, std::abs( m.ScaleCm ) * std::max( m.Center, 1.0f - m.Center ) );
        if ( geometry.Twin )
            RetireTwin( std::move( geometry.Twin ) );
        geometry.Twin = StaticMesh::MakeDisplacedTwin( *pool.Mesh, pool.Vertices, pool.Streams, indexBuffer, ranges,
                                                       pool.Capacity, growth );
        return BOOLSUCCESS;
    }

    void DisplacementCache::RetireTwin( std::shared_ptr<StaticMesh> twin )
    {
        if ( twin )
        {
            const uint64_t now = ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
            m_RetiringTwins.Park( std::move( twin ), now );
        }
    }

    Common::ResultStr<std::shared_ptr<DisplacementCache::SharedGeometry>>
    DisplacementCache::Emit( const BuildKey& key, const Tessellation& t )
    {
        using Result                                           = std::shared_ptr<SharedGeometry>;
        const Desert::Geometry::DisplacementTopology& topology = *key.Topology;
        if ( t.Clusters.size() != topology.Clusters.size() || t.VertexCount() == 0 || t.IndexCount() == 0 )
            return Common::MakeFormattedError<Result>(
                 "displacement: the tessellation has {} clusters of {}, {} vertices, {} indices", t.Clusters.size(),
                 topology.Clusters.size(), t.VertexCount(), t.IndexCount() );
        auto geometry  = std::make_shared<SharedGeometry>();
        geometry->Key  = key;
        geometry->Pool = PoolFor( key );
        VertexPool& pool = *geometry->Pool;
        const bool  streams = key.Mesh->GetStreamBuffer() != nullptr;

        // The pieces this geometry draws: the pool's, or appended.
        geometry->Pieces.resize( t.Clusters.size() );
        std::vector<uint32_t> missing;
        uint64_t              missingVertices = 0;
        for ( uint32_t c = 0; c < t.Clusters.size(); ++c )
        {
            geometry->Pieces[c] = PieceKeyOf( topology, key.Levels, c );
            if ( !pool.Pieces.contains( geometry->Pieces[c] ) )
            {
                missing.push_back( c );
                missingVertices += t.Clusters[c]->Vertices.size();
            }
        }
        if ( pool.Vertices == nullptr || pool.Used + missingVertices > pool.Capacity )
        {
            if ( const Common::BoolResultStr rebuilt = RebuildPool( pool, geometry->Pieces, t ); !rebuilt )
                return Common::MakeError<Result>( rebuilt.GetError() );
        }
        else if ( !missing.empty() )
        {
            std::vector<PoolPiece*> batch;
            for ( const uint32_t c : missing )
            {
                PoolPiece& piece = pool.Pieces[geometry->Pieces[c]];
                piece.Source     = t.Clusters[c];
                piece.Submesh    = topology.Clusters[c].Submesh;
                batch.push_back( &piece );
            }
            if ( const Common::BoolResultStr emitted = EmitPieces( pool, batch, pool.Used ); !emitted )
            {
                for ( const uint32_t c : missing )
                    pool.Pieces.erase( geometry->Pieces[c] );
                return Common::MakeError<Result>( emitted.GetError() );
            }
        }
        if ( const Common::BoolResultStr indexed = IndexGeometry( *geometry ); !indexed )
            return Common::MakeError<Result>( indexed.GetError() );
        geometry->Bytes = missingVertices * PoolStride( streams ) + t.IndexCount() * sizeof( uint32_t );
        return Common::MakeSuccess( geometry );
    }

    Common::BoolResultStr DisplacementCache::DisplaceFrame( const uint64_t frame )
    {
        // The first view of the frame displaces; later views only read (their requests count for the next frame).
        if ( frame == m_DisplacedFrame )
            return BOOLSUCCESS;
        m_DisplacedFrame      = frame;
        m_Frame               = frame;
        // Twins replaced or dropped in earlier frames are released once no frame in flight can still draw them.
        {
            const auto& frames = ::Desert::Engine::FrameManager::GetInstance();
            m_RetiringTwins.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );
        }
        m_Stats.Rebuilt       = 0;
        m_Stats.LevelsLowered = 0;
        m_Stats.LevelsRaised  = 0;
        m_Stats.LevelsHeld    = 0;
        if ( !m_Quality.Enabled || m_Entries.empty() || !EnsurePipelines() )
            return BOOLSUCCESS;

        uint64_t liveBytes = 0;
        {
            std::unordered_set<const SharedGeometry*> counted;
            for ( const auto& [key, entry] : m_Entries )
                if ( entry.Current && counted.insert( entry.Current.get() ).second )
                    liveBytes += entry.Current->Bytes;
        }

        // The keys in flight at the start of this pass, and those launched during it (value-hashed).
        // Copies: a pending key is moved out of its entry when its tessellation is collected below.
        std::unordered_multimap<uint64_t, BuildKey> pendingKeys;
        for ( const auto& [key, entry] : m_Entries )
            if ( entry.PendingKey )
                pendingKeys.emplace( HashKey( *entry.PendingKey ), *entry.PendingKey );

        std::vector<std::string> errors;
        for ( auto& [entryKey, entry] : m_Entries )
        {
            if ( entry.LastFrame != frame )
                continue; // no view submitted it this frame: its base mesh may be gone, EndFrame drops it
            if ( entry.Using.empty() )
                entry.Using = entry.Asked; // an entry's first frame: its first requests
            if ( entry.Using.empty() )
                continue;
            if ( entry.Lowered > 0 )
            {
                // §5 both ways: a lowering is undone once the pool holds the size it was measured at again.
                const uint64_t held = entry.Current && entry.Current.use_count() == 1 ? entry.Current->Bytes : 0;
                const bool     fits = m_Quality.PoolBytes == 0 ||
                                  liveBytes - held + entry.RaiseBytes.back() <= m_Quality.PoolBytes;
                if ( fits || entry.RaiseLevels.back() != entry.Using )
                {
                    --entry.Lowered;
                    entry.RaiseBytes.pop_back();
                    entry.RaiseLevels.pop_back();
                    ++m_Stats.LevelsRaised;
                }
                m_Stats.LevelsHeld += entry.Lowered;
            }
            std::vector<uint8_t> effective = entry.Using;
            for ( uint8_t& level : effective )
                level = static_cast<uint8_t>( level > entry.Lowered ? level - entry.Lowered : 0 );
            const BuildKey wanted{ entry.Mesh, entry.Topology.get(), std::move( effective ), entry.Materials };

            // Collect a finished tessellation.
            if ( entry.Pending.valid() &&
                 entry.Pending.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
            {
                Common::ResultStr<Tessellation> done = entry.Pending.get();
                const BuildKey                  key  = std::move( *entry.PendingKey );
                entry.PendingKey.reset();
                if ( !done.IsSuccess() )
                {
                    if ( !entry.RefusalLogged )
                        errors.push_back( std::format( "entry {:#x}: {}", entryKey, done.GetError() ) );
                    entry.RefusalLogged = true;
                    continue;
                }
                const Tessellation& t     = done.GetValue();
                // At most this much: pieces the pool already holds are not appended again.
                const uint64_t bytes = t.VertexCount() * ( sizeof( Vertex ) + sizeof( MeshVertexStreams ) ) +
                                       t.IndexCount() * sizeof( uint32_t );
                const uint64_t released =
                     entry.Current && entry.Current.use_count() == 1 ? entry.Current->Bytes : 0;
                if ( m_Quality.PoolBytes != 0 && liveBytes - released + bytes > m_Quality.PoolBytes )
                {
                    // §5: the pool does not hold it - the entry with the smallest screen size drops one level on
                    // every cluster above 0, counted; this result is discarded and the lowered key retessellates.
                    Entry* smallest = &entry;
                    for ( auto& [otherKey, other] : m_Entries )
                        if ( other.SmallestScreen < smallest->SmallestScreen &&
                             std::any_of( other.Using.begin(), other.Using.end(),
                                          []( uint8_t l ) { return l > 0; } ) )
                            smallest = &other;
                    for ( const uint8_t level : smallest->Using )
                        if ( level > smallest->Lowered )
                            ++m_Stats.LevelsLowered;
                    // The size one step up, exact: this result for the entry itself, else what it holds now.
                    smallest->RaiseBytes.push_back(
                         smallest == &entry ? bytes : ( smallest->Current ? smallest->Current->Bytes : 0 ) );
                    smallest->RaiseLevels.push_back( smallest->Using );
                    ++smallest->Lowered;
                    continue;
                }
                auto emitted = Emit( key, t );
                if ( !emitted.IsSuccess() )
                {
                    errors.push_back( std::format( "entry {:#x}: {}", entryKey, emitted.GetError() ) );
                    continue;
                }
                liveBytes += bytes;
                liveBytes -= released;
                if ( entry.Current )
                    RetireTwin( entry.Current->Twin );
                entry.Current = emitted.GetValue();
                m_Shared.emplace( HashKey( entry.Current->Key ), entry.Current );
                ++m_Stats.Rebuilt;
            }

            if ( ( entry.Current && entry.Current->Key == wanted ) ||
                 ( entry.PendingKey && *entry.PendingKey == wanted ) )
                continue;
            if ( auto shared = FindShared( wanted ) )
            {
                if ( entry.Current )
                    RetireTwin( entry.Current->Twin );
                entry.Current = std::move( shared );
                continue;
            }
            if ( entry.Pending.valid() )
                continue; // one tessellation in flight per entry; the newer key follows when it lands
            // Another entry is already tessellating this key: it lands in m_Shared and FindShared hands it to
            // this entry next frame - one worker per distinct key, not one per instance.
            if ( const uint64_t hash = HashKey( wanted ); pendingKeys.contains( hash ) )
            {
                const auto [first, last] = pendingKeys.equal_range( hash );
                if ( std::any_of( first, last, [&wanted]( const auto& p ) { return p.second == wanted; } ) )
                    continue;
            }
            // Tessellate on a worker: the topology is co-owned by the task, the levels copied.
            std::shared_ptr<const Desert::Geometry::DisplacementTopology> topology = entry.Topology;
            std::vector<uint8_t> levels = wanted.Levels;
            entry.PendingKey            = wanted;
            pendingKeys.emplace( HashKey( wanted ), wanted );
            entry.Pending               = Common::JobSystem::Get().Async(
                 [topology, levels = std::move( levels )]() -> Common::ResultStr<Tessellation>
                 {
                     Tessellation out;
                     out.Clusters.reserve( topology->Clusters.size() );
                     for ( uint32_t c = 0; c < topology->Clusters.size(); ++c )
                     {
                         auto cluster = Desert::Geometry::TessellateCluster( *topology, c, levels );
                         if ( !cluster.IsSuccess() )
                             return Common::MakeError<Tessellation>( cluster.GetError() );
                         out.Clusters.push_back(
                              std::make_shared<const Desert::Geometry::TessellatedCluster>( cluster.ExtractValue() ) );
                     }
                     return Common::MakeSuccess( std::move( out ) );
                 } );
        }
        if ( !errors.empty() )
        {
            std::string joined;
            for ( const std::string& e : errors )
                std::format_to( std::back_inserter( joined ), "{}{}", joined.empty() ? "" : "; ", e );
            return Common::MakeError(
                 std::format( "[Displacement] {} primitive(s) keep their base mesh: {}", errors.size(), joined ) );
        }
        return BOOLSUCCESS;
    }

    std::shared_ptr<StaticMesh> DisplacementCache::Find( const uint32_t entity, const uint32_t part ) const
    {
        const auto found = m_Entries.find( EntryKey( entity, part ) );
        if ( found == m_Entries.end() || !found->second.Current )
            return nullptr;
        return found->second.Current->Twin;
    }

    void DisplacementCache::EndFrame( const uint64_t frame )
    {
        for ( const auto& [key, entry] : m_Entries )
            if ( entry.LastFrame != frame && entry.Current )
                RetireTwin( entry.Current->Twin ); // this frame's draws may still name it
        std::erase_if( m_Entries, [frame]( const auto& item ) { return item.second.LastFrame != frame; } );
        std::erase_if( m_Shared, []( const auto& shared ) { return shared.second.expired(); } );
        std::erase_if( m_Pools, []( const auto& pool ) { return pool.second.expired(); } );

        std::unordered_set<const Desert::Geometry::DisplacementTopology*> topologies;
        std::unordered_set<const SharedGeometry*>                         geometries;
        m_Stats.Entries       = static_cast<uint32_t>( m_Entries.size() );
        m_Stats.Displaced     = 0;
        m_Stats.Pending       = 0;
        m_Stats.BufferBytes   = 0;
        m_Stats.TopologyBytes = 0;
        for ( const auto& [key, entry] : m_Entries )
        {
            if ( entry.Pending.valid() )
                ++m_Stats.Pending;
            if ( entry.Current )
            {
                ++m_Stats.Displaced;
                if ( geometries.insert( entry.Current.get() ).second )
                    m_Stats.BufferBytes += entry.Current->Bytes;
            }
            if ( entry.Topology && topologies.insert( entry.Topology.get() ).second )
                m_Stats.TopologyBytes += entry.Topology->GetBytes();
        }
        m_Stats.Geometries = static_cast<uint32_t>( geometries.size() );
        // The GPU words of a topology no live entry reads are dropped (the address could be reused by a new mesh).
        std::erase_if( m_TopologyWords, [&]( const auto& item ) { return !topologies.contains( item.first ); } );
        m_Stats.LevelsClamped = 0;
    }
} // namespace Desert::Graphic::System

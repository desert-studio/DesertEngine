#include "SkinnedMesh.hpp"

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/BoneInfo.hpp>

#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/glm.hpp>

#include <Engine/Geometry/MeshLOD.hpp>

#include <algorithm>
#include <array>
#include <unordered_map>

namespace Desert
{
    namespace
    {
        // UE's skeletal LOD reduction default shape (FSkeletalMeshOptimizationSettings::MaxBonesPerVertex): the
        // authored influences at LOD 0, two at LOD 1, one beyond - a far character's skinning is one bone a vertex.
        [[nodiscard]] uint32_t LODMaxInfluences( const std::size_t level )
        {
            if ( level == 0 )
                return static_cast<uint32_t>( SkinnedVertex::MAX_BONE_INFLUENCES );
            return level == 1 ? 2u : 1u;
        }

        // @p vertex with its @p maxInfluences heaviest influences, renormalised (UE FSkinWeightInfo reduction).
        [[nodiscard]] SkinnedVertex ReduceInfluences( const SkinnedVertex& vertex, const uint32_t maxInfluences )
        {
            std::array<std::size_t, SkinnedVertex::MAX_BONE_INFLUENCES> order{};
            for ( std::size_t k = 0; k < order.size(); ++k )
                order[k] = k;
            std::stable_sort( order.begin(), order.end(), [&]( const std::size_t a, const std::size_t b )
                              { return vertex.BoneWeights[a] > vertex.BoneWeights[b]; } );
            SkinnedVertex reduced = vertex;
            reduced.BoneIDs.fill( 0u );
            reduced.BoneWeights.fill( 0.0f );
            float sum = 0.0f;
            for ( std::size_t k = 0; k < std::min<std::size_t>( maxInfluences, order.size() ); ++k )
            {
                reduced.BoneIDs[k]     = vertex.BoneIDs[order[k]];
                reduced.BoneWeights[k] = std::max( vertex.BoneWeights[order[k]], 0.0f );
                sum += reduced.BoneWeights[k];
            }
            if ( sum <= 0.0f )
                return vertex; // an unweighted vertex stays as authored
            for ( float& weight : reduced.BoneWeights )
                weight /= sum;
            return reduced;
        }

        [[nodiscard]] std::vector<uint32_t> RequiredBoneList( const std::vector<SkinnedVertex>& vertices,
                                                              const std::size_t first, const std::size_t count )
        {
            std::vector<uint32_t> bones;
            for ( std::size_t v = first; v < first + count; ++v )
                for ( std::size_t k = 0; k < SkinnedVertex::MAX_BONE_INFLUENCES; ++k )
                    if ( vertices[v].BoneWeights[k] > 0.0f )
                        bones.push_back( vertices[v].BoneIDs[k] );
            std::sort( bones.begin(), bones.end() );
            bones.erase( std::unique( bones.begin(), bones.end() ), bones.end() );
            return bones;
        }

        // THE SKELETAL LOD CHAIN (UE FSkeletalMeshLODRenderData per LOD): the static chain's index levels
        // (Geometry::BuildLODIndexBuffer - the cooked BakedLODs, else meshopt at the unbaked chain), then each level
        // >= 1 re-pointed at its OWN vertex copies appended after the authored ones, level after level, with its
        // influences reduced (LODMaxInfluences). A submesh whose chain is shorter repeats its last level at every
        // later one, so level L of every submesh lies in level L's vertex range and skinning that range alone is
        // complete for a draw of level L. Streams and morph deltas follow their source vertex.
        struct SkinnedLODChain
        {
            std::vector<SkinnedVertex>            Vertices;
            std::vector<MeshVertexStreams>        Streams;
            std::vector<MorphTarget>              Morphs;
            std::vector<Index>                    GpuIndices;
            std::vector<Submesh>                  Submeshes;
            std::vector<SkinnedMesh::SkinnedLOD>  LODs;
        };

        [[nodiscard]] SkinnedLODChain BuildSkinnedLODChain( const std::vector<SkinnedVertex>&     vertices,
                                                            const std::vector<Index>&             indices,
                                                            const std::vector<Submesh>&           submeshes,
                                                            const std::vector<MeshVertexStreams>& streams,
                                                            const std::vector<MorphTarget>&       morphTargets )
        {
            SkinnedLODChain chain{ .Vertices = vertices, .Streams = streams, .Morphs = morphTargets,
                                   .GpuIndices = {}, .Submeshes = submeshes, .LODs = {} };
            const auto authored = static_cast<uint32_t>( vertices.size() );
            std::vector<uint32_t> authoredBones = RequiredBoneList( vertices, 0u, vertices.size() );
            chain.LODs.push_back( { .FirstVertex   = 0u,
                                    .VertexCount   = authored,
                                    .RequiredBones = static_cast<uint32_t>( authoredBones.size() ),
                                    .MaxInfluences = LODMaxInfluences( 0 ),
                                    .Bones         = std::move( authoredBones ) } );

            std::vector<Vertex> positions;
            positions.reserve( vertices.size() );
            for ( const SkinnedVertex& vertex : vertices )
                positions.push_back( vertex.StaticVertex );
            chain.GpuIndices = Geometry::BuildLODIndexBuffer( positions, indices, chain.Submeshes,
                                                              Geometry::UnbakedLODChain() );
            for ( Submesh& submesh : chain.Submeshes )
                std::vector<std::vector<Index>>().swap( submesh.BakedLODs );

            std::size_t levels = 1;
            for ( const Submesh& submesh : chain.Submeshes )
                levels = std::max( levels, submesh.LODs.size() );
            if ( levels <= 1 || vertices.empty() )
                return chain;

            // Morph arrays padded to the authored count (the asset may stop short: a zero delta), so an appended
            // vertex's delta is its source's.
            for ( MorphTarget& target : chain.Morphs )
            {
                target.DeltaPositions.resize( vertices.size(), glm::vec3( 0.0f ) );
                if ( !target.DeltaNormals.empty() )
                    target.DeltaNormals.resize( vertices.size(), glm::vec3( 0.0f ) );
            }
            // The levels as BuildLODIndexBuffer wrote them (over the authored vertices): the source of every copy.
            const std::vector<Index>    source       = chain.GpuIndices;
            const std::vector<Submesh>  sourceRanges = chain.Submeshes;
            std::unordered_map<uint32_t, uint32_t> copies;
            for ( std::size_t level = 1; level < levels; ++level )
            {
                const uint32_t maxInfluences = LODMaxInfluences( level );
                const auto     first         = static_cast<uint32_t>( chain.Vertices.size() );
                for ( std::size_t s = 0; s < chain.Submeshes.size(); ++s )
                {
                    Submesh&        submesh = chain.Submeshes[s];
                    const Submesh&  from    = sourceRanges[s];
                    if ( from.LODs.empty() )
                        continue;
                    const LODRange read  = from.LODs[std::min( level, from.LODs.size() - 1u )];
                    // A level the submesh lacks gets its own triangles at the end of the buffer.
                    if ( level >= from.LODs.size() )
                    {
                        submesh.LODs.push_back(
                             { static_cast<uint32_t>( chain.GpuIndices.size() * 3u ), read.IndexCount } );
                        chain.GpuIndices.insert( chain.GpuIndices.end(), read.IndexCount / 3u, Index{ 0u, 0u, 0u } );
                    }
                    const LODRange write = submesh.LODs[level];
                    copies.clear();
                    const auto copyOf = [&]( const uint32_t local ) -> uint32_t
                    {
                        const auto [found, fresh] = copies.try_emplace( local, 0u );
                        if ( fresh )
                        {
                            const std::size_t at = static_cast<std::size_t>( from.VertexOffset ) + local;
                            found->second = static_cast<uint32_t>( chain.Vertices.size() ) - from.VertexOffset;
                            chain.Vertices.push_back( ReduceInfluences( vertices[at], maxInfluences ) );
                            if ( !streams.empty() )
                                chain.Streams.push_back( streams[std::min( at, streams.size() - 1u )] );
                            for ( MorphTarget& target : chain.Morphs )
                            {
                                target.DeltaPositions.push_back( target.DeltaPositions[at] );
                                if ( !target.DeltaNormals.empty() )
                                    target.DeltaNormals.push_back( target.DeltaNormals[at] );
                            }
                        }
                        return found->second;
                    };
                    for ( uint32_t t = 0; t < read.IndexCount / 3u; ++t )
                    {
                        const Index& tri = source[read.IndexOffset / 3u + t];
                        chain.GpuIndices[write.IndexOffset / 3u + t] =
                             Index{ copyOf( tri.V1 ), copyOf( tri.V2 ), copyOf( tri.V3 ) };
                    }
                }
                const auto count = static_cast<uint32_t>( chain.Vertices.size() ) - first;
                std::vector<uint32_t> levelBones = RequiredBoneList( chain.Vertices, first, count );
                chain.LODs.push_back( { .FirstVertex   = first,
                                        .VertexCount   = count,
                                        .RequiredBones = static_cast<uint32_t>( levelBones.size() ),
                                        .MaxInfluences = maxInfluences,
                                        .Bones         = std::move( levelBones ) } );
            }
            return chain;
        }
    } // namespace

    SkinnedMesh::SkinnedMesh( const std::vector<SkinnedVertex>& authoredVertices, const std::vector<Index>& indices,
                              const std::vector<Submesh>& submeshes, const Animation::Skeleton* skeleton,
                              const std::vector<MeshVertexStreams>& authoredStreams,
                              const std::vector<MorphTarget>&       authoredMorphTargets )
         : m_Skeleton( skeleton )
    {
        SkinnedLODChain chain = BuildSkinnedLODChain( authoredVertices, indices, submeshes, authoredStreams,
                                                      authoredMorphTargets );
        const std::vector<SkinnedVertex>&     vertices     = chain.Vertices;
        const std::vector<MeshVertexStreams>& streams      = chain.Streams;
        const std::vector<MorphTarget>&       morphTargets = chain.Morphs;
        m_VertexCount = static_cast<uint32_t>( vertices.size() );
        m_SkinnedLODs = chain.LODs;
        m_Submeshes   = std::move( chain.Submeshes );
        std::vector<Vertex> authoredPositions;
        authoredPositions.reserve( authoredVertices.size() );
        for ( const SkinnedVertex& vertex : authoredVertices )
            authoredPositions.push_back( vertex.StaticVertex );
        m_LODScreenCoverage = Geometry::LODScreenCoverage( authoredPositions, indices, m_Submeshes );
        ResolveLODScreenSizes();
        m_Vertices = vertices; // retained on the CPU for viewport picking (posed-AABB)

        // GetBoneBounds: per submesh, the box of each influencing bone's vertices.
        for ( size_t s = 0; s < m_Submeshes.size(); ++s )
        {
            const Submesh& submesh = m_Submeshes[s];
            const size_t   first   = std::min<size_t>( submesh.VertexOffset, vertices.size() );
            const size_t   last    = std::min<size_t>( first + submesh.VertexCount, vertices.size() );
            const size_t   begin   = m_BoneBounds.size();
            for ( size_t v = first; v < last; ++v )
            {
                const SkinnedVertex& vertex = vertices[v];
                for ( size_t k = 0; k < SkinnedVertex::MAX_BONE_INFLUENCES; ++k )
                {
                    if ( vertex.BoneWeights[k] <= 0.0f )
                        continue;
                    const glm::vec3 position = vertex.StaticVertex.Position;
                    const auto known = std::find_if( m_BoneBounds.begin() + static_cast<std::ptrdiff_t>( begin ),
                                                     m_BoneBounds.end(), [&]( const BoneBounds& b )
                                                     { return b.Bone == vertex.BoneIDs[k]; } );
                    if ( known == m_BoneBounds.end() )
                    {
                        m_BoneBounds.push_back( { .Submesh = static_cast<uint32_t>( s ),
                                                  .Bone    = vertex.BoneIDs[k],
                                                  .Box     = { .Min = position, .Max = position } } );
                        continue;
                    }
                    known->Box.Min = glm::min( known->Box.Min, position );
                    known->Box.Max = glm::max( known->Box.Max, position );
                }
            }
        }

        // ONE BONE INDEX SPACE, CHECKED WHERE THE TWO SIDES MEET. This is the only moment a mesh and a rig
        // are joined, and until now nothing compared them: the property that a vertex's BoneIDs index the
        // same array as the skeleton's bones and the animator's pose was true by construction of the
        // importer and guarded by nothing at all. It is what lets this engine skip UE's per-mesh linkup
        // table and two of its three index spaces, so it is worth one pass over the influences at load.
        if ( m_Skeleton )
        {
            std::vector<uint32_t> influences;
            influences.reserve( vertices.size() * SkinnedVertex::MAX_BONE_INFLUENCES );
            for ( const auto& v : vertices )
                for ( size_t j = 0; j < SkinnedVertex::MAX_BONE_INFLUENCES; ++j )
                    if ( v.BoneWeights[j] > 0.0f )
                        influences.push_back( v.BoneIDs[j] );

            if ( const auto valid = m_Skeleton->ValidateBoneIndexSpace( influences, "skinned mesh" );
                 !valid.IsSuccess() )
            {
                LOG_ERROR( "[SkinnedMesh] {}", valid.GetError() );
            }

            if ( !m_Skeleton->GetStructureError().empty() )
            {
                LOG_ERROR( "[SkinnedMesh] the rig this mesh is skinned to is malformed: {}",
                           m_Skeleton->GetStructureError() );
            }
        }

        // Uploaded into the pool's skinned arena by Invalidate (the renderer thread).
        StageGeometry( Graphic::GeometryArena::Skinned, VertexBytes( vertices ), streams,
                       FlattenIndices( chain.GpuIndices ) );

        // The morph deltas, dense per target (a vertex a target leaves alone carries a zero delta - the asset's
        // own layout, MorphTarget). A target whose arrays are shorter than the mesh is padded with zeros, the
        // CPU blend's rule (Geometry::ApplyMorphTargets reads min(size)).
        if ( !morphTargets.empty() && !vertices.empty() )
        {
            m_MorphTargetCount = static_cast<uint32_t>( morphTargets.size() );
            std::vector<glm::vec4> deltas( static_cast<size_t>( m_MorphTargetCount ) * m_VertexCount * 2,
                                           glm::vec4( 0.0f ) );
            for ( size_t k = 0; k < morphTargets.size(); ++k )
            {
                const MorphTarget& target = morphTargets[k];
                const size_t       first  = k * m_VertexCount * 2;
                float              longest = 0.0f;
                for ( size_t i = 0; i < std::min<size_t>( m_VertexCount, target.DeltaPositions.size() ); ++i )
                {
                    deltas[first + i * 2] = glm::vec4( target.DeltaPositions[i], 0.0f );
                    longest               = std::max( longest, glm::length( target.DeltaPositions[i] ) );
                }
                m_MorphTargetReach += longest;
                for ( size_t i = 0; i < std::min<size_t>( m_VertexCount, target.DeltaNormals.size() ); ++i )
                    deltas[first + i * 2 + 1] = glm::vec4( target.DeltaNormals[i], 0.0f );
            }
            const auto bytes = static_cast<uint32_t>( deltas.size() * sizeof( glm::vec4 ) );
            m_MorphDeltas    = ShaderResources::StorageBuffer::Create( "MorphDeltas", bytes, 1, true );
            m_MorphDeltas->ClaimOwnership( Graphic::ResourceOwner::AssetService );
            if ( const auto uploaded = m_MorphDeltas->SetData( deltas.data(), bytes ); !uploaded.IsSuccess() )
            {
                LOG_ERROR( "[SkinnedMesh] the morph targets were not uploaded, the mesh draws unmorphed: {}",
                           uploaded.GetError() );
                m_MorphDeltas.reset();
                m_MorphTargetCount = 0;
            }
        }
    }

    std::shared_ptr<SkinnedMesh>
    SkinnedMesh::MakeMorphedTwin( const SkinnedMesh& base, std::shared_ptr<Graphic::VertexBuffer> morphedVertices,
                                  std::shared_ptr<Graphic::VertexBuffer> previousVertices )
    {
        // Private default constructor: make_shared cannot reach it.
        std::shared_ptr<SkinnedMesh> twin( new SkinnedMesh() );
        // The morph pass's own vertices (offset 0) over the base's pooled streams and indices at the base's
        // offsets: not pooled as a whole, so it binds per mesh (MeshGeometry::MeshBinding).
        const Graphic::MeshGeometry& baseGeometry = base.GetGeometry();
        Graphic::MeshGeometry        geometry;
        geometry.Vertices         = std::move( morphedVertices );
        geometry.PreviousVertices = std::move( previousVertices ); // binding 3: last frame's blend
        geometry.Streams          = baseGeometry.Streams;
        geometry.Indices          = baseGeometry.Indices;
        geometry.StreamByteOffset = baseGeometry.StreamByteOffset;
        geometry.IndexByteOffset  = baseGeometry.IndexByteOffset;
        geometry.VertexStride     = baseGeometry.VertexStride;
        geometry.VertexCount      = baseGeometry.VertexCount;
        geometry.IndexCount       = baseGeometry.IndexCount;
        geometry.AuthoredStreams  = baseGeometry.AuthoredStreams;
        twin->SetUnpooledGeometry( std::move( geometry ) );
        twin->m_PoolAllocation   = base.m_PoolAllocation; // the base's spans outlive every twin reading them
        twin->m_Submeshes        = base.m_Submeshes;
        twin->m_Skeleton         = base.m_Skeleton;
        twin->m_VertexCount      = base.m_VertexCount;
        twin->m_MorphTargetCount = base.m_MorphTargetCount;
        twin->m_MorphDeltas      = base.m_MorphDeltas;
        twin->m_BoneBounds       = base.m_BoneBounds;
        twin->m_SkinnedLODs      = base.m_SkinnedLODs; // the base's chain, the same vertex order
        twin->m_MorphReach       = base.m_MorphTargetReach;
        twin->m_MorphTargetReach = base.m_MorphTargetReach;
        // The morphed twin is what the skinned record draws, so it carries the base's displacement topology: the
        // displaced pass reads the morphed, skinned vertices (same order) and moves that surface.
        twin->m_DisplacementTopology = base.m_DisplacementTopology;
        return twin;
    }

    std::shared_ptr<SkinnedMesh>
    SkinnedMesh::MakeDisplacedTwin( const SkinnedMesh& base, std::shared_ptr<Graphic::VertexBuffer> vertices,
                                    std::shared_ptr<Graphic::VertexBuffer> streams,
                                    std::shared_ptr<Graphic::IndexBuffer> indices, std::vector<Submesh> submeshes,
                                    const uint32_t vertexCount, const float boundsGrowthCm )
    {
        // Private default constructor: make_shared cannot reach it.
        std::shared_ptr<SkinnedMesh> twin( new SkinnedMesh() );
        // The displacement pass's own buffers, data from offset 0 (not pooled: GPU-written, per-mesh bind).
        Graphic::MeshGeometry geometry;
        geometry.AuthoredStreams = streams != nullptr;
        geometry.IndexCount      = indices ? indices->GetCount() : 0u;
        geometry.Vertices        = std::move( vertices );
        geometry.Streams         = std::move( streams );
        geometry.Indices         = std::move( indices );
        geometry.VertexStride    = static_cast<uint32_t>( sizeof( SkinnedVertex ) );
        geometry.VertexCount     = vertexCount;
        twin->SetUnpooledGeometry( std::move( geometry ) );
        twin->m_Submeshes    = std::move( submeshes );
        twin->m_Skeleton     = base.m_Skeleton;
        twin->m_VertexCount  = vertexCount;
        twin->m_BoneBounds   = base.m_BoneBounds;
        for ( BoneBounds& bounds : twin->m_BoneBounds )
        {
            bounds.Box.Min -= glm::vec3( boundsGrowthCm );
            bounds.Box.Max += glm::vec3( boundsGrowthCm );
        }
        twin->m_MorphReach = base.m_MorphReach;
        // The displaced surface is tessellated from LOD 0 and skinned from the whole base: one level, all of it.
        twin->m_SkinnedLODs = { { .FirstVertex   = 0u,
                                  .VertexCount   = vertexCount,
                                  .RequiredBones = base.m_SkinnedLODs.empty() ? 0u : base.m_SkinnedLODs[0].RequiredBones,
                                  .MaxInfluences = static_cast<uint32_t>( SkinnedVertex::MAX_BONE_INFLUENCES ),
                                  .Bones = base.m_SkinnedLODs.empty() ? std::vector<uint32_t>{}
                                                                      : base.m_SkinnedLODs[0].Bones } };
        return twin;
    }

    Common::BoolResultWithCodes<Desert::MeshError> SkinnedMesh::Invalidate()
    {
        return UploadStagedGeometry();
    }

} // namespace Desert
#include "SkinnedMesh.hpp"
#include "SkinnedLODModel.hpp"

#include <Engine/Animation/Skeleton.hpp>

#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <Common/Core/Logger.hpp>



#include <algorithm>
#include <array>
#include <Engine/Animation/BoneInfo.hpp>
#include <glm/glm.hpp>
#include <Engine/Geometry/MeshLOD.hpp>
#include <unordered_map>
#include <Engine/Graphic/GeometryPool.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Shader.hpp>

namespace Desert
{
    SkinnedMesh::SkinnedMesh( const std::vector<SkinnedVertex>& authoredVertices, const std::vector<Index>& indices,
                              const std::vector<Submesh>& submeshes, const Animation::Skeleton* skeleton,
                              const std::vector<MeshVertexStreams>& authoredStreams,
                              const std::vector<MorphTarget>&       authoredMorphTargets )
         : m_Skeleton( skeleton )
    {
        SkinnedLODModel chain =
             BuildSkinnedLODModel( authoredVertices, indices, submeshes, authoredStreams, authoredMorphTargets );
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
#include "SkinnedMesh.hpp"

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/BoneInfo.hpp>

#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/glm.hpp>

namespace Desert
{

    SkinnedMesh::SkinnedMesh( const std::vector<SkinnedVertex>& vertices, const std::vector<Index>& indices,
                              const std::vector<Submesh>& submeshes, const Animation::Skeleton* skeleton,
                              const std::vector<MeshVertexStreams>& streams,
                              const std::vector<MorphTarget>&       morphTargets )
         : m_Skeleton( skeleton ), m_VertexCount( static_cast<uint32_t>( vertices.size() ) )
    {
        m_Submeshes = submeshes;
        m_Vertices  = vertices; // retained on the CPU for viewport picking (posed-AABB)

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

        m_VertexBuffer =
             Graphic::VertexBuffer::Create( (void*)vertices.data(), vertices.size() * sizeof( SkinnedVertex ) );
        CreateStreamBuffer( streams );

        m_IndexBuffer = Graphic::IndexBuffer::Create( indices.data(), indices.size() * sizeof( Index ) );

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
                for ( size_t i = 0; i < std::min<size_t>( m_VertexCount, target.DeltaPositions.size() ); ++i )
                    deltas[first + i * 2] = glm::vec4( target.DeltaPositions[i], 0.0f );
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
    SkinnedMesh::MakeMorphedTwin( const SkinnedMesh& base, std::shared_ptr<Graphic::VertexBuffer> morphedVertices )
    {
        // Private default constructor: make_shared cannot reach it.
        std::shared_ptr<SkinnedMesh> twin( new SkinnedMesh() );
        twin->m_Submeshes        = base.m_Submeshes;
        twin->m_IndexBuffer      = base.m_IndexBuffer;
        twin->m_StreamBuffer     = base.m_StreamBuffer;
        twin->m_Skeleton         = base.m_Skeleton;
        twin->m_VertexCount      = base.m_VertexCount;
        twin->m_MorphTargetCount = base.m_MorphTargetCount;
        twin->m_MorphDeltas      = base.m_MorphDeltas;
        twin->m_VertexBuffer     = std::move( morphedVertices );
        return twin;
    }

    Common::BoolResultWithCodes<Desert::MeshError> SkinnedMesh::Invalidate()
    {
        const auto vertices = m_VertexBuffer->RT_Invalidate();
        if ( !vertices.IsSuccess() )
            return Common::MakeErrorWithCodes<bool, MeshError>( { MeshError::GpuUploadFailed },
                                                                vertices.GetError() );

        const auto indices = m_IndexBuffer->RT_Invalidate();
        if ( !indices.IsSuccess() )
            return Common::MakeErrorWithCodes<bool, MeshError>( { MeshError::GpuUploadFailed },
                                                                indices.GetError() );

        return InvalidateStreamBuffer();
    }

} // namespace Desert
#include "SkinnedMesh.hpp"

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/BoneInfo.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert
{

    SkinnedMesh::SkinnedMesh( const std::vector<SkinnedVertex>& vertices, const std::vector<Index>& indices,
                              const std::vector<Submesh>& submeshes, const Animation::Skeleton* skeleton )
         : m_Skeleton( skeleton )
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

        m_IndexBuffer = Graphic::IndexBuffer::Create( indices.data(), indices.size() * sizeof( Index ) );
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

        return Common::MakeSuccessWithCodes<bool, MeshError>( true );
    }

} // namespace Desert
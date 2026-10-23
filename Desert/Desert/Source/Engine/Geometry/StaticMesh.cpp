#include "StaticMesh.hpp"

#include "MeshLOD.hpp"

#include <Common/Core/Logger.hpp>

#include <vector>

namespace Desert
{
    StaticMesh::StaticMesh( const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
                            const std::vector<Submesh>& submeshes, const std::vector<MeshVertexStreams>& streams )
    {
        // Build the GPU index buffer with a per-submesh LOD chain (base indices + appended LODs). The
        // CPU base indices stay untouched; only the GPU buffer + per-submesh LOD ranges carry the LODs.
        std::vector<Submesh> subs       = submeshes;
        std::vector<Index>   gpuIndices = Geometry::BuildLODIndexBuffer( vertices, indices, subs );
        m_Submeshes                     = std::move( subs );
        ResolveLODScreenSizes();

        m_VertexBuffer =
             Graphic::VertexBuffer::Create( (void*)vertices.data(), vertices.size() * sizeof( Vertex ) );
        CreateStreamBuffer( streams );
        m_IndexBuffer =
             Graphic::IndexBuffer::Create( gpuIndices.data(), gpuIndices.size() * sizeof( Index ) );
    }

    std::shared_ptr<StaticMesh> StaticMesh::MakeDisplacedTwin( const StaticMesh& base, std::shared_ptr<Graphic::VertexBuffer> vertices,
                                                           std::shared_ptr<Graphic::VertexBuffer> streams,
                                                           std::shared_ptr<Graphic::IndexBuffer>  indices,
                                                           const std::vector<DisplacedRange>& ranges,
                                                           uint32_t vertexCount, float boundsGrowthCm )
    {
        // Private default constructor: make_shared cannot reach it.
        std::shared_ptr<StaticMesh> twin( new StaticMesh() );
        twin->m_Submeshes = base.GetSubmeshes();
        for ( size_t i = 0; i < twin->m_Submeshes.size(); ++i )
        {
            Submesh& sm      = twin->m_Submeshes[i];
            sm.VertexOffset  = 0;
            sm.VertexCount   = vertexCount;
            sm.IndexOffset   = i < ranges.size() ? ranges[i].FirstIndex : 0;
            sm.IndexCount    = i < ranges.size() ? ranges[i].IndexCount : 0;
            sm.LODs          = { { sm.IndexOffset, sm.IndexCount } };
            sm.BakedLODs.clear();
            sm.BoundingBox.Min -= glm::vec3( boundsGrowthCm );
            sm.BoundingBox.Max += glm::vec3( boundsGrowthCm );
        }
        twin->m_VertexBuffer = std::move( vertices );
        twin->m_StreamBuffer = std::move( streams );
        twin->m_IndexBuffer  = std::move( indices );
        twin->m_DistanceField = base.m_DistanceField;
        twin->m_Cards         = base.m_Cards;
        twin->ResolveLODScreenSizes();
        return twin;
    }

    std::shared_ptr<StaticMesh> StaticMesh::GetBakedDisplaced()
    {
        if ( !IsBakedDisplaced() )
            return nullptr;
        const auto data = GetDisplacedMeshData();
        if ( !data )
            return nullptr;
        if ( data == m_BakedSource )
            return m_Baked;
        auto baked = std::make_shared<StaticMesh>( data->Vertices, data->Indices, data->Submeshes, data->Streams );
        if ( const auto uploaded = baked->Invalidate(); !uploaded.IsSuccess() )
        {
            LOG_ERROR( "StaticMesh: the baked displaced mesh {:016x} did not upload ({}); the base mesh is drawn",
                       data->Key, uploaded.GetError() );
            m_BakedSource = data; // one report per landed surface, not one per frame
            m_Baked.reset();
            return nullptr;
        }
        baked->SetDistanceField( DistanceFieldSlot() );
        baked->SetCards( CardsSlot() );
        m_BakedSource = data;
        m_Baked       = std::move( baked );
        return m_Baked;
    }

    Common::BoolResultWithCodes<Desert::MeshError> StaticMesh::Invalidate()
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

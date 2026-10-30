#include "StaticMesh.hpp"

#include "MeshLOD.hpp"

#include <Common/Core/Logger.hpp>

#include <cstdint>
#include <vector>

namespace Desert
{
    StaticMesh::StaticMesh( const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
                            const std::vector<Submesh>& submeshes, const std::vector<MeshVertexStreams>& streams,
                            VirtualGeometrySource virtualGeometry )
    {
        // Build the GPU index buffer with a per-submesh LOD chain (base indices + appended LODs). The
        // CPU base indices stay untouched; only the GPU buffer + per-submesh LOD ranges carry the LODs.
        std::vector<Submesh> subs       = submeshes;
        std::vector<Index>   gpuIndices = Geometry::BuildLODIndexBuffer( vertices, indices, subs );
        // The baked LOD triangle sets are now in gpuIndices (their LODRange chain in subs): transient input, not
        // kept with the drawn mesh's submeshes.
        for ( auto& sub : subs )
            std::vector<std::vector<Index>>().swap( sub.BakedLODs );
        m_Submeshes = std::move( subs );
        ResolveLODScreenSizes();

        // A virtual-geometry mesh's cluster words follow its indices in the same span: every DAG's VertexRefs
        // then its Triangles, in submesh order (the order VirtualMeshTable::RT_Register addresses them in).
        std::vector<uint32_t> clusterWords;
        std::size_t           wordCount = 0;
        for ( const Geometry::ClusterDag& dag : virtualGeometry.Dags )
            wordCount += dag.VertexRefs.size() + dag.Triangles.size();
        clusterWords.reserve( wordCount );
        for ( const Geometry::ClusterDag& dag : virtualGeometry.Dags )
        {
            clusterWords.insert( clusterWords.end(), dag.VertexRefs.begin(), dag.VertexRefs.end() );
            clusterWords.insert( clusterWords.end(), dag.Triangles.begin(), dag.Triangles.end() );
        }
        m_VirtualGeometrySource = std::move( virtualGeometry );

        // Uploaded into the pool's static arena by Invalidate (the renderer thread), the host copy released then.
        StageGeometry( Graphic::GeometryArena::Static, VertexBytes( vertices ), streams,
                       FlattenIndices( gpuIndices ), std::move( clusterWords ) );
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
        // The displacement pass's own buffers, data from offset 0 (not pooled: GPU-written, per-mesh bind).
        Graphic::MeshGeometry geometry;
        geometry.AuthoredStreams = streams != nullptr;
        geometry.IndexCount      = indices ? indices->GetCount() : 0u;
        geometry.Vertices        = std::move( vertices );
        geometry.Streams         = std::move( streams );
        geometry.Indices         = std::move( indices );
        geometry.VertexStride    = static_cast<uint32_t>( sizeof( Vertex ) );
        geometry.VertexCount     = vertexCount;
        twin->SetUnpooledGeometry( std::move( geometry ) );
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
        auto uploaded = UploadStagedGeometry();
        if ( !uploaded.IsSuccess() || m_VirtualGeometrySource.Dags.empty() )
            return uploaded;
        // The DAG rows point into the span just uploaded. A refusal leaves the mesh drawing its discrete LOD
        // chain (cut from the same DAG at cook), named in the log; the CPU DAGs are dropped either way.
        auto registered = Graphic::VirtualGeometry::VirtualMeshTable::Get().RT_Register(
             *this, m_VirtualGeometrySource.Dags, m_VirtualGeometrySource.Name );
        if ( registered.IsSuccess() )
            m_VirtualMesh =
                 std::make_shared<Graphic::VirtualGeometry::VirtualMeshRegistration>( registered.ExtractValue() );
        else
            LOG_ERROR( "StaticMesh '{}': not registered as virtual geometry ({}); its discrete LODs draw",
                       m_VirtualGeometrySource.Name, registered.GetError() );
        m_VirtualGeometrySource = {};
        return uploaded;
    }

} // namespace Desert

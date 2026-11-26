#include <Engine/Geometry/Mesh.hpp>

#include <Engine/Graphic/GeometryPool.hpp>

// The mesh's two pool calls, out of line so Mesh.hpp names the GPU geometry (Graphic/MeshGeometry.hpp) without
// the pool, the vertex and the index buffer classes (BUILD-MOD-4).
namespace Desert
{
    Common::BoolResultWithCodes<MeshError> Mesh::UploadStagedGeometry()
    {
        if ( !m_Staged )
        {
            if ( m_Geometry.IsResident() )
                return Common::MakeSuccessWithCodes<bool, MeshError>( true );
            return Common::MakeErrorWithCodes<bool, MeshError>(
                 { MeshError::GpuUploadFailed },
                 "mesh geometry uploaded again after its host copy was released" );
        }
        const bool authored = !m_StagedStreams.empty();
        auto       uploaded = Graphic::GeometryPool::Get().RT_Upload(
             m_StagedArena, m_StagedVertices, m_StagedStreams, m_StagedIndices, m_StagedClusterWords );
        if ( !uploaded.IsSuccess() )
            return Common::MakeErrorWithCodes<bool, MeshError>( { MeshError::GpuUploadFailed },
                                                                uploaded.GetError() );
        AdoptPoolAllocation( uploaded.ExtractValue(), authored );
        m_Staged = false;
        ReleaseStagedHostCopy();
        return Common::MakeSuccessWithCodes<bool, MeshError>( true );
    }

    void Mesh::AdoptPoolAllocation( Graphic::GeometryPoolAllocation allocation, const bool authoredStreams )
    {
        m_PoolAllocation = std::make_shared<Graphic::GeometryPoolAllocation>( std::move( allocation ) );
        m_Geometry       = Graphic::GeometryPool::Get().Describe( *m_PoolAllocation, authoredStreams );
    }

    auto MeshRenderRevision::Next() -> uint64_t
    {
        static std::atomic<uint64_t> next{ 1 };
        return next.fetch_add( 1, std::memory_order_relaxed );
    }

    auto Mesh::MarkGpuStale() noexcept -> void
    {
        m_GpuStale = true;
        s_StaleEpoch.fetch_add( 1, std::memory_order_acq_rel );
    }

    auto Mesh::GetStaleEpoch() noexcept -> uint64_t
    {
        return s_StaleEpoch.load( std::memory_order_acquire );
    }

    std::atomic<uint64_t> Mesh::s_StaleEpoch{ 0 };
}

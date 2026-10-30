#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualGeometryLayout.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert
{
    class Mesh;
}

namespace Desert::Geometry
{
    struct ClusterDag;
}

namespace Desert::Graphic::RDG
{
    class Builder;
}

// VIRT-GEOM contract: THE OWNER OF THE CLUSTER TABLES (UE FStreamingManager's resident cluster pages, without
// streaming until VIRT-GEOM step 5). One GpuVirtualMesh row, its GpuClusterDag rows, and their Clusters /
// Hierarchy ranges per resident virtual-geometry mesh, written ONCE when the mesh is uploaded to the GeometryPool
// (StaticMesh::Invalidate: GeometryPool::RT_Upload with the cluster words -> RT_Register here) and released on the
// GeometryPool frame rule (kRetireFrames) when the mesh is destroyed or replaced.
//
// ONE SOURCE OF TRUTH FOR GEOMETRY: this table holds metadata only (bounds, errors, counts, ranges, addresses).
// Vertex data, vertex refs and packed triangles live once, in the mesh's GeometryPool spans; the discrete LODs of
// the same mesh are index ranges cut from the same DAG (Geometry::CutClusterDag) over the same vertices.
namespace Desert::Graphic::VirtualGeometry
{
    using VirtualMeshIndex = uint32_t;

    // The frame's tables as RDG imports (UE Nanite::GStreamingManager.GetClusterPageData / HierarchyBuffer).
    struct VirtualMeshBuffers
    {
        RDG::BufferRef Meshes;           // GpuVirtualMesh[]
        RDG::BufferRef Dags;             // GpuClusterDag[]
        RDG::BufferRef Clusters;         // GpuCluster[]
        RDG::BufferRef Hierarchy;        // GpuClusterHierarchyNode[]
        uint32_t       ClusterCount = 0; // high-water marks
        uint32_t       DagCount     = 0;
    };

    struct VirtualMeshTableStats
    {
        uint32_t Meshes   = 0;
        uint32_t Clusters = 0;
        uint64_t Bytes    = 0; // the four tables' device bytes
    };

    // One submesh DAG of a registered mesh as the CPU names it (VirtualMeshTable::DescribeUploaded).
    struct VirtualMeshDagSpan
    {
        uint32_t Dag          = 0; // its row in Dags[]
        uint32_t Submesh      = 0;
        uint32_t FirstCluster = 0; // Clusters[FirstCluster, +ClusterCount)
        uint32_t ClusterCount = 0;
        uint32_t RootNode     = 0; // Hierarchy[] row the cut starts at (GpuClusterDag::RootNode)
    };

    class VirtualMeshTable;

    // A mesh's rows in the table, released (on the frame rule) when the last holder drops it. Move-only; a
    // StaticMesh holds it from its upload (StaticMesh::Invalidate) to its destruction.
    class VirtualMeshRegistration
    {
    public:
        VirtualMeshRegistration() = default;
        ~VirtualMeshRegistration();
        VirtualMeshRegistration( VirtualMeshRegistration&& other ) noexcept;
        VirtualMeshRegistration& operator=( VirtualMeshRegistration&& other ) noexcept;
        VirtualMeshRegistration( const VirtualMeshRegistration& )            = delete;
        VirtualMeshRegistration& operator=( const VirtualMeshRegistration& ) = delete;

        [[nodiscard]] explicit operator bool() const
        {
            return m_Table != nullptr;
        }
        // Its GpuVirtualMesh row (GpuScenePrimitive::VirtualMesh).
        [[nodiscard]] VirtualMeshIndex Index() const
        {
            return m_Index;
        }
        // Releases now (on the frame rule); the registration is empty after.
        void Reset();

    private:
        friend class VirtualMeshTable;
        VirtualMeshRegistration( VirtualMeshTable* table, const VirtualMeshIndex index )
             : m_Table( table ), m_Index( index )
        {
        }
        VirtualMeshTable* m_Table = nullptr;
        VirtualMeshIndex  m_Index = 0;
    };

    class VirtualMeshTable
    {
    public:
        // Rows a released mesh keeps before they can be reused: the GeometryPool's frame rule (its spans retire
        // on the same frames, so a row never names a span that was handed to another mesh).
        static constexpr uint32_t kRetireFrames = 3;

        static VirtualMeshTable& Get();

        ~VirtualMeshTable();
        VirtualMeshTable( const VirtualMeshTable& )            = delete;
        VirtualMeshTable& operator=( const VirtualMeshTable& ) = delete;

        // Registers @p mesh's submesh DAGs (one per submesh, in submesh order) once its pool spans exist; the
        // GpuClusterDag addresses come from the mesh's MeshGeometry (its cluster words after its indices, every
        // DAG's VertexRefs then Triangles in submesh order). @p name names the mesh in the log and the refusals.
        // Refused: a mesh that is not pooled, a DAG count other than the submesh count, an empty DAG, words that
        // do not match the mesh's ClusterWordCount, a buffer with no device address (0), a cluster past the
        // caps, a hierarchy child naming a group or node the DAG does not have.
        [[nodiscard]] Common::ResultStr<VirtualMeshRegistration>
        RT_Register( const Mesh& mesh, std::span<const Geometry::ClusterDag> dags, std::string_view name );

        // Once per frame before the graph records (VulkanRenderer::BeginFrame, after GeometryPool::BeginFrame):
        // returns rows released kRetireFrames ago to the free lists and uploads the rows registered since the
        // last frame (a table that outgrew its buffer gets a new one with every row).
        [[nodiscard]] Common::BoolResultStr RT_Upload();
        // This frame's tables as graph imports (a table with no buffer yet: an invalid ref).
        [[nodiscard]] VirtualMeshBuffers Import( RDG::Builder& graph ) const;

        // The submesh DAGs of mesh row @p mesh, in submesh order, once its rows reached the device (the RT_Upload
        // after its RT_Register); empty for a row not registered, released, or registered since the last upload -
        // a draw that names rows the device buffers do not hold yet would read the previous owner's.
        [[nodiscard]] std::vector<VirtualMeshDagSpan> DescribeUploaded( VirtualMeshIndex mesh ) const;

        [[nodiscard]] VirtualMeshTableStats GetStats() const;

        // The name RT_Register was given for the live mesh owning Dags[] row @p dag; empty when no live mesh owns
        // it (a GPU report that outlived the mesh). For the log of a GPU-side refusal by DAG row.
        [[nodiscard]] std::string DagMeshName( uint32_t dag ) const;

        // Device teardown: drops the buffers (Renderer::Shutdown, next to GeometryPool::RT_Shutdown).
        void RT_Shutdown();

        struct Storage;

    private:
        friend class VirtualMeshRegistration;
        VirtualMeshTable();
        // The mesh is destroyed or replaced: its rows are freed after kRetireFrames.
        void Release( VirtualMeshIndex mesh );

        std::unique_ptr<Storage> m_Storage;
    };
} // namespace Desert::Graphic::VirtualGeometry

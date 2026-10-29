#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualGeometryLayout.hpp>

#include <cstdint>
#include <memory>
#include <span>

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
// (Mesh::RT_Upload -> GeometryPool::RT_Upload with the cluster words -> RT_Register here) and released on the
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

    class VirtualMeshTable
    {
    public:
        VirtualMeshTable();
        ~VirtualMeshTable();
        VirtualMeshTable( const VirtualMeshTable& )            = delete;
        VirtualMeshTable& operator=( const VirtualMeshTable& ) = delete;

        // Registers @p mesh's submesh DAGs (one per submesh, in submesh order) once its pool spans exist; the
        // GpuClusterDag addresses come from the mesh's MeshGeometry. Refused: a mesh that is not pooled, a DAG
        // count other than the submesh count, a DAG whose refs or triangles exceed the mesh's cluster span.
        [[nodiscard]] Common::ResultStr<VirtualMeshIndex>
        RT_Register( const Mesh& mesh, std::span<const Geometry::ClusterDag> dags );
        // The mesh is destroyed or replaced: its rows are freed after kRetireFrames (GeometryPool's rule).
        void Release( VirtualMeshIndex mesh );

        // Once per frame before the graph records: uploads the rows registered since the last frame.
        [[nodiscard]] Common::BoolResultStr RT_Upload();
        // This frame's tables as graph imports.
        [[nodiscard]] VirtualMeshBuffers Import( RDG::Builder& graph ) const;

        [[nodiscard]] VirtualMeshTableStats GetStats() const;

        struct Storage;

    private:
        std::unique_ptr<Storage> m_Storage;
    };
} // namespace Desert::Graphic::VirtualGeometry

#pragma once

#include "Mesh.hpp"

#include <Engine/Geometry/ClusterDag.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualMeshTable.hpp>

#include <memory>
#include <string>

namespace Desert
{
    // A virtual-geometry mesh's cooked cluster DAGs (StaticMeshAsset::GetClusterDags, one per submesh in submesh
    // order) and the name the VirtualMeshTable logs it by. Empty Dags = not a virtual-geometry mesh.
    struct VirtualGeometrySource
    {
        std::vector<Geometry::ClusterDag> Dags;
        std::string                       Name;
    };

    class StaticMesh : public Mesh
    {
    public:
        // @p virtualGeometry: the DAG words are staged after the indices in the mesh's index span; Invalidate
        // registers the DAGs with the VirtualMeshTable once the span is uploaded and drops the CPU copy.
        StaticMesh( const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
                    const std::vector<Submesh>& submeshes, const std::vector<MeshVertexStreams>& streams,
                    VirtualGeometrySource virtualGeometry = {} );

        [[nodiscard]] MeshType GetType() const override
        {
            return MeshType::Static;
        }

        [[nodiscard]] virtual Common::BoolResultWithCodes<MeshError> Invalidate() override;

        // A DISPLACED TWIN of @p base (DisplacementCache): the same mesh to every draw path - its submeshes keep
        // their names, transforms and bounds grown by @p boundsGrowthCm - over the tessellated buffers the
        // displacement passes write (@p vertices and @p streams: BufferUsage::GpuWritten) and the index buffer the
        // CPU tessellation built (@p indices). Each submesh draws @p ranges[i] with base vertex 0 and no LOD chain:
        // the tessellation level is its LOD. No loader BLAS: DisplacementCache builds the twin's.
        struct DisplacedRange
        {
            uint32_t FirstIndex = 0;
            uint32_t IndexCount = 0;
        };
        [[nodiscard]] static std::shared_ptr<StaticMesh>
        MakeDisplacedTwin( const StaticMesh& base, std::shared_ptr<Graphic::VertexBuffer> vertices,
                           std::shared_ptr<Graphic::VertexBuffer> streams, std::shared_ptr<Graphic::IndexBuffer> indices,
                           const std::vector<DisplacedRange>& ranges, uint32_t vertexCount, float boundsGrowthCm );

        // BAKED DISPLACEMENT (Assets/MeshDisplacementBake.hpp): the dense mesh cooked into DDC, drawn instead of
        // this one as an ordinary StaticMesh - its own LOD chain, BLAS built from its buffers, and this mesh's
        // distance field and cards (keyed from the baked surface). Made once on the main thread from the landed
        // data and kept while the data is the same; null until it lands or when the mesh is not Baked.
        [[nodiscard]] std::shared_ptr<StaticMesh> GetBakedDisplaced();

        // The mesh's rows in the VirtualMeshTable: null (or an empty registration) when it is not a
        // virtual-geometry mesh or its DAGs were refused.
        [[nodiscard]] const Graphic::VirtualGeometry::VirtualMeshRegistration* GetVirtualMesh() const override
        {
            return m_VirtualMesh.get();
        }

    private:
        StaticMesh() = default;
        std::shared_ptr<const Assets::DisplacedMeshData> m_BakedSource;
        std::shared_ptr<StaticMesh>                      m_Baked;
        // Held from the upload until Invalidate registers them, then dropped (the rows hold what the GPU needs).
        VirtualGeometrySource m_VirtualGeometrySource;
        // The mesh's rows in the VirtualMeshTable (shared by a copy of the mesh, as its pool allocation is);
        // released on the frame rule with the last holder.
        std::shared_ptr<Graphic::VirtualGeometry::VirtualMeshRegistration> m_VirtualMesh;
    };

} // namespace Desert

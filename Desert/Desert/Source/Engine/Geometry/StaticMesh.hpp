#pragma once

#include "Mesh.hpp"

namespace Desert
{
    class StaticMesh : public Mesh
    {
    public:
        StaticMesh( const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
                    const std::vector<Submesh>& submeshes, const std::vector<MeshVertexStreams>& streams );

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

    private:
        StaticMesh() = default;
    };

} // namespace Desert

#pragma once

#include "Mesh.hpp"

namespace Desert
{
    class DynamicMesh : public Mesh
    {
    public:
        // generateLODs: build a distance LOD chain (meshopt) into a SEPARATE GPU index buffer, keeping
        // GetIndices() as the base geometry (so edits/serialization are unaffected). Off by default —
        // procedural/editable meshes stay plain; used for static test/content meshes.
        DynamicMesh( const std::vector<Vertex>& vertices, const std::vector<Index>& indices,
                     const std::vector<Submesh>& submeshes, bool generateLODs = false )
             : m_Vertices( vertices ), m_Indices( indices ), m_GenerateLODs( generateLODs )
        {
            m_Submeshes = submeshes;
            // Constructing touches no device: the GPU buffers are made by Invalidate, which the renderer
            // reaches through EnsureGpuResources on the first submit (or a caller with a renderer, directly).
            MarkGpuStale();
        }

        MeshType GetType() const override
        {
            return MeshType::Static;
        }

        Common::BoolResultWithCodes<MeshError> Invalidate() override;

        void Update( const std::vector<Vertex>& vertices, const std::vector<Index>& indices );

        void Flatten();

        [[nodiscard]] std::vector<Vertex>&       GetVertices() { return m_Vertices; }
        [[nodiscard]] const std::vector<Vertex>& GetVertices() const { return m_Vertices; }
        [[nodiscard]] std::vector<Index>&        GetIndices() { return m_Indices; }
        [[nodiscard]] const std::vector<Index>&  GetIndices() const { return m_Indices; }

    private:
        std::vector<Vertex> m_Vertices;
        std::vector<Index>  m_Indices;
        bool                m_GenerateLODs = false; // build a LOD chain into the GPU index buffer on Invalidate
    };
} // namespace Desert

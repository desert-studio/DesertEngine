#pragma once

#include "MeshAsset.hpp"

#include <Engine/Geometry/MeshTypes.hpp>

namespace Desert::Assets
{
    struct MeshDistanceFieldData;

    class StaticMeshAsset final : public MeshAsset
    {
    public:
        StaticMeshAsset( const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        bool IsSkinned() const override
        {
            return false;
        }

        const std::vector<Vertex>& GetVertices() const
        {
            return m_Vertices;
        }
        const std::vector<Index>& GetIndices() const
        {
            return m_Indices;
        }
        const std::vector<Submesh>& GetSubmeshes() const override
        {
            return m_Submeshes;
        }

        // Blendshapes (empty when the mesh has none). Deltas are index-aligned with GetVertices().
        // Colors / UV1 packed for the mesh's optional stream buffer; empty when the asset has neither.
        [[nodiscard]] const std::vector<MeshVertexStreams>& GetVertexStreams() const
        {
            return m_VertexStreams;
        }

        const std::vector<MorphTarget>& GetMorphTargets() const override
        {
            return m_MorphTargets;
        }

        virtual const std::vector<Common::UUID>& GetMaterialHandles() const override
        {
            return m_MaterialAssetHandles;
        }

        bool IsReadyForUse() const override
        {
            return m_IsReadyForUse;
        }

        // The mesh's signed distance field (MeshDistanceField.hpp), loaded with the render data.
        [[nodiscard]] const std::shared_ptr<const MeshDistanceFieldData>& GetDistanceField() const
        {
            return m_DistanceField;
        }

    private:
        std::vector<Vertex>       m_Vertices;
        std::vector<Index>        m_Indices;
        std::vector<Submesh>      m_Submeshes;
        std::vector<MorphTarget>  m_MorphTargets;
        std::vector<MeshVertexStreams> m_VertexStreams;
        std::vector<Common::UUID> m_MaterialAssetHandles;
        std::shared_ptr<const MeshDistanceFieldData> m_DistanceField;

        bool m_IsReadyForUse = false;
    };
} // namespace Desert::Assets
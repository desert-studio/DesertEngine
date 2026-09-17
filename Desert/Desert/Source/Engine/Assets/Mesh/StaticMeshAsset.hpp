#pragma once

#include "MeshAsset.hpp"

#include <Engine/Geometry/MeshTypes.hpp>

namespace Desert::Assets
{
    class StaticMeshAsset final : public MeshAsset
    {
    public:
        StaticMeshAsset( const AssetPriority priority, const Common::Filepath& filepath );

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

    private:
        std::vector<Vertex>       m_Vertices;
        std::vector<Index>        m_Indices;
        std::vector<Submesh>      m_Submeshes;
        std::vector<MorphTarget>  m_MorphTargets;
        std::vector<Common::UUID> m_MaterialAssetHandles;

        bool m_IsReadyForUse = false;
    };
} // namespace Desert::Assets
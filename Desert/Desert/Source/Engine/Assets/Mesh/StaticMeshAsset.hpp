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

        // Loads the distance field, which depends on the submeshes' MATERIALS (their sidedness shapes its sign,
        // as UE builds each section with its material's bTwoSided) - so it is derived here, where the manager
        // can load them, and not in LoadFromFile.
        void ResolveDependencies( AssetManager& manager ) override;
        // True once ResolveDependencies ran on the current payload (whatever it concluded): an asset read by the
        // async loader (Load, never EnsureLoaded) is resolved by its consumer exactly once.
        [[nodiscard]] bool AreDependenciesResolved() const
        {
            return m_DependenciesResolved;
        }

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

        // The mesh's signed distance field (MeshDistanceField.hpp); null until ResolveDependencies, and after it
        // when the field could not be resolved (logged as an error naming the mesh and the reason).
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
        // Per submesh, the material GUID the file states (null = none): the distance field's sidedness input.
        std::vector<Common::Content::AssetGuid>      m_MaterialGuids;
        uint64_t                                     m_RenderDataKey        = 0;
        bool                                         m_DependenciesResolved = false;
        std::shared_ptr<const MeshDistanceFieldData> m_DistanceField;

        bool m_IsReadyForUse = false;
    };
} // namespace Desert::Assets
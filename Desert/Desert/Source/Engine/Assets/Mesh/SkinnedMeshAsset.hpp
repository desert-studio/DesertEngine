#pragma once

#include "MeshAsset.hpp"

#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <optional>
#include <vector>

namespace Desert::Assets
{
    class SkinnedMeshAsset final : public MeshAsset
    {
    public:
        SkinnedMeshAsset( const Common::Filepath& filepath );

        // -------------------------------------------------
        // Asset lifecycle
        // -------------------------------------------------
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        bool IsSkinned() const override
        {
            return true;
        }

        // -------------------------------------------------
        // Accessors (CPU data only)
        // -------------------------------------------------
        const std::vector<SkinnedVertex>& GetVertices() const
        {
            return m_Vertices;
        }

        const std::vector<Index>& GetIndices() const
        {
            return m_Indices;
        }

        virtual const std::vector<Common::UUID>& GetMaterialHandles() const override
        {
            return m_MaterialAssetHandles;
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

        const auto& GetSkeletonDependency() const
        {
            return m_SkeletonDependency;
        }

        /// Binds GetSkeleton() - BY GUID, the rig's identity - to its SkeletonAsset, bones resident.
        /// Re-runnable: the previous answer is dropped first. A null GUID (unparsed shell) binds nothing.
        void ResolveDependencies( AssetManager& manager ) override;

        /// THE MESH'S SKELETON, BY GUID (SKEL-TREE; contract: Engine/Animation/SkeletonReference.hpp). UE
        /// USkeletalMesh::Skeleton. Null = the mesh names no skeleton and plays no clip. Replaces the signature
        /// as identity: ResolveDependencies binds HandleForGuid of this, GetSkeletonSignature goes away.
        [[nodiscard]] Common::Content::AssetGuid GetSkeleton() const;

        /// Authoring (Details slot, after CheckSkeletonAssignment): in memory, re-resolves on the next
        /// ResolveDependencies. Serialization::SaveMeshSkeletonReference writes the .skmesh and the source.
        void SetSkeleton( Common::Content::AssetGuid skeleton );

        bool IsReadyForUse() const override
        {
            return m_IsReadyForUse;
        }

    private:
        // Skinning geometry
        std::vector<SkinnedVertex> m_Vertices;
        std::vector<Index>         m_Indices;
        std::vector<Submesh>       m_Submeshes;
        std::vector<MorphTarget>   m_MorphTargets;
        std::vector<MeshVertexStreams> m_VertexStreams;
        std::vector<Common::UUID>  m_MaterialAssetHandles;

        Common::Content::AssetGuid m_Skeleton; // the .skmesh header's SkeletonGuid; null until parsed

        // Dependency
        AssetDependency<SkeletonAsset> m_SkeletonDependency;

        bool m_IsReadyForUse = false;
    };
} // namespace Desert::Assets
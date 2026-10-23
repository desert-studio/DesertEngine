#pragma once

#include "MeshAsset.hpp"

#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/MeshDisplacementSettings.hpp>
#include <Engine/Assets/MeshDisplacementTopology.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <memory>
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

        // THE MESH'S DISPLACEMENT MODE (MeshBinary v6 header; CONTRACT-DISPLACE §9.8): None or Runtime - Baked is
        // refused by the reader, a deforming surface has no single baked pose.
        [[nodiscard]] MeshDisplacementMode GetDisplacement() const
        {
            return m_Displacement;
        }

        // The key the displacement topology derives from (SkinnedMeshRenderDataKey of the file's bytes).
        [[nodiscard]] uint64_t GetRenderDataKey() const
        {
            return m_RenderDataKey;
        }

        // Where the topology over the REST pose's LOD0 lands (MeshDisplacementTopology.hpp): null unless the mode
        // is Runtime and a section's material assigns a Height; then a slot ResolveDependencies' worker job fills.
        // The scene's DisplacementCache dices it over the skin cache's posed vertices each frame.
        [[nodiscard]] const std::shared_ptr<const DisplacementTopologySlot>& GetDisplacementTopology() const
        {
            return m_DisplacementTopology;
        }

    private:
        // Runtime + any section Height: the topology slot and its derive job (DDC hit or editor build).
        void ResolveDisplacementTopology( AssetManager& manager );

        // Skinning geometry
        std::vector<SkinnedVertex> m_Vertices;
        std::vector<Index>         m_Indices;
        std::vector<Submesh>       m_Submeshes;
        std::vector<MorphTarget>   m_MorphTargets;
        std::vector<MeshVertexStreams> m_VertexStreams;
        std::vector<Common::UUID>  m_MaterialAssetHandles;
        // Per submesh, the material GUID the file states (null = none): where each section's Height resolves from.
        std::vector<Common::Content::AssetGuid> m_MaterialGuids;

        MeshDisplacementMode                            m_Displacement  = MeshDisplacementMode::None;
        uint64_t                                        m_RenderDataKey = 0;
        std::shared_ptr<const DisplacementTopologySlot> m_DisplacementTopology;

        Common::Content::AssetGuid m_Skeleton; // the .skmesh header's SkeletonGuid; null until parsed

        // Dependency
        AssetDependency<SkeletonAsset> m_SkeletonDependency;

        bool m_IsReadyForUse = false;
    };
} // namespace Desert::Assets
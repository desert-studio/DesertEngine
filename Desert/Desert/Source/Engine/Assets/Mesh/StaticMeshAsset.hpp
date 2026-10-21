#pragma once

#include "MeshAsset.hpp"

#include <Engine/Geometry/MeshTypes.hpp>

namespace Desert::Assets
{
    class MeshDistanceFieldSlot;
    class MeshCardsSlot;

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
        // THE COOK'S CALL (UE: the cooker derives every saved mesh's distance field and cards before it writes
        // the package): the same derivation ResolveDependencies queues, run NOW on this thread - a DDC hit, or a
        // build through the registered builders that Puts the entry the packaged game will ask for. An error
        // names what could not be had (a material's sidedness, the cards, the field). Needs a loaded payload.
        [[nodiscard]] Common::BoolResultStr DeriveDistanceFieldAndCardsNow( AssetManager& manager ) const;
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

        // Where the mesh's signed distance field lands (MeshDistanceField.hpp): null until ResolveDependencies,
        // and after it a slot the asynchronous queue fills when the field is derived (empty for good when it could
        // not be, logged as an error naming the mesh and the reason).
        [[nodiscard]] const std::shared_ptr<const MeshDistanceFieldSlot>& GetDistanceField() const
        {
            return m_DistanceField;
        }
        // Where the mesh's cards land (MeshCardRepresentation.hpp): derived in the same queued job as the
        // distance field, from the same triangles; null until ResolveDependencies.
        [[nodiscard]] const std::shared_ptr<const MeshCardsSlot>& GetCards() const
        {
            return m_Cards;
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
        std::shared_ptr<const MeshDistanceFieldSlot> m_DistanceField;
        std::shared_ptr<const MeshCardsSlot>         m_Cards;

        bool m_IsReadyForUse = false;
    };
} // namespace Desert::Assets
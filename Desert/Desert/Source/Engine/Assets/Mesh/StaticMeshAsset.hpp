#pragma once

#include "MeshAsset.hpp"

#include <Engine/Assets/MeshDisplacementBake.hpp>
#include <Engine/Assets/MeshDisplacementTopology.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <mutex>

namespace Desert::Assets
{
    namespace Serialization
    {
        struct MeshAssetData;
    }
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

        // THE CPU RENDER DATA (UE FStaticMeshLODResources' vertex/index arrays, bAllowCPUAccess): resident from the
        // load until the GPU copy is made (ReleaseCpuGeometry, MeshFactory), then dropped - the GPU buffers are the
        // render data. A consumer that reads it after that (collision, picking, a spline mesh, the rigging tools)
        // is an explicit CPU-access request: the arrays are read back from the mesh's derived data (the same key)
        // and kept for the rest of this load, as UE keeps them for a mesh flagged bAllowCPUAccess.
        const std::vector<Vertex>& GetVertices() const
        {
            EnsureCpuGeometry();
            return m_Vertices;
        }
        const std::vector<Index>& GetIndices() const
        {
            EnsureCpuGeometry();
            return m_Indices;
        }
        // The GPU copy exists (MeshFactory::CreateStatic): the CPU vertices, indices, streams and the baked LOD
        // triangle sets are freed unless a consumer already asked for CPU access. The distance field, cards and
        // displacement jobs own copies of their input taken in ResolveDependencies, before this.
        void ReleaseCpuGeometry();
        // The uploader's read (MeshFactory::CreateStatic rebuilding the GPU mesh after a release): the arrays are read
        // back for that one upload and released again after it, not kept as a CPU-access request would keep them.
        void AcquireCpuGeometryForUpload()
        {
            EnsureCpuGeometry( false );
        }
        [[nodiscard]] bool IsCpuGeometryResident() const
        {
            return !m_CpuGeometryReleased;
        }
        const std::vector<Submesh>& GetSubmeshes() const override
        {
            return m_Submeshes;
        }

        // Blendshapes (empty when the mesh has none). Deltas are index-aligned with GetVertices().
        // Colors / UV1 packed for the mesh's optional stream buffer; empty when the asset has neither.
        [[nodiscard]] const std::vector<MeshVertexStreams>& GetVertexStreams() const
        {
            EnsureCpuGeometry();
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
        // Where the mesh's displacement topology lands (MeshDisplacementTopology.hpp): null unless the import
        // settings chose Runtime displacement; then a slot the derived-data job fills.
        [[nodiscard]] const std::shared_ptr<const DisplacementTopologySlot>& GetDisplacementTopology() const
        {
            return m_DisplacementTopology;
        }
        // Where the mesh's bake-level displaced surface lands (MeshDisplacementBake.hpp): null unless the import
        // settings chose a displacement mode and a section's material assigns a Height. Baked mode draws it (its
        // own LOD chain); collision reads its LOD0 in BOTH modes, so physics never depends on a camera.
        [[nodiscard]] const std::shared_ptr<const DisplacedMeshSlot>& GetDisplacedMesh() const
        {
            return m_DisplacedMesh;
        }
        [[nodiscard]] const MeshImportSettings& GetImportSettings() const
        {
            return m_ImportSettings;
        }

    private:
        // Fills the CPU arrays (vertices, indices, streams, each submesh's baked LODs) from the decoded container.
        void FillCpuGeometry( const Serialization::MeshAssetData& data );
        // Reads the CPU arrays back after ReleaseCpuGeometry (an explicit CPU-access request); no-op when resident.
        // @p keep: a CPU reader's request (kept for this load) or the uploader's (released again after it).
        void EnsureCpuGeometry( bool keep = true ) const;

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
        std::shared_ptr<const DisplacementTopologySlot> m_DisplacementTopology;
        std::shared_ptr<const DisplacedMeshSlot>        m_DisplacedMesh;
        MeshImportSettings                              m_ImportSettings;

        bool m_IsReadyForUse = false;
        // ReleaseCpuGeometry dropped the arrays; EnsureCpuGeometry reads them back and sets m_CpuAccessRequested,
        // after which this load keeps them (UE bAllowCPUAccess). Mutable: a const reader is the request.
        mutable bool       m_CpuGeometryReleased = false;
        mutable bool       m_CpuAccessRequested  = false;
        mutable std::mutex m_CpuGeometryMutex;
    };
} // namespace Desert::Assets
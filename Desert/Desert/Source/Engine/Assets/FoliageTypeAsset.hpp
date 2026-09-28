#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/FoliageType.hpp>

namespace Desert::Assets
{
    /**
     * @brief A foliage type on disk (`.defoliage`) — UE's UFoliageType_InstancedStaticMesh as an asset of the
     * engine's one asset system.
     *
     * A foliage entity (ECS::FoliageComponent) names one of these by handle; the paint brush reads the
     * scatter numbers from it and the entity's InstancedStaticMeshComponent draws the type's mesh. Before
     * FO-1 the numbers were inline on the component, so two fields of the same grass were two unrelated
     * sets of numbers; as in UE, the type is now one file every field painted with it shares.
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a scene's reference
     * survives renames and is the same on every machine. Loading is on demand (AsyncAssetLoader through
     * LoadThroughLoader): a scene load only establishes WHICH type an entity names.
     */
    class FoliageTypeAsset final : public AssetBase
    {
    public:
        FoliageTypeAsset( const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const Common::Content::AssetGuid& Guid() const
        {
            return m_Guid;
        }

        [[nodiscard]] const Serialization::FoliageTypeData& GetData() const
        {
            return m_Data;
        }

        /// The mesh's handle (HandleForGuid of Mesh.Guid), or a null handle when the type names no mesh.
        [[nodiscard]] AssetHandle GetMeshHandle() const;

        /// The file's stem: what the paint panel's type list shows.
        [[nodiscard]] std::string GetDisplayName() const
        {
            return m_Metadata.Filepath.stem().string();
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::FoliageType;
        }

        /// Writes a type to disk (creating the directory). Static because saving is what CREATES a type.
        static Common::BoolResultStr Save( const Common::Filepath&               filepath,
                                           const Serialization::FoliageTypeData& data );

    private:
        Common::Content::AssetGuid     m_Guid;
        Serialization::FoliageTypeData m_Data;
        bool                           m_Ready = false;
    };
} // namespace Desert::Assets

#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/LandscapeGrassType.hpp>

namespace Desert::Assets
{
    /**
     * @brief A landscape grass type on disk (`.degrasstype`) — UE's ULandscapeGrassType as an asset of the
     * engine's one asset system.
     *
     * A layer info (`.delayerinfo`, LandscapeLayerInfoData::GrassType) names it; what it grows is generated
     * around the camera every session and never stored (World/Landscape/LandscapeGrass.hpp, owner decision O1).
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a layer's reference
     * survives renames and is the same on every machine. Loading is on demand (LandscapeGrassTypeService
     * through AsyncAssetLoader).
     */
    class LandscapeGrassTypeAsset final : public AssetBase
    {
    public:
        LandscapeGrassTypeAsset( AssetPriority priority, const Common::Filepath& filepath );

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

        [[nodiscard]] const Serialization::LandscapeGrassTypeData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::LandscapeGrassType;
        }

        /// Writes a grass type to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&                      filepath,
                                           const Serialization::LandscapeGrassTypeData& data );

    private:
        Common::Content::AssetGuid            m_Guid;
        Serialization::LandscapeGrassTypeData m_Data;
        bool                                  m_Ready = false;
    };
} // namespace Desert::Assets

#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/LandscapeLayerInfo.hpp>

namespace Desert::Assets
{
    /**
     * @brief A landscape layer info on disk (`.delayerinfo`) — UE's ULandscapeLayerInfoObject as an asset of
     * the engine's one asset system.
     *
     * A landscape root (ECS::LandscapeComponent::Layers) names these by handle; the weight planes are keyed
     * by the asset's LayerName and the paint stroke normalises with its Hardness/NoWeightBlend. Before LS-12b
     * the numbers were inline on the component, so two landscapes painting "Grass" held two unrelated sets
     * of numbers; as in UE, the layer is now one file every landscape painting it shares.
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a scene's reference
     * survives renames and is the same on every machine. Loading is on demand (LandscapeLayerInfoService
     * through AsyncAssetLoader): a scene load only establishes WHICH layers a landscape names.
     */
    class LandscapeLayerInfoAsset final : public AssetBase
    {
    public:
        LandscapeLayerInfoAsset( AssetPriority priority, const Common::Filepath& filepath );

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

        [[nodiscard]] const Serialization::LandscapeLayerInfoData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::LandscapeLayerInfo;
        }

        /// Writes a layer info to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&                      filepath,
                                           const Serialization::LandscapeLayerInfoData& data );

    private:
        Common::Content::AssetGuid            m_Guid;
        Serialization::LandscapeLayerInfoData m_Data;
        bool                                  m_Ready = false;
    };
} // namespace Desert::Assets

#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/LandscapeGrassType.hpp>

#include <cstdint>

namespace Desert::Assets
{
    /**
     * @brief A landscape grass type on disk (`.degrasstype`) — UE's ULandscapeGrassType as an asset of the
     * engine's one asset system.
     *
     * A layer info (`.delayerinfo`, LandscapeLayerInfoData::GrassTypes) names any number of these; what they
     * grow is generated on the GPU around the camera every frame and never stored
     * (Graphic/Systems/Scene/Grass/GrassRenderer.hpp).
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a layer's reference
     * survives renames and is the same on every machine. Loading is on demand (LandscapeGrassTypeService
     * through AsyncAssetLoader).
     */
    class LandscapeGrassTypeAsset final : public AssetBase
    {
    public:
        explicit LandscapeGrassTypeAsset( const Common::Filepath& filepath );

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

        /// Grows by one on every successful read, so a reader that caches what it derived from the varieties
        /// (the landscape system's section materials) knows a hot-reloaded type is a different one.
        [[nodiscard]] uint64_t Revision() const
        {
            return m_Revision;
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
        uint64_t                              m_Revision = 0;
        bool                                  m_Ready    = false;
    };
} // namespace Desert::Assets

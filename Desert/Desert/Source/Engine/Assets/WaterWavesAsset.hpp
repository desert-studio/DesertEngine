#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/WaterWaves.hpp>

namespace Desert::Assets
{
    /**
     * @brief A water wave set on disk (`.dwaves`) — UE's UWaterWavesAsset as an asset of the engine's one asset
     * system: the seeded Gerstner generator every water body naming it shares (Serialization/WaterWaves.hpp).
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a scene's reference
     * survives renames and is the same on every machine.
     */
    class WaterWavesAsset final : public AssetBase
    {
    public:
        WaterWavesAsset( const Common::Filepath& filepath );

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

        [[nodiscard]] const Serialization::WaterWavesData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::WaterWaves;
        }

        /// Writes a wave set to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&              filepath,
                                           const Serialization::WaterWavesData& data );

    private:
        Common::Content::AssetGuid    m_Guid;
        Serialization::WaterWavesData m_Data;
        bool                          m_Ready = false;
    };
} // namespace Desert::Assets

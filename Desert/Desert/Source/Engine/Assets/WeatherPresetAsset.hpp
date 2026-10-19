#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/WeatherPreset.hpp>

namespace Desert::Assets
{
    /**
     * @brief A weather preset on disk (`.dweatherpreset`, WTH-STATE): one weather as data
     * (Serialization/WeatherPreset.hpp). WeatherZone components name one; Runtime::WeatherPresetService reads it
     * for the world's WeatherState.
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a zone's reference survives
     * renames and is the same on every machine.
     */
    class WeatherPresetAsset final : public AssetBase
    {
    public:
        WeatherPresetAsset( const Common::Filepath& filepath );

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

        [[nodiscard]] const Serialization::WeatherPresetData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::WeatherPreset;
        }

        /// Writes a preset to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&                 filepath,
                                           const Serialization::WeatherPresetData& data );

    private:
        Common::Content::AssetGuid       m_Guid;
        Serialization::WeatherPresetData m_Data;
        bool                             m_Ready = false;
    };
} // namespace Desert::Assets

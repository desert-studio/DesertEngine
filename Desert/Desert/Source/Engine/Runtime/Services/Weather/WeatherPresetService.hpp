#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/WeatherPresetAsset.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::Runtime
{
    /**
     * @brief The `.dweatherpreset` presets weather zones name and SetWeather targets (WTH-STATE).
     *
     * READ SYNCHRONOUSLY, as the world parameter collections beside it: a preset is a few hundred bytes of JSON,
     * and a `Weather.set` from a script must find the target preset in the call it is made in.
     */
    class WeatherPresetService
    {
    public:
        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The preset's data as loaded now, or the reason it cannot be read. The pointer lives as long as
        /// the asset (the service holds it).
        Common::ResultStr<const Assets::Serialization::WeatherPresetData*>
        Get( const Assets::AssetHandle& handle );

        /// The handle of the preset at `path` (relative to the assets root, as a scene stores it, or absolute) —
        /// what a script names a preset by. Refused when no asset manager is bound or the file does not exist.
        Common::ResultStr<Assets::AssetHandle> HandleForPath( const std::filesystem::path& path );

        void Clear();

    private:
        std::weak_ptr<Assets::AssetManager>                                                  m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::Asset<Assets::WeatherPresetAsset>> m_Held;
        std::unordered_map<Assets::AssetHandle, std::string>                                 m_Failed;
    };
} // namespace Desert::Runtime

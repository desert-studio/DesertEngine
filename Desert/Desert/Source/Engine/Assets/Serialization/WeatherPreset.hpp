#pragma once

#include <Engine/Assets/Serialization/WorldParameters.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Assets::Serialization
{
    // A `.dweatherpreset` (WTH-STATE): ONE WEATHER as data — Clear, Rain, Blizzard are files, not an enum, so a
    // game adds its own without touching the engine (owner 10-18). A preset states:
    //   - values for NAMED WORLD PARAMETERS (Wetness, RainIntensity, SnowCover, ... — whatever names the scene's
    //     `.dworldparams` collection declares; the preset carries values only, never its own buffer);
    //   - the sky: the cloud layer's Coverage and the height fog's FogDensity (written into the same authored
    //     values the cloud programme and the fog component already own — no second path);
    //   - the wind's Speed, which the zone hands to its WindSource (weather stores no wind of its own).
    // A field a preset does not state (an absent optional / parameter) is left to whatever else drives it.
    // UE has no engine weather (CitySample assembles it from an MPC + Niagara + material functions).
    inline constexpr const char* kWeatherPresetExtension = ".dweatherpreset";

    inline constexpr int32_t kWeatherPresetVersion = static_cast<int32_t>( Assets::kWeatherPresetSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> WeatherPresetTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kWeatherPresetSchemaTag,
                                                static_cast<uint32_t>( kWeatherPresetVersion ) } };
        return versions;
    }

    /// One world parameter's value under this weather. A scalar parameter reads .x; the type is the collection's,
    /// so a preset does not restate it (the world instance refuses a vector written to a scalar by name).
    struct WeatherParameterValue
    {
        std::string          Name;
        std::array<float, 4> Value = { 0.0f, 0.0f, 0.0f, 0.0f };

        [[nodiscard]] bool operator==( const WeatherParameterValue& ) const = default;
    };

    /// One cloud type the weather shows and how much of it (WTH-OVERCAST): `Type` the `.decloudtype` path relative
    /// to the assets root, `Weight` 0..1 its share of CloudCoverage — the cloud layer's CloudTypeWeight of the
    /// slot holding that type. A preset that states any type states the WHOLE set: a type it leaves out is at 0.
    struct WeatherCloudTypeWeight
    {
        std::string Type;
        float       Weight = 0.0f;

        [[nodiscard]] bool operator==( const WeatherCloudTypeWeight& ) const = default;
    };

    struct WeatherPresetData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        std::vector<WeatherParameterValue> Parameters;
        // 0..1, the cloud material's Coverage (Graphic/Clouds/CloudMaterialValues.hpp).
        std::optional<float> CloudCoverage;
        // The cloud types this weather shows (empty = the layer's own weights are left alone).
        std::vector<WeatherCloudTypeWeight> CloudTypes;
        // The cloud material's DensityScale (0..2): an overcast deck is thicker than fair-weather cumulus.
        std::optional<float> CloudDensity;
        // The ExponentialHeightFog's FogDensity (its own units, 0..0.5).
        std::optional<float> FogDensity;
        // cm/s, the zone's WindSource Speed.
        std::optional<float> WindSpeed;

        [[nodiscard]] bool operator==( const WeatherPresetData& ) const = default;
    };

    /// Every parameter name an identifier and unique, every value finite, coverage in [0,1], density in [0,0.5],
    /// wind speed >= 0.
    Common::BoolResultStr ValidateWeatherPresetData( const WeatherPresetData& data );

    Common::ResultStr<WeatherPresetData> ParseWeatherPreset( const std::string& text );

    std::string WriteWeatherPreset( const WeatherPresetData& data );

    Common::BoolResultStr SaveWeatherPresetFile( const std::filesystem::path& path,
                                                 const WeatherPresetData&     data );
} // namespace Desert::Assets::Serialization

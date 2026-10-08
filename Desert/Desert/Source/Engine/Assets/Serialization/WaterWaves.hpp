#pragma once

// UE 5.8's UWaterWavesAsset (Engine/Plugins/Experimental/Water/Source/Runtime/Public/WaterWaves.h) holding a
// UGerstnerWaterWaveGeneratorSimple (Public/GerstnerWaterWaves.h:100-143), adapted: a plain struct read by
// reflect-cpp instead of a UObject with an instanced generator; the generator's parameters and seed are what the
// file stores, and the waves are derived from them by Water::GenerateGerstnerWaves (the same seed gives the same
// waves on every load), so the file cannot hold a wave list that disagrees with its own generator.

#include <Engine/Assets/TextAssetHeaderStamp.hpp>
#include <Engine/Water/WaterWaves.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace Desert::Assets::Serialization
{
    /// The extension the Content Browser and the loader agree on.
    inline constexpr const char* kWaterWavesExtension = ".dwaves";

    /**
     * @brief The FILE layout's generation.
     *
     *   1 - the text asset header (Kind "WaterWaves", the GUID that IS the asset's identity and handle, this
     *       number under `WAVS`) and UE's simple Gerstner generator (WATER-W1). No Dependencies.
     *
     * An unknown value is refused in both directions; there is no migration step in the runtime.
     */
    inline constexpr int32_t kWaterWavesVersion = static_cast<int32_t>( Assets::kWaterWavesSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> WaterWavesTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kWaterWavesSchemaTag,
                                                static_cast<uint32_t>( kWaterWavesVersion ) } };
        return versions;
    }

    /// The most waves one generator may stand for (each is a term of every surface vertex's sum).
    inline constexpr int32_t kWaterWavesMaxNumWaves = 4096;

    /// One `.dwaves` (UE: UWaterWavesAsset with a UGerstnerWaterWaveGeneratorSimple).
    struct WaterWavesData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        Water::GerstnerWaveGenerator Generator;

        [[nodiscard]] bool operator==( const WaterWavesData& ) const = default;
    };

    /// Rejects what the generator cannot honour, naming the field and the value: NumWaves outside
    /// [1, kWaterWavesMaxNumWaves], a non-finite number, a negative or inverted wavelength / amplitude range,
    /// a negative falloff or spread, Randomness or a steepness outside [0, 1].
    Common::BoolResultStr ValidateWaterWavesData( const WaterWavesData& data );

    /// Parses a `.dwaves`. A file without a header, of another version or kind, that states any Dependencies
    /// (a wave set references nothing), or with invalid numbers is an error naming why.
    Common::ResultStr<WaterWavesData> ParseWaterWaves( const std::string& text );

    /// Canonical text; stamps the header (keeping a loaded GUID, minting one otherwise).
    std::string WriteWaterWaves( const WaterWavesData& data );

    Common::BoolResultStr SaveWaterWavesFile( const std::filesystem::path& path, const WaterWavesData& data );
} // namespace Desert::Assets::Serialization

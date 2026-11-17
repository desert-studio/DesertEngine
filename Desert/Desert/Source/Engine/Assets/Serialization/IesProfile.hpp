#pragma once

#include <Engine/Assets/TextAssetHeaderStamp.hpp>
#include <Engine/Graphic/IesProfile.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Assets::Serialization
{
    // ONE IES LIGHT PROFILE (`.dies`, LIGHTS-RECT-IES): UE UTextureLightProfile, imported from an IESNA LM-63
    // `.ies` by Editor/Import/IesProfileImporter. A point, spot or rect light names one by its header GUID's
    // handle (PointLightData / SpotLightData / RectLightData::IesProfile); Runtime::IesProfileService resamples it
    // into the profile atlas the shaders sample.
    //
    // THE PHOTOMETRIC DATA, NOT THE TEXTURE. UE stores the resampled 256-wide texture in the asset; here the asset
    // keeps the file's angles and candela (the multiplier applied, Type C), so the shaders' grid
    // (Graphic::kIesProfileColumns x kIesProfileRows) can change without a re-import and nothing is lost to a
    // resample at import time. The .ies stays beside the asset as its source, as a texture's image does.
    inline constexpr const char* kIesProfileExtension = ".dies";

    inline constexpr int32_t kIesProfileVersion = static_cast<int32_t>( Assets::kIesProfileSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> IesProfileTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kIesProfileSchemaTag,
                                                static_cast<uint32_t>( kIesProfileVersion ) } };
        return versions;
    }

    struct IesProfileData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        // Degrees as the source lists them, rising (Graphic::IesPhotometricData).
        std::vector<float> VerticalAngles;
        std::vector<float> HorizontalAngles;
        // Candela[horizontal * VerticalAngles.size() + vertical], the source's multiplier applied.
        std::vector<float> Candela;
    };

    // UE UTextureLightProfile::Brightness: the profile's peak candela (the multiplier applied), the light's
    // brightness under Use IES Brightness (ECS::*LightData::UseIesBrightness). 0 for a profile with no candela.
    [[nodiscard]] inline float IesProfilePeakCandela( const IesProfileData& data )
    {
        return data.Candela.empty() ? 0.0f : *std::max_element( data.Candela.begin(), data.Candela.end() );
    }

    [[nodiscard]] IesProfileData IesProfileFromPhotometry( const Graphic::IesPhotometricData& photometry );

    [[nodiscard]] Graphic::IesPhotometricData PhotometryOf( const IesProfileData& data );

    /// At least one angle on each axis, each axis rising, vertical in [0, 180] and horizontal in [0, 360], one
    /// finite non-negative candela per angle pair and a peak above 0.
    Common::BoolResultStr ValidateIesProfileData( const IesProfileData& data );

    Common::ResultStr<IesProfileData> ParseIesProfileAsset( const std::string& text );

    std::string WriteIesProfileAsset( const IesProfileData& data );

    Common::BoolResultStr SaveIesProfileFile( const std::filesystem::path& path, const IesProfileData& data );
} // namespace Desert::Assets::Serialization

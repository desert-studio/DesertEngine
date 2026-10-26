#pragma once

#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace Desert::Assets::Serialization
{
    // ONE BURLEY SUBSURFACE PROFILE (`.dsssprofile`, SSS-PROFILE): UE's USubsurfaceProfile / FSubsurfaceProfileStruct
    // (SubsurfaceProfile.h:27-170), the Burley half only. A material of the SubsurfaceProfile shading model names
    // one (MaterialData::SubsurfaceProfile); the renderer's SubsurfaceProfileRegistry encodes it into one row of
    // the profile atlas (Graphic/SubsurfaceProfiles). Not ported (no legacy, CONTRACT §2.4/§2.5): the Separable
    // inputs ScatterRadius/SubsurfaceColor/FalloffColor, bEnableBurley/bEnableMeanFreePath, and WorldUnitScale -
    // MeanFreePathDistance is authored in centimetres, the project unit, with no second scale.
    inline constexpr const char* kSubsurfaceProfileExtension = ".dsssprofile";

    inline constexpr int32_t kSubsurfaceProfileVersion =
         static_cast<int32_t>( Assets::kSubsurfaceProfileSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> SubsurfaceProfileTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kSubsurfaceProfileSchemaTag,
                                                static_cast<uint32_t>( kSubsurfaceProfileVersion ) } };
        return versions;
    }

    // UE FSubsurfaceProfileImplementationTechniqueHint (SubsurfaceProfile.h:13-24).
    enum class SubsurfaceImplementation
    {
        AdaptiveFilteredImportanceSampling,
        Separable
    };

    // Defaults = UE's constructor (SubsurfaceProfile.h:139-165).
    struct SubsurfaceProfileData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        // -- Burley Normalized
        glm::vec3 SurfaceAlbedo        = { 0.91058f, 0.338275f, 0.2718f }; // colour, [0.01, 1]
        glm::vec3 MeanFreePathColor    = { 1.0f, 0.08896f, 0.07209f };     // colour, [0.001, 1]
        float     MeanFreePathDistance = 2.6748f;                          // cm, [0.1, 50]
        glm::vec3 Tint                 = { 1.0f, 1.0f, 1.0f };             // per channel: original <-> scattered
        glm::vec3 BoundaryColorBleed   = { 1.0f, 1.0f, 1.0f };
        SubsurfaceImplementation Implementation = SubsurfaceImplementation::AdaptiveFilteredImportanceSampling;
        // -- Transmission
        float     ExtinctionScale        = 1.0f;  // [0.01, 100], 1 = physical
        float     NormalScale            = 0.08f; // [0.01, 1]
        float     DistanceScale          = 1.0f;  // [0.1, 10], 1 = physical
        float     ScatteringDistribution = 0.93f; // [0.01, 1]
        float     IOR                    = 1.55f; // [1, 3]
        glm::vec3 TransmissionTintColor  = { 1.0f, 1.0f, 1.0f };
        // -- Dual Specular
        float Roughness0 = 0.75f; // [0.5, 2] x material roughness
        float Roughness1 = 1.30f; // [0.5, 2]
        float LobeMix    = 0.85f; // [0.1, 0.9]
    };

    /// Every value finite and inside UE's UI range (SubsurfaceProfile.h ClampMin/ClampMax).
    Common::BoolResultStr ValidateSubsurfaceProfileData( const SubsurfaceProfileData& data );

    Common::ResultStr<SubsurfaceProfileData> ParseSubsurfaceProfile( const std::string& text );

    std::string WriteSubsurfaceProfile( const SubsurfaceProfileData& data );

    Common::BoolResultStr SaveSubsurfaceProfileFile( const std::filesystem::path& path,
                                                     const SubsurfaceProfileData& data );
} // namespace Desert::Assets::Serialization

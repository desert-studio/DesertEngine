#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Foliage/Public/FoliageType.h:140-245 (UFoliageType's painting
// block), adapted: a plain struct read by reflect-cpp instead of a UObject with UPROPERTY metadata; only the
// fields the paint brush consumes today (a field nothing reads is a dead setting); Uniform scaling only
// (ScaleX drives all three axes, as UE's EFoliageScaling::Uniform does); centimetres and degrees as in UE;
// the mesh (UFoliageType_InstancedStaticMesh::Mesh) named by {Guid, Path} like every other reference in a
// text asset of this project.

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

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
    /// The extension the Content Browser, the component's slot, the paint panel and the loader agree on.
    inline constexpr const char* kFoliageTypeExtension = ".defoliage";

    /**
     * @brief The FILE layout's generation.
     *
     *   1 - the text asset header (Kind "FoliageType", the GUID that IS the type's identity and handle, this
     *       number under `FOLT`), the mesh by {Guid, Path}, and UFoliageType's painting fields (FO-1). The
     *       mesh's GUID is the header's one Dependency when a mesh is named.
     *
     * An unknown value is refused in both directions; there is no migration step in the runtime.
     */
    inline constexpr int32_t kFoliageTypeVersion = static_cast<int32_t>( Assets::kFoliageTypeSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> FoliageTypeTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kFoliageTypeSchemaTag,
                                                static_cast<uint32_t>( kFoliageTypeVersion ) } };
        return versions;
    }

    /// UE's FFloatInterval: an inclusive [Min, Max] range a value is drawn from uniformly.
    struct FoliageFloatInterval
    {
        float Min = 0.0f;
        float Max = 0.0f;

        [[nodiscard]] bool operator==( const FoliageFloatInterval& ) const = default;
    };

    /**
     * @brief One `.defoliage`: what to scatter and how (UE: UFoliageType_InstancedStaticMesh).
     *
     * Field names follow UFoliageType so a reader of the UE docs finds the same knob here. The single
     * deviation is Density, see below.
     */
    struct FoliageTypeData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        /// The static mesh the instances draw. Empty = a type that can be authored but not painted yet.
        AssetGuidRef Mesh;

        /// Instances scattered per paint dab inside the brush disk. UE's Density is per 1000x1000 cm; this
        /// brush is dab-based until FO-3 ports UE's area-density brush, and the number keeps the brush's
        /// meaning so every painted field reads back the same.
        float Density = 6.0f;
        /// Uniform scale range (UE ScaleX under EFoliageScaling::Uniform).
        FoliageFloatInterval ScaleX{ 0.8f, 1.3f };
        /// Offset along the placement up axis, cm, drawn per instance.
        FoliageFloatInterval ZOffset{ 0.0f, 0.0f };
        /// Tilt instances to the surface normal.
        bool AlignToNormal = true;
        /// Random rotation about the up axis.
        bool RandomYaw = true;
        /// Random tilt off the up axis, degrees (0 = upright).
        float RandomPitchAngle = 0.0f;
        /// Paint only where the surface slope lies in [Min, Max] degrees.
        FoliageFloatInterval GroundSlopeAngle{ 0.0f, 90.0f };

        [[nodiscard]] bool operator==( const FoliageTypeData& ) const = default;
    };

    /// Rejects numbers the brush cannot honour, naming the field and the values.
    Common::BoolResultStr ValidateFoliageTypeData( const FoliageTypeData& data );

    /// Parses a `.defoliage`. A file without a header, of another version, of another kind, with a
    /// Dependencies list that disagrees with Mesh, or with invalid numbers is an error naming why.
    Common::ResultStr<FoliageTypeData> ParseFoliageType( const std::string& text );

    /// Canonical text; stamps the header (keeping a loaded GUID, minting one otherwise).
    std::string WriteFoliageType( const FoliageTypeData& data );

    Common::BoolResultStr SaveFoliageTypeFile( const std::filesystem::path& path, const FoliageTypeData& data );
} // namespace Desert::Assets::Serialization

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
#include <vector>

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
     *   2 - UE's units and filters (FO-3): Density is instances per 1000x1000 cm (UFoliageType::Density), not
     *       per brush dab; Height, LandscapeLayers (layer info assets by {Guid, Path}) and MinimumLayerWeight
     *       join. The header's Dependencies are the mesh's GUID, then each layer info's, in list order.
     *       SceneMigrator raises a v1 file (MigrateFoliageTypeV1ToV2).
     *   3 - CullDistance joins (FO-5, UE UFoliageType::CullDistance). SceneMigrator raises a v2 file
     *       (MigrateFoliageTypeV2ToV3) with {0, 0}, UE's default: never culled.
     *   4 - Wind joins (FO-7): how the instances sway (UE: a SimpleGrassWind world-position offset in the
     *       material). SceneMigrator raises a v3 file (MigrateFoliageTypeV3ToV4) with Strength 0: still, which
     *       is what every v3 field drew. The engine reads v4 only.
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
     * @brief How a type's instances sway in the wind (UE: SimpleGrassWind's WindIntensity / WindSpeed and the
     *        material's height mask, with the direction a WindDirectionalSource would give).
     *
     * Displacement is horizontal, in world centimetres, and grows with the vertex's height in the MESH's own
     * space: zero at and below the pivot (the root), full at Height and above. Graphic::FoliageWindOffset is
     * the whole formula; Common/FoliageWind.glslh is its GPU twin.
     */
    struct FoliageWind
    {
        /// The largest displacement, cm, reached by a vertex at Height. 0 = the instances stand still.
        float Strength = 0.0f;
        /// Sway cycles per second of the fundamental.
        float Speed = 0.5f;
        /// Mesh-local height, cm, at which the sway reaches Strength (the root at 0 does not move).
        float Height = 100.0f;
        /// The direction the wind blows TOWARDS, degrees about the up axis from +X towards +Z.
        float DirectionDegrees = 0.0f;

        [[nodiscard]] bool operator==( const FoliageWind& ) const = default;
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

        /// Instances per 1000x1000 cm of brushed area (UE UFoliageType::Density, default 100). The brush tops
        /// the area under it up to this count and no further, so repeated dabs do not pile instances up.
        float Density = 100.0f;
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
        /// Paint only where the surface's world height (Y, cm) lies in [Min, Max] (UE Height).
        FoliageFloatInterval Height{ -262144.0f, 262144.0f };
        /// Paint on a landscape only where one of these layers has weight (UE LandscapeLayers, named here by
        /// their `.delayerinfo` as every reference in a text asset is). Empty = every layer. A surface that is
        /// not a landscape is not filtered by layer, as in UE.
        std::vector<AssetGuidRef> LandscapeLayers;
        /// The weight, 0..1, a listed layer must reach; above it an instance survives with probability equal
        /// to the weight (UE MinimumLayerWeight and IsFilteredByWeight).
        float MinimumLayerWeight = 0.0f;
        /// Distance from the camera, cm, over which the instances fade out (UE CullDistance): all are drawn
        /// nearer than Min, none from Max on, and between the two a share growing linearly from 0 to 1 is
        /// dropped (Graphic::KeepsInstanceAtDistance). Max = 0 is UE's "never culled".
        FoliageFloatInterval CullDistance{ 0.0f, 0.0f };
        /// How the instances sway (FO-7). Strength 0 = still.
        FoliageWind Wind;

        [[nodiscard]] bool operator==( const FoliageTypeData& ) const = default;
    };

    /// The FOLT generation a `.defoliage` text states, read from its header alone (so a file of another
    /// generation is named as such, not by the fields it lacks); nullopt when it states none.
    std::optional<uint32_t> StatedFoliageTypeGeneration( const std::string& text );

    /// Rejects numbers the brush cannot honour, naming the field and the values.
    Common::BoolResultStr ValidateFoliageTypeData( const FoliageTypeData& data );

    /// Parses a `.defoliage`. A file without a header, of another version, of another kind, with a
    /// Dependencies list that disagrees with Mesh, or with invalid numbers is an error naming why.
    Common::ResultStr<FoliageTypeData> ParseFoliageType( const std::string& text );

    /// Canonical text; stamps the header (keeping a loaded GUID, minting one otherwise).
    std::string WriteFoliageType( const FoliageTypeData& data );

    Common::BoolResultStr SaveFoliageTypeFile( const std::filesystem::path& path, const FoliageTypeData& data );

    /// A `.defoliage` on disk, as a palette or a collection names it.
    struct FoliageTypeFile
    {
        std::filesystem::path Path;
        std::string           Guid;            ///< the header GUID: the type's identity
        bool                  Created = false; ///< false = an existing file already held these numbers
    };

    /**
     * @brief The `.defoliage` under @p dir (recursively) whose content equals @p wanted apart from the header;
     *        a new `<stem>.defoliage` (or `<stem>_N`) when none does.
     *
     * UE: dropping a mesh on the foliage palette twice finds the FoliageType the first drop made instead of
     * minting a second asset for the same mesh and numbers (FO-2). The walk is in path order, so the same
     * folder always answers with the same file. A `.defoliage` under @p dir that does not parse refuses the
     * lookup and names the file: skipping it could mint a duplicate of the very type it holds.
     */
    Common::ResultStr<FoliageTypeFile> FindOrCreateFoliageTypeFile( const std::filesystem::path& dir,
                                                                    const FoliageTypeData&       wanted,
                                                                    const std::string&           stem );
} // namespace Desert::Assets::Serialization

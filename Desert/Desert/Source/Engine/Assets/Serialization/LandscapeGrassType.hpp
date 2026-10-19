#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Landscape/Classes/LandscapeGrassType.h:20-190 (EGrassScaling,
// FGrassVariety, ULandscapeGrassType::GrassVarieties), adapted: a plain struct read by reflect-cpp instead of a
// UObject with UPROPERTY metadata; the mesh named by {Guid, Path} like every other reference in a text asset;
// per-platform / per-quality numbers collapsed to one value. The mesh shades with its own section materials (UE
// without OverrideMaterials). Refused, because nothing here could consume them (a field nothing reads is a dead
// setting): bUseGrid=false (Halton placement; the GPU generator places on the grid), MinLOD, the lightmap / decal
// / lighting-channel / distance-field flags, ExcludedLandscapes and bEnableDensityScaling. Added: `Kind` (what a
// variety draws; Mesh only until GRASS-BLADE) and the variety's wind (UE's is the grass material's SimpleGrassWind
// WPO; here the WPO parameters live with the instanced type, as FoliageWind).

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/Serialization/FoliageType.hpp>
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
    /// The extension the Content Browser and the loader agree on.
    inline constexpr const char* kLandscapeGrassTypeExtension = ".degrasstype";

    /**
     * @brief The FILE layout's generation.
     *
     *   1, 2 - GR-1 / GR-2's layout, removed with the CPU grass streamer in LS-16b; no such file is tracked.
     *   3    - the text asset header (Kind "LandscapeGrassType", the GUID that IS the type's identity and handle,
     *          this number under `LGRT`, one Dependency per distinct variety mesh) and GrassVarieties, each with a
     *          Kind (GRASS-GPU).
     *
     * An unknown value is refused in both directions; there is no migration step in the runtime.
     */
    inline constexpr int32_t kLandscapeGrassTypeVersion =
         static_cast<int32_t>( Assets::kLandscapeGrassTypeSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> LandscapeGrassTypeTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kLandscapeGrassTypeSchemaTag,
                                                static_cast<uint32_t>( kLandscapeGrassTypeVersion ) } };
        return versions;
    }

    /// What a variety draws. Mesh: instances of a static mesh (UE GrassMesh). The procedural blade arrives with
    /// GRASS-BLADE as the second value.
    enum class GrassVarietyKind
    {
        Mesh,
    };

    /// UE EGrassScaling: how a variety's three scale ranges combine.
    enum class GrassScaling
    {
        Uniform, ///< one draw of ScaleX for all three axes
        Free,    ///< each axis its own draw
        LockXY,  ///< X and Y share ScaleX's draw, Z (up) draws ScaleZ
    };

    /**
     * @brief One kind of grass a type grows (UE: FGrassVariety, LandscapeGrassType.h:30-158).
     *
     * Units are UE's: Density is instances per 1000 x 1000 cm (as FoliageTypeData::Density), distances are cm,
     * Z is the up axis (the engine's Y).
     */
    struct GrassVariety
    {
        GrassVarietyKind Kind = GrassVarietyKind::Mesh;
        /// The static mesh every instance draws (Kind Mesh). Required.
        AssetGuidRef Mesh;
        float        Density = 400.0f;
        /// 0..1 of a grid cell the instance may wander from the cell centre (UE PlacementJitter, clamped to .99).
        float PlacementJitter   = 1.0f;
        float StartCullDistance = 0.0f;
        float EndCullDistance   = 10000.0f;
        /// The layer weight an instance needs: kept when Min < weight <= Max and weight beats its random draw.
        FoliageFloatInterval AllowedDensityRange{ 0.0f, 1.0f };
        GrassScaling         Scaling = GrassScaling::Uniform;
        FoliageFloatInterval ScaleX{ 1.0f, 1.0f };
        FoliageFloatInterval ScaleY{ 1.0f, 1.0f };
        FoliageFloatInterval ScaleZ{ 1.0f, 1.0f };
        bool                 WeightAttenuatesMaxScale  = false;
        float                MaxScaleWeightAttenuation = 0.5f;
        bool                 RandomRotation            = true;
        bool                 AlignToSurface            = true;
        bool                 CastDynamicShadow         = true;
        /// The response to the scene's wind (ECS::WindAt), as a foliage type's (FoliageWind).
        FoliageWind Wind;

        [[nodiscard]] bool operator==( const GrassVariety& ) const = default;
    };

    /**
     * @brief One `.degrasstype` (UE ULandscapeGrassType): the varieties a landscape layer grows. Any number of
     * them; each is its own generated instance buffer and its own indirect draws.
     */
    struct LandscapeGrassTypeData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        std::vector<GrassVariety> GrassVarieties;

        [[nodiscard]] bool operator==( const LandscapeGrassTypeData& ) const = default;
    };

    /// Rejects numbers the generator cannot honour, naming the variety, the field and the values.
    Common::BoolResultStr ValidateLandscapeGrassTypeData( const LandscapeGrassTypeData& data );

    /// Parses a `.degrasstype`. A file without a header, of another version or kind, whose Dependencies do not
    /// state exactly its meshes' GUIDs, or with invalid numbers is an error naming why.
    Common::ResultStr<LandscapeGrassTypeData> ParseLandscapeGrassType( const std::string& text );

    /// Canonical text; stamps the header (keeping a loaded GUID, minting one otherwise).
    std::string WriteLandscapeGrassType( const LandscapeGrassTypeData& data );

    Common::BoolResultStr SaveLandscapeGrassTypeFile( const std::filesystem::path&  path,
                                                      const LandscapeGrassTypeData& data );
} // namespace Desert::Assets::Serialization

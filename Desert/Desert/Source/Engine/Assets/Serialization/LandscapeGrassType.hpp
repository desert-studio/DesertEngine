#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Landscape/Classes/LandscapeGrassType.h:20-190 (EGrassScaling,
// FGrassVariety, ULandscapeGrassType::GrassVarieties), adapted: a plain struct read by reflect-cpp instead of a
// UObject with UPROPERTY metadata; the mesh named by {Guid, Path} like every other reference in a text asset;
// per-platform / per-quality numbers collapsed to one value (this engine has one quality level). Refused,
// because nothing here could consume them (a field nothing reads is a dead setting): OverrideMaterials (the
// ISM path draws one PBR slot and no slot list is authored for grass yet), MinLOD, bUseGrid/PlacementJitter
// (the Halton placement is the one sequence ported — landscape analysis A8), bWeightAttenuatesMaxScale,
// bAlignToTriangleNormals, bUseLandscapeLightmap, bReceivesDecals, bAffectDistanceFieldLighting,
// bCastContactShadow, bKeepInstanceBufferCPUCopy, InstanceWorldPositionOffsetDisableDistance, ExcludedLandscapes
// and bEnableDensityScaling.

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
    /// The extension the Content Browser and the loader agree on.
    inline constexpr const char* kLandscapeGrassTypeExtension = ".degrasstype";

    /**
     * @brief The FILE layout's generation.
     *
     *   1 - the text asset header (Kind "LandscapeGrassType", the GUID that IS the type's identity and handle,
     *       this number under `LGRT`, one Dependency per named mesh) and FGrassVariety's fields (GR-1).
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

    /// UE EGrassScaling.
    enum class GrassScaling
    {
        Uniform, ///< one random scale for X, Y and Z (drawn from ScaleX)
        Free,    ///< X, Y and Z drawn independently from ScaleX, ScaleY, ScaleZ
        LockXY,  ///< X and Y the same draw from ScaleX, Z from ScaleZ
    };

    /// UE FFloatInterval: an inclusive [Min, Max] range a value is drawn from uniformly.
    struct GrassFloatInterval
    {
        float Min = 0.0f;
        float Max = 0.0f;

        [[nodiscard]] bool operator==( const GrassFloatInterval& ) const = default;
    };

    /**
     * @brief One kind of blade a grass type grows (UE FGrassVariety). Field names follow UE's so a reader of
     * the UE docs finds the same knob here.
     */
    struct GrassVariety
    {
        /// The static mesh every instance draws (UE GrassMesh). Required: a variety without one is refused.
        AssetGuidRef GrassMesh;
        /// Instances per 1000 x 1000 cm at weight 1 (UE GrassDensity; UE's tooltip says "per 10 square meters",
        /// its maths — LandscapeGrass.cpp:2070 — divides the extent product by 1000 twice).
        float GrassDensity = 400.0f;
        /// From this distance from the camera, cm, the instances start to thin out (UE StartCullDistance). The
        /// fade is the foliage one (FO-5, Graphic/InstanceCullDistance.hpp): the renderer drops a growing share
        /// of the instances between Start and End, so the edge of the grass is a gradient, not a line.
        float StartCullDistance = 8000.0f;
        /// Beyond this distance from the camera, cm, a variety's cells are not generated and no instance is drawn
        /// (UE EndCullDistance).
        float EndCullDistance = 10000.0f;
        /// A sample keeps an instance only when its layer weight lies in (Min, Max] (UE AllowedDensityRange).
        GrassFloatInterval AllowedDensityRange{ 0.0f, 1.0f };
        GrassScaling       Scaling = GrassScaling::Uniform;
        GrassFloatInterval ScaleX{ 1.0f, 1.0f };
        GrassFloatInterval ScaleY{ 1.0f, 1.0f };
        GrassFloatInterval ScaleZ{ 1.0f, 1.0f };
        /// Random rotation about the up axis.
        bool RandomRotation = true;
        /// Tilt each instance to the landscape normal under it.
        bool AlignToSurface = true;
        /// UE bCastDynamicShadow: the instances enter the shadow cascades.
        bool CastDynamicShadow = true;

        [[nodiscard]] bool operator==( const GrassVariety& ) const = default;
    };

    /**
     * @brief One `.degrasstype` (UE ULandscapeGrassType): the varieties a landscape layer grows.
     */
    struct LandscapeGrassTypeData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        std::vector<GrassVariety> GrassVarieties;

        [[nodiscard]] bool operator==( const LandscapeGrassTypeData& ) const = default;
    };

    /// How many varieties one type may carry: each is one instanced draw per streamed cell set.
    inline constexpr size_t kLandscapeGrassMaxVarieties = 8u;
    /// UE's ClampMax on GrassDensity (LandscapeGrassType.h:44).
    inline constexpr float kLandscapeGrassMaxDensity = 1000.0f;
    /// UE's ClampMax on the cull distances, cm (LandscapeGrassType.h:58).
    inline constexpr float kLandscapeGrassMaxCullDistance = 1000000.0f;

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

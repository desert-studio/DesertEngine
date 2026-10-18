#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Landscape/Classes/LandscapeLayerInfoObject.h:60-115
// (ULandscapeLayerInfoObject's properties), adapted: a plain struct read by reflect-cpp instead of a UObject
// with UPROPERTY metadata; UE 5.7's BlendMethod collapsed back to the one bit this engine's paint stroke
// honours (NoWeightBlend = ELandscapeTargetLayerBlendMethod::None, false = FinalWeightBlending); UE's
// Hardness default 0 kept at this engine's earlier 0.5 so every layer painted before the asset reads back the
// same. Refused, because nothing here could consume them (a field nothing reads is a dead setting):
// PhysMaterial and MinimumCollisionRelevanceWeight (no physical materials on the landscape collision yet),
// SplineFalloffModulation* (no landscape splines yet).

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/Serialization/FoliageType.hpp>
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
#include <vector>

namespace Desert::Assets::Serialization
{
    /// The extension the Content Browser, the landscape panel and the loader agree on.
    inline constexpr const char* kLandscapeLayerInfoExtension = ".delayerinfo";

    /**
     * @brief The FILE layout's generation.
     *
     *   1 - the text asset header (Kind "LandscapeLayerInfo", the GUID that IS the layer's identity and
     *       handle, this number under `LLYI`, no Dependencies) and ULandscapeLayerInfoObject's fields (LS-12b).
     *   2 - GrassType: the `.degrasstype` the layer grows, by {Guid, Path}; its GUID is the header's one
     *       Dependency when one is named (GR-1). The one tracked v1 file (Landscape/Layers/Grass, INT7) was
     *       rewritten as v2 in the same change, so v1 is refused by its number.
     *   3 - GrassType leaves (LS-16): the landscape grows no grass; grass is a foliage asset placed with the
     *       Foliage tool. The header states no Dependencies again. The corpus files were rewritten as v3 in
     *       the same change, so v2 is refused by its number.
     *
     * An unknown value is refused in both directions; there is no migration step in the runtime.
     */
    inline constexpr int32_t kLandscapeLayerInfoVersion =
         static_cast<int32_t>( Assets::kLandscapeLayerInfoSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> LandscapeLayerInfoTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kLandscapeLayerInfoSchemaTag,
                                                static_cast<uint32_t>( kLandscapeLayerInfoVersion ) } };
        return versions;
    }

    /// UE EGrassScaling: how a variety's three scale ranges combine.
    enum class GrassScaling
    {
        Uniform, ///< one draw of ScaleX for all three axes
        Free,    ///< each axis its own draw
        LockXY,  ///< X and Y share ScaleX's draw, Z (up) draws ScaleZ
    };

    /**
     * @brief One kind of grass a layer grows (UE: FGrassVariety, LandscapeGrassType.h:30-158).
     *
     * Units are UE's: Density is instances per 1000 x 1000 cm (as FoliageTypeData::Density), distances are cm,
     * Z is the up axis (the engine's Y). The mesh shades with its own section materials (UE without
     * OverrideMaterials). Refused, because nothing here could consume them: bUseGrid=false (Halton placement),
     * MinLOD, lightmap / decal / lighting-channel / distance-field flags, the per-platform and quality overrides.
     */
    struct GrassVariety
    {
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
        bool                 WeightAttenuatesMaxScale = false;
        float                MaxScaleWeightAttenuation = 0.5f;
        bool                 RandomRotation            = true;
        bool                 AlignToSurface            = true;
        bool                 CastDynamicShadow         = true;
        /// The response to the scene's wind (ECS::WindAt), as a foliage type's (FoliageWind).
        FoliageWind Wind;

        [[nodiscard]] bool operator==( const GrassVariety& ) const = default;
    };

    /**
     * @brief One `.delayerinfo` (UE: ULandscapeLayerInfoObject).
     *
     * LayerName is the key every tile's weight plane is looked up by (LandscapeWeightLayer::Name); Hardness
     * and NoWeightBlend are what a paint stroke's normalisation reads (LandscapeLayerRule).
     */
    struct LandscapeLayerInfoData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        std::string LayerName;
        /// Share of a stroke's weight this layer keeps when another layer is painted over it, 0..1.
        float Hardness = 0.5f;
        /// UE ELandscapeTargetLayerBlendMethod::None: the layer is painted independently of the others.
        bool NoWeightBlend = false;
        /// The swatch the panel shows and what a layer-usage debug view would tint by (linear RGB).
        glm::vec3 LayerUsageDebugColor = glm::vec3( 1.0f );
        /// What grows where this layer is painted (UE: the LandscapeGrassType the material's LandscapeGrassOutput
        /// names for this layer). Empty: nothing.
        std::vector<GrassVariety> Grass;

        [[nodiscard]] bool operator==( const LandscapeLayerInfoData& ) const = default;
    };

    /// Rejects what the weight planes and the paint stroke cannot honour, naming the field and the value.
    Common::BoolResultStr ValidateLandscapeLayerInfoData( const LandscapeLayerInfoData& data );

    /// Parses a `.delayerinfo`. A file without a header, of another version or kind, whose Dependencies are not
    /// exactly its varieties' meshes, or with invalid numbers is an error naming why.
    Common::ResultStr<LandscapeLayerInfoData> ParseLandscapeLayerInfo( const std::string& text );

    /// Canonical text; stamps the header (keeping a loaded GUID, minting one otherwise).
    std::string WriteLandscapeLayerInfo( const LandscapeLayerInfoData& data );

    Common::BoolResultStr SaveLandscapeLayerInfoFile( const std::filesystem::path&  path,
                                                      const LandscapeLayerInfoData& data );
} // namespace Desert::Assets::Serialization

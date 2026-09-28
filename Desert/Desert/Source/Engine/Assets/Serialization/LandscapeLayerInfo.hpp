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
        /// The grass grown where this layer is painted (UE: the landscape material's LandscapeGrassOutput pin
        /// fed by this layer's weight). Empty = the layer grows nothing.
        AssetGuidRef GrassType;

        [[nodiscard]] bool operator==( const LandscapeLayerInfoData& ) const = default;
    };

    /// Rejects what the weight planes and the paint stroke cannot honour, naming the field and the value.
    Common::BoolResultStr ValidateLandscapeLayerInfoData( const LandscapeLayerInfoData& data );

    /// Parses a `.delayerinfo`. A file without a header, of another version or kind, whose Dependencies do not
    /// state exactly its GrassType's GUID, or with invalid numbers is an error naming why.
    Common::ResultStr<LandscapeLayerInfoData> ParseLandscapeLayerInfo( const std::string& text );

    /// Canonical text; stamps the header (keeping a loaded GUID, minting one otherwise).
    std::string WriteLandscapeLayerInfo( const LandscapeLayerInfoData& data );

    Common::BoolResultStr SaveLandscapeLayerInfoFile( const std::filesystem::path&  path,
                                                      const LandscapeLayerInfoData& data );
} // namespace Desert::Assets::Serialization

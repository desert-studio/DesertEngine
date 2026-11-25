#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Landscape/Public/LandscapeRaycast.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Desert::Water
{
    /// Texels per side at most; the texel is max(landscape spacing, extent / this).
    inline constexpr uint32_t kShoreFieldMaxTexels = 2048;
    /// Outside the landscape the sea floor falls from the nearest edge by this slope down to kOpenSeaDepthCm, so
    /// the field is defined and continuous everywhere: there is no "no ground" value.
    inline constexpr float kShoreSkirtSlope = 0.10f;
    inline constexpr float kOpenSeaDepthCm  = 20000.0f;
    /// The farthest a shore distance is stated (inside RGBA16F's range): past it there is no shore.
    inline constexpr float kShoreFarCm = 60000.0f;

    /// The bottom of one world's water seen from above, with the shore of every still level over it.
    /// Immutable after the bake; the GPU texture and the CPU query read these very texels with the same bilinear.
    struct WaterShoreField
    {
        glm::vec2 MinCm{ 0.0f };
        float     TexelCm = 0.0f;
        uint32_t  Width   = 0;
        uint32_t  Height  = 0;
        /// The level the GPU's heights are stored relative to (RGBA16F): the widest body's still level.
        float ReferenceLevelCm = 0.0f;
        /// World Y the floor outside the field stops at: the lowest body's level - kOpenSeaDepthCm.
        float OpenSeaFloorCm = 0.0f;
        /// The mean slope of the floor over the near 150 m of water: the one slope the shore wave's phase is
        /// integrated with (a local one would tear the crests apart).
        float BeachSlope = 0.0f;
        struct Texel
        {
            float GroundCm; // world Y of the ground, material displacement included; land is above the level
            float ShoreDistanceCm; // signed, along the still level of the body over the texel: > 0 water, < 0 land
            float ShoreDirX; // unit vector up the beach, the way waves travel there (world X, Z), on both
            float ShoreDirZ; // sides of the waterline; (0, 0) far from any shore
        };
        std::vector<Texel> Texels;
    };

    struct WaterShoreSample
    {
        float     GroundCm        = 0.0f;
        float     ShoreDistanceCm = 0.0f;
        glm::vec2 ShoreDirection{ 0.0f };
    };

    struct WaterBodyLevel
    {
        glm::vec4 FootprintMinMax{ 0.0f }; // min X, min Z, max X, max Z, cm
        float     StillLevelCm = 0.0f;
    };

    /// A world with no landscape bakes a 1 x 1 field of the open sea. @p groundOffsetCm: the mean of the landscape
    /// material's displacement (M_*_Landscape), so the drawn sand and the water's floor are one height.
    /// Fails (named) on a non-positive spacing or no body.
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const WaterShoreField>>
    BakeWaterShoreField( std::span<const World::Landscape::LandscapeRayTile> ground,
                         std::span<const WaterBodyLevel> bodies, float spacingCm, float groundOffsetCm );

    [[nodiscard]] WaterShoreSample SampleWaterShoreField( const WaterShoreField& field,
                                                          glm::vec2              worldXZ ) noexcept;
} // namespace Desert::Water

#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Landscape/Public/LandscapeRaycast.hpp>
#include <Water/Public/WaterLayout.hpp> // WaterShoreSample

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

    /// The farthest a height of the field lies from its reference level: RGBA16F holds 65504 cm.
    inline constexpr float kShoreMaxRelativeCm = 65000.0f;

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

        /// A rectangle of texels, [X0, X1) x [Z0, Z1).
        struct Rect
        {
            uint32_t X0 = 0;
            uint32_t Z0 = 0;
            uint32_t X1 = 0;
            uint32_t Z1 = 0;

            [[nodiscard]] bool Empty() const
            {
                return X0 >= X1 || Z0 >= Z1;
            }
        };
        /// A hash of the field's content, never 0: a bake that changed nothing keeps it.
        uint64_t Revision = 0;
        /// The field this one was baked after, when both lie on one grid over one reference level: what changed
        /// since then is in the two rectangles. 0: nothing is known of it, everything changed.
        uint64_t BaseRevision = 0;
        Rect     Changed;       // texels whose GPU value differs from the base's
        Rect     ChangedGround; // texels whose ground does
        /// The GPU's texels (RGBA16F, PackShoreTexel), made by the bake so that no frame packs them.
        std::vector<uint64_t> Packed;
    };

    struct WaterBodyLevel
    {
        glm::vec4 FootprintMinMax{ 0.0f }; // min X, min Z, max X, max Z, cm
        float     StillLevelCm = 0.0f;
        bool      HasShore     = true; // false: a river - a floor under its ribbon, but no still shore
    };

    /// A world with no landscape bakes a 1 x 1 field of the open sea. @p ground: every tile's frame
    /// already raised by its root material's mean displacement (ECS::WaterShoreFieldSource), so the drawn sand and
    /// the water's floor are one height.
    /// @p base: the field this one replaces (null: none), what Changed is stated against.
    /// Fails (named) on a non-positive spacing or no body.
    [[nodiscard]] Common::ResultStr<std::shared_ptr<const WaterShoreField>>
    BakeWaterShoreField( std::span<const World::Landscape::LandscapeRayTile> ground,
                         std::span<const WaterBodyLevel> bodies, float spacingCm, const WaterShoreField* base );

    /// One RGBA16F texel: the ground over @p referenceLevelCm, the signed shore distance, the shore direction.
    [[nodiscard]] uint64_t PackShoreTexel( const WaterShoreField::Texel& texel, float referenceLevelCm ) noexcept;

    [[nodiscard]] WaterShoreSample SampleWaterShoreField( const WaterShoreField& field,
                                                          glm::vec2              worldXZ ) noexcept;
} // namespace Desert::Water

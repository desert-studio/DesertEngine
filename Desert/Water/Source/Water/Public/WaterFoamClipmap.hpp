#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Desert::Water
{
    inline constexpr uint32_t kFoamLevels = 3;

    /// The foam around one view: kFoamLevels nested square windows in one atlas, level 0 the finest.
    struct WaterFoamClipmapDesc
    {
        uint32_t                       Size = 1024; // texels per side of one level, a power of two
        std::array<float, kFoamLevels> ExtentCm{ 12800.0f, 51200.0f, 204800.0f };

        [[nodiscard]] float TexelCm( const uint32_t level ) const noexcept
        {
            return ExtentCm[level] / static_cast<float>( Size );
        }
        [[nodiscard]] bool operator==( const WaterFoamClipmapDesc& ) const = default;
    };

    /// RGBA8: R coverage, G bubbles below the surface, B wetness of the ground (dries), A age. Toroidal, snapped:
    /// grid texel ij lives at ij mod Size, so a window that moves keeps every texel it still covers.
    struct WaterFoamClipmapState
    {
        std::array<glm::ivec2, kFoamLevels> Origin{}; // the grid texel of each level's lower corner
        bool                                Valid = false;
    };

    struct WaterFoamParams // of a body's material
    {
        float JacobianThreshold = 0.62f; // the folding a crest breaks at
        float JacobianSoftness  = 0.25f;
        float WhitecapGain      = 1.0f;
        float FoamDecaySeconds  = 6.0f;
        float BubbleDecaySeconds = 3.0f;
        float WetDrySeconds     = 40.0f;
    };

    /// The windows around @p eye (X, Z).
    [[nodiscard]] inline std::array<glm::ivec2, kFoamLevels> PlaceFoamWindows( const WaterFoamClipmapDesc& desc,
                                                                              const glm::vec2             eye ) noexcept
    {
        std::array<glm::ivec2, kFoamLevels> origin{};
        for ( uint32_t l = 0; l < kFoamLevels; ++l )
            origin[l] = glm::ivec2( glm::floor( eye / desc.TexelCm( l ) ) ) - glm::ivec2( static_cast<int>( desc.Size / 2 ) );
        return origin;
    }
} // namespace Desert::Water

#pragma once

// THE GUST FIELD's CPU half (WIND-FIELD-GPU): the twin of Common/WindGust.glslh (the same expressions in the same
// order; change one, change both) and the state the ECS advances it by. The GPU evaluates the field per swaying
// vertex at its instance's world point (FoliageWindOffset), from the two vec4 every view's camera block carries
// (CameraUB::WindGust / PrevWindGust, ViewFrame::WindGust): exact at any distance, the same in a scene view, a light
// view and a visibility resolve by construction, and nothing to bind beyond the block every mesh pass reads.
//
// UE: the wind source's MinGust / MaxGust modulate SpeedTree's wind over time only (one value per primitive);
// here the gusts are fronts in space that travel downwind, so a meadow shows the wind crossing it (Ghost of
// Tsushima GDC 2021).

#include <glm/glm.hpp>

#include <cmath>

namespace Desert::Graphic
{
    inline constexpr float  kWindGustPeriodCells   = 256.0f; // Common/WindGust.glslh kWindGustPeriodCells
    inline constexpr double kWindGustPeriodCellsD  = 256.0;

    [[nodiscard]] inline float WindGustHash( const float x, const float y )
    {
        const float a = glm::fract( x * 0.1031f );
        const float b = glm::fract( y * 0.1031f );
        const float s = a * ( b + 33.33f ) + b * ( a + 33.33f ) + a * ( a + 33.33f );
        return glm::fract( ( a + b + 2.0f * s ) * ( a + s ) );
    }

    [[nodiscard]] inline float WindGustLattice( const float ix, const float iy )
    {
        return WindGustHash( glm::mod( ix, kWindGustPeriodCells ), glm::mod( iy, kWindGustPeriodCells ) );
    }

    [[nodiscard]] inline float WindGustNoise( const float x, const float y )
    {
        const float ix   = std::floor( x );
        const float iy   = std::floor( y );
        const float fx   = x - ix;
        const float fy   = y - iy;
        const float ux   = fx * fx * ( 3.0f - 2.0f * fx );
        const float uy   = fy * fy * ( 3.0f - 2.0f * fy );
        const float low  = glm::mix( WindGustLattice( ix, iy ), WindGustLattice( ix + 1.0f, iy ), ux );
        const float high = glm::mix( WindGustLattice( ix, iy + 1.0f ), WindGustLattice( ix + 1.0f, iy + 1.0f ), ux );
        return glm::mix( low, high, uy );
    }

    /// The factor in [1 - Gustiness, 1] a swaying vertex's offset is scaled by at the world point (@p worldX,
    /// @p worldZ); 1 when @p gust carries no gusts (w = 0).
    [[nodiscard]] inline float WindGustScale( const float worldX, const float worldZ, const glm::vec4& gust )
    {
        if ( !( gust.w > 0.0f ) )
            return 1.0f;
        const float u = worldX * gust.z - gust.x;
        const float v = worldZ * gust.z - gust.y;
        const float n =
             0.65f * WindGustNoise( u, v ) + 0.35f * WindGustNoise( 2.0f * u + 17.0f, 2.0f * v + 31.0f );
        const float front = glm::smoothstep( 0.3f, 0.7f, n );
        return 1.0f - gust.w * ( 1.0f - front );
    }

    /// The fronts' offset (gust cells, wrapped to the lattice's period) after @p seconds of gameplay in air
    /// moving at @p velocityXZ (cm/s) with fronts @p gustSize (cm) across. Integrated, not t x v: a wind that
    /// changes speed moves the fronts on from where they are, never teleports them. Double: the step of a frame
    /// is far below a float's resolution of the offset after hours.
    [[nodiscard]] inline glm::dvec2 AdvanceWindGust( const glm::dvec2& offsetCells, const float gustSize,
                                                     const glm::vec2& velocityXZ, const double seconds )
    {
        if ( !( gustSize > 0.0f ) )
            return offsetCells;
        const glm::dvec2 step = glm::dvec2( velocityXZ ) * ( seconds / static_cast<double>( gustSize ) );
        const glm::dvec2 next = offsetCells + step;
        return { next.x - kWindGustPeriodCellsD * std::floor( next.x / kWindGustPeriodCellsD ),
                 next.y - kWindGustPeriodCellsD * std::floor( next.y / kWindGustPeriodCellsD ) };
    }

    /// CameraUB::WindGust: x, y the offset, z 1 / GustSize, w Gustiness. Zero (no gusts) when the air carries
    /// none: no gust size, no gustiness, or still air.
    [[nodiscard]] inline glm::vec4 PackWindGust( const glm::dvec2& offsetCells, const float gustiness,
                                                 const float gustSize, const glm::vec2& velocityXZ )
    {
        if ( !( gustiness > 0.0f ) || !( gustSize > 0.0f ) || !( glm::length( velocityXZ ) > 1e-6f ) )
            return glm::vec4( 0.0f );
        return { static_cast<float>( offsetCells.x ), static_cast<float>( offsetCells.y ), 1.0f / gustSize,
                 glm::clamp( gustiness, 0.0f, 1.0f ) };
    }
} // namespace Desert::Graphic

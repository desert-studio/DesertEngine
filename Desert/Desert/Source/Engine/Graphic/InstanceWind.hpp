#pragma once

// UE pattern (Engine/Content/EngineMaterials SimpleGrassWind + FoliageType's bounds scale), adapted: UE sways
// foliage with a world-position offset authored in the material and widens the primitive's bounds by a
// user-set BoundsScale so culling and shadows do not clip the moving tips. Here the offset is ONE function
// shared by every instanced vertex stage (Common/FoliageWind.glslh, the GPU twin of FoliageWindOffset below),
// its parameters come from the foliage type (FOLT 4 Wind), and the bounds are widened by exactly the largest
// offset the function can produce - derived, not a knob. Time is the scene's accumulated GAMEPLAY time (the
// fixed step under --play, zero while editing), so the same time is the same pose on every machine.

#include <Common/Core/Math/AABB.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace Desert::Graphic
{
    /// A foliage type's wind as the renderer reads it. Strength <= 0: the instances stand still.
    struct InstanceWind
    {
        glm::vec2 Direction{ 1.0f, 0.0f }; // unit, world XZ, the way the wind blows TOWARDS
        float     Strength = 0.0f;         // cm, the displacement of a vertex at Height and above
        float     Speed    = 0.0f;         // cycles per second of the fundamental
        float     Height   = 100.0f;       // mesh-local cm at which the sway is full
        float     Seconds  = 0.0f;         // gameplay time, already wrapped to the sway's period

        [[nodiscard]] bool Sways() const
        {
            return Strength > 0.0f;
        }
    };

    // The second harmonic's frequency ratio. The two sines repeat together every 10 fundamental cycles,
    // which is the period the time is wrapped to (float precision would otherwise erode after hours).
    inline constexpr float kWindHarmonicRatio = 2.3f;
    inline constexpr float kWindPeriodCycles  = 10.0f;

    /// The wind of a type from its authored numbers (cm, Hz, cm, degrees) at @p gameplaySeconds.
    [[nodiscard]] inline InstanceWind MakeInstanceWind( float strength, float speed, float height,
                                                        float directionDegrees, double gameplaySeconds )
    {
        InstanceWind wind;
        const float  radians = glm::radians( directionDegrees );
        wind.Direction       = { std::cos( radians ), std::sin( radians ) };
        wind.Strength        = strength;
        wind.Speed           = speed;
        wind.Height          = height;
        if ( speed > 0.0f )
            wind.Seconds = static_cast<float>( std::fmod(
                 gameplaySeconds, static_cast<double>( kWindPeriodCycles ) / static_cast<double>( speed ) ) );
        return wind;
    }

    /**
     * @brief The world-space offset of one vertex: the whole formula. Common/FoliageWind.glslh is the same
     *        expressions in GLSL, and every instanced vertex stage calls that one function.
     *
     * Horizontal only, along the wind. The height mask is the vertex's MESH-local height over Height,
     * clamped to [0, 1] and squared: the root (y <= 0) never moves, the tip moves most. The sway term lies in
     * [0, 1] (the plant leans downwind and swings back to upright), and the phase comes from the instance's
     * world position so neighbours do not move in lockstep. Its magnitude never exceeds Strength, which is
     * what WindBoundsPad relies on.
     */
    [[nodiscard]] inline glm::vec3 FoliageWindOffset( const glm::vec3& localPosition,
                                                      const glm::vec3& instanceOrigin, const InstanceWind& wind )
    {
        if ( !wind.Sways() )
            return glm::vec3( 0.0f );
        float mask = glm::clamp( localPosition.y / wind.Height, 0.0f, 1.0f );
        mask *= mask;
        const float phase = instanceOrigin.x * 0.0123f + instanceOrigin.z * 0.0171f;
        const float t     = 6.2831853f * wind.Speed * wind.Seconds;
        const float sway =
             0.65f * std::sin( t + phase ) + 0.35f * std::sin( kWindHarmonicRatio * t + 1.7f * phase );
        const float amount = wind.Strength * mask * ( 0.5f + 0.5f * sway );
        return glm::vec3( wind.Direction.x, 0.0f, wind.Direction.y ) * amount;
    }

    /// The largest distance FoliageWindOffset can move a vertex: what the instance's box grows by.
    [[nodiscard]] inline float WindBoundsPad( const InstanceWind& wind )
    {
        return wind.Sways() ? wind.Strength : 0.0f;
    }

    /// A world box grown horizontally by the wind's largest offset (the offset is horizontal).
    [[nodiscard]] inline Common::Math::AABB WindExpandedBounds( const Common::Math::AABB& worldBounds,
                                                                const InstanceWind&       wind )
    {
        const float pad = WindBoundsPad( wind );
        return { worldBounds.Min - glm::vec3( pad, 0.0f, pad ), worldBounds.Max + glm::vec3( pad, 0.0f, pad ) };
    }

    /**
     * @brief The push-constant tail every instanced vertex stage reads the wind from.
     *
     * At kInstancedWindPushOffset, after the shared 68-byte material block (mat4 Transform, uint
     * MaterialIndex) and std430's alignment of the next vec4 to 80. A = (dir.x, dir.z, Strength, Height),
     * B = (Speed, Seconds, 0, 0). Pushed for EVERY instanced draw, zeros for one that does not sway: a push
     * block keeps its bytes between draws, so skipping the push would lend the last field's wind to a wall.
     */
    struct InstanceWindPush
    {
        glm::vec4 A{ 0.0f };
        glm::vec4 B{ 0.0f };
    };
    inline constexpr uint32_t kInstancedWindPushOffset = 80;
    inline constexpr uint32_t kInstancedPushSize       = kInstancedWindPushOffset + sizeof( InstanceWindPush );

    [[nodiscard]] inline InstanceWindPush PackInstanceWind( const InstanceWind& wind )
    {
        if ( !wind.Sways() )
            return {};
        return { glm::vec4( wind.Direction.x, wind.Direction.y, wind.Strength, wind.Height ),
                 glm::vec4( wind.Speed, wind.Seconds, 0.0f, 0.0f ) };
    }
} // namespace Desert::Graphic

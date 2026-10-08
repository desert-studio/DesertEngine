#pragma once

// THE SCENE'S WIND (UE: AWindDirectionalSource / UWindDirectionalSourceComponent -> FScene::GetWindParameters).
// A WindSource entity states the air's motion; everything that moves with the wind - foliage sway, the cloud
// layer's drift, cloth, groom - asks ONE question, "the wind at this point" (ECS::WindAt in
// ECS/System/WindField.hpp), and keeps no wind of its own. Better than UE in one respect: UE's clouds do not read
// the wind source, here the cloud layer drifts the way the grass bends.
//
// A directional source blows everywhere along Direction. A point source (UE bPointWind) blows AWAY from the
// entity's world position, full at the centre and falling linearly to nothing at Radius. Several sources add
// up as velocities. Units: centimetres, seconds.
//
// Not carried over from UE: Strength / MinGust / MaxGust. UE's Strength is a unitless SpeedTree input and its
// gusts modulate it; here the wind is a velocity in cm/s and every consumer derives its response from that.

#include <Engine/Reflection/ReflectionMacros.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <span>

namespace Desert::ECS
{
    struct WindSourceData
    {
        REFLECT()

        PROPERTY( DisplayName( "Direction" ), Category( "Wind" ), Summary,
                  Tooltip( "World direction the wind blows TOWARDS (directional source). Normalized when read; "
                           "a zero vector contributes no wind. Ignored by a point source, which blows away from "
                           "its centre." ) )
        glm::vec3 Direction = { 1.0f, 0.0f, 0.0f };

        PROPERTY(
             DisplayName( "Speed" ), Category( "Wind" ), Summary, Range( 0.0f, 50000.0f ), Units( "cm/s" ),
             Tooltip( "The air's speed: what the cloud layer drifts at, what cloth and hair are pushed by, and "
                      "what makes foliage sway (a still scene sways nothing)." ) )
        float Speed = 500.0f; // 5 m/s

        PROPERTY( DisplayName( "Point Wind" ), Category( "Wind" ),
                  Tooltip( "UE bPointWind: blow outward from this entity's position, fading to nothing at Radius, "
                           "instead of everywhere along Direction." ) )
        bool PointWind = false;

        PROPERTY( DisplayName( "Radius" ), Category( "Wind" ), Length, Range( 0.0f, 1.0e7f ),
                  Tooltip( "Point wind only: the distance at which the wind has fallen linearly to zero." ) )
        float Radius = 1000.0f;
    };

    struct WindSourceComponent
    {
        WindSourceData Data;
    };

    /// One source as the query sees it: its data and the world position of its entity.
    struct WindSourceSample
    {
        WindSourceData Data;
        glm::vec3      Position{ 0.0f };
    };

    /// The wind at a point: a velocity, cm/s, world space. Zero = still air.
    struct WindAtPoint
    {
        glm::vec3 Velocity{ 0.0f };

        [[nodiscard]] float Speed() const
        {
            return glm::length( Velocity );
        }

        /// Unit direction of the wind; +X when the air is still (a direction a consumer can always use as an
        /// axis).
        [[nodiscard]] glm::vec3 Direction() const
        {
            const float speed = Speed();
            return speed > 1e-6f ? Velocity / speed : glm::vec3( 1.0f, 0.0f, 0.0f );
        }
    };

    /// The velocity one source contributes at @p position (UE FWindSourceSceneProxy::GetWindParameters).
    [[nodiscard]] inline glm::vec3 WindVelocityFrom( const WindSourceSample& source, const glm::vec3& position )
    {
        const WindSourceData& data = source.Data;
        if ( data.Speed <= 0.0f )
            return glm::vec3( 0.0f );
        if ( !data.PointWind )
        {
            const float lengthSquared = glm::dot( data.Direction, data.Direction );
            if ( lengthSquared <= 1e-12f )
                return glm::vec3( 0.0f );
            return data.Direction / glm::sqrt( lengthSquared ) * data.Speed;
        }
        const glm::vec3 away     = position - source.Position;
        const float     distance = glm::length( away );
        if ( data.Radius <= 0.0f || distance >= data.Radius || distance <= 1e-6f )
            return glm::vec3( 0.0f ); // outside, or at the centre where "away" has no direction
        return away / distance * ( data.Speed * ( 1.0f - distance / data.Radius ) );
    }

    /// THE QUERY's pure core: the sum of every source's velocity at @p position. No source = still air.
    [[nodiscard]] inline WindAtPoint WindAtFromSources( std::span<const WindSourceSample> sources,
                                                        const glm::vec3&                  position )
    {
        WindAtPoint wind;
        for ( const WindSourceSample& source : sources )
            wind.Velocity += WindVelocityFrom( source, position );
        return wind;
    }
} // namespace Desert::ECS

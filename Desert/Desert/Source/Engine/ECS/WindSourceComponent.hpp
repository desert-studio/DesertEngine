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
// Not carried over from UE: Strength. UE's Strength is a unitless SpeedTree input; here the wind is a velocity in
// cm/s and every consumer derives its response from that. UE's MinGust / MaxGust become a directional source's
// Gustiness and Gust Size: the gust fronts the air carries downwind (WindGustAir below, the foliage's
// Common/WindGust.glslh field), which scale the sway between a lull and the type's whole Strength.

#include <CoreReflection/ReflectionMacros.hpp>

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

        PROPERTY( DisplayName( "Gustiness" ), Category( "Wind" ), Range( 0.0f, 1.0f ),
                  Tooltip( "Directional source: how far the gust fronts the wind carries vary the foliage's "
                           "sway - 0 a steady sway, 1 lulls in which the plants stand up between fronts." ) )
        float Gustiness = 0.5f;

        PROPERTY( DisplayName( "Gust Size" ), Category( "Wind" ), Length, Range( 100.0f, 100000.0f ),
                  Tooltip( "Directional source: the size across of one gust front; the fronts travel downwind at "
                           "the wind's speed." ) )
        float GustSize = 2000.0f; // 20 m
    };

    struct WindSourceComponent
    {
        COMPONENT( Key( "WindSource" ), Block( Data ), Run( SkyAndAtmosphere ) )
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

    /// THE GUSTS the scene's air carries (WIND-FIELD-GPU): the directional sources' summed velocity in XZ (what
    /// carries the fronts) and their Gustiness and Gust Size, averaged by each source's speed. A point source is
    /// local and carries no fronts. No directional wind: Gustiness 0, no gusts.
    struct WindGustAir
    {
        glm::vec2 VelocityXZ{ 0.0f }; // cm/s
        float     Gustiness = 0.0f;   // [0, 1]
        float     GustSize  = 0.0f;   // cm
    };

    /// The per-source term of WindGustAir, summed as the sources are visited (no list is built).
    struct WindGustSums
    {
        glm::vec2 VelocityXZ{ 0.0f };
        float     Weight    = 0.0f;
        float     Gustiness = 0.0f;
        float     GustSize  = 0.0f;

        void Add( const WindSourceData& data )
        {
            if ( data.PointWind )
                return;
            const glm::vec3 velocity =
                 WindVelocityFrom( WindSourceSample{ data, glm::vec3( 0.0f ) }, glm::vec3( 0.0f ) );
            const float speed = glm::length( velocity );
            if ( !( speed > 0.0f ) )
                return;
            VelocityXZ += glm::vec2( velocity.x, velocity.z );
            Weight += speed;
            Gustiness += speed * glm::clamp( data.Gustiness, 0.0f, 1.0f );
            GustSize += speed * data.GustSize;
        }

        [[nodiscard]] WindGustAir Air() const
        {
            WindGustAir air;
            if ( !( Weight > 0.0f ) )
                return air;
            air.VelocityXZ = VelocityXZ;
            air.Gustiness  = Gustiness / Weight;
            air.GustSize   = GustSize / Weight;
            return air;
        }
    };

    [[nodiscard]] inline WindGustAir WindGustFromSources( std::span<const WindSourceSample> sources )
    {
        WindGustSums sums;
        for ( const WindSourceSample& source : sources )
            sums.Add( source.Data );
        return sums.Air();
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

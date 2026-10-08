// WIND-SRC: the one wind query (UE FScene::GetWindParameters). No source = still air; a directional source blows
// everywhere along its normalized Direction at Speed; a point source blows away from its centre, falling
// linearly to zero at Radius; several sources add as velocities.

#include <Engine/ECS/WindSourceComponent.hpp>

#include <gtest/gtest.h>

#include <vector>

using Desert::ECS::WindAtFromSources;
using Desert::ECS::WindSourceSample;

namespace
{
    WindSourceSample Directional( const glm::vec3& direction, float speed )
    {
        WindSourceSample source;
        source.Data.Direction = direction;
        source.Data.Speed     = speed;
        return source;
    }

    WindSourceSample Point( const glm::vec3& position, float speed, float radius )
    {
        WindSourceSample source;
        source.Data.PointWind = true;
        source.Data.Speed     = speed;
        source.Data.Radius    = radius;
        source.Position       = position;
        return source;
    }
} // namespace

TEST( WindSource, NoSourceIsStillAir )
{
    const auto wind = WindAtFromSources( {}, glm::vec3( 100.0f, 0.0f, -50.0f ) );
    EXPECT_EQ( wind.Velocity, glm::vec3( 0.0f ) );
    EXPECT_FLOAT_EQ( wind.Speed(), 0.0f );
    EXPECT_EQ( wind.Direction(), glm::vec3( 1.0f, 0.0f, 0.0f ) ); // a usable axis even in still air
}

TEST( WindSource, DirectionalBlowsEverywhereAlongItsNormalizedDirection )
{
    const std::vector<WindSourceSample> sources{ Directional( glm::vec3( 0.0f, 0.0f, 2.0f ), 3000.0f ) };
    for ( const glm::vec3 at : { glm::vec3( 0.0f ), glm::vec3( 1.0e6f, 50.0f, -3.0e5f ) } )
    {
        const auto wind = WindAtFromSources( sources, at );
        EXPECT_NEAR( wind.Velocity.x, 0.0f, 1e-3f );
        EXPECT_NEAR( wind.Velocity.z, 3000.0f, 1e-2f );
        EXPECT_NEAR( wind.Speed(), 3000.0f, 1e-2f );
    }
}

TEST( WindSource, ZeroDirectionOrZeroSpeedContributesNothing )
{
    const std::vector<WindSourceSample> sources{ Directional( glm::vec3( 0.0f ), 3000.0f ),
                                                 Directional( glm::vec3( 1.0f, 0.0f, 0.0f ), 0.0f ) };
    EXPECT_EQ( WindAtFromSources( sources, glm::vec3( 0.0f ) ).Velocity, glm::vec3( 0.0f ) );
}

TEST( WindSource, PointWindBlowsOutwardAndFallsLinearlyToZeroAtRadius )
{
    const std::vector<WindSourceSample> sources{ Point( glm::vec3( 100.0f, 0.0f, 0.0f ), 1000.0f, 400.0f ) };
    // Half way out along +Z: half the speed, pointing away from the centre.
    const auto half = WindAtFromSources( sources, glm::vec3( 100.0f, 0.0f, 200.0f ) );
    EXPECT_NEAR( half.Velocity.z, 500.0f, 1e-2f );
    EXPECT_NEAR( half.Velocity.x, 0.0f, 1e-3f );
    // A quarter of the way along -X: three quarters of the speed, towards -X.
    const auto near = WindAtFromSources( sources, glm::vec3( 0.0f, 0.0f, 0.0f ) );
    EXPECT_NEAR( near.Velocity.x, -750.0f, 1e-2f );
    // At and beyond the radius: nothing.
    EXPECT_EQ( WindAtFromSources( sources, glm::vec3( 500.0f, 0.0f, 0.0f ) ).Velocity, glm::vec3( 0.0f ) );
    EXPECT_EQ( WindAtFromSources( sources, glm::vec3( 100.0f, 0.0f, 9000.0f ) ).Velocity, glm::vec3( 0.0f ) );
}

TEST( WindSource, SourcesAddAsVelocities )
{
    const std::vector<WindSourceSample> sources{ Directional( glm::vec3( 1.0f, 0.0f, 0.0f ), 300.0f ),
                                                 Directional( glm::vec3( 0.0f, 0.0f, 1.0f ), 400.0f ) };
    const auto                          wind = WindAtFromSources( sources, glm::vec3( 0.0f ) );
    EXPECT_NEAR( wind.Speed(), 500.0f, 1e-2f );
    EXPECT_NEAR( wind.Direction().x, 0.6f, 1e-5f );
    EXPECT_NEAR( wind.Direction().z, 0.8f, 1e-5f );
}

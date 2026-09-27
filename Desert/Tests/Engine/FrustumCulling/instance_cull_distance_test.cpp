// FO-5: a foliage type's CullDistance (UE UFoliageType::CullDistance). Instances nearer than Min are all
// drawn, none from Max on, and across the band a share growing linearly with distance is dropped — by a stable
// per-instance threshold, so a still camera never flickers and a receding one never brings an instance back.
// The ISM geometry pass and every shadow cascade select instances through ONE function measured from the main
// camera, so an instance the view dropped casts no shadow.

#include <gtest/gtest.h>

#include <Engine/Core/Frustum.hpp>
#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/InstanceCullDistance.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <vector>

using Desert::Graphic::CollectIsmInstances;
using Desert::Graphic::InstanceCullDistance;
using Desert::Graphic::InstanceFadeThreshold;
using Desert::Graphic::KeepsInstanceAtDistance;

namespace
{
    constexpr InstanceCullDistance kBand{ 1000.0f, 3000.0f };
    const glm::vec3                kEye( 0.0f );

    glm::vec3 AtDistance( float cm )
    {
        return glm::vec3( 0.0f, 0.0f, -cm );
    }

    // The share of @p count instances kept at @p cm.
    double KeptShare( const InstanceCullDistance& cull, float cm, uint32_t count )
    {
        uint32_t kept = 0;
        for ( uint32_t i = 0; i < count; ++i )
            kept += KeepsInstanceAtDistance( cull, i, AtDistance( cm ), kEye ) ? 1u : 0u;
        return static_cast<double>( kept ) / count;
    }

    // A view down -Z from the origin, with the engine's own projection (see frustum_culling_test.cpp).
    Desert::Core::Frustum MainView()
    {
        const glm::mat4 projection =
             Desert::Core::MakePerspective( glm::radians( 60.0f ), 1.0f, 10.0f, 100000.0f );
        const glm::mat4 view = glm::lookAt( kEye, glm::vec3( 0.0f, 0.0f, -1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
        return Desert::Core::Frustum( projection, view );
    }

    // A shadow cascade: an orthographic box from a sun high above, wide enough to hold every instance.
    Desert::Core::Frustum Cascade()
    {
        const glm::mat4 projection =
             Desert::Core::MakeOrthographic( -10000.0f, 10000.0f, -10000.0f, 10000.0f, 10.0f, 50000.0f );
        const glm::mat4 view = glm::lookAt( glm::vec3( 0.0f, 20000.0f, -2000.0f ),
                                            glm::vec3( 0.0f, 0.0f, -2000.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ) );
        return Desert::Core::Frustum( projection, view );
    }
} // namespace

TEST( InstanceCullDistance, MaxZeroIsUEsNeverCulled )
{
    const InstanceCullDistance never{ 0.0f, 0.0f };
    EXPECT_FALSE( never.Culls() );
    EXPECT_TRUE( KeepsInstanceAtDistance( never, 7, AtDistance( 1.0e7f ), kEye ) );
}

TEST( InstanceCullDistance, AllNearerThanMinNoneFromMaxOn )
{
    EXPECT_DOUBLE_EQ( KeptShare( kBand, 0.0f, 1000 ), 1.0 );
    EXPECT_DOUBLE_EQ( KeptShare( kBand, 999.0f, 1000 ), 1.0 );
    EXPECT_DOUBLE_EQ( KeptShare( kBand, 1000.0f, 1000 ), 1.0 ); // the Min boundary is inside
    EXPECT_DOUBLE_EQ( KeptShare( kBand, 3000.0f, 1000 ), 0.0 ); // the Max boundary is outside
    EXPECT_DOUBLE_EQ( KeptShare( kBand, 3001.0f, 1000 ), 0.0 );
    EXPECT_DOUBLE_EQ( KeptShare( kBand, 1.0e6f, 1000 ), 0.0 );
}

TEST( InstanceCullDistance, TheShareDrawnFallsLinearlyAcrossTheBand )
{
    // 10 000 instances: the binomial spread at 0.5 is 0.005, so +-0.02 is four sigma.
    EXPECT_NEAR( KeptShare( kBand, 1500.0f, 10000 ), 0.75, 0.02 );
    EXPECT_NEAR( KeptShare( kBand, 2000.0f, 10000 ), 0.50, 0.02 );
    EXPECT_NEAR( KeptShare( kBand, 2500.0f, 10000 ), 0.25, 0.02 );
    EXPECT_NEAR( KeptShare( kBand, 2980.0f, 10000 ), 0.01, 0.01 );
}

TEST( InstanceCullDistance, AnInstanceDroppedStaysDroppedFartherAway )
{
    // No instance comes back as the camera recedes: the threshold is fixed per instance, the fade only grows.
    for ( uint32_t i = 0; i < 2000; ++i )
    {
        bool dropped = false;
        for ( float cm = 900.0f; cm <= 3100.0f; cm += 25.0f )
        {
            const bool kept = KeepsInstanceAtDistance( kBand, i, AtDistance( cm ), kEye );
            ASSERT_FALSE( dropped && kept ) << "instance " << i << " came back at " << cm << " cm";
            dropped = dropped || !kept;
        }
        ASSERT_TRUE( dropped ) << "instance " << i << " survived past Max";
    }
}

TEST( InstanceCullDistance, ThresholdsSpreadOverTheUnitInterval )
{
    float lowest = 1.0f, highest = 0.0f;
    for ( uint32_t i = 0; i < 4096; ++i )
    {
        const float t = InstanceFadeThreshold( i );
        ASSERT_GE( t, 0.0f );
        ASSERT_LT( t, 1.0f );
        lowest  = std::min( lowest, t );
        highest = std::max( highest, t );
    }
    EXPECT_LT( lowest, 0.01f );
    EXPECT_GT( highest, 0.99f );
}

TEST( InstanceCullDistance, AShadowCascadeDropsExactlyWhatTheViewDrops )
{
    // A row of instances from 100 cm to 4000 cm down the view axis: all inside both the view and the cascade,
    // so the only thing that can tell the two passes apart is the cull distance.
    std::vector<glm::mat4> transforms;
    for ( float cm = 100.0f; cm <= 4000.0f; cm += 10.0f )
        transforms.push_back( glm::translate( glm::mat4( 1.0f ), AtDistance( cm ) ) );
    const Common::Math::AABB bounds{ glm::vec3( -10.0f ), glm::vec3( 10.0f ) };

    std::vector<glm::mat4> viewKept, cascadeKept;
    std::vector<uint32_t>  viewLevels, cascadeLevels;
    CollectIsmInstances( transforms, bounds, MainView(), kBand, {}, kEye, kEye, 0, viewKept, viewLevels );
    // The cascade's LOD is asked from its own place; its cull distance from the main camera, as the renderer does.
    const glm::vec3 sun( 0.0f, 20000.0f, -2000.0f );
    CollectIsmInstances( transforms, bounds, Cascade(), kBand, {}, kEye, sun, 0, cascadeKept, cascadeLevels );

    // Without a cull distance both passes hold every instance: the frusta are not what is being compared.
    std::vector<glm::mat4> allView, allCascade;
    std::vector<uint32_t>  scratch;
    CollectIsmInstances( transforms, bounds, MainView(), {}, {}, kEye, kEye, 0, allView, scratch );
    CollectIsmInstances( transforms, bounds, Cascade(), {}, {}, kEye, sun, 0, allCascade, scratch );
    ASSERT_EQ( allView.size(), transforms.size() );
    ASSERT_EQ( allCascade.size(), transforms.size() );

    ASSERT_LT( viewKept.size(), transforms.size() );
    ASSERT_GT( viewKept.size(), 0u );
    ASSERT_EQ( cascadeKept.size(), viewKept.size() );
    for ( std::size_t i = 0; i < viewKept.size(); ++i )
        EXPECT_EQ( cascadeKept[i], viewKept[i] ) << "the cascade and the view disagree on survivor " << i;
    for ( const glm::mat4& kept : viewKept )
        EXPECT_LT( -kept[3].z, kBand.Max ) << "an instance from Max on survived";
}

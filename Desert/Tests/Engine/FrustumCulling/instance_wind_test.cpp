// FO-7: foliage wind as a world-position offset. The CPU half of the formula (Graphic::FoliageWindOffset) is
// held here; the GPU half is Common/FoliageWind.glslh, and MeshVertexPath holds that every instanced vertex
// stage - surface, G-buffer, shadow caster - reaches its position through that one function.

#include <gtest/gtest.h>

#include <Engine/Core/Frustum.hpp>
#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/InstanceCullDistance.hpp>
#include <Engine/Graphic/InstanceWind.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <vector>

using Desert::Graphic::CollectIsmInstances;
using Desert::Graphic::FoliageWindOffset;
using Desert::Graphic::InstanceWind;
using Desert::Graphic::MakeInstanceWind;
using Desert::Graphic::PackInstanceWind;
using Desert::Graphic::WindBoundsPad;
using Desert::Graphic::WindExpandedBounds;

namespace
{
    // A grass type: 40 cm of sway at 120 cm, 0.7 Hz, blowing towards +X rotated 30 degrees.
    InstanceWind Grass( double seconds )
    {
        return MakeInstanceWind( 40.0f, 0.7f, 120.0f, 30.0f, seconds );
    }

    const glm::vec3 kOrigin( 350.0f, 12.0f, -820.0f );
    const glm::vec3 kTip( 3.0f, 120.0f, -2.0f );

    Desert::Core::Frustum MainView()
    {
        const glm::mat4 projection =
             Desert::Core::MakePerspective( glm::radians( 60.0f ), 1.0f, 10.0f, 100000.0f );
        const glm::mat4 view =
             glm::lookAt( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
        return Desert::Core::Frustum( projection, view );
    }
} // namespace

TEST( InstanceWind, TheSameTimeIsTheSamePoseAndAnotherTimeMovesTheTip )
{
    const glm::vec3 a = FoliageWindOffset( kTip, kOrigin, Grass( 3.25 ) );
    const glm::vec3 b = FoliageWindOffset( kTip, kOrigin, Grass( 3.25 ) );
    EXPECT_EQ( a, b );

    // A quarter of a second later the tip is elsewhere: the offset depends on time, not only on the vertex.
    const glm::vec3 c = FoliageWindOffset( kTip, kOrigin, Grass( 3.50 ) );
    EXPECT_GT( glm::length( a - c ), 1.0f ) << "the tip did not move between 3.25 s and 3.50 s";

    // The clock wraps to the sway's period (10 fundamental cycles), and the pose does not notice.
    const double    period = 10.0 / 0.7;
    const glm::vec3 later  = FoliageWindOffset( kTip, kOrigin, Grass( 3.25 + 7.0 * period ) );
    EXPECT_NEAR( glm::length( a - later ), 0.0f, 1e-2f );
}

TEST( InstanceWind, TheRootNeverMovesAndTheTipDoes )
{
    float tipMax = 0.0f;
    for ( int frame = 0; frame < 600; ++frame )
    {
        const InstanceWind wind = Grass( frame / 60.0 );
        EXPECT_EQ( FoliageWindOffset( glm::vec3( 0.0f ), kOrigin, wind ), glm::vec3( 0.0f ) ) << "frame " << frame;
        EXPECT_EQ( FoliageWindOffset( glm::vec3( 5.0f, -3.0f, 5.0f ), kOrigin, wind ), glm::vec3( 0.0f ) )
             << "below the pivot, frame " << frame;
        tipMax = std::max( tipMax, glm::length( FoliageWindOffset( kTip, kOrigin, wind ) ) );
    }
    EXPECT_GT( tipMax, 0.9f * 40.0f ) << "the tip never swung near its Strength";

    // Horizontal only: the plant leans, it does not stretch.
    EXPECT_EQ( FoliageWindOffset( kTip, kOrigin, Grass( 1.0 ) ).y, 0.0f );
}

TEST( InstanceWind, AStillTypeMovesNothingAndPushesZeros )
{
    const InstanceWind still = MakeInstanceWind( 0.0f, 0.7f, 120.0f, 30.0f, 12.0 );
    EXPECT_FALSE( still.Sways() );
    EXPECT_EQ( FoliageWindOffset( kTip, kOrigin, still ), glm::vec3( 0.0f ) );
    EXPECT_EQ( WindBoundsPad( still ), 0.0f );
    const auto push = PackInstanceWind( still );
    EXPECT_EQ( push.A, glm::vec4( 0.0f ) );
    EXPECT_EQ( push.B, glm::vec4( 0.0f ) );
}

TEST( InstanceWind, TheWidenedBoxHoldsEveryDisplacedVertex )
{
    // A 20 x 150 x 20 cm plant, scaled, yawed and placed; its vertices sampled over ten seconds of wind.
    const Common::Math::AABB local{ glm::vec3( -10.0f, 0.0f, -10.0f ), glm::vec3( 10.0f, 150.0f, 10.0f ) };
    glm::mat4                model = glm::translate( glm::mat4( 1.0f ), kOrigin );
    model                          = glm::rotate( model, glm::radians( 70.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
    model                          = glm::scale( model, glm::vec3( 1.3f ) );
    const auto authored            = Desert::Geometry::TransformBounds( model, local );

    for ( int frame = 0; frame < 600; frame += 7 )
    {
        const InstanceWind wind  = Grass( frame / 60.0 );
        const auto         grown = WindExpandedBounds( authored, wind );
        for ( int corner = 0; corner < 8; ++corner )
            for ( float y : { 0.0f, 40.0f, 120.0f, 150.0f } )
            {
                const glm::vec3 v( ( corner & 1 ) ? local.Max.x : local.Min.x, y,
                                   ( corner & 4 ) ? local.Max.z : local.Min.z );
                const glm::vec3 offset = FoliageWindOffset( v, glm::vec3( model[3] ), wind );
                EXPECT_LE( glm::length( offset ), WindBoundsPad( wind ) + 1e-3f );
                const glm::vec3 world = glm::vec3( model * glm::vec4( v, 1.0f ) ) + offset;
                for ( int axis = 0; axis < 3; ++axis )
                {
                    EXPECT_GE( world[axis], grown.Min[axis] - 1e-2f ) << "frame " << frame << " axis " << axis;
                    EXPECT_LE( world[axis], grown.Max[axis] + 1e-2f ) << "frame " << frame << " axis " << axis;
                }
            }
    }
}

TEST( InstanceWind, AnInstanceWhoseTipSwingsIntoViewIsNotCulled )
{
    // The right plane of a 60-degree view is 1154.7 cm off-axis at 2000 cm. This plant's authored box ends
    // 30 cm outside it; 40 cm of wind can bring its tip in, so it (and its shadow) must be kept.
    const Common::Math::AABB     local{ glm::vec3( -10.0f, 0.0f, -10.0f ), glm::vec3( 10.0f, 150.0f, 10.0f ) };
    const std::vector<glm::mat4> transforms = {
         glm::translate( glm::mat4( 1.0f ), glm::vec3( 1154.7f + 10.0f + 30.0f, 0.0f, -2000.0f ) ) };

    std::vector<glm::mat4> kept;
    std::vector<uint32_t>  levels;
    CollectIsmInstances( transforms, local, MainView(), {}, {}, glm::vec3( 0.0f ), glm::vec3( 0.0f ), 0, kept,
                         levels );
    EXPECT_TRUE( kept.empty() ) << "the authored box alone is outside: the premise of the test";

    CollectIsmInstances( transforms, local, MainView(), {}, Grass( 0.0 ), glm::vec3( 0.0f ), glm::vec3( 0.0f ), 0,
                         kept, levels );
    EXPECT_EQ( kept.size(), 1u ) << "the wind-widened box reaches into the view";
}

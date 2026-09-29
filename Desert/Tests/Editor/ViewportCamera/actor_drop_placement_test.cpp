// A DROPPED ASSET RESTS ITS BOUNDS ON THE SURFACE, AND A DROP INTO NOTHING LANDS IN FRONT OF THE CAMERA.
//
// The mesh drop put the entity's ORIGIN on the surface point, so a character whose origin sits at the hips
// stood buried to the waist and one whose origin sits below its feet floated. UE's FActorPositioning
// pushes the actor's bounds out along the surface normal until their lowest point touches the hit, and a
// ray that meets nothing places the actor BackgroundDropDistance along the ray. Asserted as relations: the
// placed box's lowest point along the normal IS the surface point, whatever the box.

#include <Editor/Core/ActorDropPlacement.hpp>

#include <gtest/gtest.h>

#include <glm/glm.hpp>

namespace ActorDrop = Desert::Editor::ActorDrop;

namespace
{
    ::Common::Math::AABB Box( const glm::vec3& min, const glm::vec3& max )
    {
        return ::Common::Math::AABB{ min, max };
    }
} // namespace

TEST( ActorDropPlacement, ABoxAboveItsOriginIsLoweredOntoTheGround )
{
    const auto offset = ActorDrop::RestOffset( Box( { -20.0f, 5.0f, -20.0f }, { 20.0f, 85.0f, 20.0f } ),
                                               glm::vec3( 1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
    EXPECT_FLOAT_EQ( offset.x, 0.0f );
    EXPECT_FLOAT_EQ( offset.y, -5.0f );
    EXPECT_FLOAT_EQ( offset.z, 0.0f );
}

TEST( ActorDropPlacement, AnOriginAtTheHipsIsRaisedSoTheFeetStandOnTheSurface )
{
    const ActorDrop::Target target{ glm::vec3( 100.0f, 30.0f, -50.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) };
    const auto              bounds = Box( { -25.0f, -90.0f, -15.0f }, { 25.0f, 90.0f, 15.0f } );
    const glm::vec3         origin = ActorDrop::PlacedOrigin( target, bounds, glm::vec3( 1.0f ) );
    EXPECT_FLOAT_EQ( origin.y + bounds.Min.y, target.Point.y );
    EXPECT_FLOAT_EQ( origin.x, target.Point.x );
    EXPECT_FLOAT_EQ( origin.z, target.Point.z );
}

TEST( ActorDropPlacement, TheEntityScaleScalesTheBoxThatRests )
{
    const auto offset = ActorDrop::RestOffset( Box( { -1.0f, -10.0f, -1.0f }, { 1.0f, 10.0f, 1.0f } ),
                                               glm::vec3( 2.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
    EXPECT_FLOAT_EQ( offset.y, 20.0f );
}

TEST( ActorDropPlacement, AWallPushesTheBoxOutAlongItsNormal )
{
    // A wall facing +X: the box's lowest point along +X (Min.x = -30) is pushed to the wall.
    const auto offset = ActorDrop::RestOffset( Box( { -30.0f, 0.0f, -5.0f }, { 10.0f, 50.0f, 5.0f } ),
                                               glm::vec3( 1.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) );
    EXPECT_FLOAT_EQ( offset.x, 30.0f );
    EXPECT_FLOAT_EQ( offset.y, 0.0f );
}

TEST( ActorDropPlacement, AMeshWithNoExtentRestsAtThePoint )
{
    const auto offset = ActorDrop::RestOffset( Box( glm::vec3( 1.0e30f ), glm::vec3( -1.0e30f ) ),
                                               glm::vec3( 1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
    EXPECT_EQ( offset, glm::vec3( 0.0f ) );
}

TEST( ActorDropPlacement, ARayThatMeetsNothingLandsTheBackgroundDistanceAlongIt )
{
    const glm::vec3 origin( 10.0f, 200.0f, -40.0f );
    const auto      target = ActorDrop::TargetFor( std::nullopt, glm::vec3( 0.0f, 1.0f, 0.0f ), origin,
                                                   glm::vec3( 0.0f, 0.0f, -2.0f ) );
    EXPECT_FALSE( target.SurfaceNormal.has_value() );
    EXPECT_FLOAT_EQ( glm::length( target.Point - origin ), ActorDrop::kBackgroundDropDistance );
    EXPECT_FLOAT_EQ( target.Point.z, origin.z - ActorDrop::kBackgroundDropDistance );
    // Nothing to rest on: the origin is the background point, whatever the bounds.
    EXPECT_EQ( ActorDrop::PlacedOrigin( target, Box( glm::vec3( -5.0f ), glm::vec3( 5.0f ) ), glm::vec3( 1.0f ) ),
               target.Point );
}

TEST( ActorDropPlacement, ARayThatMeetsASurfaceNamesItsPointAndNormal )
{
    const auto target = ActorDrop::TargetFor( glm::vec3( 3.0f, 0.0f, 4.0f ), glm::vec3( 0.0f, 2.0f, 0.0f ),
                                              glm::vec3( 0.0f, 100.0f, 0.0f ), glm::vec3( 0.0f, -1.0f, 0.0f ) );
    ASSERT_TRUE( target.SurfaceNormal.has_value() );
    EXPECT_EQ( target.Point, glm::vec3( 3.0f, 0.0f, 4.0f ) );
    EXPECT_FLOAT_EQ( glm::length( *target.SurfaceNormal ), 1.0f );
}

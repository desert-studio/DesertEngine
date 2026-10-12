// PLACING THE EDITOR CAMERA MUST NOT REQUIRE A CAPTURE FLAG, AND MUST LAND WHERE IT WAS ASKED.
//
// `--camera` / `--look` are read only inside `shot.Active()`. A developer who needed the camera at a
// particular viewpoint — with no intention of taking a `--shot` at all, because the control channel takes
// the pictures now — had to launch the editor with a fictitious `--shot --shot-frames 1000000` to unlock
// the placement. The mandatory step of a proof was being performed by the very flag family the channel was
// built to replace. A capture flag must not be the switch for something that is not capture.
//
// So the pose became what it always was: a VALUE, in the request category that exists for values. The
// protocol's own header says why that category is not the palette — "a slider has no name, and giving it
// one would mean a palette entry per value" — and six continuous numbers are the same argument.
//
// WHAT IS ASSERTED HERE, and why each is a relation rather than a spot value:
//
//   1. THE CAMERA LANDS WHERE IT WAS ASKED. `Focus` moves the focal point to a world point and backs the
//      camera off along the view direction by the framing distance, so the aim point and that distance
//      must agree. They were a bare `500.0f` written twice on one line; two spellings put the camera
//      somewhere else and nothing said so.
//   2. THE CENSUS IS THE POSE. What `properties` reports must be what the camera holds, or a report that
//      quotes the numbers beside a capture is quoting a different camera.
//   3. EVERY BAD WRITE IS A NAMED REFUSAL. In particular a zero direction, which normalizes to NaN and
//      reaches the view matrix — a black frame that reads as a broken renderer rather than a bad request.

#include <Editor/Core/ViewportCameraProperties.hpp>

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <string>
#include <vector>

using Desert::Editor::DescribeViewportCamera;
using Desert::Editor::EditableProperty;
using Desert::Editor::kViewportCameraDirection;
using Desert::Editor::kViewportCameraFramingDistance;
using Desert::Editor::kViewportCameraPosition;
using Desert::Editor::ValidateViewportCameraWrite;
using Desert::Editor::ViewportCameraFocalPoint;
using Desert::Editor::ViewportCameraWrite;

namespace
{
    // The pose EditorCamera::Focus ends up in, expressed from the aim point: the focal point is where we
    // aimed, and the camera sits one framing distance BACK along the view direction. This mirrors what the
    // camera does rather than calling it — the camera needs a GPU context and an event loop, and the
    // arithmetic is the half that can be wrong on its own.
    glm::vec3 WhereTheCameraEndsUp( const glm::vec3& aimPoint, const glm::vec3& forward,
                                    float distance = kViewportCameraFramingDistance )
    {
        return aimPoint - glm::normalize( forward ) * distance;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. The placement is a relation between the aim point and the framing distance.
// ---------------------------------------------------------------------------------------------------

// THE PROPERTY, over a family of poses rather than one. A test that pinned a single position would be
// satisfied the day the framing distance moved and the placement broke.
TEST( ViewportCamera, TheCameraLandsExactlyWhereItWasAskedForEveryPoseTried )
{
    const std::vector<glm::vec3> positions = {
         { 0.0f, 200.0f, 0.0f },    { -1500.0f, 30.0f, 900.0f }, { 0.0f, 20000.0f, 0.0f },
         { 12.5f, -4.25f, 0.125f }, { 0.0f, 0.0f, 0.0f },
    };
    const std::vector<glm::vec3> forwards = {
         { 0.0f, 0.0f, -1.0f },
         { 0.0f, 0.9f, -1.0f },
         { 0.0f, -1.0f, 0.0f },
         { 1.0f, 0.0f, 1.0f },
         { -0.3f, 0.2f, 0.93f },
         // Deliberately unnormalized and deliberately tiny: the caller is not required to normalize, and
         // the aim must not depend on the magnitude it was handed.
         { 0.0f, 0.0f, -1000.0f },
         { 0.0f, 0.0f, -0.0001f },
    };

    for ( const glm::vec3& position : positions )
    {
        for ( const glm::vec3& forward : forwards )
        {
            const glm::vec3 aim    = ViewportCameraFocalPoint( position, forward );
            const glm::vec3 landed = WhereTheCameraEndsUp( aim, forward );

            EXPECT_NEAR( landed.x, position.x, 1e-2f );
            EXPECT_NEAR( landed.y, position.y, 1e-2f );
            EXPECT_NEAR( landed.z, position.z, 1e-2f );
        }
    }
}

// The aim point is one framing distance ahead, ALONG the direction asked for. Both halves stated, because
// a factor error in one and a sign error in the other would cancel in the round trip above.
TEST( ViewportCamera, TheAimPointIsOneFramingDistanceAheadAlongTheDirectionGiven )
{
    const glm::vec3 position( 0.0f, 200.0f, 0.0f );
    const glm::vec3 forward( 0.0f, 0.0f, -1.0f );
    const glm::vec3 aim = ViewportCameraFocalPoint( position, forward );

    EXPECT_NEAR( glm::length( aim - position ), kViewportCameraFramingDistance, 1e-2f );
    EXPECT_GT( glm::dot( glm::normalize( aim - position ), forward ), 0.999f );
}

// A DIFFERENT distance still lands the camera at the same place. This is what says the two uses of the
// number are one number and not two that happen to be equal today.
TEST( ViewportCamera, TheFramingDistanceCancelsWhateverItIs )
{
    const glm::vec3 position( -300.0f, 75.0f, 40.0f );
    const glm::vec3 forward( 0.4f, -0.2f, -0.9f );

    for ( const float distance : { 1.0f, 50.0f, 500.0f, 25000.0f } )
    {
        const glm::vec3 landed =
             WhereTheCameraEndsUp( ViewportCameraFocalPoint( position, forward, distance ), forward, distance );
        EXPECT_NEAR( glm::length( landed - position ), 0.0f, 1e-1f ) << "at distance " << distance;
    }
}

// ---------------------------------------------------------------------------------------------------
// 2. The census is the pose.
// ---------------------------------------------------------------------------------------------------

TEST( ViewportCamera, TheCensusReportsThePoseTheCameraIsActuallyIn )
{
    const glm::vec3 position( 10.0f, 200.0f, -30.0f );
    const glm::vec3 forward( 0.0f, 0.5f, -1.0f );

    const std::vector<EditableProperty> census = DescribeViewportCamera( position, forward, 75.0f );

    ASSERT_EQ( census.size(), 3u );
    EXPECT_EQ( census[0].Name, kViewportCameraPosition );
    EXPECT_EQ( census[1].Name, kViewportCameraDirection );
    EXPECT_EQ( census[2].Name, Desert::Editor::kViewportCameraFOV );
    EXPECT_FLOAT_EQ( census[2].Value[0], 75.0f );

    EXPECT_FLOAT_EQ( census[0].Value[0], position.x );
    EXPECT_FLOAT_EQ( census[0].Value[1], position.y );
    EXPECT_FLOAT_EQ( census[0].Value[2], position.z );
    EXPECT_FLOAT_EQ( census[1].Value[0], forward.x );
    EXPECT_FLOAT_EQ( census[1].Value[1], forward.y );
    EXPECT_FLOAT_EQ( census[1].Value[2], forward.z );
}

// THE RELATION BETWEEN THE TWO HALVES OF THE CATEGORY: everything the census offers as settable must be
// accepted by the write, with the component count the census declares. A row a client is told it can write
// and that the write then refuses is the census lying about the editor.
TEST( ViewportCamera, EverySettableRowOfTheCensusIsAcceptedByTheWrite )
{
    const std::vector<EditableProperty> census =
         DescribeViewportCamera( glm::vec3( 0.0f, 200.0f, 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ), 90.0f );

    for ( const EditableProperty& row : census )
    {
        ASSERT_TRUE( row.Settable ) << row.Name;
        ASSERT_TRUE( row.NotSettableReason.empty() ) << row.Name;

        // Built from the census's own declared component count, and given a value that is legal for both
        // rows (a non-degenerate direction is also a legal position).
        // A row with a declared range is given its own minimum.
        const std::vector<float> value( static_cast<std::size_t>( row.Components ), row.Min.value_or( 1.0f ) );
        EXPECT_TRUE( ValidateViewportCameraWrite( row.Name, value ).IsSuccess() )
             << row.Name << " is offered as settable and refused when written";
    }
}

// Every row says its component count in its TYPE too (float3 = 3, float = 1). The client checks the count
// before it sends; a type that disagreed with the count would make one of the two checks wrong.
TEST( ViewportCamera, TheDeclaredTypeAndTheDeclaredCountAgree )
{
    for ( const EditableProperty& row :
          DescribeViewportCamera( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ), 90.0f ) )
    {
        EXPECT_EQ( row.Type,
                   row.Components == 1 ? std::string( "float" ) : "float" + std::to_string( row.Components ) )
             << row.Name;
    }
}

// ---------------------------------------------------------------------------------------------------
// 3. Every bad write is a named refusal.
// ---------------------------------------------------------------------------------------------------

TEST( ViewportCamera, AWriteNamesWhichOfTheTwoItIs )
{
    const auto position = ValidateViewportCameraWrite( kViewportCameraPosition, { 0.0f, 200.0f, 0.0f } );
    ASSERT_TRUE( position.IsSuccess() );
    EXPECT_EQ( position.GetValue(), ViewportCameraWrite::Position );

    const auto direction = ValidateViewportCameraWrite( kViewportCameraDirection, { 0.0f, 0.0f, -1.0f } );
    ASSERT_TRUE( direction.IsSuccess() );
    EXPECT_EQ( direction.GetValue(), ViewportCameraWrite::Direction );
}

// A ZERO DIRECTION IS THE ONE THAT MATTERS. glm::normalize of it is NaN, and NaN reaches the view matrix,
// then every pass: a black frame that reads as a broken renderer rather than as a request nobody should
// have sent. Refused with the mechanism named, so the reader does not go looking in the renderer.
TEST( ViewportCamera, AZeroDirectionIsRefusedBeforeItCanReachTheViewMatrix )
{
    const auto refused = ValidateViewportCameraWrite( kViewportCameraDirection, { 0.0f, 0.0f, 0.0f } );

    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "NaN" ), std::string::npos );
}

// A position of (0,0,0) is a perfectly ordinary place to stand and must NOT be caught by the same check.
// The two are one line apart in the implementation, which is exactly why this is asserted.
TEST( ViewportCamera, TheOriginIsAValidPositionEvenThoughItIsNotAValidDirection )
{
    EXPECT_TRUE( ValidateViewportCameraWrite( kViewportCameraPosition, { 0.0f, 0.0f, 0.0f } ).IsSuccess() );
    EXPECT_FALSE( ValidateViewportCameraWrite( kViewportCameraDirection, { 0.0f, 0.0f, 0.0f } ).IsSuccess() );
}

// Padding a short value would place the camera somewhere nobody asked for and report it done; widening a
// long one would silently drop what the client meant. Both are refused, with the count quoted back.
TEST( ViewportCamera, TheWrongNumberOfComponentsIsRefusedRatherThanPaddedOrTruncated )
{
    for ( const std::vector<float>& value :
          { std::vector<float>{}, std::vector<float>{ 1.0f }, std::vector<float>{ 1.0f, 2.0f },
            std::vector<float>{ 1.0f, 2.0f, 3.0f, 4.0f } } )
    {
        const auto refused = ValidateViewportCameraWrite( kViewportCameraPosition, value );
        ASSERT_FALSE( refused.IsSuccess() ) << value.size() << " components were accepted";
        EXPECT_NE( refused.GetError().find( std::to_string( value.size() ) ), std::string::npos );
    }
}

// An unknown name is refused NAMING what the viewport does offer, because a client that guessed at a name
// has no other way to find the right one and would otherwise guess again.
TEST( ViewportCamera, AnUnknownPropertyIsRefusedAndTheKnownOnesAreListed )
{
    const auto refused = ValidateViewportCameraWrite( "Camera.Roll", { 1.0f, 2.0f, 3.0f } );

    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "Camera.Roll" ), std::string::npos );
    EXPECT_NE( refused.GetError().find( kViewportCameraPosition ), std::string::npos );
    EXPECT_NE( refused.GetError().find( kViewportCameraDirection ), std::string::npos );
}

// A DOCUMENT'S property name is not a viewport property, and vice versa. The two subjects share the
// request shape and nothing else; a name that fell through from one to the other would write the wrong
// thing on whichever subject happened to be selected.
TEST( ViewportCamera, ADocumentsPropertyNameIsNotAViewportProperty )
{
    EXPECT_FALSE( ValidateViewportCameraWrite( "RoughnessFactor", { 0.25f } ).IsSuccess() );
    EXPECT_FALSE( ValidateViewportCameraWrite( "", { 1.0f, 2.0f, 3.0f } ).IsSuccess() );
}

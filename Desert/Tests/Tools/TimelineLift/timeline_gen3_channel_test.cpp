// The generation-3 bone track's rotation sampler (Tools/SceneMigrator, ClipGeneration3) and the Timeline's
// RotationChannel agree bit for bit: what the lift relies on when it moves a generation-3 clip's rotation keys
// into a channel. Moved out of Engine/TimelineContract, whose runner does not compile the migrator.

#include "../../Engine/TimelineContract/TimelineFixtures.hpp"

#include "ClipGeneration3.hpp"

using namespace TimelineFixtures;
namespace Gen3 = Desert::Migration::ClipGen3;

TEST( TimelineChannel, RotationIsRotationKeyFramesSlerpBitForBit )
{
    const glm::quat a = glm::angleAxis( 0.3F, glm::vec3( 0, 1, 0 ) );
    const glm::quat b = glm::angleAxis( 1.7F, glm::normalize( glm::vec3( 1, 1, 0 ) ) );

    Gen3::BoneTrack legacy;
    legacy.BoneName     = "Spine";
    legacy.RotationKeys = { Gen3::RotationKeyFrame{ Tick( 0 ), a }, Gen3::RotationKeyFrame{ Tick( 60 ), b } };

    RotationChannel channel;
    channel.X.Keys = { Key( 0, a.x ), Key( 60, b.x ) };
    channel.Y.Keys = { Key( 0, a.y ), Key( 60, b.y ) };
    channel.Z.Keys = { Key( 0, a.z ), Key( 60, b.z ) };
    channel.W.Keys = { Key( 0, a.w ), Key( 60, b.w ) };

    const FrameRate rate{ 60, 1 };
    for ( const FrameTime at : { At( 0 ), At( 17, 0.25F ), At( 30 ), At( 59, 0.9F ), At( 60 ) } )
    {
        EXPECT_EQ( Evaluate( channel, at, rate ), legacy.Sample( at, rate ).Rotation ) << at.AsTicks();
    }
}

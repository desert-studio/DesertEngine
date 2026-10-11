// THE TIMELINE CORE'S CONTRACT, groups 1 and 2: on-disk integers and channels (ANIM-I1, Channel.cpp).

#include "TimelineFixtures.hpp"

using namespace TimelineFixtures;

// ── 1. On-disk integers ─────────────────────────────────────────────────────────────────────────────

static_assert( static_cast<int>( ChannelKind::Float ) == 0 && static_cast<int>( ChannelKind::Event ) == 5 );
static_assert( static_cast<int>( TrackKind::Transform ) == static_cast<int>( ChannelKind::Transform ) );
static_assert( static_cast<int>( TrackKind::Event ) == static_cast<int>( ChannelKind::Event ) );
static_assert( static_cast<int>( TrackKind::CameraCut ) == 7 );
static_assert( static_cast<int>( BindingKind::Widget ) == 3 );
static_assert( static_cast<int>( SequenceHost::LevelSequence ) == 2 );
static_assert( static_cast<int>( LoopMode::PingPong ) == 2 );
// ONE blend enum: the section's is ClipSection's, not a copy.
static_assert( std::is_same_v<decltype( Section{}.Blend ), Desert::Animation::SectionBlendType> );
// ONE key model: every value channel stores ScalarKey.
static_assert( std::is_same_v<decltype( FloatChannel{}.Keys )::value_type, ScalarKey> );
static_assert( std::is_same_v<decltype( Section{}.Weight )::value_type, ScalarKey> );

// ── 2. Channels ─────────────────────────────────────────────────────────────────────────────────────

TEST( TimelineChannel, KindOfMakeChannelRoundTripsEveryKind )
{
    for ( const ChannelKind kind : { ChannelKind::Float, ChannelKind::Vector, ChannelKind::Rotation,
                                     ChannelKind::Transform, ChannelKind::Bool, ChannelKind::Event } )
    {
        EXPECT_EQ( KindOf( MakeChannel( kind ) ), kind ) << ToString( kind );
    }
}

TEST( TimelineChannel, AnEmptyChannelIsItsDefaultNotZero )
{
    FloatChannel channel;
    channel.Default = 7.0F;
    EXPECT_EQ( Evaluate( channel, At( 10 ), FrameRate{ 60, 1 } ), 7.0F );

    const TransformChannel rest;
    const BoneTransform    value = Evaluate( rest, At( 0 ), FrameRate{ 60, 1 } );
    EXPECT_EQ( value.Scale, glm::vec3( 1.0F ) );
    EXPECT_EQ( value.Rotation, glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) );
}

TEST( TimelineChannel, HeldFlatOutsideTheKeysAndLinearBetween )
{
    FloatChannel channel;
    channel.Keys = { Key( 10, 2.0F ), Key( 30, 6.0F ) };
    const FrameRate rate{ 60, 1 };
    EXPECT_EQ( Evaluate( channel, At( 0 ), rate ), 2.0F );
    EXPECT_EQ( Evaluate( channel, At( 50 ), rate ), 6.0F );
    EXPECT_NEAR( Evaluate( channel, At( 20 ), rate ), 4.0F, 1e-6F );
}

TEST( TimelineChannel, EventsCrossedAreTheHalfOpenIntervalAndWrapOnce )
{
    EventChannel channel;
    channel.Keys = { EventKey{ Tick( 10 ), Tick( 0 ), "Step", 0 }, EventKey{ Tick( 90 ), Tick( 0 ), "Land", 0 } };

    std::vector<CrossedEvent> out;
    CollectCrossed( channel, At( 10 ), At( 50 ), false, Tick( 0 ), Tick( 100 ), out );
    EXPECT_TRUE( out.empty() ) << "(from, to]: an event ON `from` fired last frame";

    CollectCrossed( channel, At( 80 ), At( 20 ), true, Tick( 0 ), Tick( 100 ), out );
    ASSERT_EQ( out.size(), 2U );
    EXPECT_EQ( out[0].Key->Name, "Land" );
    EXPECT_EQ( out[1].Key->Name, "Step" );
}

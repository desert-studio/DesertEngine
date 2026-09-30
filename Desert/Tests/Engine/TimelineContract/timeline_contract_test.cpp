// THE TIMELINE CORE'S CONTRACT, WRITTEN BEFORE ITS IMPLEMENTATION (ANIM-UNIFY step 0).
//
// Every test here is against a SIGNATURE in Engine/Animation/Timeline/ and Engine/Animation/Graph/
// {LayeredBlendPerBone,LinkedAnimLayer}.hpp. At step 0 the suite compiles and does not link — the
// implementation pieces make it link one group at a time, and a piece is done when its group is green.
//
// The groups, in the order the pieces land:
//   1. enums and on-disk integers (pure static_asserts — green from day one);
//   2. channels: defaults, sampling, rotation = RotationKeyFrame's slerp bit for bit, events crossed;
//   3. sections/tracks/sequence: WeightAt, the fold, Validate's refusals, host restrictions;
//   4. the Player's time;
//   5. the Evaluator and the host seam (resolve once, unresolved REPORTED);
//   6. easing presets (the UI key model's replacement);
//   7. serialization round trip;
//   8. LiftClip: the migration is the identity, bit for bit;
//   9. LayeredBlendPerBone and LinkedAnimLayer.

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Graph/LayeredBlendPerBone.hpp>
#include <Engine/Animation/Graph/LinkedAnimLayer.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>
#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Animation/Timeline/Hosts.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/Timeline/Section.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Animation/Timeline/Track.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <type_traits>

namespace
{
    using namespace Desert::Animation;
    using namespace Desert::Animation::Timeline;
    namespace G = Desert::Animation::Graph;
    using Common::Content::AssetGuid;

    FrameNumber Tick( int32_t value )
    {
        return FrameNumber{ value };
    }
    FrameTime At( int32_t tick, float subframe = 0.0F )
    {
        return FrameTime{ Tick( tick ), subframe };
    }
    ScalarKey Key( int32_t tick, float value, KeyInterp interp = KeyInterp::Linear )
    {
        ScalarKey key;
        key.Tick   = Tick( tick );
        key.Value  = value;
        key.Interp = interp;
        return key;
    }
    BindingGuid Guid( uint64_t lo )
    {
        return BindingGuid{ AssetGuid{ 1, lo } };
    }

    /// A LevelSequence with one entity and one Float track "Opacity" holding @p channel in one section.
    Sequence OneFloatTrack( FloatChannel channel )
    {
        Sequence sequence;
        sequence.Host  = SequenceHost::LevelSequence;
        sequence.Start = Tick( 0 );
        sequence.End   = Tick( 100 );
        sequence.Bindings.push_back( Binding{ Guid( 1 ), BindingKind::Entity, "entity-uuid", "Door", {} } );
        Track track;
        track.Binding  = Guid( 1 );
        track.Property = "Opacity";
        track.Kind     = TrackKind::Float;
        Section section;
        section.Start   = Tick( 0 );
        section.End     = Tick( 100 );
        section.Content = Channel{ std::move( channel ) };
        track.Sections.push_back( std::move( section ) );
        sequence.Tracks.push_back( std::move( track ) );
        return sequence;
    }

    float EvaluateOnlyFloat( const Sequence& sequence, FrameTime at )
    {
        Evaluator      evaluator( sequence );
        EvaluatedFrame frame;
        evaluator.Evaluate( TimeStep{ at, at }, frame );
        EXPECT_EQ( frame.Values.size(), 1U );
        return frame.Values.empty() ? NAN : std::get<float>( frame.Values[0].Value );
    }
} // namespace

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

TEST( TimelineChannel, RotationIsRotationKeyFramesSlerpBitForBit )
{
    const glm::quat a = glm::angleAxis( 0.3F, glm::vec3( 0, 1, 0 ) );
    const glm::quat b = glm::angleAxis( 1.7F, glm::normalize( glm::vec3( 1, 1, 0 ) ) );

    BoneTrack legacy;
    legacy.BoneName     = "Spine";
    legacy.RotationKeys = { RotationKeyFrame{ Tick( 0 ), a }, RotationKeyFrame{ Tick( 60 ), b } };

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

// ── 3. Sections, tracks, the sequence ───────────────────────────────────────────────────────────────

TEST( TimelineSection, AnEmptyWeightIsOneNotZero )
{
    Section section;
    EXPECT_EQ( WeightAt( section, At( 5 ), FrameRate{ 60, 1 } ), 1.0F );
}

TEST( TimelineSection, FullWeightAbsoluteIsTheKeyValueBitForBit )
{
    FloatChannel channel;
    channel.Default             = 0.1F;
    channel.Keys                = { Key( 0, 0.3F ), Key( 100, 0.7F ) };
    const FloatChannel copy     = channel;
    const Sequence     sequence = OneFloatTrack( channel );
    for ( const FrameTime at : { At( 0 ), At( 33, 0.3F ), At( 100 ) } )
    {
        EXPECT_EQ( EvaluateOnlyFloat( sequence, at ), Evaluate( copy, at, sequence.TickRate ) );
    }
}

TEST( TimelineSection, LaterRowWinsAtFullWeightAndAdditiveZeroIsANoOp )
{
    FloatChannel low;
    low.Keys          = { Key( 0, 1.0F ) };
    Sequence sequence = OneFloatTrack( low );

    Section high = sequence.Tracks[0].Sections[0];
    high.Row     = 1;
    high.Content = Channel{ FloatChannel{ { Key( 0, 5.0F ) }, 0.0F } };
    sequence.Tracks[0].Sections.push_back( high );
    EXPECT_EQ( EvaluateOnlyFloat( sequence, At( 10 ) ), 5.0F );

    Section additive = high;
    additive.Row     = 2;
    additive.Blend   = SectionBlendType::Additive;
    additive.Content = Channel{ FloatChannel{ { Key( 0, 0.0F ) }, 0.0F } };
    sequence.Tracks[0].Sections.push_back( additive );
    EXPECT_EQ( EvaluateOnlyFloat( sequence, At( 10 ) ), 5.0F );
}

TEST( TimelineSection, AHalfWeightAbsoluteIsHalfTheValueFromTheDefault )
{
    FloatChannel channel;
    channel.Default                       = 2.0F;
    channel.Keys                          = { Key( 0, 10.0F ) };
    Sequence sequence                     = OneFloatTrack( channel );
    sequence.Tracks[0].Sections[0].Weight = { Key( 0, 0.5F ) };
    EXPECT_NEAR( EvaluateOnlyFloat( sequence, At( 10 ) ), 6.0F, 1e-6F );
}

TEST( TimelineSequence, AMinimalSequenceValidates )
{
    const Sequence sequence = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    const auto     valid    = Validate( sequence );
    EXPECT_TRUE( valid.IsSuccess() ) << valid.GetError();
}

TEST( TimelineSequence, ValidateRefusesEachBrokenInvariant )
{
    const Sequence good = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );

    Sequence unknownBinding          = good;
    unknownBinding.Tracks[0].Binding = Guid( 99 );
    EXPECT_FALSE( Validate( unknownBinding ).IsSuccess() ) << "a track pointing at no binding";

    Sequence kindMismatch       = good;
    kindMismatch.Tracks[0].Kind = TrackKind::Vector;
    EXPECT_FALSE( Validate( kindMismatch ).IsSuccess() ) << "a Vector track holding a Float section";

    Sequence duplicate = good;
    duplicate.Tracks.push_back( duplicate.Tracks[0] );
    EXPECT_FALSE( Validate( duplicate ).IsSuccess() ) << "two tracks for one property";

    Sequence unsorted = OneFloatTrack( FloatChannel{ { Key( 20, 1.0F ), Key( 10, 2.0F ) }, 0.0F } );
    EXPECT_FALSE( Validate( unsorted ).IsSuccess() ) << "keys out of tick order";

    Sequence nullGuid         = good;
    nullGuid.Bindings[0].Guid = BindingGuid{};
    EXPECT_FALSE( Validate( nullGuid ).IsSuccess() ) << "a null binding GUID";

    Sequence boolLinear       = good;
    boolLinear.Tracks[0].Kind = TrackKind::Bool;
    boolLinear.Tracks[0].Sections[0].Content =
         Channel{ BoolChannel{ FloatChannel{ { Key( 0, 0.0F ), Key( 10, 1.0F, KeyInterp::Linear ) }, 0.0F } } };
    EXPECT_FALSE( Validate( boolLinear ).IsSuccess() ) << "a Bool key that interpolates";

    Sequence rotation       = good;
    rotation.Tracks[0].Kind = TrackKind::Rotation;
    RotationChannel misaligned;
    misaligned.X.Keys                      = { Key( 0, 0.0F ), Key( 10, 0.0F ) };
    misaligned.W.Keys                      = { Key( 0, 1.0F ) };
    rotation.Tracks[0].Sections[0].Content = Channel{ misaligned };
    EXPECT_FALSE( Validate( rotation ).IsSuccess() ) << "rotation components keyed apart";

    Sequence badRange                    = good;
    badRange.Tracks[0].Sections[0].Start = Tick( 50 );
    badRange.Tracks[0].Sections[0].End   = Tick( 10 );
    EXPECT_FALSE( Validate( badRange ).IsSuccess() ) << "Start after End";
}

TEST( TimelineSequence, TheHostRestrictsWhatASequenceMayHold )
{
    Sequence clip = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    clip.Host     = SequenceHost::AnimationClip;
    EXPECT_FALSE( Validate( clip ).IsSuccess() ) << "an .anim cannot bind an entity";

    Sequence ui = clip;
    ui.Host     = SequenceHost::UIAnimation;
    EXPECT_FALSE( Validate( ui ).IsSuccess() ) << "a UI clip binds widgets only";

    Sequence level = clip;
    level.Host     = SequenceHost::LevelSequence;
    EXPECT_TRUE( Validate( level ).IsSuccess() );
}

// ── 4. The Player ───────────────────────────────────────────────────────────────────────────────────

TEST( TimelinePlayer, NotPlayingIsAnEmptyStep )
{
    Player         player( FrameRate{ 60, 1 }, Tick( 0 ), Tick( 60 ) );
    const TimeStep step = player.Advance( 0.5 );
    EXPECT_EQ( step.From.AsTicks(), step.To.AsTicks() );
    EXPECT_EQ( player.State(), PlayState::Stopped );
}

TEST( TimelinePlayer, LoopWrapsOnceAndReportsIt )
{
    Player player( FrameRate{ 60, 1 }, Tick( 0 ), Tick( 60 ) );
    player.SetLoopMode( LoopMode::Loop );
    player.Play();
    (void)player.Advance( 0.75 );                // tick 45
    const TimeStep step = player.Advance( 0.5 ); // 45 + 30 → wraps to 15
    EXPECT_TRUE( step.Wrapped );
    EXPECT_NEAR( player.Current().AsTicks(), 15.0, 1e-6 );
}

TEST( TimelinePlayer, OnceClampsOnTheLastTickAndStops )
{
    Player player( FrameRate{ 60, 1 }, Tick( 0 ), Tick( 60 ) );
    player.Play();
    const TimeStep step = player.Advance( 5.0 );
    EXPECT_TRUE( step.Finished );
    EXPECT_FALSE( step.Wrapped );
    EXPECT_EQ( player.Current().Frame, Tick( 60 ) );
    EXPECT_EQ( player.State(), PlayState::Stopped );
}

TEST( TimelinePlayer, PingPongTurnsAround )
{
    Player player( FrameRate{ 60, 1 }, Tick( 0 ), Tick( 60 ) );
    player.SetLoopMode( LoopMode::PingPong );
    player.Play();
    const TimeStep step = player.Advance( 1.25 ); // 75 → bounced back to 45
    EXPECT_TRUE( step.Reversed );
    EXPECT_NEAR( player.Current().AsTicks(), 45.0, 1e-6 );
}

TEST( TimelinePlayer, AJumpCrossesNothing )
{
    Player         player( FrameRate{ 60, 1 }, Tick( 0 ), Tick( 60 ) );
    const TimeStep step = player.JumpTo( At( 40 ) );
    EXPECT_EQ( step.From.AsTicks(), step.To.AsTicks() );
    EXPECT_EQ( player.Current().Frame, Tick( 40 ) );
}

// ── 5. The Evaluator and the host seam ──────────────────────────────────────────────────────────────

namespace
{
    struct CountingHost final : ITimelineHost
    {
        int                      Resolves = 0;
        bool                     Present  = true;
        std::vector<std::string> Applied;

        std::optional<ResolvedBinding> Resolve( const Binding& ) override
        {
            ++Resolves;
            return Present ? std::optional<ResolvedBinding>( ResolvedBinding{ 42 } ) : std::nullopt;
        }
        void Apply( const ResolvedBinding&, std::string_view property, const EvaluatedValue& ) override
        {
            Applied.emplace_back( property );
        }
        void Fire( const FiredEvent& ) override
        {
        }
        void SetCamera( const std::optional<ResolvedBinding>& ) override
        {
        }
        void PlayAnimation( const ResolvedBinding&, const AnimationSample& ) override
        {
        }
    };
} // namespace

TEST( TimelineEvaluator, BindingsResolveOncePerRevision )
{
    const Sequence sequence = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    Evaluator      evaluator( sequence );
    CountingHost   host;
    EvaluatedFrame frame;
    for ( int i = 0; i < 3; ++i )
    {
        evaluator.Evaluate( TimeStep{ At( i ), At( i + 1 ) }, frame );
        (void)evaluator.Apply( frame, host );
    }
    EXPECT_EQ( host.Resolves, 1 );
    EXPECT_EQ( host.Applied.size(), 3U );
}

TEST( TimelineEvaluator, AnUnresolvedBindingIsReportedByLabelNotSkippedSilently )
{
    const Sequence sequence = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    Evaluator      evaluator( sequence );
    CountingHost   host;
    host.Present = false;
    EvaluatedFrame frame;
    evaluator.Evaluate( TimeStep{ At( 0 ), At( 1 ) }, frame );
    const ApplyReport report = evaluator.Apply( frame, host );
    ASSERT_EQ( report.Unresolved.size(), 1U );
    EXPECT_EQ( report.Unresolved[0], "Door" );
    EXPECT_TRUE( host.Applied.empty() );
}

TEST( TimelineEvaluator, AMutedTrackIsAbsentNotDefaulted )
{
    Sequence sequence        = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ) }, 0.0F } );
    sequence.Tracks[0].Muted = true;
    Evaluator      evaluator( sequence );
    EvaluatedFrame frame;
    evaluator.Evaluate( TimeStep{ At( 0 ), At( 0 ) }, frame );
    EXPECT_TRUE( frame.Values.empty() );
}

// ── 6. Easing presets: the UI key model's replacement ───────────────────────────────────────────────

TEST( TimelineEasing, CubicOutIsExactOnTwoKeys )
{
    std::vector<ScalarKey> keys = { Key( 0, 0.0F ), Key( 60, 1.0F ) };
    const FrameRate        rate{ 60, 1 };
    const auto             result = ApplyEasingPreset( keys, 1, EasingPreset::CubicOut, rate, rate );
    ASSERT_TRUE( result.IsSuccess() ) << result.GetError();
    EXPECT_EQ( result.GetValue().InsertedKeys, 0U );
    EXPECT_EQ( result.GetValue().MaxDeviation, 0.0F );
    EXPECT_EQ( keys[1].Mode, TangentMode::User ) << "AutoSetTangents must not undo a preset";

    const FloatChannel channel{ keys, 0.0F };
    EXPECT_NEAR( Evaluate( channel, At( 30 ), rate ), 1.0F - 0.125F, 1e-5F ); // 1 - (1 - 0.5)^3
    EXPECT_NEAR( Evaluate( channel, At( 15 ), rate ), 1.0F - 0.421875F, 1e-5F );
}

TEST( TimelineEasing, InOutInsertsOneMiddleKeyAndElasticReportsItsBake )
{
    const FrameRate        rate{ 60, 1 };
    std::vector<ScalarKey> inOut = { Key( 0, 0.0F ), Key( 60, 1.0F ) };
    const auto             a     = ApplyEasingPreset( inOut, 1, EasingPreset::QuadInOut, rate, rate );
    ASSERT_TRUE( a.IsSuccess() );
    EXPECT_EQ( a.GetValue().InsertedKeys, 1U );
    ASSERT_EQ( inOut.size(), 3U );
    EXPECT_EQ( inOut[1].Tick, Tick( 30 ) );

    std::vector<ScalarKey> elastic = { Key( 0, 0.0F ), Key( 60, 1.0F ) };
    const auto             b       = ApplyEasingPreset( elastic, 1, EasingPreset::ElasticOut, rate, rate );
    ASSERT_TRUE( b.IsSuccess() );
    EXPECT_GT( b.GetValue().InsertedKeys, 0U );
    EXPECT_GE( b.GetValue().MaxDeviation, 0.0F );
}

TEST( TimelineEasing, TheFirstKeyOwnsNoSegment )
{
    std::vector<ScalarKey> keys = { Key( 0, 0.0F ), Key( 60, 1.0F ) };
    EXPECT_FALSE( ApplyEasingPreset( keys, 0, EasingPreset::QuadIn, FrameRate{}, FrameRate{} ).IsSuccess() );
    EXPECT_FALSE( ApplyEasingPreset( keys, 2, EasingPreset::QuadIn, FrameRate{}, FrameRate{} ).IsSuccess() );
}

// ── 7. Serialization ────────────────────────────────────────────────────────────────────────────────

TEST( TimelineFormat, WriteReadRoundTripsAndATruncatedBlockIsRefused )
{
    const Sequence original          = OneFloatTrack( FloatChannel{ { Key( 0, 1.0F ), Key( 40, 3.0F ) }, 0.5F } );
    const std::vector<uint8_t> bytes = WriteSequence( original );
    const auto                 read  = ReadSequence( bytes );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const Sequence& back = read.GetValue();
    ASSERT_EQ( back.Tracks.size(), 1U );
    EXPECT_EQ( back.Bindings[0].Guid, original.Bindings[0].Guid );
    EXPECT_EQ( back.Tracks[0].Property, "Opacity" );
    for ( const FrameTime at : { At( 0 ), At( 20 ), At( 40 ) } )
    {
        EXPECT_EQ( EvaluateOnlyFloat( back, at ), EvaluateOnlyFloat( original, at ) );
    }

    const std::span<const uint8_t> cut( bytes.data(), bytes.size() / 2 );
    EXPECT_FALSE( ReadSequence( cut ).IsSuccess() );
}

// ── 8. LiftClip: the .anim migration is the identity ────────────────────────────────────────────────

TEST( TimelineLiftClip, EveryBoneSamplesBitForBitAfterTheLift )
{
    AnimationClip clip;
    clip.AnimationName = "Walk";
    clip.DurationTicks = Tick( 60 );
    BoneTrack spine;
    spine.BoneName     = "Spine";
    spine.PositionKeys = { PositionKeyFrame{ Tick( 0 ), glm::vec3( 0, 1, 0 ) },
                           PositionKeyFrame{ Tick( 60 ), glm::vec3( 3, 1, -2 ) } };
    spine.RotationKeys = { RotationKeyFrame{ Tick( 0 ), glm::quat( 1, 0, 0, 0 ) },
                           RotationKeyFrame{ Tick( 60 ), glm::angleAxis( 1.1F, glm::vec3( 0, 0, 1 ) ) } };
    clip.Tracks.push_back( spine );
    clip.Curves.push_back( AnimationCurve{ "Footstep", { Key( 0, 0.0F ), Key( 60, 1.0F ) } } );
    clip.Notifies.push_back( AnimationNotify{ "Step", Tick( 30 ), 2, Tick( 0 ) } );

    const auto lifted = LiftClip( clip );
    ASSERT_TRUE( lifted.IsSuccess() ) << lifted.GetError();
    const Sequence& sequence = lifted.GetValue();
    EXPECT_EQ( sequence.Host, SequenceHost::AnimationClip );
    EXPECT_TRUE( Validate( sequence ).IsSuccess() );

    std::vector<BoneInfo> bones( 1 );
    bones[0].Name = "Spine";
    const Skeleton         skeleton( std::move( bones ) );
    const BoneBindingTable table = BindBones( sequence, skeleton );
    EXPECT_EQ( table.Missing, 0U );

    for ( const FrameTime at : { At( 0 ), At( 7, 0.5F ), At( 30 ), At( 59, 0.99F ), At( 60 ) } )
    {
        LocalPose pose( 1 );
        ASSERT_TRUE( EvaluatePose( sequence, table, at, pose ).IsSuccess() );
        const BoneTransform expected = spine.Sample( at, clip.TickRate );
        EXPECT_EQ( pose[0].Translation, expected.Translation ) << at.AsTicks();
        EXPECT_EQ( pose[0].Rotation, expected.Rotation ) << at.AsTicks();
        EXPECT_EQ( pose[0].Scale, expected.Scale ) << at.AsTicks();
    }

    int curveTracks = 0;
    for ( const Track& track : sequence.Tracks )
    {
        const Binding* binding = FindBinding( sequence, track.Binding );
        ASSERT_NE( binding, nullptr );
        curveTracks += ( track.Kind == TrackKind::Float && track.Property == "Footstep" &&
                         binding->Kind == BindingKind::Sequence )
                            ? 1
                            : 0;
    }
    EXPECT_EQ( curveTracks, 1 ) << "a named curve is a Float track on the sequence binding";
    int eventTracks = 0;
    for ( const Track& track : sequence.Tracks )
    {
        eventTracks += track.Kind == TrackKind::Event ? 1 : 0;
    }
    EXPECT_EQ( eventTracks, 1 ) << "notifies become ONE event track, rows kept on the keys";
}

TEST( TimelineLiftClip, TwoTracksForOneBoneAreRefusedNotMerged )
{
    AnimationClip clip;
    clip.DurationTicks = Tick( 10 );
    BoneTrack a;
    a.BoneName = "Hand_L";
    clip.Tracks.push_back( a );
    clip.Tracks.push_back( a );
    EXPECT_FALSE( LiftClip( clip ).IsSuccess() );
}

// ── 9. AnimGraph layer nodes ────────────────────────────────────────────────────────────────────────

namespace
{
    // root(0) ─ spine(1) ─ chest(2) ─ head(3)
    //        └ leg(4)
    Skeleton FiveBones()
    {
        std::vector<BoneInfo> bones( 5 );
        const char*           names[]   = { "root", "spine", "chest", "head", "leg" };
        const int             parents[] = { -1, 0, 1, 2, 0 };
        for ( size_t i = 0; i < bones.size(); ++i )
        {
            bones[i].Name = names[i];
            if ( parents[i] >= 0 )
            {
                bones[i].ParentBoneID = static_cast<uint32_t>( parents[i] );
            }
        }
        return Skeleton( std::move( bones ) );
    }
} // namespace

TEST( LayeredBlendPerBone, DepthZeroIsTheWholeBranchAtFullWeight )
{
    G::LayeredBlendPerBoneNode node;
    node.Layers        = { G::LayerSetup{ { G::BranchFilter{ "spine", 0 } } } };
    const auto weights = G::BuildPerBoneWeights( node, FiveBones() );
    ASSERT_TRUE( weights.IsSuccess() ) << weights.GetError();
    const auto& w = weights.GetValue();
    ASSERT_EQ( w.size(), 5U );
    EXPECT_EQ( w[0].Layer, -1 );
    EXPECT_EQ( w[1].Weight, 1.0F );
    EXPECT_EQ( w[3].Weight, 1.0F );
    EXPECT_EQ( w[4].Layer, -1 ) << "the leg is not under the spine";
}

TEST( LayeredBlendPerBone, APositiveDepthRampsAndANegativeOneExcludes )
{
    G::LayeredBlendPerBoneNode ramp;
    ramp.Layers  = { G::LayerSetup{ { G::BranchFilter{ "spine", 2 } } } };
    const auto r = G::BuildPerBoneWeights( ramp, FiveBones() );
    ASSERT_TRUE( r.IsSuccess() );
    EXPECT_FLOAT_EQ( r.GetValue()[1].Weight, 0.5F );
    EXPECT_FLOAT_EQ( r.GetValue()[2].Weight, 1.0F );

    G::LayeredBlendPerBoneNode exclude;
    exclude.Layers = { G::LayerSetup{ { G::BranchFilter{ "spine", 0 }, G::BranchFilter{ "head", -1 } } } };
    const auto e   = G::BuildPerBoneWeights( exclude, FiveBones() );
    ASSERT_TRUE( e.IsSuccess() );
    EXPECT_EQ( e.GetValue()[2].Weight, 1.0F );
    EXPECT_EQ( e.GetValue()[3].Weight, 0.0F );
}

TEST( LayeredBlendPerBone, AnUnknownFilterBoneIsRefusedByName )
{
    G::LayeredBlendPerBoneNode node;
    node.Layers        = { G::LayerSetup{ { G::BranchFilter{ "spien", 0 } } } };
    const auto weights = G::BuildPerBoneWeights( node, FiveBones() );
    ASSERT_FALSE( weights.IsSuccess() );
    EXPECT_NE( weights.GetError().find( "spien" ), std::string::npos );
}

TEST( LayeredBlendPerBone, ZeroLayerWeightIsTheBaseBitForBitAndSizesMustAgree )
{
    const Skeleton             skeleton = FiveBones();
    G::LayeredBlendPerBoneNode node;
    node.Layers        = { G::LayerSetup{ { G::BranchFilter{ "spine", 0 } } } };
    const auto weights = G::BuildPerBoneWeights( node, skeleton );
    ASSERT_TRUE( weights.IsSuccess() );

    G::GraphPose base;
    base.Pose                = LocalPose( 5 );
    base.Pose[2].Translation = glm::vec3( 1.25F, 0.5F, 0.0F );
    G::GraphPose layer;
    layer.Pose                = LocalPose( 5 );
    layer.Pose[2].Translation = glm::vec3( 9.0F );

    const G::GraphPose layers[] = { layer };
    const float        zero[]   = { 0.0F };
    G::GraphPose       out;
    ASSERT_TRUE(
         G::BlendLayeredPerBone( node, weights.GetValue(), skeleton, base, layers, zero, out ).IsSuccess() );
    EXPECT_EQ( out.Pose[2].Translation, base.Pose[2].Translation );

    EXPECT_FALSE( G::BlendLayeredPerBone( node, weights.GetValue(), skeleton, base, {}, {}, out ).IsSuccess() );
}

TEST( LinkedAnimLayer, UnlinkedIsTheDefaultAndAHalfImplementationIsRefused )
{
    G::AnimLayerInterface locomotion;
    locomotion.Guid      = AssetGuid{ 7, 1 };
    locomotion.Functions = { G::AnimLayerFunction{ "FullBody", { "In" }, "" },
                             G::AnimLayerFunction{ "UpperBody", { "In" }, "" } };
    G::LinkedLayerTable          table;
    const G::LinkedAnimLayerNode node{ locomotion.Guid, "UpperBody" };
    EXPECT_TRUE( table.Resolve( node ).IsNull() );

    const G::LayerImplementation half{ AssetGuid{ 8, 1 }, locomotion.Guid, { "FullBody" } };
    const auto                   refused = table.Link( locomotion, half );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "UpperBody" ), std::string::npos );

    const G::LayerImplementation other{ AssetGuid{ 8, 2 }, AssetGuid{ 6, 6 }, { "FullBody", "UpperBody" } };
    EXPECT_FALSE( table.Link( locomotion, other ).IsSuccess() ) << "implements another interface";

    const G::LayerImplementation rifle{ AssetGuid{ 8, 3 }, locomotion.Guid, { "FullBody", "UpperBody" } };
    ASSERT_TRUE( table.Link( locomotion, rifle ).IsSuccess() );
    EXPECT_EQ( table.Resolve( node ), rifle.Graph );

    table.Unlink( locomotion.Guid );
    EXPECT_TRUE( table.Resolve( node ).IsNull() );
}

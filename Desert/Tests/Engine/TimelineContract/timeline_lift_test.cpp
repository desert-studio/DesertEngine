// Group 8 of the timeline contract (timeline_contract_test.cpp): LiftClip + the bone fast path (ANIM-I7).

#include "TimelineFixtures.hpp"

#include "ClipGeneration3.hpp" // generation 3 and LiftClip live in Tools/SceneMigrator since ANIM-I8a
#include "ClipInterpShift.hpp"

#include <array>
#include <stdexcept>
#include <utility> // ANIM v5 -> v6: key modes move to the segment leaving the key

using namespace TimelineFixtures;
namespace Gen3 = Desert::Migration::ClipGen3;

// ── 8. LiftClip: the .anim migration is the identity ────────────────────────────────────────────────

TEST( TimelineLiftClip, EveryBoneSamplesBitForBitAfterTheLift )
{
    Gen3::AnimationClip clip;
    clip.AnimationName = "Walk";
    clip.DurationTicks = Tick( 60 );
    Gen3::BoneTrack spine;
    spine.BoneName     = "Spine";
    spine.PositionKeys = { Gen3::PositionKeyFrame{ Tick( 0 ), glm::vec3( 0, 1, 0 ) },
                           Gen3::PositionKeyFrame{ Tick( 60 ), glm::vec3( 3, 1, -2 ) } };
    spine.RotationKeys = { Gen3::RotationKeyFrame{ Tick( 0 ), glm::quat( 1, 0, 0, 0 ) },
                           Gen3::RotationKeyFrame{ Tick( 60 ), glm::angleAxis( 1.1F, glm::vec3( 0, 0, 1 ) ) } };
    clip.Tracks.push_back( spine );
    clip.Curves.push_back( Gen3::AnimationCurve{ "Footstep", { Key( 0, 0.0F ), Key( 60, 1.0F ) } } );
    clip.Notifies.push_back( Gen3::AnimationNotify{ "Step", Tick( 30 ), 2, Tick( 0 ) } );

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
    Gen3::AnimationClip clip;
    clip.DurationTicks = Tick( 10 );
    Gen3::BoneTrack a;
    a.BoneName = "Hand_L";
    clip.Tracks.push_back( a );
    clip.Tracks.push_back( a );
    EXPECT_FALSE( LiftClip( clip ).IsSuccess() );
}

// Gaps, overlaps and a partial weight: generation 3's "the later speaking section wins, none = authored" is cut
// into non-overlapping runs, so the fold applies exactly the one section SampleTrack applied, on every tick.
TEST( TimelineLiftClip, SectionsLiftByWinnerOnEveryTick )
{
    Gen3::AnimationClip clip;
    clip.DurationTicks = Tick( 40 );
    Gen3::BoneTrack arm;
    arm.BoneName     = "Arm";
    arm.PositionKeys = { Gen3::PositionKeyFrame{ Tick( 0 ), glm::vec3( 0, 1, 0 ) },
                         Gen3::PositionKeyFrame{ Tick( 40 ), glm::vec3( 3, 1, -2 ) } };
    arm.RotationKeys = { Gen3::RotationKeyFrame{ Tick( 0 ), glm::quat( 1, 0, 0, 0 ) },
                         Gen3::RotationKeyFrame{ Tick( 40 ), glm::angleAxis( 1.1F, glm::vec3( 0, 0, 1 ) ) } };
    arm.ScaleKeys    = { Gen3::ScaleKeyFrame{ Tick( 0 ), glm::vec3( 1 ) }, Gen3::ScaleKeyFrame{ Tick( 40 ), glm::vec3( 2 ) } };
    clip.Tracks.push_back( arm );
    Gen3::ClipSection half;
    half.Name   = "Half";
    half.Start  = Tick( 5 );
    half.End    = Tick( 25 );
    half.Weight = { Key( 5, 0.25F ), Key( 25, 0.75F ) };
    Gen3::ClipSection add;
    add.Name  = "Add";
    add.Start = Tick( 20 );
    add.End   = Tick( 30 );
    add.Blend = SectionBlendType::Additive;
    add.Weight = { Key( 20, 0.5F ) };
    clip.Sections = { half, add };

    const auto lifted = LiftClip( clip );
    ASSERT_TRUE( lifted.IsSuccess() ) << lifted.GetError();
    const Sequence& sequence = lifted.GetValue();
    std::vector<BoneInfo> bones( 1 );
    bones[0].Name = "Arm";
    const Skeleton         skeleton( std::move( bones ) );
    const BoneBindingTable table = BindBones( sequence, skeleton );
    BoneTransform reference;
    reference.Translation = glm::vec3( 0.5F, -1, 2 );
    reference.Rotation    = glm::angleAxis( 0.3F, glm::vec3( 1, 0, 0 ) );
    for ( int32_t tick = 0; tick <= 40; ++tick )
    {
        LocalPose pose( 1 );
        pose[0] = reference;
        ASSERT_TRUE( EvaluatePose( sequence, table, At( tick ), pose ).IsSuccess() );
        const BoneTransform expected = clip.SampleTrack( arm, At( tick ), reference );
        EXPECT_EQ( pose[0].Translation, expected.Translation ) << tick;
        EXPECT_EQ( pose[0].Rotation, expected.Rotation ) << tick;
        EXPECT_EQ( pose[0].Scale, expected.Scale ) << tick;
    }
}

TEST( TimelineLiftClip, AStaleBoneTableIsRefusedByName )
{
    Gen3::AnimationClip clip;
    clip.DurationTicks = Tick( 10 );
    Gen3::BoneTrack a;
    a.BoneName = "Hand_L";
    clip.Tracks.push_back( a );
    auto lifted = LiftClip( clip );
    ASSERT_TRUE( lifted.IsSuccess() );
    Sequence               sequence = lifted.ExtractValue();
    std::vector<BoneInfo>  bones( 1 );
    bones[0].Name = "Hand_L";
    const Skeleton         skeleton( std::move( bones ) );
    const BoneBindingTable table = BindBones( sequence, skeleton );
    ++sequence.Revision;
    LocalPose  pose( 1 );
    const auto result = EvaluatePose( sequence, table, At( 0 ), pose );
    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( "BindBones" ), std::string::npos );
}

// ── 8b. UE's interpolation rule (ANIM-I8b-6): a key's mode shapes the segment LEAVING it ─────────────

namespace
{
    ScalarKey ModeKey( int32_t tick, float value, KeyInterp interp )
    {
        ScalarKey key;
        key.Tick   = Tick( tick );
        key.Value  = value;
        key.Interp = interp;
        return key;
    }

    /// Generation 3 with every mode kind on one bone's position X, in the ARRIVING rule it was authored in.
    Gen3::AnimationClip MixedModeClip()
    {
        Gen3::AnimationClip clip;
        clip.AnimationName = "Mixed";
        clip.DurationTicks = Tick( 40 );
        Gen3::BoneTrack bone;
        bone.BoneName = "Hips";
        const std::array<std::pair<int32_t, KeyInterp>, 5> keys{ { { 0, KeyInterp::Linear },
                                                                   { 10, KeyInterp::Constant },
                                                                   { 20, KeyInterp::Linear },
                                                                   { 30, KeyInterp::Cubic },
                                                                   { 40, KeyInterp::Constant } } };
        float value = 0.0F;
        for ( const auto& [tick, interp] : keys )
        {
            Gen3::PositionKeyFrame key{ Tick( tick ), glm::vec3( value, 0, 0 ) };
            key.Interp = interp;
            bone.PositionKeys.push_back( key );
            value += 10.0F;
        }
        clip.Tracks.push_back( bone );
        return clip;
    }

    TransformChannel& BoneChannel( Sequence& sequence )
    {
        for ( Track& track : sequence.Tracks )
        {
            if ( track.Kind == TrackKind::Transform )
            {
                return std::get<TransformChannel>( std::get<Channel>( track.Sections.front().Content ) );
            }
        }
        throw std::runtime_error( "no bone track" );
    }
} // namespace

TEST( TimelineInterpRule, AConstantKeyHoldsItsOwnValueUntilTheNextKey )
{
    const FloatChannel channel{ { ModeKey( 0, 10.0F, KeyInterp::Constant ), ModeKey( 10, 20.0F, KeyInterp::Linear ),
                                  ModeKey( 20, 40.0F, KeyInterp::Linear ) },
                                0.0F };
    const FrameRate rate = Sequence{}.TickRate;
    EXPECT_EQ( Evaluate( channel, At( 0 ), rate ), 10.0F );
    EXPECT_EQ( Evaluate( channel, At( 5 ), rate ), 10.0F ) << "key 0 is Constant: its value holds to key 1";
    EXPECT_EQ( Evaluate( channel, At( 9, 0.99F ), rate ), 10.0F );
    EXPECT_EQ( Evaluate( channel, At( 10 ), rate ), 20.0F ) << "a key's own tick reads that key";
    // The segment AFTER the Constant key is key 1's: Linear.
    EXPECT_EQ( Evaluate( channel, At( 15 ), rate ), 30.0F ) << "the Linear segment leaving key 1";
    EXPECT_EQ( Evaluate( channel, At( 20 ), rate ), 40.0F );
}

TEST( TimelineInterpRule, TheLiftShiftsGenerationThreeModesOneKeyBackAndSamplesAsBefore )
{
    const Gen3::AnimationClip clip   = MixedModeClip();
    auto                      lifted = LiftClip( clip );
    ASSERT_TRUE( lifted.IsSuccess() ) << lifted.GetError();
    Sequence&              sequence = lifted.GetValue();
    const TransformChannel& channel = BoneChannel( sequence );
    const auto&             x       = channel.Translation.X.Keys;
    ASSERT_EQ( x.size(), 5U );
    EXPECT_EQ( x[0].Interp, KeyInterp::Constant ) << "the segment 0 -> 10 was stated on key 10";
    EXPECT_EQ( x[1].Interp, KeyInterp::Linear );
    EXPECT_EQ( x[2].Interp, KeyInterp::Cubic );
    EXPECT_EQ( x[3].Interp, KeyInterp::Constant );

    std::vector<BoneInfo> bones( 1 );
    bones[0].Name = "Hips";
    const Skeleton         skeleton( std::move( bones ) );
    const BoneBindingTable table = BindBones( sequence, skeleton );
    for ( int32_t tick = 0; tick <= 40; ++tick )
    {
        LocalPose pose( 1 );
        ASSERT_TRUE( EvaluatePose( sequence, table, At( tick ), pose ).IsSuccess() );
        EXPECT_EQ( pose[0].Translation, clip.Tracks[0].Sample( At( tick ), clip.TickRate ).Translation )
             << "tick " << tick;
    }
}

TEST( TimelineInterpRule, AMigratedV5ClipSamplesAsBeforeOnEveryTick )
{
    // The v5 sequence: the lift's keys with the ARRIVING modes generation 3 stated (what v5 wrote).
    const Gen3::AnimationClip clip   = MixedModeClip();
    auto                      lifted = LiftClip( clip );
    ASSERT_TRUE( lifted.IsSuccess() ) << lifted.GetError();
    Sequence v5 = lifted.GetValue();
    for ( std::size_t i = 0; i < clip.Tracks[0].PositionKeys.size(); ++i )
    {
        TransformChannel& channel = BoneChannel( v5 );
        for ( FloatChannel* c : { &channel.Translation.X, &channel.Translation.Y, &channel.Translation.Z } )
        {
            c->Keys[i].Interp = clip.Tracks[0].PositionKeys[i].Interp;
        }
    }

    // THE PROOF HAS TEETH: the v5 modes read by the v6 rule do not sample as v5 did.
    EXPECT_FALSE( Desert::Migration::VerifyInterpShift( v5, v5 ).IsSuccess() );

    Sequence v6 = v5;
    EXPECT_GT( Desert::Migration::ShiftInterpToLeavingKey( v6 ), 0U );
    const auto proved = Desert::Migration::VerifyInterpShift( v5, v6 );
    ASSERT_TRUE( proved.IsSuccess() ) << proved.GetError();
    EXPECT_GE( proved.GetValue(), 41U * 3U ) << "every tick of X, Y and Z";
    EXPECT_EQ( BoneChannel( v6 ).Translation.X.Keys[0].Interp, KeyInterp::Constant );
}

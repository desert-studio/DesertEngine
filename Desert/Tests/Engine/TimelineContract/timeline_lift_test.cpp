// Group 8 of the timeline contract (timeline_contract_test.cpp): LiftClip + the bone fast path (ANIM-I7).

#include "TimelineFixtures.hpp"

#include "ClipGeneration3.hpp" // generation 3 and LiftClip live in Tools/SceneMigrator since ANIM-I8a

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

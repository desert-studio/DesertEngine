// THE TIMELINE CONTRACT, keying (ANIM-I8b): a bone key is edited IN the clip's Sequence (TrackEditing's
// sequence edits — UE's IAnimationDataController over IAnimationDataModel) and the proof is always the
// same one playback uses: `BindBones` + `EvaluatePose` sees the edit. A key that only a key list sees is
// not a key.

#include "TimelineFixtures.hpp"

#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Rig/ControlKeyer.hpp>
#include <Engine/Animation/TrackEditing.hpp>

using namespace TimelineFixtures;

namespace
{
    Skeleton TwoBones()
    {
        std::vector<BoneInfo> bones( 2 );
        bones[0].Name         = "root";
        bones[1].Name         = "chest";
        bones[1].ParentBoneID = 0;
        return Skeleton( std::move( bones ) );
    }

    AnimationClip ClipOf( int32_t lastTick )
    {
        AnimationClip clip;
        clip.AnimationName  = "take";
        clip.Sequence.Start = Tick( 0 );
        clip.Sequence.End   = Tick( lastTick );
        return clip;
    }

    BoneTransform PoseOf( const glm::vec3& translation, float angle, const glm::vec3& scale )
    {
        BoneTransform out;
        out.Translation = translation;
        out.Rotation    = glm::angleAxis( angle, glm::vec3( 0.0F, 1.0F, 0.0F ) );
        out.Scale       = scale;
        return out;
    }

    /// What playback shows for bone 1 at @p tick — the Animator's path, not a channel read.
    BoneTransform Played( const AnimationClip& clip, const Skeleton& skeleton, int32_t tick )
    {
        const BoneBindingTable table = BindBones( clip.Sequence, skeleton );
        LocalPose              pose;
        pose.Resize( 2 );
        const auto evaluated = EvaluatePose( clip.Sequence, table, At( tick ), pose );
        EXPECT_TRUE( evaluated.IsSuccess() ) << evaluated.GetError();
        return pose[1];
    }

    void ExpectSame( const BoneTransform& a, const BoneTransform& b, float tolerance = 0.0F )
    {
        for ( int i = 0; i < 3; ++i )
        {
            EXPECT_NEAR( a.Translation[i], b.Translation[i], tolerance );
            EXPECT_NEAR( a.Scale[i], b.Scale[i], tolerance );
        }
        EXPECT_NEAR( std::abs( glm::dot( a.Rotation, b.Rotation ) ), 1.0F, tolerance + 1e-6F );
    }
} // namespace

TEST( TimelineKeying, AnInsertedBoneKeyIsWhatEvaluatePoseShows )
{
    const Skeleton      skeleton = TwoBones();
    AnimationClip       clip     = ClipOf( 100 );
    const BoneTransform key      = PoseOf( glm::vec3( 3.0F, -4.0F, 5.0F ), 0.7F, glm::vec3( 2.0F, 0.5F, 1.25F ) );
    const uint32_t      before   = clip.Sequence.Revision;

    const auto keyed = SetBoneKey( clip.Sequence, "chest", Tick( 40 ), key );
    ASSERT_TRUE( keyed.IsSuccess() ) << keyed.GetError();

    EXPECT_GT( clip.Sequence.Revision, before )
         << "every edit bumps the revision the Animator's binding is keyed by";
    ASSERT_EQ( clip.Sequence.Bindings.size(), 1U );
    EXPECT_EQ( clip.Sequence.Bindings[0].Kind, BindingKind::Bone );
    EXPECT_EQ( clip.Sequence.Bindings[0].Locator, "chest" );
    const Track* track = FindBoneTrack( std::as_const( clip.Sequence ), "chest" );
    ASSERT_NE( track, nullptr );
    ASSERT_EQ( track->Sections.size(), 1U );
    EXPECT_EQ( track->Sections[0].Blend, SectionBlendType::Absolute );
    EXPECT_EQ( track->Sections[0].Start, clip.Sequence.Start )
         << "no section covered the tick: one over the whole clip";
    EXPECT_EQ( track->Sections[0].End, clip.Sequence.End );
    EXPECT_TRUE( Validate( clip.Sequence ).IsSuccess() );

    // BIT-EXACT on the key's tick: nothing between the key and the pose may be arithmetic.
    const BoneTransform shown = Played( clip, skeleton, 40 );
    EXPECT_EQ( shown.Translation, key.Translation );
    EXPECT_EQ( shown.Rotation, key.Rotation );
    EXPECT_EQ( shown.Scale, key.Scale );
}

TEST( TimelineKeying, SecondKeyOnOneTickUpsertsAndInsertFromCurveMovesNothing )
{
    const Skeleton skeleton = TwoBones();
    AnimationClip  clip     = ClipOf( 100 );
    ASSERT_TRUE(
         SetBoneKey( clip.Sequence, "chest", Tick( 0 ), PoseOf( glm::vec3( 0.0F ), 0.0F, glm::vec3( 1.0F ) ) )
              .IsSuccess() );
    ASSERT_TRUE(
         SetBoneKey( clip.Sequence, "chest", Tick( 100 ), PoseOf( glm::vec3( 9.0F ), 0.5F, glm::vec3( 1.0F ) ) )
              .IsSuccess() );
    const BoneTransform target = PoseOf( glm::vec3( 10.0F, 20.0F, 30.0F ), 1.0F, glm::vec3( 1.0F ) );
    ASSERT_TRUE( SetBoneKey( clip.Sequence, "chest", Tick( 100 ), target ).IsSuccess() );

    const Track* track = FindBoneTrack( std::as_const( clip.Sequence ), "chest" );
    ASSERT_NE( track, nullptr );
    const auto& channel = std::get<TransformChannel>( std::get<Channel>( track->Sections[0].Content ) );
    EXPECT_EQ( channel.Translation.X.Keys.size(), 2U )
         << "a key on an occupied tick is an upsert, not a second key";
    ExpectSame( Played( clip, skeleton, 100 ), target );

    const BoneTransform midBefore = Played( clip, skeleton, 37 );
    const uint32_t      revision  = clip.Sequence.Revision;
    const auto          inserted  = InsertBoneKey( clip.Sequence, "chest", TrackChannel::Position, Tick( 37 ) );
    ASSERT_TRUE( inserted.IsSuccess() ) << inserted.GetError();
    EXPECT_GT( clip.Sequence.Revision, revision );
    EXPECT_EQ( channel.Translation.X.Keys.size(), 3U );
    ExpectSame( Played( clip, skeleton, 37 ), midBefore, 1e-4F );
    EXPECT_FALSE( InsertBoneKey( clip.Sequence, "chest", TrackChannel::Position, Tick( 37 ) ).IsSuccess() )
         << "an insert on an occupied tick is refused — the upsert is SetBoneKey";
}

TEST( TimelineKeying, RemovingAndMovingAKeyIsWhatPlaybackShows )
{
    const Skeleton      skeleton = TwoBones();
    AnimationClip       clip     = ClipOf( 100 );
    const BoneTransform first    = PoseOf( glm::vec3( 1.0F, 2.0F, 3.0F ), 0.0F, glm::vec3( 1.0F ) );
    const BoneTransform last     = PoseOf( glm::vec3( 7.0F, 8.0F, 9.0F ), 0.0F, glm::vec3( 1.0F ) );
    ASSERT_TRUE( SetBoneKey( clip.Sequence, "chest", Tick( 0 ), first ).IsSuccess() );
    ASSERT_TRUE( SetBoneKey( clip.Sequence, "chest", Tick( 100 ), last ).IsSuccess() );

    // MOVE 100 → 60: the value arrives at 60, and past it the curve is held flat.
    uint32_t   revision = clip.Sequence.Revision;
    const auto moved    = MoveBoneKey( clip.Sequence, "chest", TrackChannel::Position, Tick( 100 ), Tick( 60 ) );
    ASSERT_TRUE( moved.IsSuccess() ) << moved.GetError();
    EXPECT_GT( clip.Sequence.Revision, revision );
    EXPECT_EQ( Played( clip, skeleton, 60 ).Translation, last.Translation );
    EXPECT_EQ( Played( clip, skeleton, 90 ).Translation, last.Translation );
    EXPECT_FALSE(
         MoveBoneKey( clip.Sequence, "chest", TrackChannel::Position, Tick( 60 ), Tick( 0 ) ).IsSuccess() )
         << "a move onto an occupied tick would silently delete a key";

    // REMOVE 60: only the first key is left, so every tick shows it.
    revision           = clip.Sequence.Revision;
    const auto removed = RemoveBoneKey( clip.Sequence, "chest", TrackChannel::Position, Tick( 60 ) );
    ASSERT_TRUE( removed.IsSuccess() ) << removed.GetError();
    EXPECT_GT( clip.Sequence.Revision, revision );
    EXPECT_EQ( Played( clip, skeleton, 60 ).Translation, first.Translation );
    EXPECT_EQ( Played( clip, skeleton, 100 ).Translation, first.Translation );
    EXPECT_FALSE( RemoveBoneKey( clip.Sequence, "chest", TrackChannel::Position, Tick( 60 ) ).IsSuccess() );
}

TEST( TimelineKeying, AKeyOutsideEverySectionFillsTheGapAndHidesNothing )
{
    const Skeleton skeleton = TwoBones();
    AnimationClip  clip     = ClipOf( 100 );
    Track&         track    = AddBoneTrack( clip.Sequence, "chest" );
    Section&       authored = AddSection( track, Tick( 0 ), Tick( 50 ) );
    auto&          channel  = std::get<TransformChannel>( std::get<Channel>( authored.Content ) );
    channel.Translation.X.Keys.push_back( Key( 0, 5.0F ) );

    ASSERT_TRUE(
         SetBoneKey( clip.Sequence, "chest", Tick( 80 ), PoseOf( glm::vec3( -3.0F ), 0.0F, glm::vec3( 1.0F ) ) )
              .IsSuccess() );
    const Track* after = FindBoneTrack( std::as_const( clip.Sequence ), "chest" );
    ASSERT_NE( after, nullptr );
    ASSERT_EQ( after->Sections.size(), 2U );
    EXPECT_EQ( after->Sections[1].Start.Value, 51 ) << "the new section fills the gap after the authored one";
    EXPECT_EQ( after->Sections[1].End.Value, 100 ) << "and stops at the playback range";
    EXPECT_GE( after->Sections[1].Row, 0 ) << "rows are never negative (UE's row index)";
    EXPECT_EQ( Played( clip, skeleton, 20 ).Translation.x, 5.0F )
         << "the authored section still wins where it covers";
    EXPECT_EQ( Played( clip, skeleton, 80 ).Translation.x, -3.0F );

    EXPECT_FALSE( SetBoneKey( clip.Sequence, "chest", Tick( 101 ), BoneTransform{} ).IsSuccess() )
         << "a key outside the clip is never sampled; lengthening the clip is a separate edit";
}

TEST( TimelineKeying, AControlKeyAndABoneKeyLandInTheClipSequence )
{
    const Skeleton skeleton = TwoBones();
    AnimationClip  clip     = ClipOf( 240 );
    LocalPose      local;
    local.Resize( 2 );
    local[1].Translation = glm::vec3( 0.0F, 100.0F, 0.0F );
    ComponentPose    component{ skeleton, local };
    ControlHierarchy rig;
    ControlElement   hand;
    hand.Name        = "hand_ctrl";
    hand.Parents     = { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F } };
    const auto added = rig.Add( hand );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    rig.Evaluate( skeleton, component );

    LocalPose authoring      = local;
    authoring[1].Translation = glm::vec3( 4.0F, 5.0F, 6.0F );

    ControlKeyTarget target;
    target.Hierarchy    = &rig;
    target.Skeleton     = &skeleton;
    target.Clip         = &clip;
    target.Tick         = Tick( 96 );
    target.AuthoredPose = &authoring;

    ControlKeyer        keyer;
    const BoneTransform pose    = PoseOf( glm::vec3( 3.0F, -4.0F, 5.0F ), 0.7F, glm::vec3( 2.0F, 0.5F, 1.25F ) );
    const auto          written = keyer.Write( target, added.GetValue(), pose, ControlWriteSource::Authored );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    EXPECT_EQ( written.GetValue(), 1U );

    // The control's key: the Transform track of a Bone binding named after the control, folded value = pose.
    const Track* controlTrack = FindBoneTrack( std::as_const( clip.Sequence ), "hand_ctrl" );
    ASSERT_NE( controlTrack, nullptr )
         << "a control's keys live in the clip's Sequence, on a binding named after it";
    EvaluatedValue value;
    ASSERT_TRUE( EvaluateTrack( *controlTrack, At( 96 ), clip.Sequence.TickRate, value ) );
    EXPECT_EQ( std::get<BoneTransform>( value ).Translation, pose.Translation );
    EXPECT_EQ( std::get<BoneTransform>( value ).Rotation, pose.Rotation );
    EXPECT_EQ( std::get<BoneTransform>( value ).Scale, pose.Scale );

    // The bone's key: read from the authoring pose, and playback on the rig shows it.
    const auto bone = keyer.WriteBone( target, 1 );
    ASSERT_TRUE( bone.IsSuccess() ) << bone.GetError();
    EXPECT_EQ( Played( clip, skeleton, 96 ).Translation, authoring[1].Translation );
    EXPECT_TRUE( Validate( clip.Sequence ).IsSuccess() );
}

// A SECTION: WHAT IT CHANGES ABOUT A CLIP, AND THE TWO THINGS THAT MUST NOT CHANGE.
//
// Report 05 §938, on the clip as it is since ANIM-I8b: ONE Timeline::Sequence whose bone tracks each hold
// their own sections (UE's model — a section belongs to one track and carries that track's keys). The hard
// part is not the arithmetic, it is that the arithmetic must be INVISIBLE to everything already in the
// tree: every `.anim` in the repository lifted to one Absolute section at full weight per track, so if that
// case is not the identity — bit for bit, not "about equal" — then the lift silently moved every key in
// the corpus.
//
// Every sample here is taken the way playback takes it: `BindBones` + `EvaluatePose` over a pose seeded
// with the rest (reference) pose, and the clip is built by the engine's own edit (`SetBoneKey`) or by the
// shared ClipFixture. A value only a channel read sees is not what the character shows.
//
//   1. THE IDENTITY. A track under one full-weight Absolute section must play EQUAL floats to its channel.
//      `mix( a, b, 1.0F )` is `a + 1.0F * ( b - a )` and is NOT `b`, which is exactly the kind of
//      difference a tolerance hides and a corpus remembers.
//   2. THE TWO BLEND TYPES ARE DIFFERENT. `Absolute` and `Additive` are asserted to disagree on the one
//      value where the difference matters most: identity.
//   3. The weight channel, the range, and which section wins an overlap (the higher Row).
//
// §972's warning is asserted as ARITHMETIC rather than quoted: a half-weighted section is half of the
// VALUE, so a control at 90 degrees under a 0.5 section reads 45 degrees — not the pose halfway between
// two evaluated rigs. The test that says so is `AHalfWeightIsHalfTheVALUEAndNotHalfAPoseBlend`.

#include "../ClipFixture.hpp"

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Animation/TrackEditing.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using Desert::Animation::AnimationClip;
using Desert::Animation::BoneInfo;
using Desert::Animation::BoneTransform;
using Desert::Animation::FindBoneTrack;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameTime;
using Desert::Animation::KeyInterp;
using Desert::Animation::LocalPose;
using Desert::Animation::PROJECT_TICK_RATE;
using Desert::Animation::ScalarKey;
using Desert::Animation::SetBoneKey;
using Desert::Animation::Skeleton;
using Desert::Animation::Timeline::AddSection;
using Desert::Animation::Timeline::BindBones;
using Desert::Animation::Timeline::BoneBindingTable;
using Desert::Animation::Timeline::Channel;
using Desert::Animation::Timeline::ClearSectionWeight;
using Desert::Animation::Timeline::EvaluatePose;
using Desert::Animation::Timeline::MoveSection;
using Desert::Animation::Timeline::RemoveSection;
using Desert::Animation::Timeline::RemoveSectionWeightKey;
using Desert::Animation::Timeline::Section;
using Desert::Animation::Timeline::SectionBlendType;
using Desert::Animation::Timeline::SetSectionRange;
using Desert::Animation::Timeline::SetSectionRow;
using Desert::Animation::Timeline::SetSectionWeightKey;
using Desert::Animation::Timeline::Track;
using Desert::Animation::Timeline::TrackKind;
using Desert::Animation::Timeline::TransformChannel;
using Desert::Animation::Timeline::WeightAt;

namespace
{
    constexpr int kDuration = 48000; // two seconds on the project's 24000-tick grid

    BoneTransform TransformOf( const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale )
    {
        BoneTransform out;
        out.Translation = translation;
        out.Rotation    = rotation;
        out.Scale       = scale;
        return out;
    }

    FrameTime At( int tick )
    {
        return FrameTime{ FrameNumber{ tick }, 0.0F };
    }

    AnimationClip EmptyClip()
    {
        return ClipFixture::Clip( "take", FrameNumber{ kDuration } );
    }

    /// A track that MOVES, and not by a rounding-sized amount: a clip whose keys are all the reference value
    /// would let every blend in this file pass while doing nothing. Keyed by the engine's own edit, which
    /// creates the binding, the track and its one Absolute full-weight section over the clip.
    AnimationClip MovingClip( const std::string& bone )
    {
        AnimationClip clip  = EmptyClip();
        const auto    first = SetBoneKey(
             clip.Sequence, bone, FrameNumber{ 0 },
             TransformOf( glm::vec3( 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) ) );
        EXPECT_TRUE( first.IsSuccess() ) << first.GetError();
        const auto last = SetBoneKey( clip.Sequence, bone, FrameNumber{ kDuration },
                                      TransformOf( glm::vec3( 100.0F, 0.0F, 0.0F ), // one metre, in centimetres
                                                   glm::angleAxis( glm::radians( 90.0F ), glm::vec3( 0, 0, 1 ) ),
                                                   glm::vec3( 3.0F, 1.0F, 1.0F ) ) );
        EXPECT_TRUE( last.IsSuccess() ) << last.GetError();
        return clip;
    }

    /// A clip holding @p bone at @p value for its whole range (ClipFixture: one section, Absolute, weight 1).
    AnimationClip StaticClip( const std::string& bone, const BoneTransform& value )
    {
        AnimationClip clip = EmptyClip();
        (void)ClipFixture::AddStaticBone( clip, bone, value.Translation, value.Rotation, value.Scale );
        return clip;
    }

    Track& TrackOf( AnimationClip& clip, const std::string& bone )
    {
        Track* track = FindBoneTrack( clip.Sequence, bone );
        EXPECT_NE( track, nullptr ) << bone;
        return *track;
    }

    const TransformChannel& ChannelOf( const Section& section )
    {
        return std::get<TransformChannel>( std::get<Channel>( section.Content ) );
    }

    /// The authored curve of the track's first section at @p at — the value UNDER the section blend.
    BoneTransform Curve( AnimationClip& clip, const std::string& bone, FrameTime at )
    {
        return Desert::Animation::Timeline::Evaluate( ChannelOf( TrackOf( clip, bone ).Sections.front() ), at,
                                                      clip.Sequence.TickRate );
    }

    /// A rest pose that is NOT the identity. An identity reference makes `Absolute` and `Additive` agree on
    /// translation and lets a wrong composition order pass every assertion here.
    BoneTransform Rest()
    {
        return TransformOf( glm::vec3( 0.0F, 50.0F, 0.0F ),
                            glm::angleAxis( glm::radians( 30.0F ), glm::vec3( 0.0F, 0.0F, 1.0F ) ),
                            glm::vec3( 2.0F, 2.0F, 2.0F ) );
    }

    /// What playback shows for @p bone at @p at when the pose underneath is @p reference — the Animator's path.
    BoneTransform Played( const AnimationClip& clip, const std::string& bone, FrameTime at,
                          const BoneTransform& reference )
    {
        std::vector<BoneInfo> bones( 1 );
        bones[0].Name = bone;
        const Skeleton         skeleton( std::move( bones ) );
        const BoneBindingTable table = BindBones( clip.Sequence, skeleton );
        LocalPose              pose;
        pose.Resize( 1 );
        pose[0]              = reference;
        const auto evaluated = EvaluatePose( clip.Sequence, table, at, pose );
        EXPECT_TRUE( evaluated.IsSuccess() ) << evaluated.GetError();
        return pose[0];
    }

    ScalarKey WeightKey( int tick, float value )
    {
        ScalarKey key;
        key.Tick   = FrameNumber{ tick };
        key.Value  = value;
        key.Interp = KeyInterp::Linear;
        return key;
    }

    void ExpectExactlyEqual( const BoneTransform& got, const BoneTransform& want, const char* what )
    {
        EXPECT_EQ( got.Translation, want.Translation ) << what;
        EXPECT_EQ( got.Rotation, want.Rotation ) << what;
        EXPECT_EQ( got.Scale, want.Scale ) << what;
    }
} // namespace

// ── 1. THE IDENTITY, AND IT IS BIT-EXACT ─────────────────────────────────────────────────────────────

TEST( ClipSections, AFullWeightAbsoluteSectionIsTheIDENTITYOfTheBlendBitForBit )
{
    // THE ASSERTION THE WHOLE LIFT RESTS ON. Every clip in the repository lifted to exactly this section, so
    // any arithmetic here — including the `a + 1*(b-a)` that a `mix` would do — moves the whole corpus by an
    // amount no "near" comparison could see.
    AnimationClip clip = MovingClip( "arm" );
    ASSERT_EQ( TrackOf( clip, "arm" ).Sections.size(), 1U );
    const Section& section = TrackOf( clip, "arm" ).Sections.front();
    ASSERT_EQ( section.Blend, SectionBlendType::Absolute );
    ASSERT_TRUE( section.Weight.empty() );

    for ( const int tick : { 0, 1, 9999, kDuration / 3, kDuration / 2, kDuration - 1, kDuration } )
    {
        ExpectExactlyEqual( Played( clip, "arm", At( tick ), Rest() ), Curve( clip, "arm", At( tick ) ),
                            "a full-weight Absolute section must play the curve itself" );
    }
}

TEST( ClipSections, AFullWeightAbsoluteSectionIsExactWHERETHEARITHMETICWOULDNOTBE )
{
    // THE TEST ABOVE WAS DEGENERATE AND A MUTATION SAID SO (§8.4). Deleting the `w == 1` branch from the
    // fold left it GREEN, because its numbers — a rest of 0 and 50, a curve of 0 and 100 — are values for
    // which `a + 1.0F * ( b - a )` happens to land on `b` exactly. Most do. These do not:
    // `0.3F + ( 0.1F - 0.3F )` is 0.099999994, and `1234.5678F + ( 0.0001F - 1234.5678F )` is 0.00012207,
    // which is the authored value wrong by 22 %.
    AnimationClip clip =
         StaticClip( "arm", TransformOf( glm::vec3( 0.0001F, 0.1F, 0.25F ),
                                         glm::angleAxis( 0.37F, glm::normalize( glm::vec3( 0.3F, -0.8F, 0.5F ) ) ),
                                         glm::vec3( 0.0001F, 0.1F, 1.0F ) ) );

    const BoneTransform faraway =
         TransformOf( glm::vec3( 1234.5678F, 0.3F, -900.0F ),
                      glm::angleAxis( 2.9F, glm::normalize( glm::vec3( -0.9F, 0.2F, 0.35F ) ) ),
                      glm::vec3( 1234.5678F, 0.3F, 512.0F ) );

    ExpectExactlyEqual( Played( clip, "arm", At( 0 ), faraway ), Curve( clip, "arm", At( 0 ) ),
                        "full weight must RETURN the authored value, not recompute it" );
}

TEST( ClipSections, ASectionOnAnotherTrackLeavesThisOnePlayingAsAuthored )
{
    // A section is a statement about ITS track. One on the leg must leave the arm alone, and "alone" is the
    // arm's curve — not the rest pose, which is what a fold that let the leg's weight 0 leak across would
    // return, and which would look like a bone that simply stopped animating.
    AnimationClip clip = MovingClip( "arm" );
    ASSERT_TRUE( SetBoneKey( clip.Sequence, "leg", FrameNumber{ 0 }, Rest() ).IsSuccess() );
    Section& leg = TrackOf( clip, "leg" ).Sections.front();
    leg.Blend    = SectionBlendType::Additive;
    leg.Weight   = { WeightKey( 0, 0.0F ) };

    ExpectExactlyEqual( Played( clip, "arm", At( kDuration ), Rest() ), Curve( clip, "arm", At( kDuration ) ),
                        "a track the leg's section does not own is its curve" );
    EXPECT_FLOAT_EQ( Played( clip, "arm", At( kDuration ), Rest() ).Translation.x, 100.0F );
}

TEST( ClipSections, ATickOutsideEverySectionsRangeShowsThePoseUnderneath )
{
    // UE's rule, the clip's since the lift: a section contributes NOTHING outside [Start, End] — its keys
    // may lie outside and shape the curve entering the range, but past the end the pose underneath shows.
    AnimationClip clip = MovingClip( "arm" );
    Section&      half = TrackOf( clip, "arm" ).Sections.front();
    half.End           = FrameNumber{ kDuration / 2 };

    EXPECT_TRUE( half.Covers( FrameNumber{ kDuration / 2 } ) )
         << "the end tick is INSIDE: a range is inclusive, or the last frame of a section comes from "
            "somewhere else";
    EXPECT_FALSE( half.Covers( FrameNumber{ kDuration / 2 + 1 } ) );

    ExpectExactlyEqual( Played( clip, "arm", At( kDuration / 2 ), Rest() ),
                        Curve( clip, "arm", At( kDuration / 2 ) ), "on the end tick the section still speaks" );
    ExpectExactlyEqual( Played( clip, "arm", At( kDuration / 2 + 1 ), Rest() ), Rest(),
                        "one tick past it, nothing does: the reference pose shows" );
}

// ── 2. THE TWO BLEND TYPES MEAN DIFFERENT THINGS ─────────────────────────────────────────────────────

TEST( ClipSections, AbsoluteAndAdditiveDisagreeOnTheIdentityValue )
{
    // THE POSITIVE CONTROL FOR THE ENUM EXISTING AT ALL. An identity authored value means "no change" on an
    // Additive section and "go to the origin" on an Absolute one; if the two agreed, `SectionBlendType` would
    // record a decision that changes nothing and the field would be dead weight on disk.
    const BoneTransform identity =
         TransformOf( glm::vec3( 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) );
    AnimationClip clip = StaticClip( "arm", identity );

    ExpectExactlyEqual( Played( clip, "arm", At( 10 ), Rest() ), identity,
                        "an Absolute section's value IS the pose, identity included" );

    Section& section = TrackOf( clip, "arm" ).Sections.front();
    section.Blend    = SectionBlendType::Additive;
    ExpectExactlyEqual( Played( clip, "arm", At( 10 ), Rest() ), Rest(),
                        "an Additive section's identity is a no-op, at any weight" );
    section.Weight = { WeightKey( 0, 0.37F ) };
    ExpectExactlyEqual( Played( clip, "arm", At( 10 ), Rest() ), Rest(), "…including a partial one" );
}

TEST( ClipSections, AnAdditiveSectionAddsItsOffsetToTheRestPose )
{
    AnimationClip clip =
         StaticClip( "arm", TransformOf( glm::vec3( 10.0F, 0.0F, 0.0F ),
                                         glm::angleAxis( glm::radians( 60.0F ), glm::vec3( 0.0F, 0.0F, 1.0F ) ),
                                         glm::vec3( 3.0F, 1.0F, 1.0F ) ) );
    TrackOf( clip, "arm" ).Sections.front().Blend = SectionBlendType::Additive;

    const BoneTransform got = Played( clip, "arm", At( 10 ), Rest() );

    EXPECT_FLOAT_EQ( got.Translation.x, 10.0F );
    EXPECT_FLOAT_EQ( got.Translation.y, 50.0F ) << "the rest translation is kept and added to";
    // 30 degrees of rest, then 60 of offset, composed in the reference's own space.
    const float degrees = glm::degrees( glm::angle( glm::normalize( got.Rotation ) ) );
    EXPECT_NEAR( degrees, 90.0F, 1.0e-3F );
    // Scale is MULTIPLICATIVE: a rest of 2 under an offset of 3 is 6, not 5. Adding instead would make an
    // additive scale of 1 — the identity — double the bone, which is the whole trap the operator picks.
    EXPECT_FLOAT_EQ( got.Scale.x, 6.0F );
    EXPECT_FLOAT_EQ( got.Scale.y, 2.0F );
}

// ── 3. THE WEIGHT CHANNEL, AND WHAT §972 SAYS IT MEANS ───────────────────────────────────────────────

TEST( ClipSections, AHalfWeightIsHalfTheVALUEAndNotHalfAPoseBlend )
{
    // REPORT 05 §972, AS ARITHMETIC. "A 50 % layer is 50 % of the control OFFSET": a 90-degree authored
    // rotation reads 45 degrees at weight 0.5, and a 100-centimetre translation reads 50.
    AnimationClip clip =
         StaticClip( "arm", TransformOf( glm::vec3( 100.0F, 0.0F, 0.0F ),
                                         glm::angleAxis( glm::radians( 90.0F ), glm::vec3( 0.0F, 0.0F, 1.0F ) ),
                                         glm::vec3( 3.0F, 1.0F, 1.0F ) ) );
    Section& section = TrackOf( clip, "arm" ).Sections.front();
    section.Blend    = SectionBlendType::Additive;
    section.Weight   = { WeightKey( 0, 0.5F ) };
    const BoneTransform rest =
         TransformOf( glm::vec3( 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) );

    const BoneTransform half = Played( clip, "arm", At( 10 ), rest );
    EXPECT_FLOAT_EQ( half.Translation.x, 50.0F ) << "half the authored displacement";
    EXPECT_NEAR( glm::degrees( glm::angle( glm::normalize( half.Rotation ) ) ), 45.0F, 1.0e-3F )
         << "half the authored ANGLE — which is what an animator reads off the curve";
    EXPECT_FLOAT_EQ( half.Scale.x, 2.0F ) << "halfway from the multiplicative identity 1 to 3";
}

TEST( ClipSections, AnAbsoluteSectionAtHalfWeightIsHalfwayFromTheRestPose )
{
    AnimationClip clip =
         StaticClip( "arm", TransformOf( glm::vec3( 100.0F, 0.0F, 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ),
                                         glm::vec3( 4.0F ) ) );
    Section& section = TrackOf( clip, "arm" ).Sections.front();
    section.Weight   = { WeightKey( 0, 0.5F ) };
    const BoneTransform rest =
         TransformOf( glm::vec3( 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 2.0F ) );

    const BoneTransform half = Played( clip, "arm", At( 10 ), rest );
    EXPECT_FLOAT_EQ( half.Translation.x, 50.0F );
    EXPECT_FLOAT_EQ( half.Scale.x, 3.0F ) << "halfway from 2 to 4 — still a VALUE blend, not a pose one";

    section.Weight = { WeightKey( 0, 0.0F ) };
    ExpectExactlyEqual( Played( clip, "arm", At( 10 ), rest ), rest,
                        "weight 0 on an Absolute section is the rest pose exactly" );
}

TEST( ClipSections, AnUnkeyedWeightChannelIsFullWeightAndNotSilence )
{
    // A CHANNEL OF NO KEYS AND A CHANNEL HOLDING ONE KEY OF 0 ARE DIFFERENT STATEMENTS. Every lifted file has
    // the first, so reading it as the second would mute the entire corpus — and a muted clip still loads,
    // still plays and still renders a character, in its bind pose.
    Section unkeyed;
    unkeyed.End = FrameNumber{ kDuration };
    EXPECT_FLOAT_EQ( WeightAt( unkeyed, At( 1234 ), PROJECT_TICK_RATE ), 1.0F );

    Section muted = unkeyed;
    muted.Weight  = { WeightKey( 0, 0.0F ) };
    EXPECT_FLOAT_EQ( WeightAt( muted, At( 1234 ), PROJECT_TICK_RATE ), 0.0F );
}

TEST( ClipSections, TheWeightChannelFadesBetweenItsKeys )
{
    Section fade;
    fade.End    = FrameNumber{ kDuration };
    fade.Blend  = SectionBlendType::Additive;
    fade.Weight = { WeightKey( 0, 0.0F ), WeightKey( kDuration, 1.0F ) };

    EXPECT_FLOAT_EQ( WeightAt( fade, At( 0 ), PROJECT_TICK_RATE ), 0.0F );
    EXPECT_FLOAT_EQ( WeightAt( fade, At( kDuration / 2 ), PROJECT_TICK_RATE ), 0.5F );
    EXPECT_FLOAT_EQ( WeightAt( fade, At( kDuration ), PROJECT_TICK_RATE ), 1.0F );
    // OUTSIDE THE KEYED RANGE THE CHANNEL HOLDS, it does not extrapolate: a weight that ran past 1 because the
    // curve was still climbing would over-apply a layer nobody authored past its last key.
    EXPECT_FLOAT_EQ( WeightAt( fade, At( kDuration * 2 ), PROJECT_TICK_RATE ), 1.0F );
}

TEST( ClipSections, TheFadeReachesTheSamplerAndNotOnlyTheWeightFunction )
{
    // THE POSITIVE CONTROL FOR THE WIRING. The assertions above are about `WeightAt` in isolation; this one
    // is the only proof that `EvaluatePose` actually folds by it — without it, a pose evaluation that ignored
    // section weights would pass the identity test perfectly.
    AnimationClip clip                             = MovingClip( "arm" );
    TrackOf( clip, "arm" ).Sections.front().Weight = { WeightKey( 0, 0.0F ) };

    ExpectExactlyEqual( Played( clip, "arm", At( kDuration ), Rest() ), Rest(),
                        "a zero-weight Absolute section hands back the rest pose" );
    EXPECT_FLOAT_EQ( Curve( clip, "arm", At( kDuration ) ).Translation.x, 100.0F )
         << "…while the curve underneath is untouched and still says 100";
}

TEST( ClipSections, TheHIGHERRowWinsAnOverlapAndChangingTheRowChangesTheWinner )
{
    // Two full-weight Absolute sections over one tick of one track: lower rows fold first, so the higher row
    // is the value played. Changing the Row is the animator's remedy for "the wrong one wins" — without it the
    // remedy is delete-and-re-author, which loses the weight curve — so the assertion is about the WINNER.
    AnimationClip       clip = MovingClip( "arm" );
    const BoneTransform held =
         TransformOf( glm::vec3( -7.0F, 3.0F, 1.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) );
    const AnimationClip donor = StaticClip( "arm", held );

    Track&   track = TrackOf( clip, "arm" );
    Section& over  = AddSection( track, FrameNumber{ 0 }, FrameNumber{ kDuration } );
    EXPECT_TRUE( over.Weight.empty() ) << "a new section is full weight, not silence";
    EXPECT_EQ( over.Blend, SectionBlendType::Absolute );
    EXPECT_GT( over.Row, track.Sections.front().Row ) << "AddSection takes the first free row, above the curve's";
    over.Content = donor.Sequence.Tracks.front().Sections.front().Content;
    ++clip.Sequence.Revision;

    ExpectExactlyEqual( Played( clip, "arm", At( 10 ), Rest() ), held, "the higher row wins" );

    std::swap( track.Sections[0].Row, track.Sections[1].Row );
    ExpectExactlyEqual( Played( clip, "arm", At( 10 ), Rest() ), Curve( clip, "arm", At( 10 ) ),
                        "raising the curve's section past the held one is what makes it win" );
}

// ── Section edits (ANIM-I10a): the engine's one set, used by every host's Sequencer ─────────────────────

TEST( ClipSections, MovingASectionCARRIESItsKeysSoItsLengthAndItsCurveAreKept )
{
    AnimationClip clip  = MovingClip( "arm" );
    Track&        track = TrackOf( clip, "arm" );
    ASSERT_TRUE( SetSectionWeightKey( track, 0, FrameNumber{ 0 }, 0.25F ).IsSuccess() );
    ASSERT_TRUE( SetSectionWeightKey( track, 0, FrameNumber{ kDuration }, 1.0F ).IsSuccess() );
    const BoneTransform before = Played( clip, "arm", At( 10 ), Rest() );

    ASSERT_TRUE( MoveSection( track, 0, 500 ).IsSuccess() );
    const Section& moved = track.Sections[0];
    EXPECT_EQ( moved.Start.Value, 500 );
    EXPECT_EQ( moved.End.Value, kDuration + 500 ) << "a move keeps the length";
    EXPECT_EQ( ChannelOf( moved ).Translation.X.Keys.front().Tick.Value, 500 ) << "the keys went with it";
    EXPECT_EQ( ChannelOf( moved ).Rotation.W.Keys.back().Tick.Value, kDuration + 500 );
    EXPECT_EQ( moved.Weight.front().Tick.Value, 500 ) << "and so did the weight keys";

    // Every key moved by the same amount, so the sample 10 ticks into the section is the SAME arithmetic.
    ExpectExactlyEqual( Played( clip, "arm", At( 510 ), Rest() ), before, "the curve moved, not reshaped" );
    ExpectExactlyEqual( Played( clip, "arm", At( 10 ), Rest() ), Rest(),
                        "the tick the section left shows the pose underneath" );
}

TEST( ClipSections, AMoveThatLeavesTheFrameRangeIsRefusedWHOLEAndNotClamped )
{
    AnimationClip clip    = MovingClip( "arm" );
    Track&        track   = TrackOf( clip, "arm" );
    const auto    refused = MoveSection( track, 0, std::numeric_limits<int32_t>::max() );
    EXPECT_FALSE( refused.IsSuccess() );
    EXPECT_EQ( track.Sections[0].Start.Value, 0 );
    EXPECT_EQ( track.Sections[0].End.Value, kDuration ) << "the length survived a refused move";
    EXPECT_EQ( ChannelOf( track.Sections[0] ).Translation.X.Keys.front().Tick.Value, 0 )
         << "no key moved half-way: the check runs before anything is written";

    EXPECT_FALSE( MoveSection( track, 3, 10 ).IsSuccess() ) << "an index that is not there";
    // NOT refused: a section before tick 0 or past the playback range is legal content (UE, `Validate`).
    EXPECT_TRUE( MoveSection( track, 0, -100 ).IsSuccess() );
    EXPECT_EQ( track.Sections[0].Start.Value, -100 );
}

TEST( ClipSections, ARangeEditLeavesTheKeysAndRefusesAnEndBeforeItsStart )
{
    AnimationClip clip  = MovingClip( "arm" );
    Track&        track = TrackOf( clip, "arm" );

    ASSERT_TRUE( SetSectionRange( track, 0, FrameNumber{ 100 }, FrameNumber{ 900 } ).IsSuccess() );
    EXPECT_EQ( track.Sections[0].Start.Value, 100 );
    EXPECT_EQ( track.Sections[0].End.Value, 900 );
    EXPECT_EQ( ChannelOf( track.Sections[0] ).Translation.X.Keys.front().Tick.Value, 0 )
         << "a range edit is UE's SetRange: the keys outside still shape the curve entering it";
    ExpectExactlyEqual( Played( clip, "arm", At( 500 ), Rest() ), Curve( clip, "arm", At( 500 ) ),
                        "inside the new range the curve plays as keyed" );

    EXPECT_FALSE( SetSectionRange( track, 0, FrameNumber{ 900 }, FrameNumber{ 100 } ).IsSuccess() );
    EXPECT_EQ( track.Sections[0].End.Value, 900 );
    EXPECT_FALSE( SetSectionRange( track, 7, FrameNumber{ 0 }, FrameNumber{ 10 } ).IsSuccess() );

    // start == end is ONE TICK and legal: the end is inclusive, so a range cannot be empty.
    EXPECT_TRUE( SetSectionRange( track, 0, FrameNumber{ 500 }, FrameNumber{ 500 } ).IsSuccess() );
}

TEST( ClipSections, SettingTheRowIsWhatChangesTheWinnerAndARowAboveTheFirstIsRefused )
{
    AnimationClip       clip = MovingClip( "arm" );
    const BoneTransform held =
         TransformOf( glm::vec3( -7.0F, 3.0F, 1.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) );
    const AnimationClip donor = StaticClip( "arm", held );
    Track&              track = TrackOf( clip, "arm" );
    Section&            over  = AddSection( track, FrameNumber{ 0 }, FrameNumber{ kDuration } );
    over.Content              = donor.Sequence.Tracks.front().Sections.front().Content;
    ASSERT_EQ( track.Sections[1].Row, 1 );
    ExpectExactlyEqual( Played( clip, "arm", At( 10 ), Rest() ), held, "the higher row wins" );

    ASSERT_TRUE( SetSectionRow( track, 0, 2 ).IsSuccess() );
    ExpectExactlyEqual( Played( clip, "arm", At( 10 ), Rest() ), Curve( clip, "arm", At( 10 ) ),
                        "raising the curve's section above the held one is what makes it win" );

    EXPECT_FALSE( SetSectionRow( track, 0, -1 ).IsSuccess() );
    EXPECT_EQ( track.Sections[0].Row, 2 );
    EXPECT_FALSE( SetSectionRow( track, 5, 0 ).IsSuccess() );
}

TEST( ClipSections, CameraCutsRefuseEveryEditThatWouldOverlapTwoOnOneRow )
{
    Track track;
    track.Kind = TrackKind::CameraCut;
    (void)AddSection( track, FrameNumber{ 0 }, FrameNumber{ 100 } );
    (void)AddSection( track, FrameNumber{ 200 }, FrameNumber{ 300 } );
    ASSERT_EQ( track.Sections[1].Row, 0 );

    EXPECT_FALSE( MoveSection( track, 1, -150 ).IsSuccess() ) << "at every tick one camera speaks";
    EXPECT_EQ( track.Sections[1].Start.Value, 200 );
    EXPECT_FALSE( SetSectionRange( track, 1, FrameNumber{ 50 }, FrameNumber{ 300 } ).IsSuccess() );
    EXPECT_TRUE( SetSectionRange( track, 1, FrameNumber{ 101 }, FrameNumber{ 300 } ).IsSuccess() )
         << "touching is not overlapping: the ranges are inclusive, 100 and 101 are two ticks";

    ASSERT_TRUE( SetSectionRow( track, 1, 1 ).IsSuccess() );
    EXPECT_TRUE( MoveSection( track, 1, -101 ).IsSuccess() ) << "on its own row the cut may lie anywhere";
    EXPECT_FALSE( SetSectionRow( track, 1, 0 ).IsSuccess() ) << "and cannot come back onto an occupied row";

    EXPECT_FALSE( SetSectionWeightKey( track, 0, FrameNumber{ 0 }, 0.5F ).IsSuccess() )
         << "a cut is full weight by definition";
    EXPECT_TRUE( track.Sections[0].Weight.empty() );
}

TEST( ClipSections, AWeightKeyIsUpsertedInTickOrderKeepsItsSHAPEAndIsNotClamped )
{
    AnimationClip clip    = MovingClip( "arm" );
    Track&        track   = TrackOf( clip, "arm" );
    Section&      section = track.Sections[0];

    ASSERT_TRUE( SetSectionWeightKey( track, 0, FrameNumber{ 2000 }, 0.25F ).IsSuccess() );
    ASSERT_TRUE( SetSectionWeightKey( track, 0, FrameNumber{ 0 }, 1.0F ).IsSuccess() );
    ASSERT_EQ( section.Weight.size(), 2U );
    EXPECT_EQ( section.Weight[0].Tick.Value, 0 ) << "the channel stays sorted, as every sampler assumes";
    EXPECT_EQ( section.Weight[1].Tick.Value, 2000 );
    EXPECT_EQ( section.Weight[1].Interp, KeyInterp::Linear ) << "a new weight key is a ramp";

    section.Weight[1].Interp       = KeyInterp::Cubic;
    section.Weight[1].Mode         = Desert::Animation::TangentMode::User;
    section.Weight[1].LeaveTangent = 7.0F;
    ASSERT_TRUE( SetSectionWeightKey( track, 0, FrameNumber{ 2000 }, 0.5F ).IsSuccess() );
    EXPECT_EQ( section.Weight.size(), 2U ) << "an upsert, not an insert";
    EXPECT_FLOAT_EQ( section.Weight[1].Value, 0.5F );
    EXPECT_EQ( section.Weight[1].Interp, KeyInterp::Cubic );
    EXPECT_FLOAT_EQ( section.Weight[1].LeaveTangent, 7.0F );

    // Not clamped: WeightAt does not clamp (Section.hpp) and neither does UE's weight channel, so an
    // editor-keyed 1.5 is the same value a file-authored 1.5 is.
    ASSERT_TRUE( SetSectionWeightKey( track, 0, FrameNumber{ 2000 }, 1.5F ).IsSuccess() );
    EXPECT_FLOAT_EQ( section.Weight[1].Value, 1.5F );
    EXPECT_FLOAT_EQ( WeightAt( section, At( 2000 ), PROJECT_TICK_RATE ), 1.5F );

    EXPECT_FALSE( SetSectionWeightKey( track, 0, FrameNumber{ 2000 }, std::numeric_limits<float>::quiet_NaN() )
                       .IsSuccess() )
         << "not a number that was too big — a value that is not one";
    EXPECT_FALSE( SetSectionWeightKey( track, 0, FrameNumber{ 2000 }, std::numeric_limits<float>::infinity() )
                       .IsSuccess() );
    EXPECT_FLOAT_EQ( section.Weight[1].Value, 1.5F );
    EXPECT_FALSE( SetSectionWeightKey( track, 9, FrameNumber{ 0 }, 0.5F ).IsSuccess() );
}

TEST( ClipSections, RemovingEveryWeightKeyIsFullWeightAndNotSilence )
{
    AnimationClip  clip    = MovingClip( "arm" );
    Track&         track   = TrackOf( clip, "arm" );
    const Section& section = track.Sections[0];
    ASSERT_TRUE( SetSectionWeightKey( track, 0, FrameNumber{ 0 }, 0.0F ).IsSuccess() );
    ASSERT_FLOAT_EQ( WeightAt( section, At( 0 ), PROJECT_TICK_RATE ), 0.0F );

    EXPECT_FALSE( RemoveSectionWeightKey( track, 0, 4 ).IsSuccess() );
    ASSERT_TRUE( RemoveSectionWeightKey( track, 0, 0 ).IsSuccess() );
    EXPECT_FLOAT_EQ( WeightAt( section, At( 0 ), PROJECT_TICK_RATE ), 1.0F )
         << "an empty channel is FULL weight; reading it as 0 would mute the whole corpus";

    ASSERT_TRUE( SetSectionWeightKey( track, 0, FrameNumber{ 0 }, 0.0F ).IsSuccess() );
    ASSERT_TRUE( ClearSectionWeight( track, 0 ).IsSuccess() );
    EXPECT_TRUE( section.Weight.empty() );
    EXPECT_FLOAT_EQ( WeightAt( section, At( 0 ), PROJECT_TICK_RATE ), 1.0F );
    EXPECT_FALSE( ClearSectionWeight( track, 1 ).IsSuccess() );
}

TEST( ClipSections, RemovingASectionShowsThePoseUnderneathAndAMissingIndexIsRefused )
{
    AnimationClip clip  = MovingClip( "arm" );
    Track&        track = TrackOf( clip, "arm" );
    EXPECT_FALSE( RemoveSection( track, 7 ).IsSuccess() );
    ASSERT_EQ( track.Sections.size(), 1U );

    ASSERT_TRUE( RemoveSection( track, 0 ).IsSuccess() );
    EXPECT_TRUE( track.Sections.empty() );
    ExpectExactlyEqual( Played( clip, "arm", At( 10 ), Rest() ), Rest(),
                        "a track with no section contributes nothing: the pose under it plays" );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

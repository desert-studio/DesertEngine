// A SECTION: WHAT IT CHANGES ABOUT A CLIP, AND THE TWO THINGS THAT MUST NOT CHANGE.
//
// Report 05 §938. The hard part of this step is not the arithmetic, it is that the arithmetic must be
// INVISIBLE to everything already in the tree: every `.anim` in the repository migrates to one Absolute
// section at full weight, so if that case is not the identity — bit for bit, not "about equal" — then
// the version step silently moved every key in the corpus.
//
// So the suite is in three parts, and the first two are the ones that would catch a mistake:
//
//   1. THE IDENTITY. A clip with no sections and the same clip with one full-weight Absolute section
//      must produce EQUAL floats. `mix( a, b, 1.0F )` is `a + 1.0F * ( b - a )` and is NOT `b`, which is
//      exactly the kind of difference a tolerance hides and a corpus remembers.
//   2. THE TWO BLEND TYPES ARE DIFFERENT. An enum whose values behave the same way is a field that
//      records a decision nobody can make, so `Absolute` and `Additive` are asserted to disagree on the
//      one value where the difference matters most: identity.
//   3. The weight channel, the range, the track list and which section wins an overlap.
//
// §972's warning is asserted as ARITHMETIC rather than quoted: a half-weighted section is half of the
// VALUE, so a control at 90 degrees under a 0.5 section reads 45 degrees — not the pose halfway between
// two evaluated rigs. The test that says so is `AHalfWeightIsHalfTheVALUEAndNotHalfAPoseBlend`.

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/ClipSection.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using Desert::Animation::AnimationClip;
using Desert::Animation::ApplySection;
using Desert::Animation::BoneTrack;
using Desert::Animation::BoneTransform;
using Desert::Animation::ClipSection;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameTime;
using Desert::Animation::KeyInterp;
using Desert::Animation::PositionKeyFrame;
using Desert::Animation::RotationKeyFrame;
using Desert::Animation::ScalarKey;
using Desert::Animation::ScaleKeyFrame;
using Desert::Animation::SectionBlendType;
using Desert::Animation::AddSection;
using Desert::Animation::ClearSectionWeight;
using Desert::Animation::MoveSection;
using Desert::Animation::RemoveSection;
using Desert::Animation::RemoveSectionWeightKey;
using Desert::Animation::ReorderSection;
using Desert::Animation::SetSectionRange;
using Desert::Animation::SetSectionSpeaksFor;
using Desert::Animation::SetSectionSpeaksForEveryTrack;
using Desert::Animation::SetSectionWeightKey;

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

    /// A track that MOVES, and not by a rounding-sized amount: a clip whose keys are all the reference
    /// value would let every blend in this file pass while doing nothing.
    BoneTrack MovingTrack( const char* name )
    {
        BoneTrack track;
        track.BoneName = name;

        PositionKeyFrame p0;
        p0.Tick     = FrameNumber{ 0 };
        p0.Position = glm::vec3( 0.0F, 0.0F, 0.0F );
        p0.Interp   = KeyInterp::Linear;
        PositionKeyFrame p1;
        p1.Tick            = FrameNumber{ kDuration };
        p1.Position        = glm::vec3( 100.0F, 0.0F, 0.0F ); // one metre, in this engine's centimetres
        p1.Interp          = KeyInterp::Linear;
        track.PositionKeys = { p0, p1 };

        RotationKeyFrame r0;
        r0.Tick     = FrameNumber{ 0 };
        r0.Rotation = glm::quat( 1.0F, 0.0F, 0.0F, 0.0F );
        RotationKeyFrame r1;
        r1.Tick            = FrameNumber{ kDuration };
        r1.Rotation        = glm::angleAxis( glm::radians( 90.0F ), glm::vec3( 0.0F, 0.0F, 1.0F ) );
        track.RotationKeys = { r0, r1 };

        ScaleKeyFrame s0;
        s0.Tick  = FrameNumber{ 0 };
        s0.Scale = glm::vec3( 1.0F );
        ScaleKeyFrame s1;
        s1.Tick         = FrameNumber{ kDuration };
        s1.Scale        = glm::vec3( 3.0F, 1.0F, 1.0F );
        track.ScaleKeys = { s0, s1 };

        return track;
    }

    AnimationClip ClipWith( BoneTrack track )
    {
        AnimationClip clip;
        clip.AnimationName = "take";
        clip.DurationTicks = FrameNumber{ kDuration };
        clip.Tracks.push_back( std::move( track ) );
        return clip;
    }

    ClipSection WholeClip( SectionBlendType blend )
    {
        ClipSection section;
        section.Name  = "Whole clip";
        section.Start = FrameNumber{ 0 };
        section.End   = FrameNumber{ kDuration };
        section.Blend = blend;
        return section; // Tracks empty = every track; Weight empty = full weight
    }

    /// A rest pose that is NOT the identity. An identity reference makes `Absolute` and `Additive` agree
    /// on translation and lets a wrong composition order pass every assertion here.
    BoneTransform Rest()
    {
        return TransformOf( glm::vec3( 0.0F, 50.0F, 0.0F ),
                            glm::angleAxis( glm::radians( 30.0F ), glm::vec3( 0.0F, 0.0F, 1.0F ) ),
                            glm::vec3( 2.0F, 2.0F, 2.0F ) );
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
    // THE ASSERTION THE WHOLE VERSION STEP RESTS ON. Every clip in the repository migrated to exactly
    // this section, so any arithmetic here — including the `a + 1*(b-a)` that a `mix` would do — moves
    // the whole corpus by an amount no "near" comparison could see.
    const AnimationClip bare      = ClipWith( MovingTrack( "arm" ) );
    AnimationClip       sectioned = ClipWith( MovingTrack( "arm" ) );
    sectioned.Sections.push_back( WholeClip( SectionBlendType::Absolute ) );

    for ( const int tick : { 0, 1, 9999, kDuration / 3, kDuration / 2, kDuration - 1, kDuration } )
    {
        const FrameTime at{ FrameNumber{ tick }, 0.0F };
        ExpectExactlyEqual( sectioned.SampleTrack( sectioned.Tracks[0], at, Rest() ),
                            bare.SampleTrack( bare.Tracks[0], at, Rest() ),
                            "a full-weight Absolute section must be indistinguishable from no section" );
        ExpectExactlyEqual( sectioned.SampleTrack( sectioned.Tracks[0], at, Rest() ),
                            sectioned.Tracks[0].Sample( at, sectioned.TickRate ),
                            "…and both must be the curve itself" );
    }
}

TEST( ClipSections, AFullWeightAbsoluteSectionIsExactWHERETHEARITHMETICWOULDNOTBE )
{
    // THE TEST ABOVE WAS DEGENERATE AND A MUTATION SAID SO (§8.4). Deleting the `weight >= 1` branch from
    // `ApplySection` left it GREEN, because its numbers — a rest of 0 and 50, a curve of 0 and 100 — are
    // values for which `a + 1.0F * ( b - a )` happens to land on `b` exactly. Most do. These do not:
    // `0.3F + ( 0.1F - 0.3F )` is 0.099999994, and `1234.5678F + ( 0.0001F - 1234.5678F )` is 0.00012207,
    // which is the authored value wrong by 22 %.
    //
    // A corpus clip whose bone sits far from the origin and whose curve is small is EXACTLY this case, so
    // the branch is load bearing and this is the scenario that proves it.
    BoneTrack tiny;
    tiny.BoneName = "arm";
    PositionKeyFrame p;
    p.Tick     = FrameNumber{ 0 };
    p.Position = glm::vec3( 0.0001F, 0.1F, 0.25F );
    tiny.PositionKeys.push_back( p );
    ScaleKeyFrame sc;
    sc.Tick  = FrameNumber{ 0 };
    sc.Scale = glm::vec3( 0.0001F, 0.1F, 1.0F );
    tiny.ScaleKeys.push_back( sc );
    RotationKeyFrame r;
    r.Tick     = FrameNumber{ 0 };
    r.Rotation = glm::angleAxis( 0.37F, glm::normalize( glm::vec3( 0.3F, -0.8F, 0.5F ) ) );
    tiny.RotationKeys.push_back( r );

    AnimationClip clip = ClipWith( tiny );
    clip.Sections.push_back( WholeClip( SectionBlendType::Absolute ) );

    const BoneTransform faraway =
         TransformOf( glm::vec3( 1234.5678F, 0.3F, -900.0F ),
                      glm::angleAxis( 2.9F, glm::normalize( glm::vec3( -0.9F, 0.2F, 0.35F ) ) ),
                      glm::vec3( 1234.5678F, 0.3F, 512.0F ) );

    const FrameTime at{ FrameNumber{ 0 }, 0.0F };
    ExpectExactlyEqual( clip.SampleTrack( clip.Tracks[0], at, faraway ),
                        clip.Tracks[0].Sample( at, clip.TickRate ),
                        "full weight must RETURN the authored value, not recompute it" );
}

TEST( ClipSections, ATrackNoSectionSpeaksForPlaysAsAuthored )
{
    // A section list is a statement about SOME tracks. One that names others must leave this one alone,
    // and "alone" is the curve — not the rest pose, which is what a sampler defaulting to weight 0 would
    // return and which would look like a bone that simply stopped animating.
    AnimationClip clip      = ClipWith( MovingTrack( "arm" ) );
    ClipSection   elsewhere = WholeClip( SectionBlendType::Additive );
    elsewhere.Tracks        = { "leg" };
    clip.Sections.push_back( elsewhere );

    const FrameTime at{ FrameNumber{ kDuration }, 0.0F };
    ExpectExactlyEqual( clip.SampleTrack( clip.Tracks[0], at, Rest() ), clip.Tracks[0].Sample( at, clip.TickRate ),
                        "a track nobody claimed is its curve" );
    EXPECT_EQ( clip.SectionFor( "arm", FrameNumber{ 0 } ), nullptr );
    EXPECT_NE( clip.SectionFor( "leg", FrameNumber{ 0 } ), nullptr ) << "…and the named one IS claimed";
}

TEST( ClipSections, ATickOutsideEverySectionsRangePlaysAsAuthored )
{
    AnimationClip clip = ClipWith( MovingTrack( "arm" ) );
    ClipSection   half = WholeClip( SectionBlendType::Additive );
    half.End           = FrameNumber{ kDuration / 2 };
    clip.Sections.push_back( half );

    EXPECT_NE( clip.SectionFor( "arm", FrameNumber{ kDuration / 2 } ), nullptr )
         << "the end tick is INSIDE: a range is inclusive, or the last frame of a section comes from "
            "somewhere else";
    EXPECT_EQ( clip.SectionFor( "arm", FrameNumber{ kDuration / 2 + 1 } ), nullptr );
}

// ── 2. THE TWO BLEND TYPES MEAN DIFFERENT THINGS ─────────────────────────────────────────────────────

TEST( ClipSections, AbsoluteAndAdditiveDisagreeOnTheIdentityValue )
{
    // THE POSITIVE CONTROL FOR THE ENUM EXISTING AT ALL. An identity authored value means "no change" on
    // an Additive section and "go to the origin" on an Absolute one; if the two agreed, `SectionBlendType`
    // would record a decision that changes nothing and the field would be dead weight on disk.
    const BoneTransform identity =
         TransformOf( glm::vec3( 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) );

    const ClipSection absolute = WholeClip( SectionBlendType::Absolute );
    const ClipSection additive = WholeClip( SectionBlendType::Additive );

    ExpectExactlyEqual( ApplySection( absolute, identity, Rest(), 1.0F ), identity,
                        "an Absolute section's value IS the pose, identity included" );
    ExpectExactlyEqual( ApplySection( additive, identity, Rest(), 1.0F ), Rest(),
                        "an Additive section's identity is a no-op, at any weight" );
    ExpectExactlyEqual( ApplySection( additive, identity, Rest(), 0.37F ), Rest(), "…including a partial one" );
}

TEST( ClipSections, AnAdditiveSectionAddsItsOffsetToTheRestPose )
{
    const ClipSection   additive = WholeClip( SectionBlendType::Additive );
    const BoneTransform offset   = TransformOf(
         glm::vec3( 10.0F, 0.0F, 0.0F ), glm::angleAxis( glm::radians( 60.0F ), glm::vec3( 0.0F, 0.0F, 1.0F ) ),
         glm::vec3( 3.0F, 1.0F, 1.0F ) );

    const BoneTransform got = ApplySection( additive, offset, Rest(), 1.0F );

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
    // REPORT 05 §972, AS ARITHMETIC. The documented sentence is "a 50 % layer is 50 % of the control
    // OFFSET"; here that is a 90-degree authored rotation reading 45 degrees at weight 0.5, and a
    // 100-centimetre translation reading 50. A pose blend would give neither in general, and gives the
    // same answer here only because the test deliberately compares against the numbers rather than
    // against a second implementation.
    const ClipSection   additive = WholeClip( SectionBlendType::Additive );
    const BoneTransform offset   = TransformOf(
         glm::vec3( 100.0F, 0.0F, 0.0F ), glm::angleAxis( glm::radians( 90.0F ), glm::vec3( 0.0F, 0.0F, 1.0F ) ),
         glm::vec3( 3.0F, 1.0F, 1.0F ) );
    const BoneTransform rest =
         TransformOf( glm::vec3( 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 1.0F ) );

    const BoneTransform half = ApplySection( additive, offset, rest, 0.5F );
    EXPECT_FLOAT_EQ( half.Translation.x, 50.0F ) << "half the authored displacement";
    EXPECT_NEAR( glm::degrees( glm::angle( glm::normalize( half.Rotation ) ) ), 45.0F, 1.0e-3F )
         << "half the authored ANGLE — which is what an animator reads off the curve";
    EXPECT_FLOAT_EQ( half.Scale.x, 2.0F ) << "halfway from the multiplicative identity 1 to 3";
}

TEST( ClipSections, AnAbsoluteSectionAtHalfWeightIsHalfwayFromTheRestPose )
{
    const ClipSection   absolute = WholeClip( SectionBlendType::Absolute );
    const BoneTransform authored =
         TransformOf( glm::vec3( 100.0F, 0.0F, 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 4.0F ) );
    const BoneTransform rest =
         TransformOf( glm::vec3( 0.0F ), glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ), glm::vec3( 2.0F ) );

    const BoneTransform half = ApplySection( absolute, authored, rest, 0.5F );
    EXPECT_FLOAT_EQ( half.Translation.x, 50.0F );
    EXPECT_FLOAT_EQ( half.Scale.x, 3.0F ) << "halfway from 2 to 4 — still a VALUE blend, not a pose one";

    ExpectExactlyEqual( ApplySection( absolute, authored, rest, 0.0F ), rest,
                        "weight 0 on an Absolute section is the rest pose exactly" );
}

TEST( ClipSections, AnUnkeyedWeightChannelIsFullWeightAndNotSilence )
{
    // A CHANNEL OF NO KEYS AND A CHANNEL HOLDING ONE KEY OF 0 ARE DIFFERENT STATEMENTS. Every migrated
    // file has the first, so reading it as the second would mute the entire corpus — and a muted clip
    // still loads, still plays and still renders a character, in its bind pose.
    const ClipSection unkeyed = WholeClip( SectionBlendType::Absolute );
    EXPECT_FLOAT_EQ(
         unkeyed.WeightAt( FrameTime{ FrameNumber{ 1234 }, 0.0F }, Desert::Animation::PROJECT_TICK_RATE ), 1.0F );

    ClipSection muted = WholeClip( SectionBlendType::Absolute );
    ScalarKey   zero;
    zero.Tick  = FrameNumber{ 0 };
    zero.Value = 0.0F;
    muted.Weight.push_back( zero );
    EXPECT_FLOAT_EQ(
         muted.WeightAt( FrameTime{ FrameNumber{ 1234 }, 0.0F }, Desert::Animation::PROJECT_TICK_RATE ), 0.0F );
}

TEST( ClipSections, TheWeightChannelFadesBetweenItsKeys )
{
    ClipSection fade = WholeClip( SectionBlendType::Additive );
    ScalarKey   a;
    a.Tick   = FrameNumber{ 0 };
    a.Value  = 0.0F;
    a.Interp = KeyInterp::Linear;
    ScalarKey b;
    b.Tick      = FrameNumber{ kDuration };
    b.Value     = 1.0F;
    b.Interp    = KeyInterp::Linear;
    fade.Weight = { a, b };

    const auto rate = Desert::Animation::PROJECT_TICK_RATE;
    EXPECT_FLOAT_EQ( fade.WeightAt( FrameTime{ FrameNumber{ 0 }, 0.0F }, rate ), 0.0F );
    EXPECT_FLOAT_EQ( fade.WeightAt( FrameTime{ FrameNumber{ kDuration / 2 }, 0.0F }, rate ), 0.5F );
    EXPECT_FLOAT_EQ( fade.WeightAt( FrameTime{ FrameNumber{ kDuration }, 0.0F }, rate ), 1.0F );
    // OUTSIDE THE KEYED RANGE THE CHANNEL HOLDS, it does not extrapolate: a weight that ran past 1
    // because the curve was still climbing would over-apply a layer nobody authored past its last key.
    EXPECT_FLOAT_EQ( fade.WeightAt( FrameTime{ FrameNumber{ kDuration * 2 }, 0.0F }, rate ), 1.0F );
}

TEST( ClipSections, TheFadeReachesTheSamplerAndNotOnlyTheWeightFunction )
{
    // THE POSITIVE CONTROL FOR THE WIRING. Every assertion above is about `ApplySection` and `WeightAt`
    // in isolation; this one is the only proof that `SampleTrack` actually calls them — and without it a
    // `SampleTrack` that ignored sections entirely would pass the identity test perfectly, which is the
    // exact shape of "both named suites stayed green while nothing reached the graph".
    AnimationClip clip = ClipWith( MovingTrack( "arm" ) );
    ClipSection   fade = WholeClip( SectionBlendType::Absolute );
    ScalarKey     off;
    off.Tick  = FrameNumber{ 0 };
    off.Value = 0.0F;
    fade.Weight.push_back( off );
    clip.Sections.push_back( fade );

    const FrameTime     at{ FrameNumber{ kDuration }, 0.0F };
    const BoneTransform muted = clip.SampleTrack( clip.Tracks[0], at, Rest() );
    ExpectExactlyEqual( muted, Rest(), "a zero-weight Absolute section hands back the rest pose" );
    EXPECT_FLOAT_EQ( clip.Tracks[0].Sample( at, clip.TickRate ).Translation.x, 100.0F )
         << "…while the curve underneath is untouched and still says 100";
}

TEST( ClipSections, TheLATERSectionWinsAnOverlap )
{
    // Two sections claiming one track at one tick are two statements about a single stored value, and
    // there is no composition to do until a section owns its own keys (ClipSection.hpp). "Later wins" is
    // the rule, and it is asserted so that the layering step has something to change deliberately rather
    // than a behaviour it discovers.
    AnimationClip clip = ClipWith( MovingTrack( "arm" ) );
    clip.Sections.push_back( WholeClip( SectionBlendType::Absolute ) );
    clip.Sections.push_back( WholeClip( SectionBlendType::Additive ) );

    const ClipSection* winner = clip.SectionFor( "arm", FrameNumber{ 10 } );
    ASSERT_NE( winner, nullptr );
    EXPECT_EQ( winner->Blend, SectionBlendType::Additive );
}

// ── 4. AUTHORING ONE (A32) ───────────────────────────────────────────────────────────────────────────
//
// The operations behind the Sequencer's section lane. They live in the engine and not in the panel for
// the reason scripts/CI/UnreachedSources.sh keeps naming: SequencerPanel.cpp is compiled by no suite, so
// a rule written there is a rule nothing can check. Everything below is a rule the panel would otherwise
// have had to state itself.

TEST( ClipSections, AddSectionRefusesARangeThatLeavesTheClip )
{
    std::vector<ClipSection> sections;
    const auto beyond = AddSection( sections, "late", FrameNumber{ kDuration - 10 },
                                    FrameNumber{ kDuration + 1 }, SectionBlendType::Absolute,
                                    FrameNumber{ kDuration } );
    EXPECT_FALSE( beyond.IsSuccess() );
    EXPECT_TRUE( sections.empty() ) << "a refused add must leave the list alone, not half-write it";

    const auto backwards = AddSection( sections, "backwards", FrameNumber{ 100 }, FrameNumber{ 10 },
                                       SectionBlendType::Absolute, FrameNumber{ kDuration } );
    EXPECT_FALSE( backwards.IsSuccess() );

    const auto unnamed = AddSection( sections, "", FrameNumber{ 0 }, FrameNumber{ 10 },
                                     SectionBlendType::Absolute, FrameNumber{ kDuration } );
    EXPECT_FALSE( unnamed.IsSuccess() ) << "a section with no name is a row the animator cannot point at";

    EXPECT_TRUE( sections.empty() );
}

TEST( ClipSections, ANewSectionIsTheCLIPWIDEIdentityAndNotAnEmptyOne )
{
    // The two "empty means" fields are what a new section MEANS, and getting either backwards would ship
    // a button that mutes a track (Weight = one key of 0) or narrows it to nothing (Tracks = {}).
    std::vector<ClipSection> sections;
    ASSERT_TRUE( AddSection( sections, "Section 1", FrameNumber{ 0 }, FrameNumber{ kDuration },
                             SectionBlendType::Absolute, FrameNumber{ kDuration } )
                      .IsSuccess() );
    ASSERT_EQ( sections.size(), 1U );
    EXPECT_TRUE( sections[0].Tracks.empty() ) << "empty IS every track";
    EXPECT_TRUE( sections[0].Weight.empty() ) << "empty IS full weight";
    EXPECT_TRUE( sections[0].Speaks( "anything" ) );
    EXPECT_FLOAT_EQ( sections[0].WeightAt( FrameTime{ FrameNumber{ 0 }, 0.0F }, Desert::Animation::PROJECT_TICK_RATE ),
                     1.0F );
}

TEST( ClipSections, MovingASectionKEEPSItsLengthAndStopsAtTheClipsEndRatherThanShortening )
{
    std::vector<ClipSection> sections;
    ASSERT_TRUE( AddSection( sections, "s", FrameNumber{ 1000 }, FrameNumber{ 3000 },
                             SectionBlendType::Absolute, FrameNumber{ kDuration } )
                      .IsSuccess() );

    ASSERT_TRUE( MoveSection( sections, 0, 500, FrameNumber{ kDuration } ).IsSuccess() );
    EXPECT_EQ( sections[0].Start.Value, 1500 );
    EXPECT_EQ( sections[0].End.Value, 3500 );

    // Past the end: REFUSED WHOLE. A clamp that moved the start and pinned the end would silently turn a
    // move into a resize, which is the one thing this operation exists not to do.
    const auto refused = MoveSection( sections, 0, kDuration, FrameNumber{ kDuration } );
    EXPECT_FALSE( refused.IsSuccess() );
    EXPECT_EQ( sections[0].Start.Value, 1500 );
    EXPECT_EQ( sections[0].End.Value, 3500 ) << "the length survived a refused move";

    const auto before = MoveSection( sections, 0, -2000, FrameNumber{ kDuration } );
    EXPECT_FALSE( before.IsSuccess() ) << "and it refuses at tick 0 for the same reason";
    EXPECT_EQ( sections[0].Start.Value, 1500 );
}

TEST( ClipSections, ARangeEditRefusesAnEndBeforeItsStartAndAnIndexThatIsNotThere )
{
    std::vector<ClipSection> sections;
    ASSERT_TRUE( AddSection( sections, "s", FrameNumber{ 0 }, FrameNumber{ 1000 },
                             SectionBlendType::Absolute, FrameNumber{ kDuration } )
                      .IsSuccess() );

    EXPECT_FALSE(
         SetSectionRange( sections, 0, FrameNumber{ 900 }, FrameNumber{ 100 }, FrameNumber{ kDuration } )
              .IsSuccess() );
    EXPECT_EQ( sections[0].End.Value, 1000 );

    EXPECT_FALSE(
         SetSectionRange( sections, 7, FrameNumber{ 0 }, FrameNumber{ 10 }, FrameNumber{ kDuration } )
              .IsSuccess() );
    EXPECT_FALSE( RemoveSection( sections, 7 ).IsSuccess() );
    EXPECT_EQ( sections.size(), 1U );

    // start == end is ONE TICK and legal: the end is inclusive, so a range cannot be empty.
    EXPECT_TRUE(
         SetSectionRange( sections, 0, FrameNumber{ 500 }, FrameNumber{ 500 }, FrameNumber{ kDuration } )
              .IsSuccess() );
}

TEST( ClipSections, ReorderingIsWhatChangesWhichSectionWinsAnOverlap )
{
    // The rule under test is `AnimationClip::SectionFor`'s, and this is the only handle on it. Without
    // the reorder the animator's remedy for "the wrong one wins" is delete-and-re-author, which loses the
    // weight curve -- so the assertion is about the WINNER and not about the vector's order.
    AnimationClip clip = ClipWith( MovingTrack( "arm" ) );
    clip.Sections.push_back( WholeClip( SectionBlendType::Absolute ) );
    clip.Sections.push_back( WholeClip( SectionBlendType::Additive ) );
    ASSERT_EQ( clip.SectionFor( "arm", FrameNumber{ 10 } )->Blend, SectionBlendType::Additive );

    ASSERT_TRUE( ReorderSection( clip.Sections, 0, 1 ).IsSuccess() );
    EXPECT_EQ( clip.SectionFor( "arm", FrameNumber{ 10 } )->Blend, SectionBlendType::Absolute )
         << "raising the Absolute one past the Additive one is what makes it win";

    EXPECT_FALSE( ReorderSection( clip.Sections, 1, 1 ).IsSuccess() ) << "already at the end";
    EXPECT_FALSE( ReorderSection( clip.Sections, 0, -1 ).IsSuccess() ) << "already at the start";
    EXPECT_FALSE( ReorderSection( clip.Sections, 0, 2 ).IsSuccess() ) << "one place at a time";
}

TEST( ClipSections, NarrowingAClipWideSectionEXPANDSTheEmptyListBeforeRemovingFromIt )
{
    // THE TRAP. "All tracks except the hand" cannot be written as a removal from an empty list: the naive
    // version finds nothing to erase, leaves the list empty, and the section goes on speaking for every
    // track while the inspector shows the box unticked.
    const std::vector<std::string> all{ "root", "arm", "hand" };
    ClipSection                    section = WholeClip( SectionBlendType::Absolute );
    ASSERT_TRUE( section.Tracks.empty() );

    ASSERT_TRUE( SetSectionSpeaksFor( section, "hand", false, all ).IsSuccess() );
    EXPECT_EQ( section.Tracks.size(), 2U );
    EXPECT_TRUE( section.Speaks( "root" ) );
    EXPECT_TRUE( section.Speaks( "arm" ) );
    EXPECT_FALSE( section.Speaks( "hand" ) ) << "the removal actually took effect";
}

TEST( ClipSections, TickingTheLastTrackBackOnCollapsesTheListToTheEmptySpelling )
{
    // ONE MEANING, ONE SPELLING. A list naming every track behaves identically today and goes stale the
    // first time the keyer adds a track, so the file must never be allowed to carry it.
    const std::vector<std::string> all{ "root", "arm", "hand" };
    ClipSection                    section = WholeClip( SectionBlendType::Absolute );
    section.Tracks                         = { "root", "arm" };

    ASSERT_TRUE( SetSectionSpeaksFor( section, "hand", true, all ).IsSuccess() );
    EXPECT_TRUE( section.Tracks.empty() )
         << "naming all three would be a second copy of the track list, which goes stale";
    EXPECT_TRUE( section.Speaks( "a track added tomorrow" ) );
}

TEST( ClipSections, ASectionThatWouldSpeakForNOTHINGIsRefused )
{
    // An empty list is every track, so "untick the last one" cannot be written. Refusing is the only
    // answer that does not mean the opposite of what the animator asked for.
    const std::vector<std::string> all{ "root" };
    ClipSection                    section = WholeClip( SectionBlendType::Absolute );
    section.Tracks                         = { "root" };

    EXPECT_FALSE( SetSectionSpeaksFor( section, "root", false, all ).IsSuccess() );
    EXPECT_EQ( section.Tracks.size(), 1U ) << "and it left the list as it was";

    EXPECT_FALSE( SetSectionSpeaksFor( section, "ghost", true, all ).IsSuccess() )
         << "a track the clip does not have is a typo that would only show up as silence";

    SetSectionSpeaksForEveryTrack( section );
    EXPECT_TRUE( section.Tracks.empty() );
}

TEST( ClipSections, AWeightKeyIsUpsertedInTickOrderClampedAndKeepsAnExistingKeysSHAPE )
{
    ClipSection section = WholeClip( SectionBlendType::Absolute );

    ASSERT_TRUE( SetSectionWeightKey( section, FrameNumber{ 2000 }, 0.25F ).IsSuccess() );
    ASSERT_TRUE( SetSectionWeightKey( section, FrameNumber{ 0 }, 1.0F ).IsSuccess() );
    ASSERT_EQ( section.Weight.size(), 2U );
    EXPECT_EQ( section.Weight[0].Tick.Value, 0 ) << "the channel stays sorted, as every sampler assumes";
    EXPECT_EQ( section.Weight[1].Tick.Value, 2000 );

    // An authored shape survives a value change -- the same rule TrackEditing::SetTransformKey states.
    section.Weight[1].Interp = KeyInterp::Cubic;
    section.Weight[1].Mode   = Desert::Animation::TangentMode::User;
    section.Weight[1].LeaveTangent = 7.0F;
    ASSERT_TRUE( SetSectionWeightKey( section, FrameNumber{ 2000 }, 0.5F ).IsSuccess() );
    EXPECT_EQ( section.Weight.size(), 2U ) << "an upsert, not an insert";
    EXPECT_FLOAT_EQ( section.Weight[1].Value, 0.5F );
    EXPECT_EQ( section.Weight[1].Interp, KeyInterp::Cubic );
    EXPECT_FLOAT_EQ( section.Weight[1].LeaveTangent, 7.0F );

    ASSERT_TRUE( SetSectionWeightKey( section, FrameNumber{ 2000 }, 9.0F ).IsSuccess() );
    EXPECT_FLOAT_EQ( section.Weight[1].Value, 1.0F ) << "weight is a proportion, so it clamps";
    ASSERT_TRUE( SetSectionWeightKey( section, FrameNumber{ 2000 }, -3.0F ).IsSuccess() );
    EXPECT_FLOAT_EQ( section.Weight[1].Value, 0.0F );

    EXPECT_FALSE( SetSectionWeightKey( section, FrameNumber{ 2000 },
                                       std::numeric_limits<float>::quiet_NaN() )
                       .IsSuccess() )
         << "not a number that was too big — a value that is not one";
    EXPECT_FLOAT_EQ( section.Weight[1].Value, 0.0F );
}

TEST( ClipSections, RemovingEveryWeightKeyIsFullWeightAndNotSilence )
{
    ClipSection section = WholeClip( SectionBlendType::Absolute );
    ASSERT_TRUE( SetSectionWeightKey( section, FrameNumber{ 0 }, 0.0F ).IsSuccess() );
    ASSERT_FLOAT_EQ( section.WeightAt( FrameTime{ FrameNumber{ 0 }, 0.0F }, Desert::Animation::PROJECT_TICK_RATE ),
                     0.0F );

    EXPECT_FALSE( RemoveSectionWeightKey( section, 4 ).IsSuccess() );
    ASSERT_TRUE( RemoveSectionWeightKey( section, 0 ).IsSuccess() );
    EXPECT_FLOAT_EQ( section.WeightAt( FrameTime{ FrameNumber{ 0 }, 0.0F }, Desert::Animation::PROJECT_TICK_RATE ),
                     1.0F )
         << "an empty channel is FULL weight; reading it as 0 would mute the whole corpus";

    ASSERT_TRUE( SetSectionWeightKey( section, FrameNumber{ 0 }, 0.0F ).IsSuccess() );
    ClearSectionWeight( section );
    EXPECT_TRUE( section.Weight.empty() );
    EXPECT_FLOAT_EQ( section.WeightAt( FrameTime{ FrameNumber{ 0 }, 0.0F }, Desert::Animation::PROJECT_TICK_RATE ),
                     1.0F );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

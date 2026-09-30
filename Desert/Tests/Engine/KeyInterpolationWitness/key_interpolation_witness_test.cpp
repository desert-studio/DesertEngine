// A PAIR OF CLIPS THAT DIFFER IN ONE FIELD, AND THE POSE THAT PROVES IT IS THE RIGHT ONE.
//
// `A6Curve_Linear.anim` and `A6Curve_Cubic.anim` carry the SAME three position keys on IKProbe's chain
// root — up to a peak and back — and differ only in each key's `Shape.Interp`. That is what makes the
// frames taken of them a statement about interpolation rather than about anything else, and it is a claim
// about two files that nothing but this suite checks.
//
// The middle key is a PEAK on purpose. The auto pass makes an extremum flat, so that is where a cubic and
// a straight line disagree most, and it is also where a curve editor's worst bug — sailing past the
// highest key the animator authored — would show up in a frame.
//
// WHAT IS ASSERTED, and each of the three is a different kind of claim:
//   1. the two files are identical except for that one field, key for key;
//   2. ON a key the two clips give the SAME pose — both curves pass through their own keys, and a
//      difference there would mean the curve does not interpolate;
//   3. BETWEEN keys they differ, and the cubic never leaves the interval its keys bound.

//
// THE CLIP IS ITS Timeline::Sequence (ANIM v5): the keys asserted below are the X/Y/Z components of the one
// bone Transform track's one section, read and edited with the functions the engine uses — the file through
// `ReadAnimationJson` + `BuildClipFromAssetData`, the pose through `Timeline::EvaluateTrack`, the insert
// through the Sequence edit `InsertBoneKey`, the channel rules through TrackEditing's TransformChannel API.

#include <Engine/Animation/AnimationClip.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>
#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Animation/Timeline/Section.hpp>
#include <Engine/Animation/Timeline/Track.hpp>
#include <Engine/Animation/TrackEditing.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <rflcpp/rfl.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>

namespace Ser      = Desert::Assets::Serialization;
namespace Timeline = Desert::Animation::Timeline;

namespace
{
    constexpr const char* kCookedDir = "Editor/Resources/Assets/Meshes/Skinned/";
    constexpr const char* kBone      = "IK_Shoulder";

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + std::string( kCookedDir ) + "A6Curve_Linear.anim" );
            if ( probe )
            {
                return prefix;
            }
            prefix += "../";
        }
        return {};
    }

    /// The file through the engine's own reader (header, generation and schema checks included).
    Ser::AnimationAssetData Load( const char* stem )
    {
        const std::ifstream in( RepoRoot() + kCookedDir + stem + ".anim", std::ios::binary );
        EXPECT_TRUE( in.good() ) << stem;
        std::ostringstream text;
        text << in.rdbuf();
        auto data = Ser::ReadAnimationJson( text.str() );
        EXPECT_TRUE( data.IsSuccess() ) << stem << ": " << ( data.IsSuccess() ? "" : data.GetError() );
        return data.IsSuccess() ? data.GetValue() : Ser::AnimationAssetData{};
    }

    Desert::Animation::AnimationClip Built( const char* stem )
    {
        auto built = Ser::BuildClipFromAssetData( Load( stem ) );
        EXPECT_TRUE( built ) << stem << ": " << ( built ? "" : built.GetError() );
        return built ? built.ExtractValue() : Desert::Animation::AnimationClip{};
    }

    /// The keys of a clip's one bone track: the Transform channel of its one section. One body for a clip
    /// that is only read and one that is edited: the constness of the answer is the constness of the clip.
    template <class Clip>
    std::conditional_t<std::is_const_v<Clip>, const Timeline::TransformChannel*, Timeline::TransformChannel*>
    ChannelOf( Clip& clip )
    {
        if ( clip.Sequence.Tracks.size() != 1U || clip.Sequence.Tracks[0].Sections.size() != 1U )
        {
            ADD_FAILURE() << "expected one track with one section, got " << clip.Sequence.Tracks.size()
                          << " track(s)";
            return nullptr;
        }
        auto* channel = std::get_if<Timeline::Channel>( &clip.Sequence.Tracks[0].Sections[0].Content );
        return channel == nullptr ? nullptr : std::get_if<Timeline::TransformChannel>( channel );
    }

    /// What playback shows on the track at @p tick — the evaluator's fold, not a key read.
    float HeightAtTick( const Desert::Animation::AnimationClip& clip, int32_t tick )
    {
        EXPECT_EQ( clip.Sequence.Tracks.size(), 1u );
        if ( clip.Sequence.Tracks.empty() )
        {
            return std::numeric_limits<float>::quiet_NaN();
        }
        Timeline::EvaluatedValue value;
        const bool               covered = Timeline::EvaluateTrack(
             clip.Sequence.Tracks[0], Desert::Animation::FrameTime{ Desert::Animation::FrameNumber{ tick }, 0.0F },
             clip.Sequence.TickRate, value );
        EXPECT_TRUE( covered ) << "no section covers tick " << tick;
        const auto* pose = std::get_if<Desert::Animation::BoneTransform>( &value );
        EXPECT_NE( pose, nullptr ) << "a bone track did not evaluate to a transform";
        return covered && pose != nullptr ? pose->Translation.y : std::numeric_limits<float>::quiet_NaN();
    }

    /// One translation key on all three components, the shape a vec3 key had before the clip became a Sequence.
    void PushTranslationKey( Timeline::TransformChannel& channel, int32_t tick, const glm::vec3& value,
                             Desert::Animation::KeyInterp   interp,
                             Desert::Animation::TangentMode mode   = Desert::Animation::TangentMode::Auto,
                             const glm::vec3&               arrive = glm::vec3( 0.0F ),
                             const glm::vec3&               leave  = glm::vec3( 0.0F ) )
    {
        Timeline::FloatChannel* components[3] = { &channel.Translation.X, &channel.Translation.Y,
                                                  &channel.Translation.Z };
        for ( int c = 0; c < 3; ++c )
        {
            Desert::Animation::ScalarKey key;
            key.Tick          = Desert::Animation::FrameNumber{ tick };
            key.Value         = value[c];
            key.Interp        = interp;
            key.Mode          = mode;
            key.ArriveTangent = arrive[c];
            key.LeaveTangent  = leave[c];
            components[c]->Keys.push_back( key );
        }
    }
} // namespace

// ---------------------------------------------------------------- 1. one field apart

TEST( KeyInterpolationWitness, TheTwoClipsDifferInExactlyOneFieldPerKey )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository root from the working directory";

    const Ser::AnimationAssetData linearData = Load( "A6Curve_Linear" );
    const Ser::AnimationAssetData cubicData  = Load( "A6Curve_Cubic" );

    EXPECT_EQ( Desert::Assets::StatedVersion( linearData.Header, Desert::Assets::kAnimationSchemaTag ),
               Ser::kAnimationVersion );
    EXPECT_EQ( Desert::Assets::StatedVersion( cubicData.Header, Desert::Assets::kAnimationSchemaTag ),
               Ser::kAnimationVersion );
    EXPECT_EQ( linearData.Skeleton, cubicData.Skeleton );

    const auto linear = Built( "A6Curve_Linear" );
    const auto cubic  = Built( "A6Curve_Cubic" );
    EXPECT_EQ( linear.DurationTicks().Value, cubic.DurationTicks().Value );
    ASSERT_EQ( linear.Sequence.Bindings.size(), 1u );
    ASSERT_EQ( cubic.Sequence.Bindings.size(), 1u );
    EXPECT_EQ( linear.Sequence.Bindings[0].Locator, cubic.Sequence.Bindings[0].Locator );
    EXPECT_EQ( linear.Sequence.Bindings[0].Locator, kBone );

    const Timeline::TransformChannel* a = ChannelOf( linear );
    const Timeline::TransformChannel* b = ChannelOf( cubic );
    ASSERT_NE( a, nullptr );
    ASSERT_NE( b, nullptr );

    const Timeline::FloatChannel* aComponents[3] = { &a->Translation.X, &a->Translation.Y, &a->Translation.Z };
    const Timeline::FloatChannel* bComponents[3] = { &b->Translation.X, &b->Translation.Y, &b->Translation.Z };
    for ( int c = 0; c < 3; ++c )
    {
        const auto& ak = aComponents[c]->Keys;
        const auto& bk = bComponents[c]->Keys;
        ASSERT_EQ( ak.size(), 3u ) << "three keys: up to a peak and back (component " << c << ")";
        ASSERT_EQ( bk.size(), ak.size() );

        for ( std::size_t i = 0; i < ak.size(); ++i )
        {
            EXPECT_EQ( ak[i].Tick.Value, bk[i].Tick.Value )
                 << "key " << i << " is at a different time in the two clips";
            EXPECT_EQ( ak[i].Value, bk[i].Value ) << "key " << i << " holds a different value in the two clips";
            EXPECT_EQ( ak[i].ArriveTangent, bk[i].ArriveTangent );
            EXPECT_EQ( ak[i].LeaveTangent, bk[i].LeaveTangent );
            EXPECT_EQ( ak[i].Mode, bk[i].Mode );

            // THE ONE FIELD. If this ever stops differing, the frames taken of these two scenes stop being
            // about interpolation and nothing else in the protocol would notice.
            EXPECT_EQ( ak[i].Interp, Desert::Animation::KeyInterp::Linear );
            EXPECT_EQ( bk[i].Interp, Desert::Animation::KeyInterp::Cubic );
        }
    }
}

// ---------------------------------------------------------------- 2. on a key

TEST( KeyInterpolationWitness, OnAKeyTheTwoClipsGiveTheSamePose )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto linear = Built( "A6Curve_Linear" );
    const auto cubic  = Built( "A6Curve_Cubic" );

    // A curve that does not pass through its own keys is not an interpolation. This is also the assertion
    // the frame protocol rests on: the captured frame that lands on the middle key must be pixel-identical
    // between the two scenes, and it is.
    for ( const int32_t tick : { 0, 24000, 48000 } )
    {
        EXPECT_FLOAT_EQ( HeightAtTick( linear, tick ), HeightAtTick( cubic, tick ) ) << "at tick " << tick;
    }
    EXPECT_FLOAT_EQ( HeightAtTick( linear, 24000 ), 260.0F ) << "the peak is not where the file says";
}

// ---------------------------------------------------------------- 3. between keys

TEST( KeyInterpolationWitness, BetweenKeysTheyDifferAndTheCubicStaysInsideItsKeys )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto linear = Built( "A6Curve_Linear" );
    const auto cubic  = Built( "A6Curve_Cubic" );

    // Quarter of the way down from the peak. A straight line is at 232.5; a cubic easing out of a flat
    // extremum is still high. If these ever agree, the pair has stopped being an instrument.
    const float straight = HeightAtTick( linear, 30000 );
    const float curved   = HeightAtTick( cubic, 30000 );
    EXPECT_NEAR( straight, 232.5F, 0.1F );
    EXPECT_GT( curved - straight, 5.0F )
         << "the cubic is only " << ( curved - straight ) << " cm from the straight line here";

    // AND IT NEVER SAILS PAST THE PEAK. The auto pass flattens an extremum precisely so that this holds,
    // and it is the property an animator notices before any other.
    float peak = 0.0F;
    for ( int32_t tick = 0; tick <= 48000; tick += 100 )
    {
        peak = std::max( peak, HeightAtTick( cubic, tick ) );
    }
    EXPECT_NEAR( peak, 260.0F, 0.01F ) << "the cubic reached " << peak << " cm above a 260 cm key";
}

TEST( KeyInterpolationWitness, TheAutoPassFlattensThePeakOfThisVeryClip )
{
    ASSERT_FALSE( RepoRoot().empty() );

    // The files ship with zero tangents, because `Auto` means "computed", and the pass is what computes
    // them. Running it here proves the shipped data and the rule agree about this clip rather than about
    // an example written next to it.
    auto                        clip    = Built( "A6Curve_Cubic" );
    Timeline::TransformChannel* channel = ChannelOf( clip );
    ASSERT_NE( channel, nullptr );
    Desert::Animation::RefreshTangents( *channel, clip.Sequence.TickRate );

    const auto& keys = channel->Translation.Y.Keys;
    ASSERT_EQ( keys.size(), 3u );
    EXPECT_FLOAT_EQ( keys[0].LeaveTangent, 0.0F ) << "the first key is not flat";
    EXPECT_FLOAT_EQ( keys[1].ArriveTangent, 0.0F ) << "the peak is not flat";
    EXPECT_FLOAT_EQ( keys[1].LeaveTangent, 0.0F );
    EXPECT_FLOAT_EQ( keys[2].ArriveTangent, 0.0F ) << "the last key is not flat";
}

// ---------------------------------------------------------------- 4. inserting into this very clip

TEST( KeyInterpolationWitness, InsertingAKeyAtThePlayheadRecordsTheCurveAndNotTheOrigin )
{
    ASSERT_FALSE( RepoRoot().empty() );

    // THE HOLE A MUTATION FOUND. This suite read the shipped clips and asserted their shape, and it was
    // green against a build whose "Add Key @ Playhead" inserted `glm::vec3( 0.0f )` — a position key AT
    // THE ORIGIN, which yanks the bone across the scene. Reading data cannot test an operation.
    auto clip = Built( "A6Curve_Cubic" );
    {
        Timeline::TransformChannel* channel = ChannelOf( clip );
        ASSERT_NE( channel, nullptr );
        Desert::Animation::RefreshTangents( *channel, clip.Sequence.TickRate );
    }

    constexpr int32_t AT       = 30000; // a quarter of the way down from the peak
    const float       expected = HeightAtTick( clip, AT );
    EXPECT_GT( expected, 200.0F ) << "the probe point is not on the interesting part of the curve";

    // The Sequence edit the Sequencer's button calls — the key lands in the section that covers the tick.
    const auto inserted = Desert::Animation::InsertBoneKey(
         clip.Sequence, kBone, Desert::Animation::TrackChannel::Position, Desert::Animation::FrameNumber{ AT } );
    ASSERT_TRUE( inserted.IsSuccess() ) << inserted.GetError();

    const Timeline::TransformChannel* channel = ChannelOf( clip );
    ASSERT_NE( channel, nullptr );
    ASSERT_EQ( channel->Translation.Y.Keys.size(), 4u );
    EXPECT_FLOAT_EQ( HeightAtTick( clip, AT ), expected )
         << "the pose moved at the very tick the animator asked to record";

    // The new key is USER and carries the slope it was seeded with: an auto pass must not immediately
    // replace it with what the neighbours imply, which is what would move the pose.
    const auto& key = channel->Translation.Y.Keys[2];
    EXPECT_EQ( key.Tick.Value, AT );
    EXPECT_EQ( key.Mode, Desert::Animation::TangentMode::User );
    EXPECT_LT( key.LeaveTangent, 0.0F ) << "the curve is falling here and the seed did not follow it";

    // Inserting again on the same tick is refused: silently overwriting the key an animator is standing on
    // is not what a button called "add" does.
    EXPECT_FALSE( Desert::Animation::InsertBoneKey( clip.Sequence, kBone,
                                                    Desert::Animation::TrackChannel::Position,
                                                    Desert::Animation::FrameNumber{ AT } )
                       .IsSuccess() );
}

// ------------------------------------------------- 5. the OTHER button, the one on the lane

TEST( KeyInterpolationWitness, TheFirstKeyOfAnEmptyChannelRecordsThePoseAndNotTheOrigin )
{
    // THE SAME DEFECT, ELEVEN LINES BELOW THE ONE THAT WAS FIXED. "Add Key @ Playhead" in the inspector
    // was corrected to record the curve; the per-lane "+" beside every channel still pushed
    // `glm::vec3( 0.0f )`, an identity quaternion and a scale of 1. A populated channel has a curve to
    // read, but the lane button's stated purpose is an EMPTY channel — and there the honest answer is the
    // pose on screen, which is the one thing neither `InsertKeyFromCurve` nor the old code could give.
    Timeline::TransformChannel channel;

    glm::mat4 pose = glm::translate( glm::mat4( 1.0F ), glm::vec3( 11.0F, 222.0F, -33.0F ) );
    pose           = glm::rotate( pose, glm::radians( 40.0F ), glm::vec3( 0.0F, 1.0F, 0.0F ) );
    pose           = glm::scale( pose, glm::vec3( 2.0F, 2.0F, 2.0F ) );

    // Empty channels: all three record the pose.
    ASSERT_TRUE( Desert::Animation::InsertFirstKeyFromPose( channel, Desert::Animation::TrackChannel::Position,
                                                            Desert::Animation::FrameNumber{ 0 }, pose ) );
    ASSERT_TRUE( Desert::Animation::InsertFirstKeyFromPose( channel, Desert::Animation::TrackChannel::Scale,
                                                            Desert::Animation::FrameNumber{ 0 }, pose ) );
    ASSERT_TRUE( Desert::Animation::InsertFirstKeyFromPose( channel, Desert::Animation::TrackChannel::Rotation,
                                                            Desert::Animation::FrameNumber{ 0 }, pose ) );

    ASSERT_EQ( channel.Translation.Y.Keys.size(), 1u );
    ASSERT_EQ( channel.Scale.X.Keys.size(), 1u );
    ASSERT_EQ( channel.Rotation.Y.Keys.size(), 1u );
    EXPECT_NEAR( channel.Translation.Y.Keys[0].Value, 222.0F, 1e-3F )
         << "the first key of an empty channel is at the origin again";
    EXPECT_NEAR( channel.Scale.X.Keys[0].Value, 2.0F, 1e-3F ) << "the first scale key flattened the pose to 1";
    EXPECT_GT( std::abs( channel.Rotation.Y.Keys[0].Value ), 0.1F )
         << "the first rotation key is the identity and the bone snapped upright";

    // A POPULATED channel is refused: this function decomposes a matrix, and letting it run over authored
    // keys would overwrite them with one pose. That channel's operation is InsertKeyFromCurve.
    EXPECT_FALSE( Desert::Animation::InsertFirstKeyFromPose( channel, Desert::Animation::TrackChannel::Position,
                                                             Desert::Animation::FrameNumber{ 24000 }, pose ) );
    EXPECT_EQ( channel.Translation.Y.Keys.size(), 1u );
}

// ------------------------------------------- 6. the seam the curve view edits through

TEST( KeyInterpolationWitness, LiftAndApplyAreOneStatementOfWhatAChannelIs )
{
    Timeline::TransformChannel channel;
    for ( int i = 0; i < 3; ++i )
    {
        PushTranslationKey( channel, i * 24000,
                            glm::vec3( static_cast<float>( i ), 100.0F * static_cast<float>( i ), -3.0F ),
                            Desert::Animation::KeyInterp::Cubic );
    }
    Desert::Animation::RefreshTangents( channel, Desert::Animation::PROJECT_TICK_RATE );

    // The lift is the SAME one the auto pass uses, so the tangents it reports are the ones in the keys.
    const auto  lifted = Desert::Animation::LiftChannel( channel, Desert::Animation::TrackChannel::Position, 1 );
    const auto& y      = channel.Translation.Y.Keys;
    ASSERT_EQ( lifted.size(), y.size() );
    for ( std::size_t i = 0; i < lifted.size(); ++i )
    {
        EXPECT_FLOAT_EQ( lifted[i].Value, y[i].Value );
        EXPECT_FLOAT_EQ( lifted[i].LeaveTangent, y[i].LeaveTangent );
        EXPECT_EQ( lifted[i].Tick.Value, y[i].Tick.Value );
    }

    // Round trip: apply what was lifted and nothing moves.
    const Timeline::VectorChannel before = channel.Translation;
    ASSERT_TRUE(
         Desert::Animation::ApplyChannel( channel, Desert::Animation::TrackChannel::Position, 1, lifted ) );
    for ( std::size_t i = 0; i < before.Y.Keys.size(); ++i )
    {
        EXPECT_FLOAT_EQ( channel.Translation.Y.Keys[i].Value, before.Y.Keys[i].Value );
        EXPECT_FLOAT_EQ( channel.Translation.X.Keys[i].Value, before.X.Keys[i].Value )
             << "another component moved";
    }

    // An edited value lands on the key it came from, and ONLY on that component.
    auto edited     = lifted;
    edited[1].Value = 777.0F;
    ASSERT_TRUE(
         Desert::Animation::ApplyChannel( channel, Desert::Animation::TrackChannel::Position, 1, edited ) );
    EXPECT_FLOAT_EQ( channel.Translation.Y.Keys[1].Value, 777.0F );
    EXPECT_FLOAT_EQ( channel.Translation.X.Keys[1].Value, 1.0F );
    EXPECT_FLOAT_EQ( channel.Translation.Z.Keys[1].Value, -3.0F );
}

TEST( KeyInterpolationWitness, ApplyChannelRefusesAShapeItDoesNotMatchInsteadOfWritingWhatFits )
{
    Timeline::TransformChannel channel;
    for ( int i = 0; i < 3; ++i )
    {
        PushTranslationKey( channel, i * 24000, glm::vec3( 5.0F ), Desert::Animation::KeyInterp::Linear );
    }

    // THE MIDDLE LINK OF A CHAIN IS WHERE THIS PROJECT KEEPS LOSING THINGS. A curve view that inserted a
    // key into its working copy and wrote it back would otherwise overwrite N keys with N+1 values, and both
    // ends of the chain would still look right.
    auto shorter = Desert::Animation::LiftChannel( channel, Desert::Animation::TrackChannel::Position, 0 );
    shorter.pop_back();
    EXPECT_FALSE(
         Desert::Animation::ApplyChannel( channel, Desert::Animation::TrackChannel::Position, 0, shorter ) );

    auto retimed     = Desert::Animation::LiftChannel( channel, Desert::Animation::TrackChannel::Position, 0 );
    retimed[2].Tick  = Desert::Animation::FrameNumber{ 999 };
    retimed[2].Value = 42.0F;
    EXPECT_FALSE(
         Desert::Animation::ApplyChannel( channel, Desert::Animation::TrackChannel::Position, 0, retimed ) )
         << "a retime went through an operation that only moves values";
    EXPECT_FLOAT_EQ( channel.Translation.X.Keys[2].Value, 5.0F ) << "the refusal still wrote something";
    EXPECT_EQ( channel.Translation.X.Keys[2].Tick.Value, 48000 ) << "the refusal still retimed something";

    // Rotation has no curve view and no scalar lift — the refusal is the answer, not an empty gap.
    EXPECT_TRUE( Desert::Animation::LiftChannel( channel, Desert::Animation::TrackChannel::Rotation, 0 ).empty() );
    EXPECT_FALSE( Desert::Animation::ApplyChannel( channel, Desert::Animation::TrackChannel::Rotation, 0, {} ) );
    EXPECT_TRUE( Desert::Animation::LiftChannel( channel, Desert::Animation::TrackChannel::Position, 3 ).empty() );
}

// THE AUTO PASS'S PROMISE HOLDS ON A ONE-KEY CHANNEL TOO.
//
// `AutoSetTangents`' header says "`User` and `Break` keys are left exactly as they are", and it honours
// that — but `RefreshTangents` short-circuits a channel of fewer than two keys BEFORE reaching it, and
// that short-circuit used to zero every tangent regardless of mode. It was invisible by construction: a
// one-key channel is sampled as a constant, so its tangents do nothing until a SECOND key arrives, and by
// then the authored slope is long gone with no edit to blame it on. Found by T5.3's re-key test.
TEST( KeyInterpolationWitness, RefreshingTangentsLeavesAUserKeyAloneEvenWhenItIsTheOnlyOne )
{
    Timeline::TransformChannel channel;
    PushTranslationKey( channel, 0, glm::vec3( 1.0F, 2.0F, 3.0F ), Desert::Animation::KeyInterp::Cubic,
                        Desert::Animation::TangentMode::User, glm::vec3( 5.0F, 6.0F, 7.0F ),
                        glm::vec3( -5.0F, -6.0F, -7.0F ) );

    Desert::Animation::RefreshTangents( channel, Desert::Animation::PROJECT_TICK_RATE );

    ASSERT_EQ( channel.Translation.Y.Keys.size(), 1U );
    EXPECT_FLOAT_EQ( channel.Translation.Y.Keys[0].ArriveTangent, 6.0F );
    EXPECT_FLOAT_EQ( channel.Translation.Y.Keys[0].LeaveTangent, -6.0F );

    // The positive control: an `Auto` key in the same position IS flattened, so the guard above is a mode
    // check and not a refusal to do the job.
    channel.Translation.Y.Keys[0].Mode          = Desert::Animation::TangentMode::Auto;
    channel.Translation.Y.Keys[0].ArriveTangent = 6.0F;
    Desert::Animation::RefreshTangents( channel, Desert::Animation::PROJECT_TICK_RATE );
    EXPECT_FLOAT_EQ( channel.Translation.Y.Keys[0].ArriveTangent, 0.0F );
}

// REMOVING A KEY RESHAPES ITS NEIGHBOURS' AUTO TANGENTS. An `Auto` tangent is a function of the keys beside
// it, so deleting one of them changes the answer; a RemoveKey that kept the old tangents would leave a curve
// shaped by a key that no longer exists (and nothing else in the suites noticed when the refresh was cut).
TEST( KeyInterpolationWitness, RemovingAKeyRefreshesTheAutoTangentsItLeaves )
{
    Timeline::TransformChannel channel;
    const float                heights[4] = { 0.0F, 100.0F, 300.0F, 600.0F };
    for ( int i = 0; i < 4; ++i )
    {
        PushTranslationKey( channel, i * 24000, glm::vec3( 0.0F, heights[i], 0.0F ),
                            Desert::Animation::KeyInterp::Cubic );
    }
    Desert::Animation::RefreshTangents( channel, Desert::Animation::PROJECT_TICK_RATE );
    const float before = channel.Translation.Y.Keys[1].LeaveTangent;

    ASSERT_TRUE( Desert::Animation::RemoveKey( channel, Desert::Animation::TrackChannel::Position,
                                               Desert::Animation::FrameNumber{ 48000 },
                                               Desert::Animation::PROJECT_TICK_RATE ) );
    ASSERT_EQ( channel.Translation.Y.Keys.size(), 3u );

    // The reference: the same three keys with the auto pass run on them explicitly.
    Timeline::TransformChannel reference = channel;
    Desert::Animation::RefreshTangents( reference, Desert::Animation::PROJECT_TICK_RATE );
    EXPECT_NE( reference.Translation.Y.Keys[1].LeaveTangent, before )
         << "the instrument is blind: this removal does not change the neighbour's tangent";
    for ( std::size_t i = 0; i < 3; ++i )
    {
        EXPECT_FLOAT_EQ( channel.Translation.Y.Keys[i].ArriveTangent,
                         reference.Translation.Y.Keys[i].ArriveTangent )
             << "key " << i << " kept a tangent shaped by the removed key";
        EXPECT_FLOAT_EQ( channel.Translation.Y.Keys[i].LeaveTangent, reference.Translation.Y.Keys[i].LeaveTangent )
             << "key " << i;
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

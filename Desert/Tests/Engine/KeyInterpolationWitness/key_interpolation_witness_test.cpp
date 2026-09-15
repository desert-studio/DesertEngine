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

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/TrackEditing.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace Ser = Desert::Assets::Serialization;

namespace
{
    constexpr const char* kCookedDir = "Editor/Cooked/Meshes/";

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

    Ser::AnimationAssetData Load( const char* stem )
    {
        const std::ifstream in( RepoRoot() + kCookedDir + stem + ".anim", std::ios::binary );
        EXPECT_TRUE( in.good() ) << stem;
        std::ostringstream text;
        text << in.rdbuf();
        auto data = rfl::json::read<Ser::AnimationAssetData, rfl::DefaultIfMissing>( text.str() );
        EXPECT_TRUE( data.has_value() ) << stem;
        return data.has_value() ? data.value() : Ser::AnimationAssetData{};
    }

    Desert::Animation::AnimationClip Built( const char* stem )
    {
        auto built = Ser::BuildClipFromAssetData( Load( stem ) );
        EXPECT_TRUE( built ) << stem << ": " << ( built ? "" : built.GetError() );
        return built ? built.ExtractValue() : Desert::Animation::AnimationClip{};
    }

    float HeightAtTick( const Desert::Animation::AnimationClip& clip, int32_t tick )
    {
        EXPECT_EQ( clip.Tracks.size(), 1u );
        return clip.Tracks[0]
             .GetInterpolatedPosition( Desert::Animation::FrameTime{ Desert::Animation::FrameNumber{ tick },
                                                                     0.0F },
                                       clip.TickRate )
             .y;
    }
} // namespace

// ---------------------------------------------------------------- 1. one field apart

TEST( KeyInterpolationWitness, TheTwoClipsDifferInExactlyOneFieldPerKey )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository root from the working directory";

    const Ser::AnimationAssetData linear = Load( "A6Curve_Linear" );
    const Ser::AnimationAssetData cubic  = Load( "A6Curve_Cubic" );

    EXPECT_EQ( linear.Version, Ser::kAnimationVersion );
    EXPECT_EQ( cubic.Version, Ser::kAnimationVersion );
    EXPECT_EQ( linear.DurationTicks, cubic.DurationTicks );
    EXPECT_EQ( linear.SkeletonSignature, cubic.SkeletonSignature );
    ASSERT_EQ( linear.Channels.size(), 1u );
    ASSERT_EQ( cubic.Channels.size(), linear.Channels.size() );
    EXPECT_EQ( linear.Channels[0].BoneName, cubic.Channels[0].BoneName );

    const auto& a = linear.Channels[0].Positions;
    const auto& b = cubic.Channels[0].Positions;
    ASSERT_EQ( a.size(), 3u ) << "three keys: up to a peak and back";
    ASSERT_EQ( b.size(), a.size() );

    for ( std::size_t i = 0; i < a.size(); ++i )
    {
        EXPECT_EQ( a[i].Tick, b[i].Tick ) << "key " << i << " is at a different time in the two clips";
        EXPECT_EQ( a[i].Value, b[i].Value ) << "key " << i << " holds a different value in the two clips";
        EXPECT_EQ( a[i].ArriveTangent, b[i].ArriveTangent );
        EXPECT_EQ( a[i].LeaveTangent, b[i].LeaveTangent );
        EXPECT_EQ( a[i].Shape.Mode, b[i].Shape.Mode );

        // THE ONE FIELD. If this ever stops differing, the frames taken of these two scenes stop being
        // about interpolation and nothing else in the protocol would notice.
        EXPECT_EQ( a[i].Shape.Interp, static_cast<int>( Desert::Animation::KeyInterp::Linear ) );
        EXPECT_EQ( b[i].Shape.Interp, static_cast<int>( Desert::Animation::KeyInterp::Cubic ) );
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
    EXPECT_GT( curved - straight, 5.0F ) << "the cubic is only " << ( curved - straight )
                                         << " cm from the straight line here";

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
    auto clip = Built( "A6Curve_Cubic" );
    ASSERT_EQ( clip.Tracks.size(), 1u );
    Desert::Animation::RefreshTangents( clip.Tracks[0], clip.TickRate );

    const auto& keys = clip.Tracks[0].PositionKeys;
    ASSERT_EQ( keys.size(), 3u );
    EXPECT_FLOAT_EQ( keys[0].LeaveTangent.y, 0.0F ) << "the first key is not flat";
    EXPECT_FLOAT_EQ( keys[1].ArriveTangent.y, 0.0F ) << "the peak is not flat";
    EXPECT_FLOAT_EQ( keys[1].LeaveTangent.y, 0.0F );
    EXPECT_FLOAT_EQ( keys[2].ArriveTangent.y, 0.0F ) << "the last key is not flat";
}

// ---------------------------------------------------------------- 4. inserting into this very clip

TEST( KeyInterpolationWitness, InsertingAKeyAtThePlayheadRecordsTheCurveAndNotTheOrigin )
{
    ASSERT_FALSE( RepoRoot().empty() );

    // THE HOLE A MUTATION FOUND. This suite read the shipped clips and asserted their shape, and it was
    // green against a build whose "Add Key @ Playhead" inserted `glm::vec3( 0.0f )` — a position key AT
    // THE ORIGIN, which yanks the bone across the scene. Reading data cannot test an operation.
    auto clip = Built( "A6Curve_Cubic" );
    ASSERT_EQ( clip.Tracks.size(), 1u );
    Desert::Animation::RefreshTangents( clip.Tracks[0], clip.TickRate );

    constexpr int32_t AT       = 30000; // a quarter of the way down from the peak
    const float       expected = HeightAtTick( clip, AT );
    EXPECT_GT( expected, 200.0F ) << "the probe point is not on the interesting part of the curve";

    ASSERT_TRUE( Desert::Animation::InsertKeyFromCurve( clip.Tracks[0],
                                                        Desert::Animation::TrackChannel::Position,
                                                        Desert::Animation::FrameNumber{ AT },
                                                        clip.TickRate ) );

    ASSERT_EQ( clip.Tracks[0].PositionKeys.size(), 4u );
    EXPECT_FLOAT_EQ( HeightAtTick( clip, AT ), expected )
         << "the pose moved at the very tick the animator asked to record";

    // The new key is USER and carries the slope it was seeded with: an auto pass must not immediately
    // replace it with what the neighbours imply, which is what would move the pose.
    const auto& inserted = clip.Tracks[0].PositionKeys[2];
    EXPECT_EQ( inserted.Tick.Value, AT );
    EXPECT_EQ( inserted.Mode, Desert::Animation::TangentMode::User );
    EXPECT_LT( inserted.LeaveTangent.y, 0.0F ) << "the curve is falling here and the seed did not follow it";

    // Inserting again on the same tick is refused: silently overwriting the key an animator is standing on
    // is not what a button called "add" does.
    EXPECT_FALSE( Desert::Animation::InsertKeyFromCurve( clip.Tracks[0],
                                                         Desert::Animation::TrackChannel::Position,
                                                         Desert::Animation::FrameNumber{ AT },
                                                         clip.TickRate ) );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

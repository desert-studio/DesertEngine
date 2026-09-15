// THE REPOSITORY'S ANIMATION CLIPS, PARSED AND PINNED — the data half of D34.
//
// WHY THE CORPUS EXISTS AT ALL. Until 2026-09-09 this repository contained NOT ONE `.anim` file. The clip
// half of the animation system — the scan, the library, the rig match, the Animator, the skinning
// matrices — had therefore never been exercised end to end from disk by anything a person could run, and
// that is what let the animation library go UNFILLED in the packaged game for as long as it did: every
// character T-posed and no frame in the tree could show it, because no frame in the tree had a clip to
// play. It is the same hole the skinned and static mesh probes were added to close, one content kind
// later (see .gitignore's four deliberate exceptions under Editor/Cooked/Meshes).
//
// WHAT THIS SUITE IS FOR, and it is not "the format parses". It pins the three properties that make the
// corpus USABLE AS AN INSTRUMENT, each of which can be destroyed by an edit that still parses:
//
//   1. The clips must claim the rig the shipped probe skeleton actually has. A signature typo turns every
//      frame taken against them into a picture of a bind pose, and the frame still renders — which is the
//      worst kind of broken evidence, because it looks like evidence.
//   2. The clips must MOVE something, by a lot. A corpus whose keys are all the bind value is a corpus
//      that proves an animation system works while it does nothing at all. So the travel is asserted, in
//      centimetres and in radians, against a floor far above rounding.
//   3. Foreign_Hips must NOT drive the probe rig. A positive control alone cannot tell "the match rule
//      works" from "the match rule says yes to everything", and this project has already paid for a
//      match rule that was wrong in one direction (see ClipSkeletonMatch.hpp).
//
// The files are read from the repository rather than written into a temp directory: the point is these
// exact bytes, the ones a frame is taken against, the way Desert/Tests/Engine/StaticMeshCooked reads the
// static probe next door.

#include <gtest/gtest.h>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/ClipSkeletonMatch.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    constexpr const char* kCorpusDir = "Editor/Cooked/Meshes/";
    constexpr const char* kProbeRig  = "Editor/Cooked/Meshes/SkinProbe.skeleton";

    // The probe rig's one bone. Named here as well as read from the file so that a corpus clip pointing at
    // a bone the rig does not have is a failure with a readable message rather than a silent non-match.
    constexpr const char* kProbeBoneName = "Root";

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + kProbeRig );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // The clip exactly as AnimationAsset::Load builds it: the same reader with the same DefaultIfMissing
    // policy, then the same pure build step. Anything this suite accepts, the engine accepts.
    Desert::Animation::AnimationClip LoadClip( const std::string& stem )
    {
        const std::string path = RepoRoot() + kCorpusDir + stem + ".anim";
        const std::string raw  = ReadFile( path );
        EXPECT_FALSE( raw.empty() ) << "could not read the corpus clip " << path;

        const auto data =
             rfl::json::read<Desert::Assets::Serialization::AnimationAssetData, rfl::DefaultIfMissing>( raw );
        EXPECT_TRUE( data.has_value() ) << path << ": " << ( data.has_value() ? "" : data.error().what() );
        if ( !data.has_value() )
            return {};

        auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data.value() );
        EXPECT_TRUE( built.IsSuccess() ) << path << ": " << ( built.IsSuccess() ? "" : built.GetError() );
        if ( !built.IsSuccess() )
            return {};
        return built.ExtractValue();
    }

    // The rig is built from the SHIPPED file, and its signature is therefore recomputed by
    // Skeleton::ComputeSignature rather than read out of the file's own `Signature` field. That is
    // deliberate: the clips are checked against the number the ENGINE derives for this rig, which is the
    // number the match rule uses.
    Desert::Animation::Skeleton ProbeRig()
    {
        const std::string raw = ReadFile( RepoRoot() + kProbeRig );
        EXPECT_FALSE( raw.empty() ) << "could not read " << kProbeRig;

        auto data =
             rfl::json::read<Desert::Assets::Serialization::SkeletonAssetData, rfl::DefaultIfMissing>( raw );
        EXPECT_TRUE( data.has_value() );

        std::vector<Desert::Animation::BoneInfo> bones;
        if ( data.has_value() )
            bones = data.value().Bones;
        return Desert::Animation::Skeleton( std::move( bones ) );
    }

    Desert::Animation::ClipRigIdentity IdentityOf( const Desert::Animation::AnimationClip& clip )
    {
        Desert::Animation::ClipRigIdentity id;
        id.ClipName          = clip.AnimationName;
        id.SkeletonSignature = clip.SkeletonSignature;
        for ( const auto& track : clip.Tracks )
            if ( !track.BoneName.empty() )
                id.AnimatedBones.push_back( track.BoneName );
        return id;
    }

    const Desert::Animation::BoneTrack* TrackFor( const Desert::Animation::AnimationClip& clip,
                                                  const std::string&                      bone )
    {
        const auto it =
             std::find_if( clip.Tracks.begin(), clip.Tracks.end(),
                           [&]( const Desert::Animation::BoneTrack& t ) { return t.BoneName == bone; } );
        return it == clip.Tracks.end() ? nullptr : &*it;
    }
} // namespace

// PROPERTY 1: the two probe clips claim the rig the shipped probe skeleton computes for itself. Asserted
// as an EQUALITY BETWEEN TWO FILES rather than against a literal, because a literal typed here would
// happily agree with a clip and disagree with the rig.
TEST( AnimationClipCorpus, TheProbeClipsClaimTheRigTheProbeSkeletonHas )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository root from the working directory";

    const auto rig = ProbeRig();
    ASSERT_EQ( rig.GetBones().size(), 1u ) << "the probe rig is one bone; the corpus was authored for it";
    EXPECT_EQ( rig.GetBones()[0].Name, kProbeBoneName );

    for ( const char* stem : { "SkinProbe_Hover", "SkinProbe_Tilt" } )
    {
        const auto clip = LoadClip( stem );
        EXPECT_EQ( clip.SkeletonSignature, rig.GetSignature() )
             << stem
             << " claims a different rig from SkinProbe.skeleton. Every frame taken against it would show a "
                "bind pose and still render, which is broken evidence rather than no evidence.";
        // A5 REPLACED THE SENTENCE THAT USED TO STAND HERE, and the old one is worth quoting because it
        // was the fiction itself: "Key times ARE seconds when TicksPerSecond is 1, and the whole corpus is
        // authored that way." The field was called ticks-per-second and pinned to 1 so that a tick would
        // mean a second — and this suite defended that. A clip now states the grid its integer ticks are
        // counted on, and two seconds is a number of them.
        EXPECT_EQ( clip.TickRate, Desert::Animation::PROJECT_TICK_RATE )
             << stem << " is not on the project tick grid, so its key times are not comparable with any "
                        "other clip's.";
        EXPECT_EQ( clip.DurationTicks.Value, 2 * Desert::Animation::PROJECT_TICK_RATE.Numerator )
             << stem << " is no longer the 2 s cycle the probe scene and the shot frame counts are chosen "
                        "against.";
        ASSERT_NE( TrackFor( clip, kProbeBoneName ), nullptr )
             << stem << " has no track for '" << kProbeBoneName
             << "'. The bone name is the only key playback binds on, so it would animate nothing.";
    }
}

// PROPERTY 2: THE CORPUS ACTUALLY MOVES SOMETHING. A clip whose keys never leave the bind value is the
// exact shape of a test instrument that certifies a dead system, and it parses perfectly.
TEST( AnimationClipCorpus, TheProbeClipsTravelFarEnoughToBeSeenInAFrame )
{
    ASSERT_FALSE( RepoRoot().empty() );

    // Hover: the root rises and comes back. 1 world unit = 1 cm, and the probe box is 100 cm, so a peak of
    // 200 cm is two box heights -- unmistakable in a frame rather than a subpixel argument.
    {
        const auto  clip  = LoadClip( "SkinProbe_Hover" );
        const auto* track = TrackFor( clip, kProbeBoneName );
        ASSERT_NE( track, nullptr );
        ASSERT_GE( track->PositionKeys.size(), 5u ) << "too few position keys to describe a cycle";

        float lowest = track->PositionKeys.front().Position.y;
        float peak   = lowest;
        for ( const auto& key : track->PositionKeys )
        {
            lowest = std::min( lowest, key.Position.y );
            peak   = std::max( peak, key.Position.y );
        }
        EXPECT_GE( peak - lowest, 150.0f )
             << "SkinProbe_Hover travels only " << ( peak - lowest )
             << " cm. It is the corpus's positive control for translation and has to be visible.";

        // Loopable: the endpoints agree, or a looping clip snaps every cycle and the frame at the wrap is a
        // picture of the discontinuity rather than of the pose.
        EXPECT_NEAR( track->PositionKeys.front().Position.y, track->PositionKeys.back().Position.y, 1e-3f );
    }

    // Tilt: one full turn about Z and no translation at all. A DIFFERENT KIND of motion on the same rig, so
    // a frame showing both proves which clip played rather than only that something played.
    {
        const auto  clip  = LoadClip( "SkinProbe_Tilt" );
        const auto* track = TrackFor( clip, kProbeBoneName );
        ASSERT_NE( track, nullptr );
        ASSERT_GE( track->RotationKeys.size(), 5u ) << "too few rotation keys to describe a turn";

        // The largest angle between any key and the first one, which for a full turn passes through pi.
        const glm::quat first  = track->RotationKeys.front().Rotation;
        float           widest = 0.0f;
        for ( const auto& key : track->RotationKeys )
        {
            const float dot   = std::clamp( std::fabs( glm::dot( first, key.Rotation ) ), 0.0f, 1.0f );
            const float angle = 2.0f * std::acos( dot );
            widest            = std::max( widest, angle );
        }
        EXPECT_GE( widest, 1.5f ) << "SkinProbe_Tilt turns only " << widest
                                  << " rad away from its start. It is the corpus's positive control for "
                                     "rotation; a cube barely turned is a cube that looks unchanged.";
    }
}

// PROPERTY 3: THE NEGATIVE CONTROL. `Foreign_Hips` is a different rig signature animating a bone name the
// probe does not have, so a library that offers it to this rig is wrong -- and a suite with only positive
// controls cannot tell a working match rule from one that says yes to everything.
TEST( AnimationClipCorpus, TheForeignClipIsRefusedForTheProbeRig )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto rig      = Desert::Animation::IdentifyRig( ProbeRig() );
    const auto foreign  = LoadClip( "Foreign_Hips" );
    const auto identity = IdentityOf( foreign );

    ASSERT_FALSE( identity.AnimatedBones.empty() );
    EXPECT_NE( identity.SkeletonSignature, rig.Signature )
         << "Foreign_Hips claims the probe rig, so it is no longer a negative control at all.";
    EXPECT_EQ( rig.BoneNames.count( identity.AnimatedBones.front() ), 0u )
         << "Foreign_Hips animates '" << identity.AnimatedBones.front()
         << "', which the probe rig HAS. ClipDrivesRig accepts a clip whose animated bones are mostly "
            "present by name -- deliberately, see ClipSkeletonMatch.hpp -- so a foreign clip sharing bone "
            "names is not foreign. Rename the bone in the corpus file, not the rule.";

    EXPECT_FALSE( Desert::Animation::ClipDrivesRig( identity, rig ) )
         << "the match rule offers Foreign_Hips to the probe rig.";

    // And the two probe clips must still be accepted by the same rule, in the same test: a rule that
    // refuses everything would pass the assertion above.
    for ( const char* stem : { "SkinProbe_Hover", "SkinProbe_Tilt" } )
        EXPECT_TRUE( Desert::Animation::ClipDrivesRig( IdentityOf( LoadClip( stem ) ), rig ) )
             << stem << " is not offered to the rig it names.";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

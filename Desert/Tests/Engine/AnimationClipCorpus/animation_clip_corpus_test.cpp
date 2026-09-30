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
//   1. The clips must name the shipped probe skeleton (its header GUID). A wrong reference turns every
//      frame taken against them into a picture of a bind pose, and the frame still renders — which is the
//      worst kind of broken evidence, because it looks like evidence.
//   2. The clips must MOVE something, by a lot. A corpus whose keys are all the bind value is a corpus
//      that proves an animation system works while it does nothing at all. So the travel is asserted, in
//      centimetres and in radians, against a floor far above rounding.
//   3. A clip on ANOTHER skeleton must NOT play on the probe mesh. A positive control alone cannot tell
//      "the match rule works" from "the match rule says yes to everything". The negative control is built
//      in memory (test data does not live in Editor/), and it animates a bone the probe HAS, so only the
//      skeleton reference can refuse it (ClipPlaysOnMesh, SkeletonReference.hpp).
//
// The files are read from the repository rather than written into a temp directory: the point is these
// exact bytes, the ones a frame is taken against, the way Desert/Tests/Engine/StaticMeshCooked reads the
// static probe next door.

#include <gtest/gtest.h>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/ClipSkeletonMatch.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/SkeletonReference.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <optional>
#include <vector>
#include <cstdio>

#include <Common/Core/Core.hpp>
namespace
{
    constexpr const char* kCorpusDir = "Editor/Resources/Assets/Meshes/Skinned/";
    constexpr const char* kProbeRig  = "Editor/Resources/Assets/Meshes/Skinned/SkinProbe.skeleton";

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

    // THE CORPUS IS WHAT GIT TRACKS, AND ASKING THE DISK WAS WRONG THREE TIMES.
    //
    // This walk began as "every `.anim` under the root" and then grew an exclusion per incident:
    // `/build/` (copies of the sources), then `/.claude/` (other branches' worktrees, each pinned to
    // whatever commit that agent branched from). The third incident is the one that ends the pattern:
    // `Editor/Cooked/Meshes/TwoJointProbe._ArmSwing.anim` is ignored, untracked, and WRITTEN AT RUN TIME
    // BY ANOTHER SUITE. No exclusion list can be complete against a file that does not exist until a
    // sibling test creates it, and the verdict would depend on whether that sibling had run yet — on CI
    // it had not, so this was red on a developer machine and green in the pipeline.
    //
    // So the question is asked of the repository instead of the filesystem. `git ls-files` answers
    // exactly "what a clean clone carries", which is what the sentence "every clip in the repository"
    // meant all along.
    //
    // std::nullopt means THE QUESTION COULD NOT BE ASKED — git missing, or not a checkout — and the
    // caller fails on it rather than reading it as "no clips are tracked". An empty successful answer is
    // the shape this whole class of defect hides behind.
    std::optional<std::vector<std::string>> TrackedClips()
    {
        const std::string command = "git -C \"" + RepoRoot() + ".\" ls-files -z -- \"*.anim\" 2>" +
#if defined( DESERT_PLATFORM_WINDOWS )
                                    std::string( "nul" );
#else
                                    std::string( "/dev/null" );
#endif

#if defined( DESERT_PLATFORM_WINDOWS )
        FILE* pipe = _popen( command.c_str(), "r" );
#else
        FILE* pipe = popen( command.c_str(), "r" );
#endif
        if ( pipe == nullptr )
            return std::nullopt;

        std::string output;
        char        buffer[4096];
        std::size_t read = 0;
        while ( ( read = std::fread( buffer, 1, sizeof( buffer ), pipe ) ) > 0 )
            output.append( buffer, read );

#if defined( DESERT_PLATFORM_WINDOWS )
        if ( _pclose( pipe ) != 0 )
#else
        if ( pclose( pipe ) != 0 )
#endif
            return std::nullopt;

        std::vector<std::string> clips;
        std::string              current;
        for ( const char c : output )
        {
            if ( c == '\0' )
            {
                if ( !current.empty() )
                    clips.push_back( RepoRoot() + current );
                current.clear();
                continue;
            }
            current += c;
        }
        return clips;
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

        const auto data = Common::Json::Read<Desert::Assets::Serialization::AnimationAssetData>( raw );
        EXPECT_TRUE( data.IsSuccess() ) << path << ": " << ( data.IsSuccess() ? "" : data.GetError() );
        if ( !data.IsSuccess() )
            return {};

        auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data.GetValue() );
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

        auto data = Common::Json::Read<Desert::Assets::Serialization::SkeletonAssetData>( raw );
        EXPECT_TRUE( data.IsSuccess() );

        std::vector<Desert::Animation::BoneInfo> bones;
        if ( data.IsSuccess() )
            bones = data.GetValue().Bones;
        return Desert::Animation::Skeleton( std::move( bones ) );
    }

    Desert::Animation::ClipRigIdentity IdentityOf( const Desert::Animation::AnimationClip& clip )
    {
        Desert::Animation::ClipRigIdentity id;
        id.ClipName = clip.AnimationName;
        id.Skeleton = { clip.Skeleton, clip.AnimationName + "'s skeleton" };
        return id;
    }

    // The probe skeleton's identity: the GUID in SkinProbe.skeleton's own header, which is what a clip names.
    Common::Content::AssetGuid ProbeSkeletonGuid()
    {
        const std::string raw  = ReadFile( RepoRoot() + kProbeRig );
        auto              data = Desert::Assets::Serialization::ReadSkeletonJson( raw );
        EXPECT_TRUE( data.IsSuccess() ) << kProbeRig << ": " << ( data.IsSuccess() ? "" : data.GetError() );
        if ( !data.IsSuccess() || !data.GetValue().Header )
            return {};
        auto guid = Common::Content::AssetGuidFromText( data.GetValue().Header->Guid );
        EXPECT_TRUE( guid.IsSuccess() ) << kProbeRig << " has no readable header GUID";
        return guid.IsSuccess() ? guid.GetValue() : Common::Content::AssetGuid{};
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
        EXPECT_FALSE( ProbeSkeletonGuid().IsNull() );
        EXPECT_TRUE( clip.Skeleton == ProbeSkeletonGuid() )
             << stem
             << " names a different skeleton from SkinProbe.skeleton. Every frame taken against it would show a "
                "bind pose and still render, which is broken evidence rather than no evidence.";
        // A5 REPLACED THE SENTENCE THAT USED TO STAND HERE, and the old one is worth quoting because it
        // was the fiction itself: "Key times ARE seconds when TicksPerSecond is 1, and the whole corpus is
        // authored that way." The field was called ticks-per-second and pinned to 1 so that a tick would
        // mean a second — and this suite defended that. A clip now states the grid its integer ticks are
        // counted on, and two seconds is a number of them.
        EXPECT_EQ( clip.TickRate, Desert::Animation::PROJECT_TICK_RATE )
             << stem
             << " is not on the project tick grid, so its key times are not comparable with any "
                "other clip's.";
        EXPECT_EQ( clip.DurationTicks.Value, 2 * Desert::Animation::PROJECT_TICK_RATE.Numerator )
             << stem
             << " is no longer the 2 s cycle the probe scene and the shot frame counts are chosen "
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

// PROPERTY 3: THE NEGATIVE CONTROL, built in memory: a clip that animates the probe's own bone name but names
// a SEPARATE skeleton. Bone names decide nothing at play time any more, so only the reference can refuse it
// -- and the refusal must name both skeletons, or nobody can act on it.
TEST( AnimationClipCorpus, AClipOnAnotherSkeletonIsRefusedForTheProbeMesh )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto rig = ProbeRig();
    ASSERT_FALSE( rig.GetBones().empty() );
    const Desert::Animation::SkeletonAssetRef probe{ ProbeSkeletonGuid(), "SkinProbe" };
    ASSERT_FALSE( probe.Guid.IsNull() );

    Desert::Animation::AnimationClip foreign;
    foreign.AnimationName = "Foreign_Hips";
    foreign.Skeleton      = Common::Content::AssetGuid::Generate();
    Desert::Animation::BoneTrack track;
    track.BoneName = rig.GetBones()[0].Name; // the probe HAS this bone: names must not rescue the clip
    foreign.Tracks.push_back( track );
    ASSERT_FALSE( foreign.Skeleton == probe.Guid );

    const Desert::Animation::SkeletonAssetRef foreignSkeleton{ foreign.Skeleton, "ForeignHipsSkeleton" };
    const auto refused = Desert::Animation::ClipPlaysOnMesh( foreignSkeleton, probe, {} );
    ASSERT_FALSE( refused.IsSuccess() ) << "a clip on another skeleton plays on the probe mesh.";
    EXPECT_NE( refused.GetError().find( "ForeignHipsSkeleton" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "SkinProbe" ), std::string::npos ) << refused.GetError();

    // The picker asks the same rule: the foreign clip is not offered, the two probe clips are.
    std::vector<Desert::Animation::ClipRigIdentity> clips{ IdentityOf( foreign ) };
    for ( const char* stem : { "SkinProbe_Hover", "SkinProbe_Tilt" } )
        clips.push_back( IdentityOf( LoadClip( stem ) ) );
    const auto offered = Desert::Animation::SelectClipsForMesh( clips, { probe, {} } );
    EXPECT_EQ( offered, ( std::vector<size_t>{ 1, 2 } ) )
         << "the picker offers the foreign clip, or refuses a probe clip that names SkinProbe.skeleton.";

    // Listing the foreign skeleton as compatible on the MESH side is the one way to let it play.
    const std::vector<Common::Content::AssetGuid> compatible{ foreign.Skeleton };
    EXPECT_TRUE( Desert::Animation::ClipPlaysOnMesh( foreignSkeleton, probe, compatible ).IsSuccess() );
}

// A CENSUS OVER THE WHOLE REPOSITORY, not over the six names this suite knows. The loader refuses a
// generation-0 `.anim`, which turns a forgotten conversion into a clip that will not load — visible, but
// only to whoever opens the scene that plays it. This walks every `.anim` in the tree instead, so a file
// added or restored at the old generation is caught by a test rather than by a character standing still.
//
// It is deliberately NOT a count: a count is satisfied by editing the count. Each file is read and its
// stated generation checked, and the failure names the file and the tool that converts it.
TEST( AnimationClipCorpus, EveryClipInTheRepositoryIsAtTheCurrentGeneration )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto clips = TrackedClips();
    ASSERT_TRUE( clips.has_value() )
         << "THE CENSUS COULD NOT RUN, which is a different answer from 'every clip is current'. "
            "`git ls-files` failed or "
         << RepoRoot() << " is not a checkout.";

    std::size_t seen = 0;
    for ( const std::string& path : *clips )
    {
        ++seen;
        const auto data =
             Common::Json::Read<Desert::Assets::Serialization::AnimationAssetData>( ReadFile( path ) );
        ASSERT_TRUE( data.IsSuccess() ) << path << " does not parse as a `.anim` at all";
        const int stated =
             Desert::Assets::StatedVersion( data.GetValue().Header, Desert::Assets::kAnimationSchemaTag );
        EXPECT_EQ( stated, Desert::Assets::Serialization::kAnimationVersion )
             << path << " is at `.anim` generation " << stated << " and this build reads "
             << Desert::Assets::Serialization::kAnimationVersion
             << ". Run Tools/SceneMigrator over it: the loader refuses it, so whatever plays it stands "
                "still.";
    }

    // A sweep that found nothing is not a clean sweep — it is a sweep that ran somewhere else.
    EXPECT_GE( seen, 6u ) << "only " << seen << " tracked `.anim` file(s) were found from " << RepoRoot()
                          << "; this census is measuring the wrong tree.";
}

// THE CONDITION THE GENERATION-2 STEP WAS GRANTED ON, MADE CHECKABLE.
//
// A version that changed only the number a file states about ITSELF would be versioning for its own sake.
// What generation 2 buys is that a key SAYS what shape its segment has, instead of inheriting one from
// `rfl::DefaultIfMissing` — and an invented default is indistinguishable from an authored one for ever
// after. So this reads the FILES, not the parsed structs: parsing is exactly the step that would hide a
// missing field by filling it in.
//
// It asserts the PROPERTY over every key of every clip rather than one phrase in one file: the same lie
// eleven lines further down is the failure mode a single-instance pin has.
TEST( AnimationClipCorpus, EveryKeyInEveryClipSTATESItsShapeRatherThanInheritingOne )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto trackedClips = TrackedClips();
    ASSERT_TRUE( trackedClips.has_value() )
         << "THE CENSUS COULD NOT RUN, which is a different answer from 'every key states its shape'. "
            "`git ls-files` failed or this is not a checkout.";

    std::size_t clips = 0;
    std::size_t keys  = 0;
    for ( const std::string& clipPath : *trackedClips )
    {
        ++clips;
        const std::string text = ReadFile( clipPath );
        ASSERT_FALSE( text.empty() ) << clipPath;

        // Every key object in the file must carry a "Shape". Counted rather than searched for once,
        // because one key stating its shape while the other 266 stay silent is exactly the state this
        // census exists to refuse.
        std::size_t shapes = 0;
        for ( std::size_t at = text.find( "\"Shape\"" ); at != std::string::npos;
              at             = text.find( "\"Shape\"", at + 1 ) )
        {
            ++shapes;
        }
        std::size_t ticks = 0;
        for ( std::size_t at = text.find( "\"Tick\"" ); at != std::string::npos;
              at             = text.find( "\"Tick\"", at + 1 ) )
        {
            ++ticks;
        }

        EXPECT_EQ( shapes, ticks ) << clipPath << " has " << ticks << " key(s) and " << shapes
                                   << " stated shape(s). A key whose shape is missing from the file gets "
                                      "one invented by DefaultIfMissing, and an invented default cannot "
                                      "afterwards be told from an authored one.";
        keys += ticks;
    }

    EXPECT_GE( clips, 6u ) << "this census is measuring the wrong tree";
    EXPECT_GE( keys, 200u ) << "only " << keys << " key(s) were seen across " << clips
                            << " clip(s); the corpus is 267";
}

TEST( AnimationClipCorpus, EveryClipInTheRepositorySTATESTheSectionItsValuesAreReadUnder )
{
    // THE CONDITION GENERATION 3 WAS TAKEN ON, and the same one generation 2 was: a step that only
    // changes the number a file states about ITSELF is a relabelling. A generation-3 file says something
    // its predecessor could not — what its values MEAN — and this census reads the shipped bytes back to
    // check that the migration actually wrote it rather than leaving `DefaultIfMissing` to invent it.
    //
    // A clip whose `Sections` were silent would still load, still play and still look right, because an
    // empty list is the identity of the blend. That is exactly why it has to be checked HERE and not by
    // a behavioural test: the failure is invisible in every frame and only visible in the file.
    ASSERT_FALSE( RepoRoot().empty() );

    const auto trackedClips = TrackedClips();
    ASSERT_TRUE( trackedClips.has_value() )
         << "THE CENSUS COULD NOT RUN, which is a different answer from 'every clip states its section'. "
            "`git ls-files` failed or this is not a checkout.";

    std::size_t clips = 0;
    for ( const std::string& clipPath : *trackedClips )
    {
        ++clips;

        const std::string raw = ReadFile( clipPath );
        ASSERT_FALSE( raw.empty() ) << clipPath;
        const auto parsed = Common::Json::Read<Desert::Assets::Serialization::AnimationAssetData>( raw );
        ASSERT_TRUE( parsed.IsSuccess() ) << clipPath;
        const auto& data = parsed.GetValue();

        ASSERT_FALSE( data.Sections.empty() )
             << clipPath
             << " states no section. It would play correctly and say nothing about why — which is the "
                "state this step exists to end.";
        EXPECT_EQ( data.Sections[0].StartTick, 0 ) << clipPath;
        EXPECT_EQ( data.Sections[0].EndTick, data.DurationTicks )
             << clipPath << " has a section that does not reach its own stated length";
        EXPECT_EQ( data.Sections[0].Blend, 0 )
             << clipPath
             << " migrated to something other than Absolute. A migration states the behaviour a file "
                "already had; it does not choose a new one.";
    }

    EXPECT_GE( clips, 6u ) << "this census is measuring the wrong tree";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

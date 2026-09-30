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
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Animation/Timeline/Evaluator.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Common/Content/CanonicalText.hpp>

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

} // namespace

// ANIM v5 (ANIM-I8a): every `.anim` the repository carries is an AnimationClip-hosted TMLN block that reads
// back through the one reader and passes `Validate` — the files, not an in-memory clip.
TEST( AnimationClipCorpus, EveryClipInTheRepositoryIsATimelineClipThatValidates )
{
    using namespace Desert::Assets::Serialization;
    const auto clips = TrackedClips();
    ASSERT_TRUE( clips.has_value() ) << "git ls-files failed from " << RepoRoot();
    std::size_t read = 0;
    for ( const std::string& path : *clips )
    {
        const auto data = ReadAnimationJson( ReadFile( path ) );
        ASSERT_TRUE( data.IsSuccess() ) << path << ": " << data.GetError();
        ASSERT_TRUE( data.GetValue().Header.has_value() ) << path;
        EXPECT_EQ( Common::Content::TextHeaderVersion( *data.GetValue().Header, Desert::Assets::kAnimationSchemaTag ),
                   std::optional<uint32_t>( Desert::Assets::kAnimationSchemaVersion ) )
             << path;
        const auto clip = BuildClipFromAssetData( data.GetValue() );
        ASSERT_TRUE( clip.IsSuccess() ) << path << ": " << clip.GetError();
        const auto& sequence = clip.GetValue().Sequence;
        EXPECT_EQ( sequence.Host, Desert::Animation::Timeline::SequenceHost::AnimationClip ) << path;
        EXPECT_TRUE( Desert::Animation::Timeline::Validate( sequence ).IsSuccess() ) << path;
        EXPECT_FALSE( sequence.Tracks.empty() ) << path << ": a clip that animates nothing";
        ++read;
    }
    EXPECT_GE( read, 10u ) << "too few clips read from " << RepoRoot();
}

// THE ONE WRITER IS THE FORMAT: each file is exactly what BuildAssetDataFromClip + WriteAnimationJson write
// for the clip it holds (header and import record kept). A writer that dropped or reshaped the TMLN block
// turns this red on every file.
TEST( AnimationClipCorpus, EveryClipIsWhatTheOneWriterWritesForIt )
{
    using namespace Desert::Assets::Serialization;
    const auto clips = TrackedClips();
    ASSERT_TRUE( clips.has_value() );
    for ( const std::string& path : *clips )
    {
        const std::string text = ReadFile( path );
        const auto        data = ReadAnimationJson( text );
        ASSERT_TRUE( data.IsSuccess() ) << path << ": " << data.GetError();
        const auto clip = BuildClipFromAssetData( data.GetValue() );
        ASSERT_TRUE( clip.IsSuccess() ) << path << ": " << clip.GetError();
        auto written = BuildAssetDataFromClip( clip.GetValue() );
        ASSERT_TRUE( written.IsSuccess() ) << path << ": " << written.GetError();
        AnimationAssetData out = written.ExtractValue();
        out.Header             = data.GetValue().Header;
        out.Import             = data.GetValue().Import;
        const auto canonical   = Common::Content::CanonicalJsonTextOfWriterOutput( WriteAnimationJson( out ) );
        ASSERT_TRUE( canonical.IsSuccess() ) << path;
        EXPECT_EQ( canonical.GetValue(), text ) << path << ": the file is not what the writer writes";
    }
}

// GENERATION 3 IS REFUSED BY NAME, pointing at the migrator — not reported as a missing member.
TEST( AnimationClipCorpus, Generation3IsRefusedByName )
{
    const std::string generation3 = R"({
    "Header": {
        "Kind": "Animation",
        "Guid": "ec838680efeee3ada2b232f3cfec5d67",
        "Versions": {
            "ANIM": 4
        },
        "Dependencies": []
    },
    "Name": "Old",
    "TickRate": {"Numerator": 24000, "Denominator": 1},
    "DisplayRate": {"Numerator": 30, "Denominator": 1},
    "DurationTicks": 0,
    "SkeletonSignature": 0,
    "Channels": [],
    "Notifies": [],
    "Sections": [],
    "Curves": []
})";
    const auto read = Desert::Assets::Serialization::ReadAnimationJson( generation3 );
    ASSERT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "ANIM v4" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "generation 3" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "SceneMigrator" ), std::string::npos ) << read.GetError();
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

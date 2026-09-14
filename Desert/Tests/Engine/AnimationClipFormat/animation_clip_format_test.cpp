// THE `.anim` AND `.skeleton` FILE FORMATS ARE THESE STRUCTS, so this suite is a census of them.
//
// It exists because three fields named BoneIndex were declared without an initialiser and one of them —
// Serialization::ChannelData::BoneIndex — was written straight into every clip the Sequencer saved. The
// Sequencer's "New Clip" built its tracks without ever setting the index, so what reached the file was
// whatever the stack held, and the loader then sized its track array from `max(index) + 1`.
//
// The fix was not an initialiser. The index restated a bone's position in an array, which nothing at
// playback ever read (Animator::ResolveTrack binds by NAME), so the field is gone from the format. The two
// census tests below pin that: a field cannot come back without a line here saying so.

#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace Ser = Desert::Assets::Serialization;

namespace
{
    template <typename T>
    std::vector<std::string> FieldNames()
    {
        std::vector<std::string> names;
        for ( const auto& name : rfl::fields<T>() )
            names.push_back( std::string( name.name() ) );
        std::sort( names.begin(), names.end() );
        return names;
    }

    Ser::ChannelData Channel( const char* bone )
    {
        Ser::ChannelData ch;
        ch.BoneName = bone;
        ch.Positions.push_back( { 0.0f, glm::vec3( 1.0f, 2.0f, 3.0f ) } );
        return ch;
    }
} // namespace

// ============================================================================
// The census. NAMED ROWS, not a count: a count can be satisfied by editing the count.
// ============================================================================

TEST( AnimationClipFormat, ChannelFieldCensus )
{
    EXPECT_EQ( FieldNames<Ser::ChannelData>(),
               ( std::vector<std::string>{ "BoneName", "Positions", "Rotations", "Scales" } ) )
         << "a field added to or removed from the .anim channel changes every cooked clip on every machine";
}

TEST( AnimationClipFormat, AssetFieldCensus )
{
    EXPECT_EQ( FieldNames<Ser::AnimationAssetData>(),
               ( std::vector<std::string>{ "Channels", "Duration", "Name", "Notifies", "SkeletonSignature",
                                           "TicksPerSecond" } ) );
    EXPECT_EQ( FieldNames<Ser::NotifyData>(), ( std::vector<std::string>{ "Name", "Time" } ) );
}

TEST( AnimationClipFormat, SkeletonFieldCensus )
{
    EXPECT_EQ( FieldNames<Ser::SkeletonAssetData>(), ( std::vector<std::string>{ "Bones", "Signature" } ) );
    // BoneInfo is written to .skeleton verbatim; it carried the same redundant index.
    EXPECT_EQ( FieldNames<Desert::Animation::BoneInfo>(),
               ( std::vector<std::string>{ "LocalBindTransform", "Name", "OffsetMatrix", "ParentBoneID" } ) );
}

// Every scalar in the format has a default member initialiser, so a producer that forgets an assignment
// writes a defined value rather than heap noise. For these all-scalar structs the compiler can say so:
// without an initialiser they would be trivially default constructible, with one they are not.
static_assert( !std::is_trivially_default_constructible_v<Ser::KeyPosition> );
static_assert( !std::is_trivially_default_constructible_v<Ser::KeyRotation> );
static_assert( !std::is_trivially_default_constructible_v<Ser::KeyScale> );

// ============================================================================
// The behaviour the removed index used to control.
// ============================================================================

TEST( AnimationClipFormat, ChannelOrderIsPreservedAndBoundByName )
{
    Ser::AnimationAssetData data;
    data.Name              = "Walk";
    data.Duration          = 2.0f;
    data.TicksPerSecond    = 30.0f;
    data.SkeletonSignature = 1234u;
    data.Channels          = { Channel( "hips" ), Channel( "spine" ), Channel( "head" ) };

    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data );
    ASSERT_TRUE( built ) << built.GetError();

    const auto& clip = built.GetValue();
    ASSERT_EQ( clip.Tracks.size(), 3u ) << "one track per channel: no index-shaped holes";
    EXPECT_EQ( clip.Tracks[0].BoneName, "hips" );
    EXPECT_EQ( clip.Tracks[1].BoneName, "spine" );
    EXPECT_EQ( clip.Tracks[2].BoneName, "head" );
    EXPECT_EQ( clip.AnimationName, "Walk" );
    EXPECT_FLOAT_EQ( clip.TicksPerSecond, 30.0f );
    EXPECT_EQ( clip.SkeletonSignature, 1234u );
}

TEST( AnimationClipFormat, AnUnnamedChannelIsRefusedByName )
{
    Ser::AnimationAssetData data;
    data.Name     = "Broken";
    data.Channels = { Channel( "hips" ), Channel( "" ) };

    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data );
    ASSERT_FALSE( built ) << "an unnamed channel can never bind to a bone; accepting it is a silent loss";
    EXPECT_NE( built.GetError().find( "Broken" ), std::string::npos );
}

TEST( AnimationClipFormat, TwoChannelsForOneBoneAreRefused )
{
    Ser::AnimationAssetData data;
    data.Name     = "Doubled";
    data.Channels = { Channel( "hips" ), Channel( "hips" ) };

    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data );
    ASSERT_FALSE( built ) << "playback resolves a bone to ONE track, so the loser's keys vanish";
    EXPECT_NE( built.GetError().find( "hips" ), std::string::npos );
}

TEST( AnimationClipFormat, NotifiesComeOutSortedByTime )
{
    Ser::AnimationAssetData data;
    data.Name     = "Notified";
    data.Notifies = { { "late", 0.9f }, { "early", 0.1f }, { "middle", 0.5f } };

    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data );
    ASSERT_TRUE( built ) << built.GetError();
    ASSERT_EQ( built.GetValue().Notifies.size(), 3u );
    EXPECT_EQ( built.GetValue().Notifies[0].Name, "early" );
    EXPECT_EQ( built.GetValue().Notifies[2].Name, "late" );
}

// ============================================================================
// The on-disk migration, which is a migration BY OMISSION: reflect-cpp ignores fields the struct no longer
// declares, so a clip cooked before this change loads unchanged and re-cooks without the index. There is no
// version branch and no legacy path, which is the only reason that is acceptable — the test is what makes
// the claim checkable rather than believed.
// ============================================================================

TEST( AnimationClipFormat, AClipCookedWithTheOldBoneIndexStillLoads )
{
    const std::string legacy = R"({"Name":"Legacy","Duration":1.5,"TicksPerSecond":24.0,)"
                               R"("SkeletonSignature":77,"Channels":[)"
                               R"({"BoneName":"spine","BoneIndex":4,"Positions":[],"Rotations":[],"Scales":[]},)"
                               R"({"BoneName":"hips","BoneIndex":0,"Positions":[],"Rotations":[],"Scales":[]}])"
                               R"(,"Notifies":[]})";

    const auto read = rfl::json::read<Ser::AnimationAssetData, rfl::DefaultIfMissing>( legacy );
    ASSERT_TRUE( read.has_value() ) << "the retired field must be IGNORED, not rejected";

    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( read.value() );
    ASSERT_TRUE( built ) << built.GetError();

    const auto& clip = built.GetValue();
    ASSERT_EQ( clip.Tracks.size(), 2u )
         << "the old index said 4 and 0; the file's ORDER is what survives, and nothing is sized from it";
    EXPECT_EQ( clip.Tracks[0].BoneName, "spine" );
    EXPECT_EQ( clip.Tracks[1].BoneName, "hips" );
}

TEST( AnimationClipFormat, ANewlyWrittenClipCarriesNoBoneIndex )
{
    Ser::AnimationAssetData data;
    data.Name     = "Fresh";
    data.Channels = { Channel( "hips" ) };

    const std::string json = rfl::json::write( data );
    EXPECT_EQ( json.find( "BoneIndex" ), std::string::npos ) << json;
}

TEST( AnimationClipFormat, ASkeletonCookedWithTheOldBoneIndexStillLoads )
{
    const std::string legacy =
         R"({"Signature":4699069763035776985,"Bones":[{"BoneIndex":0,"Name":"Root",)"
         R"("OffsetMatrix":[1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0],)"
         R"("LocalBindTransform":[1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0]}]})";

    const auto read = rfl::json::read<Ser::SkeletonAssetData, rfl::DefaultIfMissing>( legacy );
    ASSERT_TRUE( read.has_value() );
    ASSERT_EQ( read.value().Bones.size(), 1u );
    EXPECT_EQ( read.value().Bones[0].Name, "Root" );
}

// ---------------------------------------------------------------------------------------------------
// THE WRITE HALF (Д35)
// ---------------------------------------------------------------------------------------------------
//
// Until Д35 this suite could only test the direction that READS a `.anim`. The direction that writes one
// lived inside SequencerPanel::SaveClipToDisk — a member of an ImGui panel — so the format's round trip
// was an assumption, and the row Д31-D called its WORST was in the part no test could reach: the panel
// did `out << rfl::json::write( data )` with no check after it at all and returned the path as proof of
// a save.

namespace
{
    std::filesystem::path ClipScratch()
    {
        const auto dir = std::filesystem::temp_directory_path() / "desert_anim_clip_write";
        std::filesystem::remove_all( dir );
        std::filesystem::create_directories( dir );
        return dir;
    }

    Desert::Animation::AnimationClip SampleClip()
    {
        Desert::Animation::AnimationClip clip;
        clip.AnimationName     = "Walk";
        clip.Duration          = 2.5f;
        clip.TicksPerSecond    = 30.0f;
        clip.SkeletonSignature = 0x1234'5678'9abc'def0ull;

        Desert::Animation::BoneTrack hip;
        hip.BoneName = "Hips";
        hip.PositionKeys.push_back( { 0.0f, glm::vec3( 1.0f, 2.0f, 3.0f ) } );
        hip.PositionKeys.push_back( { 1.0f, glm::vec3( 4.0f, 5.0f, 6.0f ) } );
        hip.RotationKeys.push_back( { 0.5f, glm::quat( 0.7071f, 0.0f, 0.7071f, 0.0f ) } );
        hip.ScaleKeys.push_back( { 0.0f, glm::vec3( 1.0f, 1.5f, 2.0f ) } );
        clip.Tracks.push_back( hip );

        Desert::Animation::BoneTrack spine;
        spine.BoneName = "Spine";
        spine.PositionKeys.push_back( { 0.25f, glm::vec3( -1.0f, 0.0f, 0.5f ) } );
        clip.Tracks.push_back( spine );

        clip.Notifies.push_back( { "Footstep", 0.75f } );
        return clip;
    }
} // namespace

// THE RELATION, not either side alone: what SaveClipToFile writes is what BuildClipFromAssetData reads.
// Both directions are in this binary, which is the only way a field that one side forgets comes out red.
TEST( AnimationClipFormat, AClipWrittenToDiskReadsBackAsTheSameClip )
{
    const auto path = ClipScratch() / "_Walk.anim";
    const auto clip = SampleClip();

    const auto saved = Desert::Assets::Serialization::SaveClipToFile( path, clip );
    ASSERT_TRUE( saved ) << saved.GetError();

    std::ifstream     in( path, std::ios::binary );
    std::stringstream text;
    text << in.rdbuf();
    const auto parsed = rfl::json::read<Ser::AnimationAssetData>( text.str() );
    ASSERT_TRUE( parsed.has_value() ) << "the file this engine wrote does not parse as the format it is";

    const auto rebuilt = Desert::Assets::Serialization::BuildClipFromAssetData( parsed.value() );
    ASSERT_TRUE( rebuilt ) << rebuilt.GetError();
    const auto& back = rebuilt.GetValue();

    EXPECT_EQ( back.AnimationName, clip.AnimationName );
    EXPECT_FLOAT_EQ( back.Duration, clip.Duration );
    EXPECT_FLOAT_EQ( back.TicksPerSecond, clip.TicksPerSecond );
    EXPECT_EQ( back.SkeletonSignature, clip.SkeletonSignature )
         << "the rig the clip claims did not survive the round trip";

    ASSERT_EQ( back.Tracks.size(), clip.Tracks.size() );
    for ( size_t t = 0; t < clip.Tracks.size(); ++t )
    {
        EXPECT_EQ( back.Tracks[t].BoneName, clip.Tracks[t].BoneName );
        ASSERT_EQ( back.Tracks[t].PositionKeys.size(), clip.Tracks[t].PositionKeys.size() ) << "track " << t;
        for ( size_t k = 0; k < clip.Tracks[t].PositionKeys.size(); ++k )
        {
            EXPECT_FLOAT_EQ( back.Tracks[t].PositionKeys[k].Time, clip.Tracks[t].PositionKeys[k].Time );
            EXPECT_EQ( back.Tracks[t].PositionKeys[k].Position, clip.Tracks[t].PositionKeys[k].Position );
        }
        ASSERT_EQ( back.Tracks[t].RotationKeys.size(), clip.Tracks[t].RotationKeys.size() ) << "track " << t;
        ASSERT_EQ( back.Tracks[t].ScaleKeys.size(), clip.Tracks[t].ScaleKeys.size() ) << "track " << t;
    }

    ASSERT_EQ( back.Notifies.size(), clip.Notifies.size() );
    EXPECT_EQ( back.Notifies[0].Name, clip.Notifies[0].Name );
    EXPECT_FLOAT_EQ( back.Notifies[0].Time, clip.Notifies[0].Time );
}

// THE MUTATION SITE FOR Д31-D'S WORST ROW. Delete the `if ( !written )` in SaveClipToFile and this test
// goes red: the save reports success and hands back a path for a file that is not there.
TEST( AnimationClipFormat, AClipSaveThatCannotBeWrittenIsARefusalNamingTheClip )
{
    const auto dir  = ClipScratch();
    const auto path = dir / "_Walk.anim";

    // The destination is writable and already occupied; the primitive's working file is not. A save that
    // "fails" by destroying the clip that was already there would be the defect, not the fix.
    const std::string previousClip = "{\"Name\":\"the clip that was already saved\"}";
    {
        std::ofstream previous( path, std::ios::binary | std::ios::trunc );
        previous << previousClip;
    }
    std::filesystem::path temp = path;
    temp += ".tmp";
    std::filesystem::create_directories( temp );

    const auto saved = Desert::Assets::Serialization::SaveClipToFile( path, SampleClip() );
    EXPECT_FALSE( saved ) << "a clip that was never written reported itself saved";
    EXPECT_NE( saved.GetError().find( "Walk" ), std::string::npos )
         << "the refusal must name the clip the person pressed Save on: " << saved.GetError();

    std::ifstream     in( path, std::ios::binary );
    std::stringstream still;
    still << in.rdbuf();
    EXPECT_EQ( still.str(), previousClip ) << "the failed save cost the clip that was already on disk";
}

// The pure conversion on its own, because the round trip above cannot distinguish "the writer dropped
// it" from "the reader dropped it" when BOTH drop the same field.
TEST( AnimationClipFormat, BuildAssetDataFromClipCarriesEveryChannelAndNotify )
{
    const auto clip = SampleClip();
    const auto data = Desert::Assets::Serialization::BuildAssetDataFromClip( clip );

    EXPECT_EQ( data.Name, "Walk" );
    EXPECT_EQ( data.SkeletonSignature, clip.SkeletonSignature );
    ASSERT_EQ( data.Channels.size(), 2u );
    EXPECT_EQ( data.Channels[0].BoneName, "Hips" );
    EXPECT_EQ( data.Channels[0].Positions.size(), 2u );
    EXPECT_EQ( data.Channels[0].Rotations.size(), 1u );
    EXPECT_EQ( data.Channels[0].Scales.size(), 1u );
    EXPECT_EQ( data.Channels[1].BoneName, "Spine" );
    ASSERT_EQ( data.Notifies.size(), 1u );
    EXPECT_EQ( data.Notifies[0].Name, "Footstep" );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

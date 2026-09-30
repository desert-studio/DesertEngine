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

#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>
#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Animation/ClipSection.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/AnimationClipMigrate.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Assets/AssetGuidRef.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <rflcpp/rfl.hpp>

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
        ch.Positions.push_back( { 0, glm::vec3( 1.0f, 2.0f, 3.0f ) } );
        return ch;
    }
    // THE MIGRATION STOPS AT GENERATION 3, whose version is a top-level `Version`; the header raise
    // (Tools/SceneMigrator) cuts that member and states the version in the header instead. The struct is the
    // current generation and reads strictly, so the member is taken off here the way the raise takes it off
    // -- after checking it says 3 -- rather than read leniently past it. Generation 3 still names its rig by
    // the bone hash `SkeletonSignature`; the ANIM 5 step (Tools/SceneMigrator, MigrateSkeletonReference)
    // replaces it by the one .skeleton's GUID, which needs the registered skeletons and is tested there
    // (SkeletonReference, SceneMigrator). Here the member is checked PRESENT -- the migration must carry the
    // signature to the step that resolves it -- and taken off.
    Common::ResultStr<Ser::AnimationAssetData> ReadGenerationThree( const std::string& text )
    {
        const auto parsed = Common::Json::Parse( text );
        if ( !parsed )
            return Common::MakeError<Ser::AnimationAssetData>( parsed.GetError() );
        const Common::Json::Node root    = Common::Json::Root( parsed.GetValue() );
        const auto               version = root.Get( "Version" );
        if ( !version )
            return Common::MakeError<Ser::AnimationAssetData>( version.GetError() );
        const auto stated = version.GetValue().AsInteger();
        if ( !stated || stated.GetValue() != Ser::kAnimationLastVersionMember )
            return Common::MakeError<Ser::AnimationAssetData>( "the migration did not stop at generation 3" );

        Common::Json::ObjectBuilder body;
        root.ForEachMember(
             [&]( std::string_view name, const Common::Json::Node& member )
             {
                 if ( name != "Version" && name != "SkeletonSignature" )
                     body.Set( name, member.Raw() );
             } );
        if ( !root.Get( "SkeletonSignature" ) )
            return Common::MakeError<Ser::AnimationAssetData>(
                 "the migration dropped SkeletonSignature, which the ANIM 5 step resolves to a skeleton GUID" );
        const Common::Json::Value raised( body.Build() );
        return Common::Json::Root( raised ).As<Ser::AnimationAssetData>();
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
    // A5: `Duration`/`TicksPerSecond` (float seconds under a rate every shipped clip set to 1) are gone;
    // the file now states its own generation, the grid its ticks are counted on, the grid an artist edits
    // on, and a length in ticks.
    // A28 (generation 3): `Sections` — a clip now states the range, blend type and weight its values are
    // read under, which is report 05 §938's "from day one".
    // T7e (generation 4): `Version` moves into the text asset `Header`, beside the clip's GUID.
    // THM-FIXJ: `Import` - the source file the clip was imported from (UE: UAnimSequence::
    // AssetImportData), so Reimport reads it from the asset itself.
    EXPECT_EQ( FieldNames<Ser::AnimationAssetData>(),
               ( std::vector<std::string>{ "Channels", "Curves", "DisplayRate", "DurationTicks", "Header", "Import",
                                           "Name", "Notifies", "Sections", "Skeleton", "TickRate" } ) );
    // SKEL-TREE (ANIM 5): the clip names its rig by the .skeleton's GUID (UE UAnimSequence::Skeleton), no
    // longer by a hash of the bones; the reference is the one GUID+path pair every text format uses.
    EXPECT_EQ( FieldNames<Desert::Assets::AssetGuidRef>(), ( std::vector<std::string>{ "Guid", "Path" } ) );
    // ANV1b: a notify states the Animation Editor row it is drawn on (UE's Notify Tracks).
    // ANV3: and its length — a notify with one is UE's Notify State.
    EXPECT_EQ( FieldNames<Ser::NotifyData>(),
               ( std::vector<std::string>{ "DurationTicks", "Name", "Tick", "Track" } ) );
    // ANV3: an anim curve is a name and the scalar keys a section weight already stores.
    EXPECT_EQ( FieldNames<Ser::CurveData>(), ( std::vector<std::string>{ "Keys", "Name" } ) );
    EXPECT_EQ( FieldNames<Ser::FrameRateData>(), ( std::vector<std::string>{ "Denominator", "Numerator" } ) );
    EXPECT_EQ( FieldNames<Ser::SectionData>(),
               ( std::vector<std::string>{ "Blend", "EndTick", "Name", "StartTick", "Tracks", "Weight" } ) );
    EXPECT_EQ( FieldNames<Ser::SectionWeightKey>(),
               ( std::vector<std::string>{ "ArriveTangent", "LeaveTangent", "Shape", "Tick", "Value" } ) );
}

TEST( AnimationClipFormat, SkeletonFieldCensus )
{
    EXPECT_EQ( FieldNames<Ser::SkeletonAssetData>(),
               ( std::vector<std::string>{ "Bones", "CompatibleSkeletons", "Header", "Import", "PreviewMesh",
                                           "Signature" } ) )
         << "SKEL 2: the preview mesh and the one-way compatible list are the skeleton's; the signature stays as "
            "a "
            "payload hash, never an identity";
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

    data.Name = "Walk";

    const Common::Content::AssetGuid rig{ 0x1234ull, 0x5678ull };
    data.DurationTicks = 48000;
    data.Skeleton      = Desert::Assets::AssetGuidRef{ Common::Content::AssetGuidToText( rig ), "Rig.skeleton" };
    data.Channels      = { Channel( "hips" ), Channel( "spine" ), Channel( "head" ) };

    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data );
    ASSERT_TRUE( built ) << built.GetError();

    const auto& clip = built.GetValue();
    ASSERT_EQ( clip.Tracks.size(), 3u ) << "one track per channel: no index-shaped holes";
    EXPECT_EQ( clip.Tracks[0].BoneName, "hips" );
    EXPECT_EQ( clip.Tracks[1].BoneName, "spine" );
    EXPECT_EQ( clip.Tracks[2].BoneName, "head" );
    EXPECT_EQ( clip.AnimationName, "Walk" );
    EXPECT_EQ( clip.TickRate, Desert::Animation::PROJECT_TICK_RATE );
    EXPECT_EQ( clip.DurationTicks.Value, 48000 );
    EXPECT_EQ( clip.Skeleton, rig ) << "the clip's skeleton is the GUID the file states, not a hash of bones";
}

TEST( AnimationClipFormat, AClipStatingNoSkeletonBuildsWithANullReference )
{
    Ser::AnimationAssetData data;
    data.Name     = "Unrigged";
    data.Channels = { Channel( "hips" ) };

    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data );
    ASSERT_TRUE( built ) << built.GetError();
    EXPECT_TRUE( built.GetValue().Skeleton.IsNull() ) << "absent = names no skeleton, plays on no mesh";
}

TEST( AnimationClipFormat, ASkeletonGuidThatDoesNotParseIsRefusedNamingThePath )
{
    Ser::AnimationAssetData data;
    data.Name     = "BadRig";
    data.Skeleton = Desert::Assets::AssetGuidRef{ "not-a-guid", "Characters/Hero.skeleton" };
    data.Channels = { Channel( "hips" ) };

    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data );
    ASSERT_FALSE( built ) << "a reference that does not parse would load as a clip that plays on nothing";
    EXPECT_NE( built.GetError().find( "BadRig" ), std::string::npos ) << built.GetError();
    EXPECT_NE( built.GetError().find( "Characters/Hero.skeleton" ), std::string::npos ) << built.GetError();
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

TEST( AnimationClipFormat, NotifiesComeOutSortedByTick )
{
    Ser::AnimationAssetData data;
    data.Name     = "Notified";
    data.Notifies = { { "late", 21600 }, { "early", 2400 }, { "middle", 12000 } };

    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data );
    ASSERT_TRUE( built ) << built.GetError();
    ASSERT_EQ( built.GetValue().Notifies.size(), 3u );
    EXPECT_EQ( built.GetValue().Notifies[0].Name, "early" );
    EXPECT_EQ( built.GetValue().Notifies[2].Name, "late" );
}

// ANV1b: the row a notify is drawn on survives the file and the build — a notify moved to track 2 and saved
// is on track 2 when the clip is opened again, not on the first row.
TEST( AnimationClipFormat, ANotifysTrackSurvivesTheFileAndTheBuild )
{
    const Ser::NotifyData written{ "FootSync", 12000, 2 };
    const auto            read = Common::Json::Read<Ser::NotifyData>( Common::Json::Write( written ) );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    EXPECT_EQ( read.GetValue().Track, 2 );

    Ser::AnimationAssetData data;
    data.Name        = "Tracked";
    data.Notifies    = { { "b", 4800, 1 }, { "a", 2400, 2 } };
    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data );
    ASSERT_TRUE( built ) << built.GetError();
    ASSERT_EQ( built.GetValue().Notifies.size(), 2u );
    EXPECT_EQ( built.GetValue().Notifies[0].Track, 2 );
    EXPECT_EQ( built.GetValue().Notifies[1].Track, 1 );

    const auto back = Desert::Assets::Serialization::BuildAssetDataFromClip( built.GetValue() );
    ASSERT_EQ( back.Notifies.size(), 2u );
    EXPECT_EQ( back.Notifies[0].Track, 2 );
}

TEST( AnimationClipFormat, ANegativeNotifyTrackIsRefusedByName )
{
    Ser::AnimationAssetData data;
    data.Name        = "Negative";
    data.Notifies    = { { "Hit", 2400, -1 } };
    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data );
    ASSERT_FALSE( built );
    EXPECT_NE( built.GetError().find( "Hit" ), std::string::npos ) << built.GetError();
}

// ============================================================================
// ============================================================================
// GENERATION 0 IS REFUSED, NOT READ. A `.anim` written before A5 carries `Time` in float seconds under a
// `TicksPerSecond` every shipped clip set to 1, and no version field at all. Read through DefaultIfMissing
// that file yields tick 0 for every key and generation 0 — so the loader must refuse it and name the tool,
// because the alternative is a clip that loads without a word and animates nothing.
// ============================================================================

TEST( AnimationClipFormat, AGenerationZeroClipIsRefusedAndNamesTheTool )
{
    const std::string legacy = R"({"Name":"Legacy","Duration":1.5,"TicksPerSecond":24.0,)"
                               R"("SkeletonSignature":77,"Channels":[)"
                               R"({"BoneName":"spine","Positions":[{"Time":0.5,"Value":[1.0,2.0,3.0]}],)"
                               R"("Rotations":[],"Scales":[]}])"
                               R"(,"Notifies":[]})";

    const auto read = Desert::Assets::Serialization::ReadAnimationJson( legacy );
    ASSERT_FALSE( read ) << "a v0 clip was accepted; every one of its keys would sit on tick 0";
    EXPECT_NE( read.GetError().find( "SceneMigrator" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "version 0" ), std::string::npos ) << read.GetError();
}

TEST( AnimationClipFormat, AClipFromANewerGenerationIsRefusedToo )
{
    Ser::AnimationAssetData data;
    data.Name   = "FromTheFuture";
    data.Header = Common::Content::MakeTextHeader(
         Common::Content::ContentKind::Animation, Common::Content::AssetGuid::Generate(),
         std::array{ Common::Content::SubsystemVersion{ Desert::Assets::kAnimationSchemaTag,
                                                        Desert::Assets::kAnimationSchemaVersion + 1 } } );

    const auto read = Desert::Assets::Serialization::ReadAnimationJson( Common::Json::Write( data ) );
    EXPECT_FALSE( read ) << "a file from a newer build may hold fields this one would drop on the next save";
}

TEST( AnimationClipFormat, AnInvalidRateIsRefusedRatherThanUsed )
{
    Ser::AnimationAssetData data;
    data.Name     = "NoRate";
    data.TickRate = { 0, 1 };
    EXPECT_FALSE( Desert::Assets::Serialization::BuildClipFromAssetData( data ) );

    Ser::AnimationAssetData display;

    display.Name        = "NoDisplay";
    display.DisplayRate = { 30, 0 };
    EXPECT_FALSE( Desert::Assets::Serialization::BuildClipFromAssetData( display ) );
}

// ============================================================================
// The conversion out of generation 0, which is what makes the refusal above actionable.
// ============================================================================

TEST( AnimationClipFormat, TheMigrationMovesSecondsOntoTicksAndDerivesTheDisplayGrid )
{
    // A clip authored the way all six shipped ones were: `TicksPerSecond` 1, so key times ARE seconds,
    // at multiples of an eighth of a second.
    const std::string legacy = R"({"Name":"Corpus","Duration":2.0,"TicksPerSecond":1.0,)"
                               R"("SkeletonSignature":42,"Channels":[{"BoneName":"Base",)"
                               R"("Positions":[{"Time":0.0,"Value":[0.0,0.0,0.0]},)"
                               R"({"Time":0.125,"Value":[1.0,0.0,0.0]},)"
                               R"({"Time":0.25,"Value":[2.0,0.0,0.0]}],)"
                               R"("Rotations":[],"Scales":[]}],"Notifies":[{"Name":"step","Time":0.5}]})";

    Desert::Assets::Serialization::AnimationMigrationReport report;
    const auto migrated = Desert::Assets::Serialization::MigrateAnimationJson( legacy, report );
    ASSERT_TRUE( migrated ) << migrated.GetError();

    EXPECT_EQ( report.FromVersion, 0 );
    EXPECT_NE( migrated.GetValue().find( R"("SkeletonSignature":42)" ), std::string::npos )
         << "the rig's bone hash must reach the ANIM 5 step, which resolves it to the .skeleton's GUID";
    EXPECT_EQ( report.KeysMoved, 0u ) << "an eighth of a second is a whole number of 24000ths";
    // THE DISPLAY GRID IS DERIVED FROM THE KEYS, not defaulted: these all lie on eighths, so 8 fps is the
    // coarsest grid that represents every one of them exactly. Handing them the default 30 would leave
    // every existing key off the grid the Sequencer snaps to.
    EXPECT_EQ( report.DisplayRateNumerator, 8 );
    EXPECT_FALSE( report.DisplayRateIsAFallback );

    const auto read = ReadGenerationThree( migrated.GetValue() );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( read.GetValue() );
    ASSERT_TRUE( built ) << built.GetError();

    const auto& clip = built.GetValue();
    EXPECT_EQ( clip.DurationTicks.Value, 48000 );
    ASSERT_EQ( clip.Tracks.size(), 1u );
    ASSERT_EQ( clip.Tracks[0].PositionKeys.size(), 3u );
    EXPECT_EQ( clip.Tracks[0].PositionKeys[0].Tick.Value, 0 );
    EXPECT_EQ( clip.Tracks[0].PositionKeys[1].Tick.Value, 3000 );
    EXPECT_EQ( clip.Tracks[0].PositionKeys[2].Tick.Value, 6000 );
    ASSERT_EQ( clip.Notifies.size(), 1u );
    EXPECT_EQ( clip.Notifies[0].Tick.Value, 12000 );
}

TEST( AnimationClipFormat, TheMigrationRefusesToRunTwice )
{
    const std::string legacy = R"({"Name":"Once","Duration":1.0,"TicksPerSecond":1.0,)"
                               R"("SkeletonSignature":1,"Channels":[],"Notifies":[]})";

    Desert::Assets::Serialization::AnimationMigrationReport first;
    const auto once = Desert::Assets::Serialization::MigrateAnimationJson( legacy, first );
    ASSERT_TRUE( once ) << once.GetError();

    // A SECOND RUN MUST REFUSE. Not because a re-run is rare, but because this step is not idempotent:
    // reading integer ticks as seconds would multiply every key by 24000 again. The engine has shipped
    // exactly that defect once before, when a migration doubled a text sigil (`#menu.play` ->
    // `##menu.play`), and the guard is the version stamp rather than a hope.
    Desert::Assets::Serialization::AnimationMigrationReport second;
    const auto twice = Desert::Assets::Serialization::MigrateAnimationJson( once.GetValue(), second );
    EXPECT_FALSE( twice );
    EXPECT_EQ( second.FromVersion, Desert::Assets::Serialization::kAnimationLastVersionMember );
}

TEST( AnimationClipFormat, AMigratedRateOfZeroIsRefusedRatherThanDividedBy )
{
    const std::string broken = R"({"Name":"NoRate","Duration":1.0,"TicksPerSecond":0.0,)"
                               R"("SkeletonSignature":1,"Channels":[],"Notifies":[]})";
    Desert::Assets::Serialization::AnimationMigrationReport report;
    EXPECT_FALSE( Desert::Assets::Serialization::MigrateAnimationJson( broken, report ) );
}

TEST( AnimationClipFormat, ANewlyWrittenClipCarriesNoBoneIndex )
{
    Ser::AnimationAssetData data;
    data.Name     = "Fresh";
    data.Channels = { Channel( "hips" ) };

    const std::string json = Common::Json::Write( data );
    EXPECT_EQ( json.find( "BoneIndex" ), std::string::npos ) << json;
}

// The field is gone from the format and the reader is strict, so a skeleton that still states it is refused
// NAMING the key -- it does not load with the key silently dropped. No committed .skeleton carries it.
TEST( AnimationClipFormat, ASkeletonStillStatingTheOldBoneIndexIsRefusedByName )
{
    const std::string legacy =
         R"({"Signature":4699069763035776985,"Bones":[{"BoneIndex":0,"Name":"Root",)"
         R"("OffsetMatrix":[1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0],)"
         R"("LocalBindTransform":[1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0]}]})";

    const auto read = Common::Json::Read<Ser::SkeletonAssetData>( legacy );
    ASSERT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( "Bones.BoneIndex" ), std::string::npos ) << read.GetError();
}

// ---------------------------------------------------------------------------------------------------
// THE WRITE HALF (Д35)
// ---------------------------------------------------------------------------------------------------
//
// Until Д35 this suite could only test the direction that READS a `.anim`. The direction that writes one
// lived inside SequencerPanel::SaveClipToDisk — a member of an ImGui panel — so the format's round trip
// was an assumption, and the row Д31-D called its WORST was in the part no test could reach: the panel
// did `out << <the data written as JSON>` with no check after it at all and returned the path as proof of
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
        clip.DurationTicks     = Desert::Animation::FrameNumber{ 60000 }; // 2.5 s at 24000
        clip.TickRate          = Desert::Animation::PROJECT_TICK_RATE;
        clip.DisplayRate       = Desert::Animation::FrameRate{ 30, 1 };
        clip.Skeleton          = Common::Content::AssetGuid{ 0x1234'5678'9abc'def0ull, 0x0fed'cba9'8765'4321ull };

        Desert::Animation::BoneTrack hip;
        hip.BoneName = "Hips";
        hip.PositionKeys.push_back( { Desert::Animation::FrameNumber{ 0 }, glm::vec3( 1.0f, 2.0f, 3.0f ) } );
        hip.PositionKeys.push_back( { Desert::Animation::FrameNumber{ 24000 }, glm::vec3( 4.0f, 5.0f, 6.0f ) } );
        hip.RotationKeys.push_back(
             { Desert::Animation::FrameNumber{ 12000 }, glm::quat( 0.7071f, 0.0f, 0.7071f, 0.0f ) } );
        hip.ScaleKeys.push_back( { Desert::Animation::FrameNumber{ 0 }, glm::vec3( 1.0f, 1.5f, 2.0f ) } );
        clip.Tracks.push_back( hip );

        Desert::Animation::BoneTrack spine;
        spine.BoneName = "Spine";
        spine.PositionKeys.push_back( { Desert::Animation::FrameNumber{ 6000 }, glm::vec3( -1.0f, 0.0f, 0.5f ) } );
        clip.Tracks.push_back( spine );

        clip.Notifies.push_back( { "Footstep", Desert::Animation::FrameNumber{ 18000 } } );
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

    const std::ifstream in( path, std::ios::binary );
    std::stringstream text;
    text << in.rdbuf();
    const auto parsed = Common::Json::Read<Ser::AnimationAssetData>( text.str() );
    ASSERT_TRUE( parsed.IsSuccess() ) << "the file this engine wrote does not parse as the format it is";

    const auto rebuilt = Desert::Assets::Serialization::BuildClipFromAssetData( parsed.GetValue() );
    ASSERT_TRUE( rebuilt ) << rebuilt.GetError();
    const auto& back = rebuilt.GetValue();

    EXPECT_EQ( back.AnimationName, clip.AnimationName );
    EXPECT_EQ( back.DurationTicks.Value, clip.DurationTicks.Value );
    EXPECT_EQ( back.TickRate, clip.TickRate );
    EXPECT_EQ( back.DisplayRate, clip.DisplayRate );
    EXPECT_EQ( back.Skeleton, clip.Skeleton ) << "the rig the clip claims did not survive the round trip";
    // The writer states the skeleton as the header's dependency too (UE: the package imports its USkeleton),
    // so the registry's reference graph sees the edge without parsing the body.
    ASSERT_TRUE( parsed.GetValue().Header.has_value() );
    const auto& deps = parsed.GetValue().Header->Dependencies;
    EXPECT_NE( std::find( deps.begin(), deps.end(), Common::Content::AssetGuidToText( clip.Skeleton ) ),
               deps.end() )
         << "the clip's skeleton is not among the header's dependencies";

    ASSERT_EQ( back.Tracks.size(), clip.Tracks.size() );
    for ( size_t t = 0; t < clip.Tracks.size(); ++t )
    {
        EXPECT_EQ( back.Tracks[t].BoneName, clip.Tracks[t].BoneName );
        ASSERT_EQ( back.Tracks[t].PositionKeys.size(), clip.Tracks[t].PositionKeys.size() ) << "track " << t;
        for ( size_t k = 0; k < clip.Tracks[t].PositionKeys.size(); ++k )
        {
            // AN EQUALITY, not a tolerance. That is the whole of what the tick grid bought: a key time
            // that survives a write and a read is the SAME NUMBER, not a number within an epsilon.
            EXPECT_EQ( back.Tracks[t].PositionKeys[k].Tick.Value, clip.Tracks[t].PositionKeys[k].Tick.Value );
            EXPECT_EQ( back.Tracks[t].PositionKeys[k].Position, clip.Tracks[t].PositionKeys[k].Position );
        }
        ASSERT_EQ( back.Tracks[t].RotationKeys.size(), clip.Tracks[t].RotationKeys.size() ) << "track " << t;
        ASSERT_EQ( back.Tracks[t].ScaleKeys.size(), clip.Tracks[t].ScaleKeys.size() ) << "track " << t;
    }

    ASSERT_EQ( back.Notifies.size(), clip.Notifies.size() );
    EXPECT_EQ( back.Notifies[0].Name, clip.Notifies[0].Name );
    EXPECT_EQ( back.Notifies[0].Tick.Value, clip.Notifies[0].Tick.Value );
}

TEST( AnimationClipFormat, ASectionAUTHOREDTheWayTheSequencerAuthorsOneSurvivesTheRoundTripFieldByField )
{
    // THE CLAIM THE SECTION LANE MAKES, END TO END (A32). The editor's buttons call the functions in
    // Engine/Animation/ClipSection.hpp; this test calls the same ones, writes the clip with the same
    // writer the Save button uses, reads it back with the same reader the loader uses, and compares BY
    // VALUE. A hand-filled `ClipSection` would have proved the writer and the reader agree with each
    // other and nothing about what the editor produces.
    namespace Anim = Desert::Animation;

    auto clip = SampleClip();
    clip.Sections.clear();
    ASSERT_TRUE( Anim::AddSection( clip.Sections, "Whole clip", Anim::FrameNumber{ 0 }, clip.DurationTicks,
                                   Anim::SectionBlendType::Absolute, clip.DurationTicks )
                      .IsSuccess() );
    ASSERT_TRUE( Anim::AddSection( clip.Sections, "Lean", Anim::FrameNumber{ 800 }, Anim::FrameNumber{ 2400 },
                                   Anim::SectionBlendType::Additive, clip.DurationTicks )
                      .IsSuccess() );

    // Narrow the second one to a strict subset, which is the case that has a list on disk at all -- and
    // the case whose two spellings ClipSection.hpp collapses to one.
    std::vector<std::string> allTracks;
    allTracks.reserve( allTracks.size() + clip.Tracks.size() );
    for ( const auto& track : clip.Tracks )
    {
        allTracks.push_back( track.BoneName );
    }
    ASSERT_GE( allTracks.size(), 1U );
    clip.Sections[1].Tracks = { allTracks.front() };

    ASSERT_TRUE( Anim::SetSectionWeightKey( clip.Sections[1], Anim::FrameNumber{ 800 }, 0.0f ).IsSuccess() );
    ASSERT_TRUE( Anim::SetSectionWeightKey( clip.Sections[1], Anim::FrameNumber{ 2400 }, 0.75f ).IsSuccess() );

    const auto path  = ClipScratch() / "_Sectioned.anim";
    const auto saved = Desert::Assets::Serialization::SaveClipToFile( path, clip );
    ASSERT_TRUE( saved ) << saved.GetError();

    const std::ifstream in( path, std::ios::binary );
    std::stringstream text;
    text << in.rdbuf();
    const auto parsed = Common::Json::Read<Ser::AnimationAssetData>( text.str() );
    ASSERT_TRUE( parsed.IsSuccess() );
    EXPECT_EQ( Desert::Assets::StatedVersion( parsed.GetValue().Header, Desert::Assets::kAnimationSchemaTag ),
               Ser::kAnimationVersion )
         << "the authoring surface must not need a new generation; sections have been in the format since A28";

    const auto rebuilt = Desert::Assets::Serialization::BuildClipFromAssetData( parsed.GetValue() );
    ASSERT_TRUE( rebuilt ) << rebuilt.GetError();
    const auto& back = rebuilt.GetValue();

    ASSERT_EQ( back.Sections.size(), clip.Sections.size() );
    for ( size_t i = 0; i < clip.Sections.size(); ++i )
    {
        const Anim::ClipSection& want = clip.Sections[i];
        const Anim::ClipSection& got  = back.Sections[i];
        EXPECT_EQ( got.Name, want.Name ) << "section " << i;
        EXPECT_EQ( got.Start.Value, want.Start.Value ) << "section " << i;
        EXPECT_EQ( got.End.Value, want.End.Value ) << "section " << i;
        EXPECT_EQ( got.Blend, want.Blend ) << "section " << i;
        EXPECT_EQ( got.Tracks, want.Tracks ) << "section " << i;
        ASSERT_EQ( got.Weight.size(), want.Weight.size() ) << "section " << i;
        for ( size_t k = 0; k < want.Weight.size(); ++k )
        {
            // EQUALITY, not a tolerance -- the same argument the key round trip above makes.
            EXPECT_EQ( got.Weight[k].Tick.Value, want.Weight[k].Tick.Value );
            EXPECT_EQ( got.Weight[k].Value, want.Weight[k].Value );
            EXPECT_EQ( got.Weight[k].Interp, want.Weight[k].Interp );
            EXPECT_EQ( got.Weight[k].Mode, want.Weight[k].Mode );
        }
    }
    // The ORDER is the overlap rule, so it is asserted as a rule and not as an incidental consequence of
    // the loop above: the second section is still the one that wins its range.
    const Anim::ClipSection* winner = back.SectionFor( allTracks.front(), Anim::FrameNumber{ 1200 } );
    ASSERT_NE( winner, nullptr );
    EXPECT_EQ( winner->Name, "Lean" );
    EXPECT_EQ( winner->Blend, Anim::SectionBlendType::Additive );
}

// THE MUTATION SITE FOR Д31-D'S WORST ROW. Delete the `if ( !written )` in SaveClipToFile and this test
// goes red: the save reports success and hands back a path for a file that is not there.
TEST( AnimationClipFormat, AClipSaveThatCannotBeWrittenIsARefusalNamingTheClip )
{
    const auto dir  = ClipScratch();
    const auto path = dir / "_Walk.anim";

    // The destination is writable and already occupied; the primitive's working file is not. A save that
    // "fails" by destroying the clip that was already there would be the defect, not the fix.
    const std::string previousClip = R"({"Name":"the clip that was already saved"})";
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

    const std::ifstream in( path, std::ios::binary );
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
    ASSERT_TRUE( data.Skeleton.has_value() ) << "the clip names a skeleton; the data must state it";
    EXPECT_EQ( data.Skeleton->Guid, Common::Content::AssetGuidToText( clip.Skeleton ) );
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

// ── GENERATION 2 -> 3: THE SECTION IS ADDED AND NOTHING ELSE MOVES (A28) ────────────────────────────
//
// THE TRAP THIS CLOSES, AND IT WAS LIVE FOR THE LENGTH OF ONE COMMIT. `MigrateAnimationJson` wrote a
// constant `Linear`/`Auto` shape over EVERY key, which was harmless while generation 2 was current —
// no file carrying a shape could reach the function, because one at the current generation is refused.
// The moment generation 3 existed, `A6Curve_Cubic.anim` — a corpus file whose entire purpose is to hold
// `Cubic` keys with authored tangents — became a file the migrator converts, and the conversion would
// have flattened it while reporting success. A migration that relabels a file and loses its contents is
// worse than none, so the shape is carried and this is the test that says so.

namespace
{
    /// A generation-2 clip with a CUBIC position key and non-zero tangents — the shape a naive conversion
    /// would overwrite.
    std::string GenerationTwoJson()
    {
        Ser::AnimationAssetData data;
        data.Name          = "curve";
        data.TickRate      = { 24000, 1 };
        data.DisplayRate   = { 30, 1 };
        data.DurationTicks = 24000;

        Ser::KeyPosition key;
        key.Tick          = 12000;
        key.Value         = glm::vec3( 7.0f, 8.0f, 9.0f );
        key.Shape.Interp  = 2; // Cubic
        key.Shape.Mode    = 1; // not Auto
        key.ArriveTangent = glm::vec3( 1.5f, 0.0f, 0.0f );
        key.LeaveTangent  = glm::vec3( -2.5f, 0.0f, 0.0f );

        Ser::ChannelData channel;
        channel.BoneName = "hips";
        channel.Positions.push_back( key );
        data.Channels.push_back( channel );

        // Written WITHOUT the sections field, which is what a real generation-2 file on disk looks like.
        // ...and naming its rig by the bone hash, as every generation before ANIM 5 did.
        std::string json = R"({"Version":2,"SkeletonSignature":7,)" + Common::Json::Write( data ).substr( 1 );
        const auto  at   = json.find( R"(,"Sections":[])" );
        if ( at != std::string::npos )
        {
            json.erase( at, std::string( R"(,"Sections":[])" ).size() );
        }
        return json;
    }
} // namespace

TEST( AnimationClipFormat, TheMigrationToGenerationThreeKeepsEveryAuthoredKeySHAPE )
{
    Desert::Assets::Serialization::AnimationMigrationReport report;
    const auto migrated = Desert::Assets::Serialization::MigrateAnimationJson( GenerationTwoJson(), report );
    ASSERT_TRUE( migrated ) << migrated.GetError();
    EXPECT_EQ( report.FromVersion, 2 );
    EXPECT_EQ( report.ShapesWritten, 0U )
         << "this step ADDS no shapes — a non-zero count here means it overwrote authored ones";
    EXPECT_EQ( report.SectionsWritten, 1 ) << "…and it is not a relabelling: the file gained a section";

    const auto reread = ReadGenerationThree( migrated.GetValue() );
    ASSERT_TRUE( reread.IsSuccess() ) << reread.GetError();
    const auto& out = reread.GetValue();
    ASSERT_EQ( out.Channels.size(), 1U );
    ASSERT_EQ( out.Channels[0].Positions.size(), 1U );

    const auto& key = out.Channels[0].Positions[0];
    EXPECT_EQ( key.Tick, 12000 ) << "every tick kept";
    EXPECT_EQ( key.Shape.Interp, 2 ) << "CUBIC, not the Linear a constant shape would have written";
    EXPECT_EQ( key.Shape.Mode, 1 );
    EXPECT_FLOAT_EQ( key.ArriveTangent.x, 1.5f );
    EXPECT_FLOAT_EQ( key.LeaveTangent.x, -2.5f );
}

TEST( AnimationClipFormat, AMigratedClipSTATESOneWholeClipAbsoluteSectionAtFullWeight )
{
    Desert::Assets::Serialization::AnimationMigrationReport report;
    const auto migrated = Desert::Assets::Serialization::MigrateAnimationJson( GenerationTwoJson(), report );
    ASSERT_TRUE( migrated ) << migrated.GetError();

    const auto reread = ReadGenerationThree( migrated.GetValue() );
    ASSERT_TRUE( reread.IsSuccess() ) << reread.GetError();
    const auto& out = reread.GetValue();

    EXPECT_NE( migrated.GetValue().find( R"("Version":3)" ), std::string::npos )
         << "the migration's output is frozen at generation 3, which the header raise reads";
    EXPECT_FALSE( out.Header.has_value() ) << "the GUID is minted once, by the header raise";
    ASSERT_EQ( out.Sections.size(), 1U );
    EXPECT_EQ( out.Sections[0].StartTick, 0 );
    EXPECT_EQ( out.Sections[0].EndTick, out.DurationTicks ) << "the whole clip, to its stated length";
    EXPECT_EQ( out.Sections[0].Blend, 0 ) << "Absolute — the behaviour the file already had";
    EXPECT_TRUE( out.Sections[0].Tracks.empty() ) << "empty = every track; a list of names would go stale";
    EXPECT_TRUE( out.Sections[0].Weight.empty() ) << "empty = full weight, which is NOT a key of zero";
}

TEST( AnimationClipFormat, AClipWithABackwardsOrUnknownSectionIsRefusedRatherThanRepaired )
{
    Ser::AnimationAssetData data;
    data.Name          = "broken";
    data.TickRate      = { 24000, 1 };
    data.DisplayRate   = { 30, 1 };
    data.DurationTicks = 24000;
    data.Channels.push_back( Channel( "hips" ) );

    Ser::SectionData backwards;
    backwards.Name      = "backwards";
    backwards.StartTick = 100;
    backwards.EndTick   = 10;
    data.Sections.push_back( backwards );
    EXPECT_FALSE( Desert::Assets::Serialization::BuildClipFromAssetData( data ) )
         << "a section covering no tick at all would make its tracks silently play unsectioned";

    data.Sections.clear();
    Ser::SectionData unknown;
    unknown.Name    = "future";
    unknown.EndTick = 24000;
    unknown.Blend   = 7;
    data.Sections.push_back( unknown );
    EXPECT_FALSE( Desert::Assets::Serialization::BuildClipFromAssetData( data ) )
         << "reading an unknown blend as Absolute turns an offset into a pose";

    // POSITIVE CONTROL: the same data with a sane section loads.
    data.Sections[0].Blend = 1; // Additive
    EXPECT_TRUE( Desert::Assets::Serialization::BuildClipFromAssetData( data ) );
}

TEST( AnimationClipFormat, AScaleKeysSHAPEAndTANGENTSSurviveARoundTrip )
{
    // THE WRITER WROTE THEM AND THE READER DROPPED THEM. `BuildClipFromAssetData`'s scale branch built
    // `{ Tick, Value }` and stopped while `BuildAssetDataFromClip` wrote the shape and both tangents, so
    // a save, a load and a second save silently flattened every authored scale curve in the project.
    // Both ends of the chain looked right; the middle link dropped a property.
    Ser::AnimationAssetData data;
    data.Name          = "scaled";
    data.TickRate      = { 24000, 1 };
    data.DisplayRate   = { 30, 1 };
    data.DurationTicks = 24000;

    Ser::KeyScale key;
    key.Tick          = 6000;
    key.Value         = glm::vec3( 2.0f, 3.0f, 4.0f );
    key.Shape.Interp  = 2; // Cubic
    key.Shape.Mode    = 1; // not Auto
    key.ArriveTangent = glm::vec3( 0.25f, 0.0f, 0.0f );
    key.LeaveTangent  = glm::vec3( -0.75f, 0.0f, 0.0f );

    Ser::ChannelData channel;
    channel.BoneName = "hips";
    channel.Scales.push_back( key );
    data.Channels.push_back( channel );

    const auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data );
    ASSERT_TRUE( built ) << built.GetError();
    ASSERT_EQ( built.GetValue().Tracks.size(), 1U );
    ASSERT_EQ( built.GetValue().Tracks[0].ScaleKeys.size(), 1U );

    const auto& loaded = built.GetValue().Tracks[0].ScaleKeys[0];
    EXPECT_EQ( static_cast<int>( loaded.Interp ), 2 );
    EXPECT_EQ( static_cast<int>( loaded.Mode ), 1 );
    EXPECT_FLOAT_EQ( loaded.ArriveTangent.x, 0.25f );
    EXPECT_FLOAT_EQ( loaded.LeaveTangent.x, -0.75f );
}

// ANV3: a notify state's length and an anim curve's keys survive the file, the build and the write back —
// one clip through JSON text, not two structs compared field by field.
TEST( AnimationClipFormat, NotifyStatesAndCurvesSurviveTheFileRoundTrip )
{
    Ser::AnimationAssetData data;
    data.Name          = "Curved";
    data.DurationTicks = 24000;
    data.Notifies      = { { "Trail", 4800, 1, 7200 }, { "Hit", 2400, 0, 0 } };
    Ser::CurveData blink;
    blink.Name  = "Blink";
    blink.Keys  = { { 0, 0.0f, Ser::KeyShape{ 1, 0, 0.0f, 0.0f }, 0.0f, 0.0f },
                    { 12000, 1.0f, Ser::KeyShape{ 2, 1, 0.0f, 0.0f }, 0.5f, -0.25f },
                    { 24000, 0.0f, Ser::KeyShape{ 0, 0, 0.0f, 0.0f }, 0.0f, 0.0f } };
    data.Curves = { blink };

    const auto read = Common::Json::Read<Ser::AnimationAssetData>( Common::Json::Write( data ) );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    const auto built = Ser::BuildClipFromAssetData( read.GetValue() );
    ASSERT_TRUE( built ) << built.GetError();
    const auto& clip = built.GetValue();
    ASSERT_EQ( clip.Notifies.size(), 2u );
    EXPECT_EQ( clip.Notifies[1].Name, "Trail" );
    EXPECT_EQ( clip.Notifies[1].DurationTicks.Value, 7200 );
    EXPECT_TRUE( clip.Notifies[1].IsState() );
    EXPECT_FALSE( clip.Notifies[0].IsState() );
    const auto* curve = clip.FindCurve( "Blink" );
    ASSERT_NE( curve, nullptr );
    ASSERT_EQ( curve->Keys.size(), 3u );
    EXPECT_EQ( curve->Keys[1].Interp, Desert::Animation::KeyInterp::Cubic );
    EXPECT_EQ( curve->Keys[1].Mode, Desert::Animation::TangentMode::User );
    EXPECT_EQ( curve->Keys[2].Interp, Desert::Animation::KeyInterp::Constant );

    const auto back = Ser::BuildAssetDataFromClip( clip );
    ASSERT_EQ( back.Curves.size(), 1u );
    EXPECT_EQ( Common::Json::Write( back.Curves[0] ), Common::Json::Write( blink ) );
    ASSERT_EQ( back.Notifies.size(), 2u );
    EXPECT_EQ( back.Notifies[1].DurationTicks, 7200 );
}

TEST( AnimationClipFormat, ANegativeNotifyLengthAndATwiceNamedCurveAreRefusedByName )
{
    Ser::AnimationAssetData data;
    data.Name           = "Broken";
    data.Notifies       = { { "Trail", 4800, 0, -1 } };
    const auto negative = Ser::BuildClipFromAssetData( data );
    ASSERT_FALSE( negative );
    EXPECT_NE( negative.GetError().find( "Trail" ), std::string::npos ) << negative.GetError();

    data.Notifies.clear();
    Ser::CurveData twice;
    twice.Name           = "Blink";
    data.Curves          = { twice, twice };
    const auto duplicate = Ser::BuildClipFromAssetData( data );
    ASSERT_FALSE( duplicate );
    EXPECT_NE( duplicate.GetError().find( "Blink" ), std::string::npos ) << duplicate.GetError();
}

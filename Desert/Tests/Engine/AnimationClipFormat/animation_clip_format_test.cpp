// THE `.anim` AND `.skeleton` FILE FORMATS, as the engine reads and writes them today: ANIM v6, whose body is
// the clip's one TMLN v2 block (Timeline/Sequence.hpp), and the `.skeleton` census.
//
// The engine reads ONLY the current generation. Every older one — generation 0 (float seconds), generation 3
// (per-bone Channels, ANIM v4), ANIM v5 (key modes shaping the ARRIVING segment), a TMLN v1 block — is refused
// here BY NAME, pointing at Tools/SceneMigrator. Reading and lifting those generations is the migrator's job and
// is tested in Desert/Tests/Tools/SceneMigratorWritePath (clip_timeline_migration_test.cpp).

#include "../ClipFixture.hpp"

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>
#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <rflcpp/rfl.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace Ser      = Desert::Assets::Serialization;
namespace Anim     = Desert::Animation;
namespace Timeline = Desert::Animation::Timeline;

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

    std::string ReadFile( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::stringstream   text;
        text << in.rdbuf();
        return text.str();
    }

    std::string BytesText( const std::vector<uint8_t>& bytes )
    {
        return std::string( reinterpret_cast<const char*>( bytes.data() ), bytes.size() );
    }

    std::filesystem::path ClipScratch()
    {
        const auto dir = std::filesystem::temp_directory_path() / "desert_anim_clip_write";
        std::filesystem::remove_all( dir );
        std::filesystem::create_directories( dir );
        return dir;
    }

    Anim::ScalarKey ShapedKey( int32_t tick, float value, Anim::KeyInterp interp )
    {
        Anim::ScalarKey key = ClipFixture::Key( Anim::FrameNumber{ tick }, value );
        key.Interp          = interp;
        return key;
    }

    // A clip that states everything a `.anim` can carry: two bones (one with keys of all three shapes), a
    // notify and a notify state on the master Event track, and a named anim curve.
    Anim::AnimationClip SampleClip()
    {
        Anim::AnimationClip clip  = ClipFixture::Clip( "Walk", Anim::FrameNumber{ 60000 } );
        clip.Skeleton             = Common::Content::AssetGuidFromText( "123456789abcdef00fedcba987654321" ).GetValue();
        clip.Sequence.DisplayRate = Anim::FrameRate{ 30, 1 };

        Timeline::TransformChannel hips;
        hips.Translation.X.Keys = { ShapedKey( 0, 1.0F, Anim::KeyInterp::Constant ),
                                    ShapedKey( 24000, 4.0F, Anim::KeyInterp::Cubic ),
                                    ShapedKey( 60000, -2.0F, Anim::KeyInterp::Linear ) };
        hips.Translation.Y.Keys = { ShapedKey( 0, 2.0F, Anim::KeyInterp::Linear ) };
        hips.Translation.Z.Keys = { ShapedKey( 0, 3.0F, Anim::KeyInterp::Linear ) };
        hips.Rotation.X.Keys    = { ShapedKey( 12000, 0.0F, Anim::KeyInterp::Linear ) };
        hips.Rotation.Y.Keys    = { ShapedKey( 12000, 0.7071F, Anim::KeyInterp::Linear ) };
        hips.Rotation.Z.Keys    = { ShapedKey( 12000, 0.0F, Anim::KeyInterp::Linear ) };
        hips.Rotation.W.Keys    = { ShapedKey( 12000, 0.7071F, Anim::KeyInterp::Linear ) };
        hips.Scale.X.Keys       = { ShapedKey( 0, 1.0F, Anim::KeyInterp::Linear ) };
        hips.Scale.Y.Keys       = { ShapedKey( 0, 1.5F, Anim::KeyInterp::Linear ) };
        hips.Scale.Z.Keys       = { ShapedKey( 0, 2.0F, Anim::KeyInterp::Linear ) };
        (void)ClipFixture::AddBoneChannel( clip, "Hips", std::move( hips ) );
        (void)ClipFixture::AddStaticBone( clip, "Spine", glm::vec3( -1.0F, 0.0F, 0.5F ) );

        ClipFixture::AddNotify( clip, "Footstep", Anim::FrameNumber{ 18000 } );
        ClipFixture::AddNotify( clip, "Trail", Anim::FrameNumber{ 6000 }, Anim::FrameNumber{ 12000 } );
        ClipFixture::AddCurve(
             clip, "Blink",
             { ShapedKey( 0, 0.0F, Anim::KeyInterp::Cubic ), ShapedKey( 30000, 1.0F, Anim::KeyInterp::Linear ) } );
        return clip;
    }

    std::string SequenceText( const Timeline::Sequence& sequence )
    {
        const auto written = Timeline::WriteSequence( sequence );
        EXPECT_TRUE( written ) << written.GetError();
        return written ? BytesText( written.GetValue() ) : std::string{};
    }

    // The TMLN block of @p sequence with its header's TMLN number replaced by @p version — the layout of v1 is
    // v2's, only the meaning of a key's mode moved, so this is exactly a v1 block the engine never shifted.
    struct TimelineEnvelope
    {
        Common::Content::TextAssetHeaderSerialized Header;
        Common::Json::CarriedKeys                  Body;
    };

    Common::Json::Value BlockStating( const Timeline::Sequence& sequence, uint32_t version )
    {
        auto envelope = Common::Json::Read<TimelineEnvelope>( SequenceText( sequence ) );
        EXPECT_TRUE( envelope ) << envelope.GetError();
        TimelineEnvelope restated = envelope.ExtractValue();
        restated.Header.Versions[Common::Content::FourCCToString( Desert::Assets::kTimelineSchemaTag )] = version;
        auto value = Common::Json::Read<Common::Json::Value>( Common::Json::Write( restated ) );
        EXPECT_TRUE( value ) << value.GetError();
        return value.ExtractValue();
    }

    Ser::AnimationAssetData HeadedAt( uint32_t animVersion )
    {
        Ser::AnimationAssetData data;
        data.Name   = "Stated";
        data.Header = Common::Content::MakeTextHeader(
             Common::Content::ContentKind::Animation, Common::Content::AssetGuid::Generate(),
             std::array{ Common::Content::SubsystemVersion{ Desert::Assets::kAnimationSchemaTag, animVersion } } );
        return data;
    }
} // namespace

// ============================================================================
// The census. NAMED ROWS, not a count: a count can be satisfied by editing the count.
// ============================================================================

TEST( AnimationClipFormat, AssetFieldCensus )
{
    // ANIM-I8a (ANIM v5): Channels/Notifies/Curves/Sections/TickRate/DisplayRate/DurationTicks are the TMLN
    // block's now, stated once in `Sequence`; the file restates none of them.
    // SKEL-TREE: `Skeleton` — the clip's .skeleton by GUID (UE: UAnimSequence::Skeleton); no bone hash.
    // THM-FIXJ: `Import` — the source file the clip was imported from (UE: UAnimSequence::AssetImportData).
    EXPECT_EQ( FieldNames<Ser::AnimationAssetData>(),
               ( std::vector<std::string>{ "Header", "Import", "Name", "Sequence", "Skeleton" } ) );
}

// SKEL-TREE: a clip naming its skeleton by a GUID that does not parse is refused, naming the path it stated.
TEST( AnimationClipFormat, ASkeletonGuidThatDoesNotParseIsRefusedNamingThePath )
{
    auto data = Ser::BuildAssetDataFromClip( SampleClip() );
    ASSERT_TRUE( data ) << data.GetError();
    Ser::AnimationAssetData stated = data.ExtractValue();
    stated.Skeleton                = Desert::Assets::AssetGuidRef{ "not-a-guid", "Meshes/Skinned/Broken.skeleton" };
    const auto built               = Ser::BuildClipFromAssetData( stated );
    ASSERT_FALSE( built );
    EXPECT_NE( built.GetError().find( "Meshes/Skinned/Broken.skeleton" ), std::string::npos ) << built.GetError();
}

// SKEL-TREE: a clip stating no skeleton builds with a null reference (it plays on no mesh; ClipPlaysOnMesh).
TEST( AnimationClipFormat, AClipStatingNoSkeletonBuildsWithANullReference )
{
    auto data = Ser::BuildAssetDataFromClip( SampleClip() );
    ASSERT_TRUE( data ) << data.GetError();
    const auto built = Ser::BuildClipFromAssetData( data.GetValue() );
    ASSERT_TRUE( built ) << built.GetError();
    EXPECT_TRUE( built.GetValue().Skeleton.IsNull() );
}

TEST( AnimationClipFormat, SkeletonFieldCensus )
{
    EXPECT_EQ( FieldNames<Ser::SkeletonAssetData>(),
               ( std::vector<std::string>{ "Bones", "Header", "Import", "Signature" } ) );
    // BoneInfo is written to .skeleton verbatim; it carried a redundant BoneIndex once.
    EXPECT_EQ( FieldNames<Anim::BoneInfo>(),
               ( std::vector<std::string>{ "LocalBindTransform", "Name", "OffsetMatrix", "ParentBoneID" } ) );
}

// The field is gone from the format and the reader is strict, so a skeleton that still states it is refused
// NAMING the key -- it does not load with the key silently dropped.
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

// ============================================================================
// Every older generation is REFUSED BY NAME, pointing at the one tool that lifts it.
// ============================================================================

TEST( AnimationClipFormat, AGenerationZeroClipIsRefusedAndNamesTheTool )
{
    const std::string legacy = R"({"Name":"Legacy","Duration":1.5,"TicksPerSecond":24.0,)"
                               R"("SkeletonSignature":77,"Channels":[)"
                               R"({"BoneName":"spine","Positions":[{"Time":0.5,"Value":[1.0,2.0,3.0]}],)"
                               R"("Rotations":[],"Scales":[]}])"
                               R"(,"Notifies":[]})";

    const auto read = Ser::ReadAnimationJson( legacy );
    ASSERT_FALSE( read ) << "a v0 clip was accepted; every one of its keys would sit on tick 0";
    EXPECT_NE( read.GetError().find( "SceneMigrator" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "version 0" ), std::string::npos ) << read.GetError();
}

TEST( AnimationClipFormat, AGenerationThreeClipIsRefusedByNameNotAsAMissingSequence )
{
    const auto read =
         Ser::ReadAnimationJson( Common::Json::Write( HeadedAt( Ser::kAnimationLastChannelsVersion ) ) );
    ASSERT_FALSE( read );
    EXPECT_NE( read.GetError().find( "generation 3" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "SceneMigrator" ), std::string::npos ) << read.GetError();
}

// ANIM 4 (bone hash) is generation 3 as much as ANIM 5 (Skeleton GUID): both per-bone Channels, both refused by name.
TEST( AnimationClipFormat, AnAnimV4ClipIsRefusedAsGenerationThreeToo )
{
    const auto read = Ser::ReadAnimationJson( Common::Json::Write( HeadedAt( 4u ) ) );
    ASSERT_FALSE( read );
    EXPECT_NE( read.GetError().find( "generation 3" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "SceneMigrator" ), std::string::npos ) << read.GetError();
}

TEST( AnimationClipFormat, AClipFromANewerGenerationIsRefusedToo )
{
    const auto read =
         Ser::ReadAnimationJson( Common::Json::Write( HeadedAt( Desert::Assets::kAnimationSchemaVersion + 1 ) ) );
    EXPECT_FALSE( read ) << "a file from a newer build may hold fields this one would drop on the next save";
}

// ANIM v6 whose TMLN block still states v1: the key modes would be read under the wrong rule, so the one
// Timeline reader refuses the block by name, and the clip build carries the refusal with the clip's name.
TEST( AnimationClipFormat, ATimelineV1BlockInsideAClipIsRefusedByNameNamingTheClip )
{
    const Anim::AnimationClip clip = SampleClip();
    auto                      data = Ser::BuildAssetDataFromClip( clip );
    ASSERT_TRUE( data ) << data.GetError();
    Ser::AnimationAssetData stale = data.ExtractValue();
    stale.Sequence                = BlockStating( clip.Sequence, Timeline::kTimelineLastArrivingInterpVersion );

    const auto built = Ser::BuildClipFromAssetData( stale );
    ASSERT_FALSE( built ) << "a TMLN v1 block was read under the v2 rule: every key's shape moved one segment";
    EXPECT_NE( built.GetError().find( "Walk" ), std::string::npos ) << built.GetError();
    EXPECT_NE( built.GetError().find( "SceneMigrator" ), std::string::npos ) << built.GetError();
}

TEST( AnimationClipFormat, ABlockOfAnotherHostIsRefusedAsAClip )
{
    Anim::AnimationClip ui = SampleClip();
    ui.Sequence.Host       = Timeline::SequenceHost::UIAnimation;
    ui.Sequence.Bindings.clear();
    ui.Sequence.Tracks.clear();

    Ser::AnimationAssetData data;
    data.Name        = "NotAClip";
    data.Sequence    = BlockStating( ui.Sequence, Timeline::kTimelineFormatVersion );
    const auto built = Ser::BuildClipFromAssetData( data );
    ASSERT_FALSE( built );
    EXPECT_NE( built.GetError().find( "not an AnimationClip" ), std::string::npos ) << built.GetError();
}

// ============================================================================
// THE WRITE HALF AND THE ROUND TRIP (Д35): what SaveClipToFile writes is what the engine reads back.
// ============================================================================

// THE RELATION, not either side alone: the sequence that comes back is the one that went in, compared as the
// one writer's bytes — every track, section, key tick, value, shape and tangent, every notify and curve.
TEST( AnimationClipFormat, AClipWrittenToDiskReadsBackAsTheSameClipBitForBit )
{
    const auto path = ClipScratch() / "_Walk.anim";
    const auto clip = SampleClip();

    const auto saved = Ser::SaveClipToFile( path, clip );
    ASSERT_TRUE( saved ) << saved.GetError();

    const auto read = Ser::ReadAnimationJson( ReadFile( path ) );
    ASSERT_TRUE( read ) << "the file this engine wrote is refused by its own reader: " << read.GetError();
    ASSERT_TRUE( read.GetValue().Header.has_value() );
    EXPECT_EQ( Common::Content::TextHeaderVersion( *read.GetValue().Header, Desert::Assets::kAnimationSchemaTag ),
               std::optional<uint32_t>( Desert::Assets::kAnimationSchemaVersion ) );

    const auto rebuilt = Ser::BuildClipFromAssetData( read.GetValue() );
    ASSERT_TRUE( rebuilt ) << rebuilt.GetError();
    const Anim::AnimationClip& back = rebuilt.GetValue();

    EXPECT_EQ( back.AnimationName, clip.AnimationName );
    EXPECT_TRUE( back.Skeleton == clip.Skeleton ) << "the skeleton the clip names did not survive";
    // ONE REFERENCE, TWO STATEMENTS: the header's one Dependency is that skeleton.
    ASSERT_TRUE( read.GetValue().Skeleton.has_value() );
    EXPECT_EQ( read.GetValue().Header->Dependencies,
               ( std::vector<std::string>{ Common::Content::AssetGuidToText( clip.Skeleton ) } ) );
    EXPECT_EQ( back.Sequence.Host, Timeline::SequenceHost::AnimationClip );
    EXPECT_EQ( back.DurationTicks().Value, clip.DurationTicks().Value );
    EXPECT_EQ( back.Sequence.DisplayRate, clip.Sequence.DisplayRate );
    EXPECT_EQ( SequenceText( back.Sequence ), SequenceText( clip.Sequence ) );
}

// A second save of a clip read from disk keeps the file's GUID: the GUID IS the clip's identity.
TEST( AnimationClipFormat, ResavingAClipKeepsItsGuid )
{
    const auto path  = ClipScratch() / "_Walk.anim";
    const auto first = Ser::SaveClipToFile( path, SampleClip() );
    ASSERT_TRUE( first ) << first.GetError();
    const auto before = Ser::ReadAnimationJson( ReadFile( path ) );
    ASSERT_TRUE( before && before.GetValue().Header ) << ( before ? "" : before.GetError() );

    const auto second = Ser::SaveClipToFile( path, SampleClip() );
    ASSERT_TRUE( second ) << second.GetError();
    const auto after = Ser::ReadAnimationJson( ReadFile( path ) );
    ASSERT_TRUE( after && after.GetValue().Header ) << ( after ? "" : after.GetError() );
    EXPECT_EQ( after.GetValue().Header->Guid, before.GetValue().Header->Guid );
}

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

    const auto saved = Ser::SaveClipToFile( path, SampleClip() );
    EXPECT_FALSE( saved ) << "a clip that was never written reported itself saved";
    EXPECT_NE( saved.GetError().find( "Walk" ), std::string::npos )
         << "the refusal must name the clip the person pressed Save on: " << saved.GetError();
    EXPECT_EQ( ReadFile( path ), previousClip ) << "the failed save cost the clip that was already on disk";
}

// The pure conversion on its own, because the round trip cannot tell "the writer dropped it" from "the reader
// dropped it" when BOTH drop the same field.
TEST( AnimationClipFormat, BuildAssetDataFromClipStatesTheCurrentTimelineAndEveryTrack )
{
    const Anim::AnimationClip clip = SampleClip();
    const auto                data = Ser::BuildAssetDataFromClip( clip );
    ASSERT_TRUE( data ) << data.GetError();
    EXPECT_EQ( data.GetValue().Name, "Walk" );
    // The skeleton's AssetGuidRef is the writer's (SaveClipToFile asks the registry for its path).
    EXPECT_FALSE( data.GetValue().Skeleton.has_value() );
    EXPECT_EQ( Common::Json::Write( data.GetValue().Sequence ),
               Common::Json::Write( BlockStating( clip.Sequence, Timeline::kTimelineFormatVersion ) ) )
         << "the Sequence member is not the one writer's TMLN v" << Timeline::kTimelineFormatVersion << " block";

    Anim::AnimationClip ui = clip;
    ui.Sequence.Host       = Timeline::SequenceHost::UIAnimation;
    EXPECT_FALSE( Ser::BuildAssetDataFromClip( ui ) ) << "a UI clip was written as a .anim";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

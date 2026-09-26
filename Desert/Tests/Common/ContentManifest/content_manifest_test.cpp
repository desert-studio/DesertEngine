#include <Common/Utilities/ContentManifest.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
    fs::path MakeTempDir()
    {
        const fs::path dir = fs::temp_directory_path() / "desert_content_manifest_test";
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir;
    }

    void WriteFile( const fs::path& path, const std::string& content )
    {
        fs::create_directories( path.parent_path() );
        std::ofstream out( path, std::ios::binary | std::ios::trunc );
        out.write( content.data(), static_cast<std::streamsize>( content.size() ) );
    }
} // namespace

// THE RELATION THIS WHOLE MECHANISM RESTS ON: a manifest of a TREE and a manifest of the ARCHIVE built
// from that tree must be the same manifest. Every use of the type crosses that boundary — a release
// records its manifest from the packaged archive, and the next release's diff is computed against a
// tree, or the other way round — so if the two spellings ever disagree, every patch is wrong and
// nothing else here would notice.
TEST( ContentManifest, ATreeAndThePakBuiltFromItAgree )
{
    const fs::path dir = MakeTempDir();
    const fs::path src = dir / "src";

    WriteFile( src / "Assets" / "a.txt", "alpha" );
    WriteFile( src / "Assets" / "Scenes" / "main.desce", R"({"scene":true})" );
    WriteFile( src / "Shaders" / "s.shader", std::string( 5000, 'z' ) );
    WriteFile( src / "empty.bin", "" ); // a zero-byte file is content, not an absence

    {
        Common::Utils::PakWriter writer( dir / "Content.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        for ( auto it = fs::recursive_directory_iterator( src ); it != fs::recursive_directory_iterator(); ++it )
        {
            if ( !it->is_regular_file() )
                continue;
            ASSERT_TRUE(
                 writer.AddFile( "Resources/" + fs::relative( it->path(), src ).generic_string(), it->path() ) );
        }
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    auto fromTree = Common::Utils::ContentManifest::FromDirectory( src, "Resources" );
    ASSERT_TRUE( fromTree.IsSuccess() ) << fromTree.GetError();

    Common::Utils::PakReader reader( dir / "Content.dpak" );
    ASSERT_TRUE( reader.IsOpen() ) << reader.OpenError();
    const auto fromPak = Common::Utils::ContentManifest::FromPak( reader );

    ASSERT_EQ( fromTree.GetValue().Count(), 4u );
    EXPECT_EQ( fromTree.GetValue().Serialize(), fromPak.Serialize() );
    // Not merely equal as a set: the SERIALIZED bytes are equal, which is the property that lets a
    // release keep one artifact per version and compare it by eye or by diff.
    EXPECT_TRUE( Common::Utils::CompareManifests( fromTree.GetValue(), fromPak ).Empty() );
}

TEST( ContentManifest, SerializeParseRoundTrip )
{
    Common::Utils::ContentManifest manifest;
    manifest.Insert( { "z/last.txt", 3, 0xdeadbeefcafef00dull } );
    manifest.Insert( { "a/first.txt", 0, 0ull } );
    manifest.Insert( { "mid/a file with spaces.demat", 511, 1ull } );

    const std::string text = manifest.Serialize();
    auto              back = Common::Utils::ContentManifest::Parse( text );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_EQ( back.GetValue().Serialize(), text );

    // Sorted regardless of insertion order — the artifact must not depend on how the walk happened to
    // reach the files.
    ASSERT_EQ( back.GetValue().Count(), 3u );
    EXPECT_EQ( back.GetValue().Entries()[0].Key, "a/first.txt" );
    EXPECT_EQ( back.GetValue().Entries()[2].Key, "z/last.txt" );

    // The key is LAST on the line precisely so spaces in it survive.
    const auto* spaced = back.GetValue().Find( "mid/a file with spaces.demat" );
    ASSERT_NE( spaced, nullptr );
    EXPECT_EQ( spaced->Size, 511u );
    EXPECT_EQ( spaced->Hash, 1ull );
}

TEST( ContentManifest, ARefusalNamesTheLine )
{
    EXPECT_FALSE( Common::Utils::ContentManifest::Parse( "" ).IsSuccess() );
    EXPECT_FALSE( Common::Utils::ContentManifest::Parse( "not a manifest\n" ).IsSuccess() );

    // A wrong version is refused rather than read hopefully: the columns could mean anything.
    EXPECT_FALSE( Common::Utils::ContentManifest::Parse( "DesertContentManifest 2\n" ).IsSuccess() );

    const auto truncated = Common::Utils::ContentManifest::Parse( "DesertContentManifest 1\n0000 12\n" );
    EXPECT_FALSE( truncated.IsSuccess() );
    EXPECT_NE( truncated.GetError().find( "line 2" ), std::string::npos ) << truncated.GetError();

    const auto badHash = Common::Utils::ContentManifest::Parse( "DesertContentManifest 1\nzzzz 12 a.txt\n" );
    EXPECT_FALSE( badHash.IsSuccess() );
    EXPECT_NE( badHash.GetError().find( "line 2" ), std::string::npos ) << badHash.GetError();
}

TEST( ContentManifest, CompareNamesWhatMoved )
{
    Common::Utils::ContentManifest from;
    from.Insert( { "keep.txt", 4, 100 } );
    from.Insert( { "change.txt", 4, 200 } );
    from.Insert( { "gone.txt", 4, 300 } );

    Common::Utils::ContentManifest to;
    to.Insert( { "keep.txt", 4, 100 } );
    to.Insert( { "change.txt", 5, 201 } );
    to.Insert( { "new.txt", 4, 400 } );

    const auto diff = Common::Utils::CompareManifests( from, to );
    ASSERT_EQ( diff.Added.size(), 1u );
    EXPECT_EQ( diff.Added[0], "new.txt" );
    ASSERT_EQ( diff.Changed.size(), 1u );
    EXPECT_EQ( diff.Changed[0], "change.txt" );
    ASSERT_EQ( diff.Removed.size(), 1u );
    EXPECT_EQ( diff.Removed[0], "gone.txt" );

    EXPECT_TRUE( Common::Utils::CompareManifests( from, from ).Empty() );

    // Same hash, different size: reported as CHANGED. The size column costs nothing and turns the one
    // shape a 64-bit non-cryptographic hash cannot rule out into a false positive rather than a miss.
    Common::Utils::ContentManifest sameHash;
    sameHash.Insert( { "keep.txt", 9, 100 } );
    sameHash.Insert( { "change.txt", 4, 200 } );
    sameHash.Insert( { "gone.txt", 4, 300 } );
    EXPECT_EQ( Common::Utils::CompareManifests( from, sameHash ).Changed.size(), 1u );
}

TEST( ContentManifest, ThePakDeletionListIsNotRecordedAsContent )
{
    const fs::path dir = MakeTempDir();

    const std::string payload = "shipped";
    {
        Common::Utils::PakWriter writer( dir / "Patch.dpak" );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( "Assets/a.txt", payload.data(), payload.size() ) );
        ASSERT_TRUE( writer.SetDeletedKeys( { "Assets/b.txt" } ) );
        ASSERT_TRUE( writer.Finalize() > 0 );
    }

    Common::Utils::PakReader reader( dir / "Patch.dpak" );
    ASSERT_TRUE( reader.IsOpen() ) << reader.OpenError();
    const auto manifest = Common::Utils::ContentManifest::FromPak( reader );

    // A record of what the source handed over must not contain the bookkeeping that says what it took
    // away — otherwise the next patch computed against this manifest would try to ship it as a file.
    ASSERT_EQ( manifest.Count(), 1u );
    EXPECT_EQ( manifest.Entries()[0].Key, "Assets/a.txt" );
    EXPECT_EQ( manifest.Find( std::string( Common::Utils::kDeletedEntriesKey ) ), nullptr );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

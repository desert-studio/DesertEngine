// ONE FILE PER ENTITY (WP16, SCNE v35) - Engine/Core/Serialize/ExternalEntities.hpp.
//
// What is asserted, and why each is the property and not a proxy for it:
//   1. SPLIT THEN JOIN IS THE IDENTITY, over every scene the repository ships: the canonical text of the joined
//      document is the canonical text of the original, byte for byte, and every entity reads back as the same
//      typed record. The corpus is not partitioned, but the split does not look at that - it is the widest set
//      of real records there is (every component block, every id width).
//   2. THE PATH IS A FUNCTION OF THE ID ALONE: the same id gives the same file whatever the scene order, and
//      reordering a world's entities rewrites the header and no entity file.
//   3. AN EDIT TO ONE ENTITY CHANGES EXACTLY ONE FILE - the bytes of every file are compared, not a counter.
//   4. A DELETED ENTITY'S FILE IS DELETED, and the world reads back without it.
//   5. A LISTED FILE THAT IS MISSING IS A REFUSAL naming the path and the id; an unlisted file is a refusal
//      naming the path; a partitioned world with inline records is refused and names the migrator.

#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Json/Carry.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace
{
    namespace EE = Desert::Core::ExternalEntities;
    using Common::Json::TextDocument;

    // Same probe as world_partition_test.cpp: a file only the repository has, walked up from the runner's cwd.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::ifstream( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" ) )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    void WriteAll( const std::filesystem::path& path, const std::string& text )
    {
        std::filesystem::create_directories( path.parent_path() );
        std::ofstream out( path, std::ios::binary );
        out << text;
    }

    std::string Canonical( const TextDocument& document )
    {
        const auto text = Common::Json::WriteCanonical( document );
        EXPECT_TRUE( text ) << ( text ? "" : text.GetError() );
        return text ? text.GetValue() : std::string();
    }

    TextDocument Doc( const std::string& json )
    {
        auto document = TextDocument::Parse( json );
        EXPECT_TRUE( document ) << ( document ? "" : document.GetError() );
        return document ? document.ExtractValue() : TextDocument();
    }

    // Every file below `root` and its bytes: what "changed exactly one file" is measured against.
    std::map<std::string, std::string> Snapshot( const std::filesystem::path& root )
    {
        std::map<std::string, std::string> files;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( root ) )
            if ( entry.is_regular_file() )
                files[entry.path().lexically_relative( root ).generic_string()] = ReadAll( entry.path() );
        return files;
    }

    // A fresh directory under the system temp dir (tests never write into the source tree).
    struct TempWorld
    {
        std::filesystem::path Dir;
        std::filesystem::path Scene;

        TempWorld()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            Dir              = std::filesystem::temp_directory_path() / ( "wp16_ofpa_" + std::to_string( stamp ) );
            Scene            = Dir / "World.desce";
            std::filesystem::create_directories( Dir );
        }
        ~TempWorld()
        {
            std::error_code ec;
            std::filesystem::remove_all( Dir, ec );
        }
        TempWorld( const TempWorld& )            = delete;
        TempWorld& operator=( const TempWorld& ) = delete;
    };

    // Three records; the second id is above 2^63 so a width slip (a negative spelling) shows up in the bytes.
    constexpr const char* kIdA = "1001";
    constexpr const char* kIdB = "12345678901234567890";
    constexpr const char* kIdC = "77";

    std::string World( const std::string& aValue = "1.5", bool withB = true, bool reversed = false )
    {
        const std::string a = std::string( R"({"id":)" ) + kIdA + R"(,"Tag":"A","X":)" + aValue + "}";
        const std::string b = std::string( R"({"id":)" ) + kIdB + R"(,"Tag":"B","Foreign":{"k":[1,2]}})";
        const std::string c = std::string( R"({"id":)" ) + kIdC + R"(,"Tag":"C","parent":1001})";
        std::string       records =
             reversed ? c + "," + ( withB ? b + "," : "" ) + a : a + "," + ( withB ? b + "," : "" ) + c;
        return R"({"SceneName":"World","Entities":[)" + records +
               R"(],"Settings":{"S":1},"WorldPartition":{"Grids":[{"CellSize":12800.0,"LoadingRange":25600.0}]}})";
    }

    std::filesystem::path FileOfText( const std::filesystem::path& scene, const char* id )
    {
        return EE::FileOf( scene, Common::UUID( std::stoull( id ) ) );
    }
} // namespace

// 1. The corpus, split and joined.
TEST( ExternalEntities, SplitThenJoinIsTheIdentityOverTheCorpus )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    std::size_t scenes   = 0;
    std::size_t entities = 0;
    for ( const auto& entry :
          std::filesystem::recursive_directory_iterator( root + "Editor/Resources/Assets/Scenes" ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".desce" ||
             entry.path().generic_string().find( "/Autosave/" ) != std::string::npos )
            continue;
        const TextDocument original = Doc( ReadAll( entry.path() ) );
        auto               split    = EE::Split( original, entry.path().string() );
        ASSERT_TRUE( split ) << split.GetError();

        std::map<std::uint64_t, std::string> files;
        for ( const auto& [id, record] : split.GetValue().Records )
            files[static_cast<std::uint64_t>( id )] = Canonical( record );
        const auto joined =
             EE::Assemble( split.GetValue().Header, entry.path().string(),
                           [&]( Common::UUID id ) -> Common::ResultStr<std::string>
                           { return Common::MakeSuccess( files.at( static_cast<std::uint64_t>( id ) ) ); } );
        ASSERT_TRUE( joined ) << joined.GetError();
        EXPECT_EQ( Canonical( joined.GetValue() ), Canonical( original ) ) << entry.path();

        // Entity by entity, through the typed tree the loader instantiates.
        const auto before = original.AsDocument<Desert::Core::SceneSerialized>();
        const auto after  = joined.GetValue().AsDocument<Desert::Core::SceneSerialized>();
        ASSERT_TRUE( before && after ) << entry.path();
        ASSERT_EQ( before.GetValue().Entities.size(), after.GetValue().Entities.size() ) << entry.path();
        for ( std::size_t i = 0; i < before.GetValue().Entities.size(); ++i )
            EXPECT_EQ( Common::Json::Write( before.GetValue().Entities[i] ),
                       Common::Json::Write( after.GetValue().Entities[i] ) )
                 << entry.path() << " entity " << i;
        ++scenes;
        entities += split.GetValue().Records.size();
    }
    std::cout << "[ corpus ] " << scenes << " scenes, " << entities << " entities split and joined\n";
    EXPECT_GT( scenes, 100u );
    EXPECT_GT( entities, 1000u );
}

// 2. Stable paths.
TEST( ExternalEntities, ThePathIsAFunctionOfTheIdAlone )
{
    const std::filesystem::path scene = "Some/Dir/World.desce";
    EXPECT_EQ( EE::FileOf( scene, Common::UUID( 1001 ) ).generic_string(),
               "Some/Dir/__ExternalEntities__/World/e9/1001.deent" );
    EXPECT_EQ( EE::FileOf( scene, Common::UUID( 1001 ) ), EE::FileOf( scene, Common::UUID( 1001 ) ) );
    EXPECT_NE( EE::FileOf( scene, Common::UUID( 1001 ) ), EE::FileOf( scene, Common::UUID( 77 ) ) );

    TempWorld world;
    ASSERT_TRUE( EE::WriteSceneFile( world.Scene, Doc( World() ) ) );
    const auto before = Snapshot( world.Dir );
    // The same world with its entities in the opposite order: only the header's list moves.
    const auto outcome = EE::WriteSceneFile( world.Scene, Doc( World( "1.5", true, true ) ) );
    ASSERT_TRUE( outcome ) << outcome.GetError();
    EXPECT_EQ( outcome.GetValue().Written, 1u );
    const auto after = Snapshot( world.Dir );
    ASSERT_EQ( before.size(), after.size() );
    for ( const auto& [file, bytes] : before )
        if ( file != "World.desce" )
            EXPECT_EQ( after.at( file ), bytes ) << file;
}

// 1+3. The world on disk round-trips, and one edit is one file.
TEST( ExternalEntities, AnEditToOneEntityChangesExactlyOneFile )
{
    TempWorld  world;
    const auto first = EE::WriteSceneFile( world.Scene, Doc( World() ) );
    ASSERT_TRUE( first ) << first.GetError();
    EXPECT_EQ( first.GetValue().Written, 4u ); // the header and three entities
    for ( const char* id : { kIdA, kIdB, kIdC } )
        EXPECT_TRUE( std::filesystem::is_regular_file( FileOfText( world.Scene, id ) ) ) << id;
    EXPECT_EQ( ReadAll( world.Scene ).find( "\"Entities\"" ), std::string::npos );

    const auto read = EE::ReadSceneFileText( world.Scene );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_EQ( Canonical( Doc( read.GetValue() ) ), Canonical( Doc( World() ) ) );
    EXPECT_NE( read.GetValue().find( kIdB ), std::string::npos ); // the uint64 kept its width

    const auto before = Snapshot( world.Dir );
    const auto edit   = EE::WriteSceneFile( world.Scene, Doc( World( "2.5" ) ) );
    ASSERT_TRUE( edit ) << edit.GetError();
    const auto after = Snapshot( world.Dir );
    ASSERT_EQ( before.size(), after.size() );
    std::set<std::string> changed;
    for ( const auto& [file, bytes] : after )
        if ( before.at( file ) != bytes )
            changed.insert( file );
    const std::string expected = FileOfText( world.Scene, kIdA ).lexically_relative( world.Dir ).generic_string();
    EXPECT_EQ( changed, std::set<std::string>{ expected } );
    EXPECT_EQ( edit.GetValue().Written, 1u );
    EXPECT_EQ( edit.GetValue().Unchanged, 3u );
}

// 4. Delete.
TEST( ExternalEntities, DeletingAnEntityDeletesItsFile )
{
    TempWorld world;
    ASSERT_TRUE( EE::WriteSceneFile( world.Scene, Doc( World() ) ) );
    const auto removed = EE::WriteSceneFile( world.Scene, Doc( World( "1.5", false ) ) );
    ASSERT_TRUE( removed ) << removed.GetError();
    EXPECT_EQ( removed.GetValue().Removed, 1u );
    EXPECT_FALSE( std::filesystem::exists( FileOfText( world.Scene, kIdB ) ) );
    const auto read = EE::ReadSceneFileText( world.Scene );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_EQ( Canonical( Doc( read.GetValue() ) ), Canonical( Doc( World( "1.5", false ) ) ) );
}

// 5. Refusals.
TEST( ExternalEntities, AMissingEntityFileIsRefusedByPathAndId )
{
    TempWorld world;
    ASSERT_TRUE( EE::WriteSceneFile( world.Scene, Doc( World() ) ) );
    const auto file = FileOfText( world.Scene, kIdC );
    std::filesystem::remove( file );
    const auto read = EE::ReadSceneFileText( world.Scene );
    ASSERT_FALSE( read );
    EXPECT_NE( read.GetError().find( file.string() ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( std::string( "entity " ) + kIdC ), std::string::npos ) << read.GetError();
}

TEST( ExternalEntities, AnEntityFileTheSceneDoesNotListIsRefused )
{
    TempWorld world;
    ASSERT_TRUE( EE::WriteSceneFile( world.Scene, Doc( World() ) ) );
    const auto stray = EE::FileOf( world.Scene, Common::UUID( 5 ) );
    WriteAll( stray, R"({"id":5})" );
    const auto read = EE::ReadSceneFileText( world.Scene );
    ASSERT_FALSE( read );
    EXPECT_NE( read.GetError().find( stray.string() ), std::string::npos ) << read.GetError();
}

TEST( ExternalEntities, AFileThatIsNotTheRecordItIsNamedForIsRefused )
{
    TempWorld world;
    ASSERT_TRUE( EE::WriteSceneFile( world.Scene, Doc( World() ) ) );
    WriteAll( FileOfText( world.Scene, kIdA ), R"({"id":78,"Tag":"A"})" );
    const auto read = EE::ReadSceneFileText( world.Scene );
    ASSERT_FALSE( read );
    EXPECT_NE( read.GetError().find( "id 78" ), std::string::npos ) << read.GetError();
}

TEST( ExternalEntities, APartitionedWorldWithInlineRecordsIsRefusedAndNamesTheMigrator )
{
    TempWorld world;
    WriteAll( world.Scene, World() );
    const auto read = EE::ReadSceneFileText( world.Scene );
    ASSERT_FALSE( read );
    EXPECT_NE( read.GetError().find( "migrate.sh --write" ), std::string::npos ) << read.GetError();
}

TEST( ExternalEntities, AnUnpartitionedSceneIsReadAsItsBytes )
{
    TempWorld         world;
    const std::string text = R"({"SceneName":"Plain","Entities":[{"id":1}]})";
    WriteAll( world.Scene, text );
    const auto read = EE::ReadSceneFileText( world.Scene );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_EQ( read.GetValue(), text );
}

TEST( ExternalEntities, TheLoadersParseRefusesAHeaderOnItsOwn )
{
    TempWorld world;
    ASSERT_TRUE( EE::WriteSceneFile( world.Scene, Doc( World() ) ) );
    const auto parsed = Desert::Core::ParseLoadableScene( world.Scene.string(), ReadAll( world.Scene ) );
    ASSERT_FALSE( parsed );
    EXPECT_NE( parsed.GetError().find( "header of a partitioned world" ), std::string::npos ) << parsed.GetError();
}

TEST( ExternalEntities, ARecordWithoutAnIdOrWithADuplicateIdCannotBeSplit )
{
    EXPECT_FALSE( EE::Split( Doc( R"({"Entities":[{"Tag":"no id"}]})" ), "t" ) );
    EXPECT_FALSE( EE::Split( Doc( R"({"Entities":[{"id":3},{"id":3}]})" ), "t" ) );
}

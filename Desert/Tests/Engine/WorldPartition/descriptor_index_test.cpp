// THE DESCRIPTOR INDEX (WP18, EntityDescriptorIndex.hpp): one descriptor per entity file of a partitioned world,
// read without loading the world, refreshed per changed file, and REFUSED when stale.

#include <Engine/Core/Serialize/EntityDescriptorIndex.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace
{
    namespace EE    = Desert::Core::ExternalEntities;
    namespace DI    = Desert::Core::DescriptorIndex;
    namespace Rules = Desert::Core::Rules;

    struct TempWorld
    {
        std::filesystem::path Dir;
        std::filesystem::path Scene;

        TempWorld()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            Dir              = std::filesystem::temp_directory_path() / ( "wp18_desc_" + std::to_string( stamp ) );
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

    std::string World( const std::string& tagA = "A", bool withB = true )
    {
        const std::string a = R"({"id":1001,"Tag":")" + tagA + R"("})";
        const std::string b = R"({"id":12345678901234567890,"Tag":"B"})";
        const std::string c = R"({"id":77,"Tag":"C","parent":1001})";
        return R"({"SceneName":"World","Entities":[)" + a + "," + ( withB ? b + "," : "" ) + c +
               R"(],"WorldPartition":{"Grids":[{"CellSize":12800.0,"LoadingRange":25600.0}]}})";
    }

    bool Write( const TempWorld& world, const std::string& json )
    {
        const auto written = EE::WriteSceneText( world.Scene, json );
        EXPECT_TRUE( written ) << ( written ? std::string() : written.GetError() );
        return static_cast<bool>( written );
    }
} // namespace

// The index is the world's entities, described: row for row what DescribeEntity says of each file's record.
TEST( DescriptorIndex, TheIndexIsTheEntitiesDescribed )
{
    TempWorld world;
    ASSERT_TRUE( Write( world, World() ) );
    const auto built = DI::Refresh( world.Scene );
    ASSERT_TRUE( built ) << built.GetError();
    EXPECT_EQ( built.GetValue().Described, 3u );
    EXPECT_TRUE( built.GetValue().Written );

    const auto listed = EE::ListedEntities( world.Scene );
    ASSERT_TRUE( listed ) << listed.GetError();
    const auto& rows = built.GetValue().Index.Entities;
    ASSERT_EQ( rows.size(), listed.GetValue().size() );
    for ( std::size_t at = 0; at < rows.size(); ++at )
    {
        const auto text =
             Common::Utils::FileSystem::ReadFileContent( EE::FileOf( world.Scene, listed.GetValue()[at] ) );
        ASSERT_TRUE( text );
        const auto record = Common::Json::Read<Desert::Assets::EntityData>( text.GetValue() );
        ASSERT_TRUE( record ) << record.GetError();
        EXPECT_EQ( Common::Json::Write( rows[at].Descriptor ),
                   Common::Json::Write( Rules::DescribeEntity( record.GetValue() ) ) );
    }
    EXPECT_EQ( rows.back().Descriptor.Parent, std::optional<std::uint64_t>( 1001 ) );

    // Fresh as built; a second refresh reads nothing again and writes nothing.
    EXPECT_TRUE( DI::ReadFresh( world.Scene ) );
    const auto again = DI::Refresh( world.Scene );
    ASSERT_TRUE( again ) << again.GetError();
    EXPECT_EQ( again.GetValue().Described, 0u );
    EXPECT_EQ( again.GetValue().Reused, 3u );
    EXPECT_FALSE( again.GetValue().Written );
}

// An entity file edited behind the index's back is caught by the gate; a refresh re-describes exactly that one.
TEST( DescriptorIndex, AnEditedFileIsStaleUntilRefreshed )
{
    TempWorld world;
    ASSERT_TRUE( Write( world, World() ) );
    ASSERT_TRUE( DI::Refresh( world.Scene ) );

    const auto file = EE::FileOf( world.Scene, Common::UUID( 1001 ) );
    {
        std::ofstream out( file, std::ios::binary | std::ios::trunc );
        out << R"({"id":1001,"Tag":"Edited"})";
    }
    const auto stale = DI::ReadFresh( world.Scene );
    ASSERT_FALSE( stale );
    EXPECT_NE( stale.GetError().find( file.filename().string() ), std::string::npos ) << stale.GetError();

    const auto refreshed = DI::Refresh( world.Scene );
    ASSERT_TRUE( refreshed ) << refreshed.GetError();
    EXPECT_EQ( refreshed.GetValue().Described, 1u );
    EXPECT_EQ( refreshed.GetValue().Reused, 2u );
    EXPECT_EQ( refreshed.GetValue().Index.Entities.front().Descriptor.Tag, "Edited" );
    EXPECT_TRUE( DI::ReadFresh( world.Scene ) );
}

// A deleted entity leaves the index; until the refresh the gate refuses the index that still has it.
TEST( DescriptorIndex, ADeletedEntityLeavesTheIndex )
{
    TempWorld world;
    ASSERT_TRUE( Write( world, World() ) );
    ASSERT_TRUE( DI::Refresh( world.Scene ) );
    ASSERT_TRUE( Write( world, World( "A", false ) ) );
    EXPECT_FALSE( DI::ReadFresh( world.Scene ) );

    const auto refreshed = DI::Refresh( world.Scene );
    ASSERT_TRUE( refreshed ) << refreshed.GetError();
    EXPECT_EQ( refreshed.GetValue().Dropped, 1u );
    ASSERT_EQ( refreshed.GetValue().Index.Entities.size(), 2u );
    for ( const auto& row : refreshed.GetValue().Index.Entities )
        EXPECT_NE( row.Id, 12345678901234567890ull );
    EXPECT_TRUE( DI::ReadFresh( world.Scene ) );
}

// No index is a refusal of the gate, never an empty world.
TEST( DescriptorIndex, NoIndexIsRefused )
{
    TempWorld world;
    ASSERT_TRUE( Write( world, World() ) );
    const auto none = DI::ReadFresh( world.Scene );
    ASSERT_FALSE( none );
    EXPECT_NE( none.GetError().find( std::string( DI::kFileName ) ), std::string::npos ) << none.GetError();
}

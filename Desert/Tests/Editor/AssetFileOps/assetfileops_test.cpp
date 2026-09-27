#include <gtest/gtest.h>

#include <Editor/Core/AssetFileOps.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using Desert::Editor::AssetFileOps::UniqueName;
namespace AssetFileOps = Desert::Editor::AssetFileOps;

namespace
{
    auto InSet( const std::set<std::string>& taken )
    {
        return [&taken]( const std::string& n ) { return taken.count( n ) != 0; };
    }
} // namespace

TEST( AssetFileOps, UniqueNameKeepsFreeName )
{
    std::set<std::string> taken;
    EXPECT_EQ( UniqueName( "Texture", ".png", InSet( taken ) ), "Texture.png" );
}

TEST( AssetFileOps, UniqueNameAvoidsCollision )
{
    std::set<std::string> taken = { "Texture.png" };
    EXPECT_EQ( UniqueName( "Texture", ".png", InSet( taken ) ), "Texture 2.png" );

    taken.insert( "Texture 2.png" );
    taken.insert( "Texture 3.png" );
    EXPECT_EQ( UniqueName( "Texture", ".png", InSet( taken ) ), "Texture 4.png" );
}

TEST( AssetFileOps, UniqueNameHandlesNoExtension )
{
    std::set<std::string> taken = { "Folder" };
    EXPECT_EQ( UniqueName( "Folder", "", InSet( taken ) ), "Folder 2" );
}

// FIX8: a source's import record (<name>.<ext>.deimport) is its imported asset's identity, so it goes where
// the source goes (move, rename, delete) and a copy does not take it (a duplicate is a new asset).
namespace
{
    std::filesystem::path Sandbox( const char* name )
    {
        const auto      root = std::filesystem::temp_directory_path() / ( std::string( "desert_fileops_" ) + name );
        std::error_code ec;
        std::filesystem::remove_all( root, ec );
        std::filesystem::create_directories( root / "a" );
        std::filesystem::create_directories( root / "b" );
        std::ofstream( root / "a" / "Rock.fbx" ) << "rock";
        std::ofstream( root / "a" / "Rock.fbx.deimport" ) << "{}";
        return root;
    }
} // namespace

TEST( AssetFileOps, TheImportRecordMovesAndRenamesWithItsSource )
{
    const auto  root = Sandbox( "move" );
    std::string out, error;
    ASSERT_TRUE( AssetFileOps::Move( ( root / "a" / "Rock.fbx" ).string(), ( root / "b" ).string(), out, error ) )
         << error;
    EXPECT_TRUE( std::filesystem::exists( root / "b" / "Rock.fbx.deimport" ) );
    EXPECT_FALSE( std::filesystem::exists( root / "a" / "Rock.fbx.deimport" ) );

    ASSERT_TRUE( AssetFileOps::Rename( ( root / "b" / "Rock.fbx" ).string(), "Boulder.fbx", out, error ) ) << error;
    EXPECT_TRUE( std::filesystem::exists( root / "b" / "Boulder.fbx.deimport" ) );
    EXPECT_FALSE( std::filesystem::exists( root / "b" / "Rock.fbx.deimport" ) );

    ASSERT_TRUE( AssetFileOps::Delete( ( root / "b" / "Boulder.fbx" ).string(), error ) ) << error;
    EXPECT_FALSE( std::filesystem::exists( root / "b" / "Boulder.fbx.deimport" ) );
}

TEST( AssetFileOps, ACopyDoesNotTakeTheSourcesIdentity )
{
    const auto  root = Sandbox( "copy" );
    std::string out, error;
    ASSERT_TRUE( AssetFileOps::Duplicate( ( root / "a" / "Rock.fbx" ).string(), out, error ) ) << error;
    EXPECT_FALSE( std::filesystem::exists( out + ".deimport" ) ) << "a duplicate stated its original's GUID";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

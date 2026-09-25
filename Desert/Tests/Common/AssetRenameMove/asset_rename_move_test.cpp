// AF10c: renaming / moving an asset leaves a redirector at its old path, the registry follows it, the
// referrers are listed before the move, and undo gives back the very same bytes and rows.

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/AssetMove.hpp>
#include <Common/Content/AssetRedirector.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Editor/Core/Commands/AssetMoveCommand.hpp>
#include <Engine/Assets/ContentRegistry.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Common::Content::AssetGuid;
using Common::Content::ContentKind;

namespace
{
    AssetGuid Guid( uint64_t n )
    {
        return AssetGuid{ 0xAF10C00000000000ull | n, n };
    }

    struct Project
    {
        fs::path Root;
        fs::path SavedProject = Common::Constants::Path::CurrentProjectRoot().ProjectDir;

        explicit Project( const std::string& name )
        {
            Root = fs::temp_directory_path() / name;
            fs::remove_all( Root );
            fs::create_directories( Root );
            Root = fs::canonical( Root );
            Common::Constants::Path::SetProjectRoot( Root, "Resources/Assets" );
        }
        ~Project()
        {
            Common::Constants::Path::SetProjectRoot( SavedProject, "Resources/Assets" );
            fs::remove_all( Root );
        }
        Project( const Project& )            = delete;
        Project& operator=( const Project& ) = delete;
        Project( Project&& )                 = delete;
        Project& operator=( Project&& )      = delete;

        [[nodiscard]] fs::path In( ContentKind kind, const std::string& name ) const
        {
            const fs::path& spec = *Common::Content::KindSpec( kind ).Root;
            const fs::path  root = spec.is_absolute() ? spec : Root / spec;
            fs::create_directories( root );
            return root / ( name + std::string( Common::Content::KindSpec( kind ).Extension ) );
        }
    };

    std::string Key( const fs::path& file )
    {
        return Common::AssetHandle::StableKeyForPath( file );
    }

    std::string Bytes( const fs::path& file )
    {
        std::ifstream in( file, std::ios::binary );
        return { std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() };
    }

    void WriteAsset( const fs::path& file, ContentKind kind, const AssetGuid& guid )
    {
        Common::Content::AssetEnvelope envelope;
        envelope.Asset.Kind = kind;
        envelope.Asset.Guid = guid;
        ASSERT_TRUE( Common::Content::WriteAssetEnvelopeFile( file, envelope ) );
    }

    void Scan( Common::Utils::AssetRegistry& registry, const fs::path& file, ContentKind kind )
    {
        auto row =
             Common::Content::RegistryRowFor( Key( file ), Common::Content::DescribeContentFile( file, kind ) );
        if ( !row )
        {
            ADD_FAILURE() << row.GetError();
            return;
        }
        ASSERT_TRUE( registry.Insert( row.GetValue() ) );
    }

    // A material, a scene that names it (the edge the cook writes), and a scene that does not.
    struct Corpus
    {
        Project                      Proj{ "AF10c_Move" };
        fs::path                     Material = Proj.In( ContentKind::Material, "M_Rock" );
        fs::path                     Renamed  = Proj.In( ContentKind::Material, "M_Stone" );
        fs::path                     User     = Proj.In( ContentKind::Scene, "Uses" );
        fs::path                     Other    = Proj.In( ContentKind::Scene, "Other" );
        Common::Utils::AssetRegistry Registry;
        std::vector<uint64_t>        MaterialEdges = { Common::Content::HandleForGuid( Guid( 4 ) ) };

        Corpus()
        {
            WriteAsset( Material, ContentKind::Material, Guid( 1 ) );
            WriteAsset( User, ContentKind::Scene, Guid( 2 ) );
            WriteAsset( Other, ContentKind::Scene, Guid( 3 ) );
            Scan( Registry, Material, ContentKind::Material );
            Scan( Registry, User, ContentKind::Scene );
            Scan( Registry, Other, ContentKind::Scene );
            EXPECT_TRUE(
                 Registry.SetDependencies( Key( User ), { Common::Content::HandleForGuid( Guid( 1 ) ) } ) );
            // The material's own edge (a texture it samples): the cook learned it, the move must carry it.
            EXPECT_TRUE( Registry.SetDependencies( Key( Material ), MaterialEdges ) );
        }
    };
} // namespace

TEST( AssetRenameMove, TheConfirmListNamesExactlyTheReferrers )
{
    Corpus      c;
    const auto* row = c.Registry.FindByKey( Key( c.Material ) );
    ASSERT_NE( row, nullptr );
    EXPECT_EQ( Common::Content::ReferrersOf( c.Registry, *row ), std::vector<std::string>{ Key( c.User ) } );
}

TEST( AssetRenameMove, AMovedMaterialResolvesThroughTheRedirectorAndUndoRestoresEveryByte )
{
    Corpus            c;
    const std::string materialBytes  = Bytes( c.Material );
    const std::string registryBefore = c.Registry.Serialize();

    auto moved = Common::Content::MoveAssetLeavingRedirector( c.Registry, c.Material, c.Renamed );
    if ( !moved )
    {
        ADD_FAILURE() << moved.GetError();
        return;
    }
    // The asset's bytes moved unchanged; the old path holds a redirector naming it.
    EXPECT_EQ( Bytes( c.Renamed ), materialBytes );
    const auto redirector = Common::Content::ReadRedirectorFile( c.Material );
    if ( !redirector )
    {
        ADD_FAILURE() << redirector.GetError();
        return;
    }
    EXPECT_EQ( redirector.GetValue().Target, Guid( 1 ) );
    EXPECT_EQ( redirector.GetValue().OldKey, Key( c.Material ) );

    // The referring scene's edge and its old-path spelling both land on the moved file.
    const auto* byEdge = c.Registry.FindByHandle( Common::Content::HandleForGuid( Guid( 1 ) ) );
    ASSERT_NE( byEdge, nullptr );
    EXPECT_EQ( byEdge->Key, Key( c.Renamed ) );
    // The file did not change, so the edges the cook read out of it still hold at the new key.
    EXPECT_EQ( byEdge->Dependencies, c.MaterialEdges );
    const auto* byOldPath = c.Registry.FindByReference( 0, c.Material.string() );
    ASSERT_NE( byOldPath, nullptr );
    EXPECT_EQ( byOldPath->Key, Key( c.Renamed ) );
    const auto* redirectorRow = c.Registry.FindByKey( Key( c.Material ) );
    ASSERT_NE( redirectorRow, nullptr );
    EXPECT_EQ( redirectorRow->Kind, "Redirector" );
    EXPECT_EQ( Common::Content::ReferrersOf( c.Registry, *byEdge ).front(), Key( c.User ) );

    // Undo: the file is back byte for byte, no redirector, the registry is the one before the move.
    ASSERT_TRUE( Common::Content::UndoAssetMove( c.Registry, moved.GetValue() ) );
    EXPECT_EQ( Bytes( c.Material ), materialBytes );
    EXPECT_FALSE( fs::exists( c.Renamed ) );
    EXPECT_EQ( c.Registry.Serialize(), registryBefore );

    // Redo with the recorded redirector identity writes the same redirector bytes again.
    auto redone = Common::Content::MoveAssetLeavingRedirector( c.Registry, c.Material, c.Renamed,
                                                               moved.GetValue().Redirector.Self );
    ASSERT_TRUE( redone );
    EXPECT_EQ( redone.GetValue().Redirector, moved.GetValue().Redirector );
}

TEST( AssetRenameMove, RenamingOntoATakenNameIsRefusedByNameAndTouchesNothing )
{
    Corpus         c;
    const fs::path taken = c.Proj.In( ContentKind::Material, "M_Taken" );
    WriteAsset( taken, ContentKind::Material, Guid( 9 ) );
    const std::string before   = Bytes( c.Material );
    const std::string registry = c.Registry.Serialize();

    const auto refused = Common::Content::MoveAssetLeavingRedirector( c.Registry, c.Material, taken );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "already exists" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( taken.string() ), std::string::npos ) << refused.GetError();
    EXPECT_EQ( Bytes( c.Material ), before );
    EXPECT_EQ( c.Registry.Serialize(), registry );

    // A redirector is not a thing to move, and neither is a file the registry does not know.
    ASSERT_TRUE( Common::Content::MoveAssetLeavingRedirector( c.Registry, c.Material, c.Renamed ) );
    const auto again = Common::Content::MoveAssetLeavingRedirector(
         c.Registry, c.Material, c.Proj.In( ContentKind::Material, "M_Third" ) );
    ASSERT_FALSE( again );
    EXPECT_NE( again.GetError().find( "redirector" ), std::string::npos ) << again.GetError();
}

TEST( AssetRenameMove, UndoRefusesARedirectorThatIsNoLongerTheOneTheMoveWrote )
{
    Corpus c;
    auto   moved = Common::Content::MoveAssetLeavingRedirector( c.Registry, c.Material, c.Renamed );
    ASSERT_TRUE( moved );
    ASSERT_TRUE( Common::Content::WriteRedirectorFile(
         c.Material, Common::Content::AssetRedirector{ Guid( 77 ), Guid( 1 ), Key( c.Material ) } ) );
    const auto refused = Common::Content::UndoAssetMove( c.Registry, moved.GetValue() );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "no longer the redirector" ), std::string::npos ) << refused.GetError();
    EXPECT_TRUE( fs::exists( c.Renamed ) );
}

// The editor's route: the process registry moves under its lock, the undo stack takes the move back and
// redoes it, and the old path opens the moved file.
TEST( AssetRenameMove, TheEditorRouteMovesThroughTheRegistryAndTheUndoStack )
{
    namespace CR = Desert::Assets::ContentRegistry;
    Corpus c;
    CR::ResetForTest();
    ASSERT_GT( CR::Detail::Publish( c.Registry ), 0u );
    const std::string materialBytes = Bytes( c.Material );
    Desert::Editor::CommandHistory::Get().Clear();

    EXPECT_TRUE( CR::HasRow( c.Material ) );
    EXPECT_EQ( CR::Referrers( c.Material ), std::vector<std::string>{ Key( c.User ) } );
    const auto moved = Desert::Editor::MoveAssetWithUndo( c.Material, c.Renamed, "Rename" );
    ASSERT_TRUE( moved ) << moved.GetError();
    EXPECT_TRUE( CR::Dirty() );
    EXPECT_EQ( CR::FileToOpen( c.Material ), c.Renamed );
    EXPECT_EQ( CR::FileToOpen( c.User ), c.User );
    EXPECT_EQ( CR::Referrers( c.Renamed ), std::vector<std::string>{ Key( c.User ) } );

    ASSERT_TRUE( Desert::Editor::CommandHistory::Get().Undo() );
    EXPECT_EQ( Bytes( c.Material ), materialBytes );
    EXPECT_FALSE( fs::exists( c.Renamed ) );
    EXPECT_EQ( CR::Get().Serialize(), c.Registry.Serialize() );

    ASSERT_TRUE( Desert::Editor::CommandHistory::Get().Redo() );
    EXPECT_EQ( Bytes( c.Renamed ), materialBytes );
    EXPECT_TRUE( Common::Content::ReadRedirectorFile( c.Material ) );

    // A refusal pushes nothing: the next undo is the move above, not a phantom entry.
    const auto refused = Desert::Editor::MoveAssetWithUndo( c.Other, c.Renamed, "Rename" );
    EXPECT_FALSE( refused );
    ASSERT_TRUE( Desert::Editor::CommandHistory::Get().Undo() );
    EXPECT_FALSE( fs::exists( c.Renamed ) );
    Desert::Editor::CommandHistory::Get().Clear();
    CR::ResetForTest();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ASSET-TRASH: the editor's delete moves an asset into the project's trash and a restore gives back the very
// same tree, byte for byte, and the very same registry rows (GUID and every reference intact).

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/AssetTrash.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <optional>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
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
        Project                      Proj{ "AssetTrash_Corpus" };
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

    // Every file under @p root with its bytes: "the tree", compared whole.
    std::map<std::string, std::string> Tree( const fs::path& root )
    {
        std::map<std::string, std::string> tree;
        for ( const fs::path& file : Common::Utils::FileSystem::ListFilesRecursive( root ) )
            tree.emplace( fs::relative( file, root ).generic_string(), Bytes( file ) );
        return tree;
    }

    struct TrashedCorpus : Corpus
    {
        fs::path TrashRoot = fs::temp_directory_path() / "AssetTrash_Bin";
        fs::path Import    = Common::Content::ImportRecordPathFor( Material );

        TrashedCorpus()
        {
            fs::remove_all( TrashRoot );
            std::ofstream( Import ) << "source identity";
        }
        ~TrashedCorpus()
        {
            fs::remove_all( TrashRoot );
        }
        TrashedCorpus( const TrashedCorpus& )            = delete;
        TrashedCorpus& operator=( const TrashedCorpus& ) = delete;
        TrashedCorpus( TrashedCorpus&& )                 = delete;
        TrashedCorpus& operator=( TrashedCorpus&& )      = delete;
    };
} // namespace

TEST( AssetTrash, DeleteThenRestoreIsTheOriginalTreeByteForByte )
{
    TrashedCorpus     c;
    const auto        treeBefore     = Tree( c.Proj.Root );
    const std::string registryBefore = c.Registry.Serialize();

    auto trashed = Common::Content::MoveToTrash( c.Registry, c.Material, c.TrashRoot );
    ASSERT_TRUE( trashed ) << trashed.GetError();
    // Gone from the project and from the registry; the slot names the path and the GUID.
    EXPECT_FALSE( fs::exists( c.Material ) );
    EXPECT_FALSE( fs::exists( c.Import ) );
    EXPECT_EQ( c.Registry.FindByKey( Key( c.Material ) ), nullptr );
    EXPECT_EQ( trashed.GetValue().Guid, std::optional<Guid>( Guid( 1 ) ) );

    // Read back from disk (as after an editor restart), then restored.
    auto slot = Common::Content::ReadTrashSlot( trashed.GetValue().Slot );
    ASSERT_TRUE( slot ) << slot.GetError();
    ASSERT_TRUE( Common::Content::RestoreFromTrash( c.Registry, slot.GetValue() ) );

    EXPECT_EQ( Tree( c.Proj.Root ), treeBefore );
    EXPECT_EQ( c.Registry.Serialize(), registryBefore );
    EXPECT_FALSE( fs::exists( trashed.GetValue().Slot ) );
    // The row is back under its key with its GUID, so the referrer's GUID edge names it again.
    const auto* row = c.Registry.FindByKey( Key( c.Material ) );
    ASSERT_NE( row, nullptr );
    EXPECT_EQ( row->Guid, std::optional<AssetGuid>( Guid( 1 ) ) );
}

TEST( AssetTrash, ARestoreOntoATakenPathIsRefusedAndChangesNothing )
{
    TrashedCorpus c;
    auto          trashed = Common::Content::MoveToTrash( c.Registry, c.Material, c.TrashRoot );
    ASSERT_TRUE( trashed ) << trashed.GetError();
    std::ofstream( c.Material ) << "made after the delete";
    const std::string registryBefore = c.Registry.Serialize();

    EXPECT_FALSE( Common::Content::RestoreFromTrash( c.Registry, trashed.GetValue() ) );
    EXPECT_EQ( Bytes( c.Material ), "made after the delete" );
    EXPECT_EQ( c.Registry.Serialize(), registryBefore );
    EXPECT_TRUE( fs::exists( trashed.GetValue().Slot ) );
}

TEST( AssetTrash, APathNotOnDiskIsRefusedAndLeavesNoSlot )
{
    TrashedCorpus c;
    EXPECT_FALSE( Common::Content::MoveToTrash( c.Registry, c.Proj.Root / "Nope.demat", c.TrashRoot ) );
    EXPECT_FALSE( fs::exists( c.TrashRoot ) && !fs::is_empty( c.TrashRoot ) );
}

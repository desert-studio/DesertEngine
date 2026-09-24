// THE REGISTRY GATE - the editor's asset registry builds itself, and a cache of it is the same registry.
//
// Since AF9a no asset registry is committed. The editor gathers one at start the way UE's asset registry
// does (FAssetDataGatherer): a walk of the content roots reading each file's HEADER only, with a local cache
// outside git (Intermediate/AssetRegistry.cache) that spares the headers of unchanged files. The packager
// cooks the registry a shipped game reads. What used to be "the committed registry equals what git tracks"
// is therefore two relations:
//
//   * A CHECKOUT WITH NOTHING DERIVED FINDS ITS CONTENT - the owner's case: `Editor/Cooked` deleted, no
//     `AssetRegistry.dreg`, and the shader preload must still find StaticMeshPBR.
//   * THE REGISTRY FROM THE SCAN IS THE REGISTRY FROM THE CACHE - byte for byte, with no header re-read.
//
// plus the old census that every tracked content file reaches the engine, now against the gathered rows.

#include <Common/Content/ContentScan.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
    // The suite runs from build/Bin/Tests/<cfg>, so the repository is some way up. Probed by a file
    // that can only be this repository's.
    fs::path RepoRoot()
    {
        fs::path prefix = ".";
        for ( int up = 0; up < 8; ++up )
        {
            if ( fs::exists( prefix / "Editor" / "Desert.deproj" ) )
                return fs::absolute( prefix ).lexically_normal();
            prefix /= "..";
        }
        return {};
    }

    // Opens the sandbox project the way the editor opens it, INCLUDING the working directory. Engine
    // resource roots are never remapped by a project (Constants.hpp says so beside them), so
    // `Resources/Shaders/` resolves against the process's working directory and both hosts `cd` into
    // the directory that holds it before starting. A gate that did not would find no shaders and
    // certify a registry that is missing 76 rows.
    class SandboxProject
    {
    public:
        explicit SandboxProject( const fs::path& repoRoot )
             : m_SavedRoot( Common::Constants::Path::CurrentProjectRoot() ), m_SavedCwd( fs::current_path() )
        {
            const fs::path editorDir = repoRoot / "Editor";
            fs::current_path( editorDir );

            const auto json =
                 Common::Utils::FileSystem::ReadFileContent( ( editorDir / "Desert.deproj" ).string() );
            if ( !json )
                return;

            const auto project = Common::Project::ReadProjectFile( json.GetValue() );
            if ( !project )
                return;

            Common::Constants::Path::SetProjectRoot( editorDir, project.GetValue().AssetsRoot );
            m_Opened = true;
        }

        ~SandboxProject()
        {
            Common::Constants::Path::SetProjectRoot( m_SavedRoot.ProjectDir, m_SavedRoot.AssetsRoot );
            std::error_code ec;
            fs::current_path( m_SavedCwd, ec );
        }

        SandboxProject( const SandboxProject& )            = delete;
        SandboxProject& operator=( const SandboxProject& ) = delete;

        [[nodiscard]] bool Opened() const
        {
            return m_Opened;
        }

    private:
        Common::Constants::Path::ProjectRootState m_SavedRoot;
        fs::path                                  m_SavedCwd;
        bool                                      m_Opened = false;
    };
} // namespace

// 0. THE GATE CAN SEE WHAT IT CLAIMS TO CHECK. Without this every assertion below runs over an empty
// set and reports green — a census that found nothing wrong is byte-identical to one that found
// nothing at all.

TEST( CookedRegistryGate, WithNoCacheAndNoCookedRegistryTheScanFindsTheShaders )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject sandbox( root );
    ASSERT_TRUE( sandbox.Opened() );

    const Common::Content::GatheredRegistry gathered = Common::Content::GatherContentRegistry( {} );
    for ( const std::string& refusal : gathered.Refused )
        ADD_FAILURE() << refusal;
    EXPECT_EQ( gathered.FromCache, 0u );
    EXPECT_EQ( gathered.Read, gathered.Registry.Count() );

    bool staticMeshPBR = false;
    for ( const Common::Utils::AssetRegistryEntry* row : gathered.Registry.OfKind( "Shader" ) )
        staticMeshPBR = staticMeshPBR || row->Key.find( "StaticMeshPBR" ) != std::string::npos;
    EXPECT_TRUE( staticMeshPBR ) << "the header scan found " << gathered.Registry.OfKind( "Shader" ).size()
                                 << " shader row(s) and none is StaticMeshPBR";
}

TEST( CookedRegistryGate, TheRegistryFromTheScanIsTheRegistryFromTheCache )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject sandbox( root );
    ASSERT_TRUE( sandbox.Opened() );

    const Common::Content::GatheredRegistry scanned = Common::Content::GatherContentRegistry( {} );
    const auto                              cache =
         Common::Content::ParseRegistryCache( Common::Content::SerializeRegistryCache( scanned.Registry ) );
    ASSERT_TRUE( cache ) << cache.GetError();

    const Common::Content::GatheredRegistry loaded = Common::Content::GatherContentRegistry( cache.GetValue() );
    EXPECT_EQ( loaded.Read, 0u ) << "an unchanged tree re-read headers the cache already held";
    EXPECT_EQ( loaded.FromCache, scanned.Registry.Count() );
    EXPECT_EQ( loaded.Registry.Serialize(), scanned.Registry.Serialize() );

    // And the gathered rows agree with the full description of the same tree, row for row.
    const auto problems =
         Common::Content::Compare( loaded.Registry, Common::Content::ScanContentRoots(), "on disk" );
    for ( const Common::Content::RegistryDisagreement& problem : problems )
        ADD_FAILURE() << problem.Detail;
}

TEST( CookedRegistryGate, AFileWhoseStampChangedIsReadAgain )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject sandbox( root );
    ASSERT_TRUE( sandbox.Opened() );

    const Common::Content::GatheredRegistry scanned = Common::Content::GatherContentRegistry( {} );
    ASSERT_FALSE( scanned.Registry.Empty() );
    auto cache =
         Common::Content::ParseRegistryCache( Common::Content::SerializeRegistryCache( scanned.Registry ) );
    ASSERT_TRUE( cache );
    Common::Content::RegistryCache stale = cache.GetValue();
    stale.Modified[scanned.Registry.Entries().front().Key] -= 1;

    const Common::Content::GatheredRegistry again = Common::Content::GatherContentRegistry( stale );
    EXPECT_EQ( again.Read, 1u );
    EXPECT_EQ( again.FromCache + 1, scanned.Registry.Count() );
}

TEST( CookedRegistryGate, EveryTrackedContentFileIsGathered )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject sandbox( root );
    ASSERT_TRUE( sandbox.Opened() );

    const auto tracked = Common::Content::TrackedContent( root );
    ASSERT_TRUE( tracked.has_value() ) << "git could not be asked what this repository tracks";
    const Common::Content::GatheredRegistry gathered = Common::Content::GatherContentRegistry( {} );
    for ( const auto& [key, file] : *tracked )
        EXPECT_NE( gathered.Registry.FindByKey( key ), nullptr )
             << key << " is tracked and the scan did not gather it";
}

TEST( CookedRegistryGate, EveryContentKindIsRepresentedByTheShippedCorpus )
{
    // A GATE THAT CANNOT SEE A KIND CANNOT GUARD IT. If a content kind exists in the census and this
    // repository ships no file of it, then no run of the relation above has ever exercised that kind's
    // root or extension — and a mistake in either (a root that does not exist, an extension spelled
    // without its dot) would sit there green until the day somebody authored the first file.
    //
    // It is the same argument as "a census must be able to name what it forbids": the thing that makes
    // a census worth having is that it would have gone red, and that is only true of rows it reaches.
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const SandboxProject project( root );
    ASSERT_TRUE( project.Opened() );

    auto loaded = Common::MakeSuccess( Common::Content::GatherContentRegistry( {} ).Registry );
    ASSERT_TRUE( loaded ) << loaded.GetError();

    const auto tracked = Common::Content::TrackedContent( root );
    if ( !tracked.has_value() )
    {
        ADD_FAILURE() << "the gate could not run — see the first test for what that means";
        return;
    }

    std::set<std::string> kindsTracked;
    for ( const auto& [key, file] : *tracked )
        kindsTracked.insert( std::string( Common::Content::KindName( file.Kind ) ) );

    // KINDS THAT ARE PRODUCED, NEVER COMMITTED — one named row each, with the suite that reaches the row
    // instead. A kind listed here that the repository DOES track again is red: the exemption would then be
    // hiding the ordinary check, and the row has to go.
    //
    // Texture left this list when AF3 committed `.detex` files again; every kind still here is cook output only.
    struct CookOnlyKind
    {
        const char* Kind;
        const char* Suite; // repository-relative source file of the suite that reaches the row
        const char* Test;  // the test in it that goes red when the row is wrong
    };
    constexpr CookOnlyKind kCookOnlyKinds[] = {
         // A partitioned world's cells and index (AF2) exist only as cook output; the WorldCells suite holds
         // the census's extensions equal to the cook's file names and reads the kind back from a cooked header.
         { "WorldCell", "Desert/Tests/Engine/WorldCells/world_cells_test.cpp",
           "ACookedFileNamesItsKindInItsHeader" },
         { "WorldIndex", "Desert/Tests/Engine/WorldCells/world_cells_test.cpp",
           "ACookedFileNamesItsKindInItsHeader" },
    };

    for ( std::size_t i = 0; i < Common::Content::CONTENT_KIND_COUNT; ++i )
    {
        const auto        kind = static_cast<Common::Content::ContentKind>( i );
        const std::string name( Common::Content::KindName( kind ) );

        const auto* const cookOnly =
             std::find_if( std::begin( kCookOnlyKinds ), std::end( kCookOnlyKinds ),
                           [&name]( const CookOnlyKind& row ) { return name == row.Kind; } );
        if ( cookOnly != std::end( kCookOnlyKinds ) )
        {
            EXPECT_EQ( kindsTracked.find( name ), kindsTracked.end() )
                 << "'" << name << "' is registered as produced-never-committed, and the repository tracks a "
                 << "file of it again: delete its row in kCookOnlyKinds so the ordinary check applies";
            const std::ifstream suite( root / cookOnly->Suite );
            std::stringstream text;
            text << suite.rdbuf();
            EXPECT_NE( text.str().find( std::string( ", " ) + cookOnly->Test + " )" ), std::string::npos )
                 << "'" << name << "' is exempted on the strength of " << cookOnly->Suite << " / "
                 << cookOnly->Test << ", which no longer exists - the census row is reached by nothing";
            continue;
        }

        EXPECT_NE( kindsTracked.find( name ), kindsTracked.end() )
             << "this repository ships no '" << name
             << "' file, so nothing has ever exercised that census row: its root ("
             << Common::Content::KindSpec( kind ).Root->string() << ") and its extension ("
             << Common::Content::KindSpec( kind ).Extension
             << ") could both be wrong and every run of this gate would still be green. Add one file of "
                "that kind to the repository, or remove the row.\n"
                "COMMITTED, not merely present on your machine: a fixture nobody pushed certifies this "
                "gate for its author and for nobody else, which has already happened twice.";

        EXPECT_FALSE( loaded.GetValue().OfKind( name ).empty() )
             << "the registry holds no '" << name << "' row while the repository tracks one";
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

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

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <vector>
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

// ── THE REGISTRY IS KEYED BY GUID, END TO END (T6e) ──────────────────────────────────────────────────
//
// Built from the headers of the whole committed corpus, no editor and no load: (a) a row that states a GUID
// is known by HandleForGuid( GUID ) and by nothing else; (b) every edge names a row BY GUID — a number that
// folds from no row's GUID is either a path-derived handle or a reference to nothing, and both are refused.
// The checks are functions so the mutation tests below can prove each one turns red.
namespace
{
    std::vector<std::string> IdentityProblems( const Common::Utils::AssetRegistry& registry )
    {
        std::vector<std::string> problems;
        for ( const Common::Utils::AssetRegistryEntry& row : registry.Entries() )
        {
            if ( !row.Guid.has_value() )
            {
                if ( row.Identity != 0 )
                    problems.push_back( row.Key + ": states no GUID and still declares an identity" );
                continue;
            }
            const uint64_t fold = static_cast<uint64_t>( Common::Content::HandleForGuid( *row.Guid ) );
            if ( row.Identity != fold )
                problems.push_back( row.Key + ": identity " + std::to_string( row.Identity ) +
                                    " is not its GUID's fold " + std::to_string( fold ) );
            const Common::Utils::AssetRegistryEntry* found = registry.FindByHandle( fold );
            if ( found == nullptr || found->Key != row.Key )
                problems.push_back( row.Key + ": FindByHandle( its GUID's fold ) does not return it" );
        }
        return problems;
    }

    std::vector<std::string> DependencyProblems( const Common::Utils::AssetRegistry& registry )
    {
        std::map<uint64_t, std::string> byFold;
        for ( const Common::Utils::AssetRegistryEntry& row : registry.Entries() )
            if ( row.Guid.has_value() )
                byFold.emplace( static_cast<uint64_t>( Common::Content::HandleForGuid( *row.Guid ) ), row.Key );

        std::vector<std::string> problems;
        for ( const Common::Utils::AssetRegistryEntry& row : registry.Entries() )
        {
            for ( const uint64_t edge : row.Dependencies )
            {
                if ( byFold.find( edge ) != byFold.end() )
                    continue;
                const Common::Utils::AssetRegistryEntry* byPath = registry.FindByHandle( edge );
                problems.push_back( row.Key + ": edge " + std::to_string( edge ) +
                                    ( byPath != nullptr ? " is the PATH-derived number of " + byPath->Key
                                                        : std::string( " names no row (dangling)" ) ) );
            }
        }
        return problems;
    }

    Common::Utils::AssetRegistry ScannedCorpus()
    {
        return Common::Content::GatherContentRegistry( {} ).Registry;
    }
} // namespace

TEST( CookedRegistryGate, EveryRowWithAGuidIsKnownByItsGuidsFold )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject sandbox( root );
    ASSERT_TRUE( sandbox.Opened() );

    const Common::Utils::AssetRegistry         registry = ScannedCorpus();
    std::map<std::string, std::pair<int, int>> byKind; // kind -> rows, rows with a GUID
    for ( const Common::Utils::AssetRegistryEntry& row : registry.Entries() )
    {
        ++byKind[row.Kind].first;
        byKind[row.Kind].second += row.Guid.has_value() ? 1 : 0;
    }
    for ( const auto& [kind, counts] : byKind )
        std::printf( "  %-22s %4d row(s), %4d with a GUID\n", kind.c_str(), counts.first, counts.second );

    for ( const std::string& problem : IdentityProblems( registry ) )
        ADD_FAILURE() << problem;
}

TEST( CookedRegistryGate, EveryDependencyNamesARowByItsGuid )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject sandbox( root );
    ASSERT_TRUE( sandbox.Opened() );

    const Common::Utils::AssetRegistry registry = ScannedCorpus();
    std::size_t                        edges    = 0;
    for ( const Common::Utils::AssetRegistryEntry& row : registry.Entries() )
        edges += row.Dependencies.size();
    // A gate over zero edges proves nothing: the committed materials state their textures and cloud assets.
    EXPECT_GT( edges, 0u ) << "no row states a dependency; the header's edges are not reaching the registry";
    std::printf( "  %zu dependency edge(s) checked\n", edges );

    for ( const std::string& problem : DependencyProblems( registry ) )
        ADD_FAILURE() << problem;
}

TEST( CookedRegistryGate, AnEdgeWithAChangedGuidIsCaught )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject sandbox( root );
    ASSERT_TRUE( sandbox.Opened() );

    Common::Utils::AssetRegistry registry = ScannedCorpus();
    const auto                   withEdge =
         std::find_if( registry.Entries().begin(), registry.Entries().end(),
                       []( const Common::Utils::AssetRegistryEntry& row ) { return !row.Dependencies.empty(); } );
    ASSERT_NE( withEdge, registry.Entries().end() );
    const std::string key = withEdge->Key;

    // The same target, one bit of its GUID different: a reference to an asset that does not exist.
    const Common::Utils::AssetRegistryEntry* target = registry.FindByHandle( withEdge->Dependencies.front() );
    ASSERT_NE( target, nullptr );
    ASSERT_TRUE( target->Guid.has_value() );
    Common::Content::AssetGuid other = *target->Guid;
    other.Lo ^= 1u;
    std::vector<uint64_t> edges = withEdge->Dependencies;
    edges.front()               = static_cast<uint64_t>( Common::Content::HandleForGuid( other ) );
    ASSERT_TRUE( registry.SetDependencies( key, edges ) );
    EXPECT_FALSE( DependencyProblems( registry ).empty() );

    // And the edge spelled as the target's PATH-derived number, the form the runtime writer used to leave.
    edges.front() = target->PathHandle();
    ASSERT_TRUE( registry.SetDependencies( key, edges ) );
    EXPECT_FALSE( DependencyProblems( registry ).empty() );
}

TEST( CookedRegistryGate, ACorruptIdentityIsCaught )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject sandbox( root );
    ASSERT_TRUE( sandbox.Opened() );

    Common::Utils::AssetRegistry registry = ScannedCorpus();
    const auto                   withGuid =
         std::find_if( registry.Entries().begin(), registry.Entries().end(),
                       []( const Common::Utils::AssetRegistryEntry& row ) { return row.Guid.has_value(); } );
    ASSERT_NE( withGuid, registry.Entries().end() );
    const std::string key      = withGuid->Key;
    const uint64_t    identity = withGuid->Identity;
    ASSERT_TRUE( registry.SetIdentity( key, identity + 1 ) );
    EXPECT_FALSE( IdentityProblems( registry ).empty() );
    ASSERT_TRUE( registry.SetIdentity( key, withGuid->PathHandle() ) );
    EXPECT_FALSE( IdentityProblems( registry ).empty() );
}

// A cache written in another row form is refused by its version and the gather reads every header again —
// nobody deletes Intermediate/AssetRegistry.cache by hand.
TEST( CookedRegistryGate, ACacheOfAnotherRowFormIsRebuiltWithoutBeingDeleted )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    SandboxProject sandbox( root );
    ASSERT_TRUE( sandbox.Opened() );

    const Common::Utils::AssetRegistry registry = ScannedCorpus();
    const std::string                  current  = Common::Content::SerializeRegistryCache( registry );
    ASSERT_TRUE( Common::Content::ParseRegistryCache( current ) );

    const std::string firstLine = current.substr( 0, current.find( '\n' ) );
    const std::string body      = current.substr( current.find( '\n' ) );
    // The previous form, whose rows carried runtime-learned identities and edges.
    const std::string previous = "DesertAssetRegistryCache 2" + body;
    EXPECT_FALSE( Common::Content::ParseRegistryCache( previous ) ) << "a version-2 cache was reused";
    // A later form is not this one either; the magic is compared whole, not as a prefix.
    EXPECT_FALSE( Common::Content::ParseRegistryCache( firstLine + "0" + body ) );
    // An older registry form embedded in a current cache would hand back rows without their header columns.
    std::string olderRegistry = current;
    const auto  at            = olderRegistry.find( "\nDesertAssetRegistry 3" );
    ASSERT_NE( at, std::string::npos );
    olderRegistry.replace( at, std::string( "\nDesertAssetRegistry 3" ).size(), "\nDesertAssetRegistry 2" );
    EXPECT_FALSE( Common::Content::ParseRegistryCache( olderRegistry ) );

    // And what the editor does with a refused cache: an empty one, i.e. every header read afresh.
    const Common::Content::GatheredRegistry rebuilt = Common::Content::GatherContentRegistry( {} );
    EXPECT_EQ( rebuilt.FromCache, 0u );
    EXPECT_EQ( rebuilt.Registry.Serialize(), registry.Serialize() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

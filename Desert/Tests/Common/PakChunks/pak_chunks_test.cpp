// DIVIDING THE CONTENT INTO CHUNKS — the half of the owner's request that the pak FORMAT could not
// answer: "we should try to divide into modules, so it is easier to update and patch."
//
// Three properties are at stake here, and each of them fails in a direction the other two cannot see.
//
//   * THE CENSUS: every asset ships in exactly ONE archive, and none in zero. The second half is the
//     one that is easy to forget and it is the one that means content never reaches a player. It is
//     asserted over the REAL content tree — one file of every kind the engine knows plus every other
//     extension the packaged trees actually carry — because the defect this repository already paid
//     for (`PackagedContentTrees.hpp`: the packager forgot fonts and icons, and no `.ttf` reached a
//     built game) was invisible to every test that consulted a list instead of the tree.
//   * OBSERVABLE PRECEDENCE: the archive a byte came from is ASKED, never inferred. A patch that
//     re-ships a file unchanged is legal and common, and it makes "which archive won" invisible to a
//     comparison of the bytes — so the test calls `VFS::SourcePak` and reads the answer.
//   * THE BASE IS NOT REWRITTEN: the base archive's bytes are compared before and after the patch is
//     built and mounted. That is what makes a failed update recoverable, and nothing else in the
//     tree asserts it about a CHUNKED layout.

#include <Common/Content/ContentChunks.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Project/ProjectFormat.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>
#include <Common/Utilities/VFS.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Common::Content;
using Common::Utils::AssetRegistry;
using Common::Utils::AssetRegistryEntry;
using Common::Utils::PakReader;
using Common::Utils::VFS;

namespace
{
    // ── SYNTHETIC CORPUS: the derivation, with the graph in plain sight ──────────────────────────

    AssetRegistryEntry Row( std::string key, const std::vector<std::string>& dependsOnKeys )
    {
        AssetRegistryEntry entry;
        entry.Key  = std::move( key );
        entry.Kind = "Material";
        entry.Size = 1;
        for ( const std::string& dep : dependsOnKeys )
            entry.Dependencies.push_back( static_cast<uint64_t>( Common::AssetHandle::FromKey( dep ) ) );
        return entry;
    }

    AssetRegistry RegistryOf( std::initializer_list<AssetRegistryEntry> rows )
    {
        AssetRegistry registry;
        for ( const AssetRegistryEntry& row : rows )
            EXPECT_TRUE( registry.Insert( row ) ) << row.Key;
        return registry;
    }

    // ── THE REAL TREE ────────────────────────────────────────────────────────────────────────────

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

    // Opens the sandbox project the way the editor opens it, WORKING DIRECTORY included: engine
    // resource roots are never remapped by a project, so `Resources/Shaders/` resolves against the
    // process's cwd and both hosts `cd` into the directory that holds it. A census that did not would
    // walk no shaders at all and still report itself green.
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
            VFS::Unmount();
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

    // Every regular file under the three roots a stable key can be minted from. This is the same
    // universe the packager packs — ASSETS_PATH and COOKED_PATH are two of them and the third,
    // RESOURCE_PATH, contains the shader, font and icon trees — and it is taken from
    // `AssetHandle::ContentRoots()`'s own table rather than re-typed, so a root added there reaches
    // this census in the same edit.
    std::vector<fs::path> WalkContentTree()
    {
        std::vector<fs::path> files;
        std::set<fs::path>    seen;
        for ( const fs::path* root :
              { &Common::Constants::Path::ASSETS_PATH, &Common::Constants::Path::COOKED_PATH,
                &Common::Constants::Path::RESOURCE_PATH } )
        {
            std::error_code ec;
            if ( !fs::exists( *root, ec ) )
                continue;
            for ( auto it = fs::recursive_directory_iterator( *root, ec );
                  it != fs::recursive_directory_iterator(); it.increment( ec ) )
            {
                if ( ec )
                    break;
                if ( !it->is_regular_file() )
                    continue;
                const fs::path absolute = fs::absolute( it->path() ).lexically_normal();
                if ( seen.insert( absolute ).second )
                    files.push_back( absolute );
            }
        }
        std::sort( files.begin(), files.end() );
        return files;
    }

    // The archive key a file travels under, in the one form both directions of this suite agree on.
    std::string ArchiveKey( const fs::path& file )
    {
        std::string key = Common::AssetHandle::StableKeyForPath( file );
        std::replace( key.begin(), key.end(), ':', '/' );
        return key;
    }

    // ONE FILE PER CONTENT KIND, plus one per extension the tree carries that is NOT a content kind
    // (a font, an icon, a scene, a prefab, a script, a shader graph, raw source art). The second half
    // is the census's real subject: those are exactly the files that have no registry row, so a
    // derivation-driven packer is at its most likely to drop them, and that is the defect this
    // repository has already shipped once.
    // COOK-ONLY KINDS, EXCLUDED FROM THE SAMPLE: a developer's own cook puts them on disk, and counting them
    // when present would make this census answer differently on a clean clone. Texture left this list when AF3
    // committed `.detex` files again.
    // `WorldCell`/`WorldIndex` (AF2) are a partitioned world's cook output, written beside the scene only by a
    // cook and carried into a package by the packager's own world cook (PackagedContent reaches them).
    constexpr const char* kCookOnlyKinds[]   = { "WorldCell", "WorldIndex" };
    constexpr std::size_t kCookOnlyKindCount = sizeof( kCookOnlyKinds ) / sizeof( kCookOnlyKinds[0] );

    bool IsCookOnlyKind( const std::string& kind )
    {
        return std::any_of( std::begin( kCookOnlyKinds ), std::end( kCookOnlyKinds ),
                            [&kind]( const char* row ) { return kind == row; } );
    }

    // ONE SAMPLE PER (EXTENSION, KIND), NOT PER EXTENSION. Texture and Skybox share `.detex` and are told
    // apart by the census's deepest-root rule (the header carries the same answer), so sampling by
    // extension alone would pick one texture and never a skybox. The kind comes from `KindOfContentFile`,
    // the classification the registry itself is cooked through, never from a local extension table.
    std::vector<fs::path> OneOfEveryKindAndEveryOtherExtension( const std::vector<fs::path>&     tree,
                                                                std::map<std::string, fs::path>& byKind )
    {
        std::vector<fs::path> picked;
        std::set<std::string> samplesTaken;
        for ( const fs::path& file : tree )
        {
            std::string extension = file.extension().string();
            std::transform( extension.begin(), extension.end(), extension.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
            const std::optional<ContentKind> kind     = KindOfContentFile( file );
            const std::string                kindName = kind ? std::string( KindName( *kind ) ) : std::string();
            if ( kind && IsCookOnlyKind( kindName ) )
                continue;
            if ( !samplesTaken.insert( extension + "|" + kindName ).second )
                continue;
            picked.push_back( file );
            if ( kind )
                byKind.emplace( kindName, file );
        }
        return picked;
    }

    // THE WALK'S OWN HONESTY CHECK, AND IT IS NOT A MAGIC NUMBER.
    //
    // A threshold like "more than five hundred files" was the first draft and it was WRONG in the way
    // this repository has already paid for twice: `Editor/Cooked/` holds a developer's own cook —
    // shader SPIR-V, thumbnails, font atlases — which `.gitignore` excludes by design, so the same
    // walk finds 1514 files in the main checkout and 458 in a fresh worktree. An instrument whose
    // verdict depends on which machine ran it is not an instrument.
    //
    // The committed project registry is the list that IS the same in every clone, so the walk is held
    // against it: every row it carries must name a file the walk found. A root that silently stopped
    // being walked then fails here with the key it lost, on any machine.
    void WalkAndProveItCoveredTheCommittedRegistry( const AssetRegistry& registry, std::vector<fs::path>& out )
    {
        out = WalkContentTree();
        std::set<std::string> found;
        for ( const fs::path& file : out )
            found.insert( Common::AssetHandle::StableKeyForPath( file ) );

        for ( const AssetRegistryEntry& row : registry.Entries() )
            ASSERT_EQ( found.count( row.Key ), 1u )
                 << row.Key << " is in the committed registry but the content walk did not find it";
        ASSERT_GE( out.size(), registry.Count() );
    }

    Common::ResultStr<AssetRegistry> LoadCommittedRegistry()
    {
        const auto text =
             Common::Utils::FileSystem::ReadFileContent( Common::Utils::AssetRegistry::DefaultPath().string() );
        if ( !text )
            return Common::MakeFormattedError<AssetRegistry>( "the project registry could not be read: {}",
                                                              text.GetError() );
        return AssetRegistry::Parse( text.GetValue() );
    }

    fs::path MakeTempDir( const std::string& name )
    {
        const fs::path  dir = fs::temp_directory_path() / ( "desert_pak_chunks_" + name );
        std::error_code ec;
        fs::remove_all( dir, ec );
        fs::create_directories( dir, ec );
        return dir;
    }

    std::string ReadBytes( const fs::path& path )
    {
        std::ifstream in( path, std::ios::binary );
        return { ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() };
    }
} // namespace

// ════════════════════════════════════════════════════════════════════════════════════════════════
// 1. THE DERIVATION — membership is DATA, read out of the registry's own dependency column
// ════════════════════════════════════════════════════════════════════════════════════════════════

TEST( PakChunks, AChunkIsTheDependencyClosureOfItsRootsAndNobodyAuthorsTheMembers )
{
    // Two regions, one shared texture. Nothing below names T; the edges do.
    const AssetRegistry registry = RegistryOf( {
         Row( "assets:A.demat", { "assets:B.demat" } ),
         Row( "assets:B.demat", { "assets:T.demat" } ),
         Row( "assets:D.demat", { "assets:T.demat" } ),
         Row( "assets:T.demat", {} ),
         Row( "assets:Lonely.demat", {} ),
    } );

    ChunkScheme scheme;
    scheme.Chunks.emplace_back( ChunkRule{ "North", { "assets:A.demat" } } );
    scheme.Chunks.emplace_back( ChunkRule{ "South", { "assets:D.demat" } } );

    const auto plan = BuildChunkPlan( registry, scheme );
    ASSERT_TRUE( plan ) << plan.GetError();

    const std::size_t north = 1;
    const std::size_t south = 2;
    EXPECT_EQ( plan.GetValue().ChunkFor( "assets:A.demat" ), north );
    // B was never named by the scheme. It travels with North because A depends on it — this is the
    // whole point: adding the edge is what moves the asset, not editing a list.
    EXPECT_EQ( plan.GetValue().ChunkFor( "assets:B.demat" ), north );
    EXPECT_EQ( plan.GetValue().ChunkFor( "assets:D.demat" ), south );

    // Reached from BOTH: it goes to the base rather than being duplicated into two archives, because
    // a duplicate makes "which copy did the player get" depend on mount order for a file nobody
    // meant to override.
    EXPECT_EQ( plan.GetValue().ChunkFor( "assets:T.demat" ), BASE_CHUNK );

    // Reached from NEITHER, and a key the registry has never heard of. Both are in the base — the
    // totality that makes "no asset in zero chunks" a property of the type rather than a promise.
    EXPECT_EQ( plan.GetValue().ChunkFor( "assets:Lonely.demat" ), BASE_CHUNK );
    EXPECT_EQ( plan.GetValue().ChunkFor( "engine:Fonts/NotoSans-Regular.ttf" ), BASE_CHUNK );
}

TEST( PakChunks, APinnedKeyReturnsToTheBaseHoweverTheDerivationPlacedIt )
{
    const AssetRegistry registry = RegistryOf( {
         Row( "assets:Menu.demat", { "assets:MenuFontMaterial.demat" } ),
         Row( "assets:MenuFontMaterial.demat", {} ),
    } );

    ChunkScheme scheme;
    scheme.Chunks.emplace_back( ChunkRule{ "Menu", { "assets:Menu.demat" } } );

    const auto derived = BuildChunkPlan( registry, scheme );
    ASSERT_TRUE( derived ) << derived.GetError();
    ASSERT_EQ( derived.GetValue().ChunkFor( "assets:MenuFontMaterial.demat" ), 1u );

    scheme.AlwaysBase.push_back( "assets:MenuFontMaterial.demat" );
    const auto pinned = BuildChunkPlan( registry, scheme );
    ASSERT_TRUE( pinned ) << pinned.GetError();
    EXPECT_EQ( pinned.GetValue().ChunkFor( "assets:MenuFontMaterial.demat" ), BASE_CHUNK );
    EXPECT_EQ( pinned.GetValue().ChunkFor( "assets:Menu.demat" ), 1u );
}

TEST( PakChunks, ADependencyThatNamesNoRowIsReportedRatherThanDropped )
{
    AssetRegistryEntry broken = Row( "assets:A.demat", {} );
    broken.Dependencies.push_back( 0xDEADBEEFULL );

    AssetRegistry registry;
    ASSERT_TRUE( registry.Insert( broken ) );

    ChunkScheme scheme;
    scheme.Chunks.emplace_back( ChunkRule{ "North", { "assets:A.demat" } } );

    const auto plan = BuildChunkPlan( registry, scheme );
    ASSERT_TRUE( plan ) << plan.GetError();
    // Packaging a project with a dangling reference must still be possible — that defect belongs to
    // AssetReferences — but a closure that silently ignored the edge would produce a chunk that is
    // too small and look exactly like a correct one.
    ASSERT_EQ( plan.GetValue().UnresolvedEdges().size(), 1u );
    EXPECT_EQ( plan.GetValue().UnresolvedEdges().front(), 0xDEADBEEFULL );
}

// ── THE REFUSALS: every one of them is a case where a silent answer would be an EMPTY one ───────

TEST( PakChunks, EveryWayOfAskingForAnEmptyOrAmbiguousChunkIsRefusedByName )
{
    const AssetRegistry registry = RegistryOf( { Row( "assets:A.demat", {} ) } );

    const auto Refused = []( const AssetRegistry& reg, const ChunkScheme& scheme )
    {
        const auto plan = BuildChunkPlan( reg, scheme );
        return !plan.IsSuccess();
    };

    ChunkScheme unknownRoot;
    unknownRoot.Chunks.emplace_back( ChunkRule{ "North", { "assets:TypoedName.demat" } } );
    EXPECT_TRUE( Refused( registry, unknownRoot ) ) << "a root that names nothing yields an EMPTY chunk, "
                                                       "which reads exactly like a correct small region";

    ChunkScheme noRoots;
    noRoots.Chunks.emplace_back( ChunkRule{ "North", {} } );
    EXPECT_TRUE( Refused( registry, noRoots ) );

    ChunkScheme duplicate;
    duplicate.Chunks.emplace_back( ChunkRule{ "North", { "assets:A.demat" } } );
    duplicate.Chunks.emplace_back( ChunkRule{ "North", { "assets:A.demat" } } );
    EXPECT_TRUE( Refused( registry, duplicate ) );

    ChunkScheme callsItselfBase;
    callsItselfBase.Chunks.emplace_back( ChunkRule{ std::string( BASE_CHUNK_NAME ), { "assets:A.demat" } } );
    EXPECT_TRUE( Refused( registry, callsItselfBase ) );

    ChunkScheme unnameable;
    unnameable.Chunks.emplace_back( ChunkRule{ "../../etc", { "assets:A.demat" } } );
    EXPECT_TRUE( Refused( registry, unnameable ) ) << "a chunk name becomes part of an archive filename";

    ChunkScheme pinnedGhost;
    pinnedGhost.Chunks.emplace_back( ChunkRule{ "North", { "assets:A.demat" } } );
    pinnedGhost.AlwaysBase.push_back( "assets:NotHere.demat" );
    EXPECT_TRUE( Refused( registry, pinnedGhost ) );

    // And the control: the shape they are all deviations from must pass, or the assertions above
    // would be satisfied by a function that refuses everything.
    ChunkScheme good;
    good.Chunks.emplace_back( ChunkRule{ "North", { "assets:A.demat" } } );
    EXPECT_FALSE( Refused( registry, good ) );
}

TEST( PakChunks, TheSchemeRoundTripsAndAnEmptyFileIsAProjectThatWasNeverDivided )
{
    ChunkScheme scheme;
    scheme.Chunks.emplace_back( ChunkRule{ "North", { "assets:A.demat", "assets:B.demat" } } );
    scheme.AlwaysBase.push_back( "assets:Loading.demat" );

    const auto parsed = ParseChunkScheme( WriteChunkScheme( scheme ) );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    ASSERT_EQ( parsed.GetValue().Chunks.size(), 1u );
    EXPECT_EQ( parsed.GetValue().Chunks.front().Name, "North" );
    EXPECT_EQ( parsed.GetValue().Chunks.front().Roots.size(), 2u );
    EXPECT_EQ( parsed.GetValue().AlwaysBase, scheme.AlwaysBase );

    const auto empty = ParseChunkScheme( "   \n\t " );
    ASSERT_TRUE( empty ) << empty.GetError();
    EXPECT_TRUE( empty.GetValue().Chunks.empty() );

    const auto rubbish = ParseChunkScheme( "{ this is not json" );
    EXPECT_FALSE( rubbish.IsSuccess() );
}

TEST( PakChunks, TheChunkListSurvivesTheHostThatWroteIt )
{
    // Windows is the target platform and this text crosses hosts inside an archive. A name with a
    // trailing carriage return names a file that does not exist, and the refusal would arrive at the
    // player as "an update is missing" rather than as the line-ending problem it is.
    const auto parsed = ParseChunkManifest( "North\r\nSouth\r\n" );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    ASSERT_EQ( parsed.GetValue().size(), 2u );
    EXPECT_EQ( parsed.GetValue()[0], "North" );
    EXPECT_EQ( parsed.GetValue()[1], "South" );

    EXPECT_FALSE( ParseChunkManifest( "../escape\n" ).IsSuccess() );
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// 2. THE CENSUS — over the REAL tree, against the REAL archives
//
// COUNTED BY THE TREE, NEVER BY THE SCHEME. That relation is what `PackagedContentTrees.hpp` was
// written for: a list of what ships is a list you can fall out of silently, and the day somebody did,
// a built game contained no font at all. So the population below is a directory walk, and the
// question asked of every file it finds is "how many archives carry your key" — with BOTH wrong
// answers named, because they fail in opposite directions: two means the player's bytes depend on
// mount order, zero means the content never reaches them.
// ════════════════════════════════════════════════════════════════════════════════════════════════

TEST( PakChunks, EveryFileOfEveryContentKindLandsInExactlyOneArchiveAndNoneInZero )
{
    const fs::path repo = RepoRoot();
    ASSERT_FALSE( repo.empty() ) << "the repository root could not be found from the test's cwd";
    const SandboxProject project( repo );
    ASSERT_TRUE( project.Opened() ) << "Editor/Desert.deproj could not be read";

    const auto registry = LoadCommittedRegistry();
    ASSERT_TRUE( registry ) << registry.GetError();

    std::vector<fs::path> tree;
    ASSERT_NO_FATAL_FAILURE( WalkAndProveItCoveredTheCommittedRegistry( registry.GetValue(), tree ) );

    std::map<std::string, fs::path> byKind;
    const std::vector<fs::path>     corpus = OneOfEveryKindAndEveryOtherExtension( tree, byKind );

    // EVERY KIND, NOT ONE. A round trip that exercised a single extension would prove nothing about
    // every committed kind the engine enumerates.
    ASSERT_EQ( byKind.size(), CONTENT_KIND_COUNT - kCookOnlyKindCount )
         << "the tree no longer carries a file of every committed kind ContentKinds.hpp declares";

    // A chunk rooted at a real asset of the project, so the closure below is over real edges.
    const fs::path    materialFile = byKind.at( "Material" );
    const std::string materialKey  = Common::AssetHandle::StableKeyForPath( materialFile );
    ASSERT_NE( registry.GetValue().FindByKey( materialKey ), nullptr ) << materialKey;

    ChunkScheme scheme;
    scheme.Chunks.emplace_back( ChunkRule{ "Region", { materialKey } } );

    const auto plan = BuildChunkPlan( registry.GetValue(), scheme );
    ASSERT_TRUE( plan ) << plan.GetError();
    ASSERT_EQ( plan.GetValue().Count(), 2u );

    std::vector<std::pair<std::string, fs::path>> files;
    files.reserve( files.size() + corpus.size() );
    for ( const fs::path& file : corpus )
        files.emplace_back( ArchiveKey( file ), file );

    const fs::path dir     = MakeTempDir( "census" );
    const auto     written = WriteChunkedPaks( dir / "Content.dpak", plan.GetValue(), files );
    ASSERT_TRUE( written ) << written.GetError();

    // ── THE CENSUS ITSELF ───────────────────────────────────────────────────────────────────────
    std::vector<std::unique_ptr<PakReader>> archives;
    for ( const fs::path& archive : written.GetValue().Archives )
    {
        archives.push_back( std::make_unique<PakReader>( archive ) );
        ASSERT_TRUE( archives.back()->IsOpen() ) << archive.string() << ": " << archives.back()->OpenError();
    }

    for ( const auto& [key, source] : files )
    {
        int carriers = 0;
        for ( const auto& archive : archives )
            carriers += archive->Contains( key ) ? 1 : 0;

        EXPECT_EQ( carriers, 1 ) << key << " is in " << carriers
                                 << " archive(s): 0 means it never reaches a player, 2 means which "
                                    "bytes they get depends on mount order";
    }

    // AND THE TOTAL, because a per-key loop cannot see a file the writer INVENTED. The base also
    // carries the chunk list, which is one entry and is not content.
    std::size_t entries = 0;
    for ( const std::size_t count : written.GetValue().Entries )
        entries += count;
    EXPECT_EQ( entries, files.size() + 1 );

    // The division actually happened: the region is not empty and it is not everything.
    EXPECT_GT( written.GetValue().Entries[1], 0u );
    EXPECT_GT( written.GetValue().Entries[BASE_CHUNK], written.GetValue().Entries[1] );
}

TEST( PakChunks, TheWholeContentTreeIsAssignedAndTheAssignmentIsAFunction )
{
    const fs::path repo = RepoRoot();
    ASSERT_FALSE( repo.empty() );
    const SandboxProject project( repo );
    ASSERT_TRUE( project.Opened() );

    const auto registry = LoadCommittedRegistry();
    ASSERT_TRUE( registry ) << registry.GetError();

    std::vector<fs::path> tree;
    ASSERT_NO_FATAL_FAILURE( WalkAndProveItCoveredTheCommittedRegistry( registry.GetValue(), tree ) );

    const auto plan = BuildChunkPlan( registry.GetValue(), ChunkScheme{} );
    ASSERT_TRUE( plan ) << plan.GetError();

    // THE SCALE THE ARCHIVE CENSUS ABOVE CANNOT AFFORD (the tree is 353 MB), asked of the assignment
    // instead of the artifact: every file the walk finds resolves to a chunk that exists, and no two
    // files claim one key. The second half is what would catch a key derivation that collapsed two
    // different files onto one archive entry, which is a silent loss of one of them.
    std::map<std::string, fs::path> keyOwner;
    for ( const fs::path& file : tree )
    {
        const std::size_t chunk = plan.GetValue().ChunkFor( Common::AssetHandle::StableKeyForPath( file ) );
        ASSERT_LT( chunk, plan.GetValue().Count() ) << file.string();

        const std::string key  = ArchiveKey( file );
        const auto [it, fresh] = keyOwner.emplace( key, file );
        EXPECT_TRUE( fresh ) << key << " is claimed by both " << it->second.string() << " and " << file.string();
    }
    EXPECT_EQ( keyOwner.size(), tree.size() );
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// 3. PRECEDENCE, ASKED RATHER THAN INFERRED — and the base left untouched
// ════════════════════════════════════════════════════════════════════════════════════════════════

TEST( PakChunks, APatchOverridesBaseAndChunkAndTheSourceArchiveIsAnAnswerNotAnInference )
{
    const fs::path repo = RepoRoot();
    ASSERT_FALSE( repo.empty() );
    const SandboxProject project( repo );
    ASSERT_TRUE( project.Opened() );

    const auto registry = LoadCommittedRegistry();
    ASSERT_TRUE( registry ) << registry.GetError();

    std::vector<fs::path> tree;
    ASSERT_NO_FATAL_FAILURE( WalkAndProveItCoveredTheCommittedRegistry( registry.GetValue(), tree ) );
    std::map<std::string, fs::path> byKind;
    const std::vector<fs::path>     corpus = OneOfEveryKindAndEveryOtherExtension( tree, byKind );
    ASSERT_EQ( byKind.size(), CONTENT_KIND_COUNT - kCookOnlyKindCount );

    const fs::path    materialFile = byKind.at( "Material" );
    const std::string materialKey  = Common::AssetHandle::StableKeyForPath( materialFile );
    ChunkScheme       scheme;
    scheme.Chunks.emplace_back( ChunkRule{ "Region", { materialKey } } );

    const auto plan = BuildChunkPlan( registry.GetValue(), scheme );
    ASSERT_TRUE( plan ) << plan.GetError();

    std::vector<std::pair<std::string, fs::path>> files;
    files.reserve( files.size() + corpus.size() );
    for ( const fs::path& file : corpus )
        files.emplace_back( ArchiveKey( file ), file );

    const fs::path dir     = MakeTempDir( "patch" );
    const fs::path base    = dir / "Content.dpak";
    const auto     written = WriteChunkedPaks( base, plan.GetValue(), files );
    ASSERT_TRUE( written ) << written.GetError();

    const std::string baseBefore = ReadBytes( base );
    ASSERT_FALSE( baseBefore.empty() );

    // Pick one key from the base and one from the chunk, so the patch is shown to beat BOTH.
    const std::string chunkedKey = ArchiveKey( materialFile );
    std::string       baseKey;
    for ( const auto& [key, source] : files )
        if ( plan.GetValue().ChunkFor( Common::AssetHandle::StableKeyForPath( source ) ) == BASE_CHUNK )
        {
            baseKey = key;
            break;
        }
    ASSERT_FALSE( baseKey.empty() );

    const std::string patched = "PATCHED-BY-THE-UPDATE";
    const fs::path    patch   = dir / "Patch_01.dpak";
    {
        Common::Utils::PakWriter writer( patch );
        ASSERT_TRUE( writer.IsOpen() );
        ASSERT_TRUE( writer.AddData( baseKey, patched.data(), patched.size() ) );
        ASSERT_TRUE( writer.AddData( chunkedKey, patched.data(), patched.size() ) );
        ASSERT_GT( writer.Finalize(), 0u );
    }

    // THE BASE IS NOT REWRITTEN. Byte for byte, after the patch exists — an update that edits the
    // archive it overlays has no rollback, and the whole overlay model is there to avoid exactly that.
    EXPECT_EQ( ReadBytes( base ), baseBefore )
         << "building a patch changed the base archive; an interrupted update would then have nothing "
            "to fall back to";

    VFS::Unmount();
    ASSERT_TRUE( VFS::MountPak( base ) );
    for ( std::size_t i = 1; i < written.GetValue().Archives.size(); ++i )
        ASSERT_TRUE( VFS::MountPak( written.GetValue().Archives[i] ) );

    // BEFORE the patch is mounted: the chunked key must come from the CHUNK, not the base. This is
    // the half that proves the division is real — the bytes are identical either way, so only
    // SourcePak can tell.
    EXPECT_EQ( VFS::SourcePak( dir / fs::path( chunkedKey ) ),
               std::optional<fs::path>( written.GetValue().Archives[1] ) );
    EXPECT_EQ( VFS::SourcePak( dir / fs::path( baseKey ) ), std::optional<fs::path>( base ) );

    ASSERT_TRUE( VFS::MountPak( patch ) );

    EXPECT_EQ( VFS::SourcePak( dir / fs::path( baseKey ) ), std::optional<fs::path>( patch ) );
    EXPECT_EQ( VFS::SourcePak( dir / fs::path( chunkedKey ) ), std::optional<fs::path>( patch ) );
    EXPECT_EQ( VFS::ReadFile( dir / fs::path( baseKey ) ), std::optional<std::string>( patched ) );
    EXPECT_EQ( VFS::ReadFile( dir / fs::path( chunkedKey ) ), std::optional<std::string>( patched ) );

    // The negative control: a key the patch does NOT carry still comes from where it was, so the
    // assertions above are about precedence and not about the patch having swallowed the stack.
    std::string untouched;
    for ( const auto& [key, source] : files )
        if ( key != baseKey && key != chunkedKey )
        {
            untouched = key;
            break;
        }
    ASSERT_FALSE( untouched.empty() );
    EXPECT_NE( VFS::SourcePak( dir / fs::path( untouched ) ), std::optional<fs::path>( patch ) );

    VFS::Unmount();
}

TEST( PakChunks, EveryContentKindSurvivesTheDivisionByteForByte )
{
    const fs::path repo = RepoRoot();
    ASSERT_FALSE( repo.empty() );
    const SandboxProject project( repo );
    ASSERT_TRUE( project.Opened() );

    const auto registry = LoadCommittedRegistry();
    ASSERT_TRUE( registry ) << registry.GetError();

    std::vector<fs::path> tree;
    ASSERT_NO_FATAL_FAILURE( WalkAndProveItCoveredTheCommittedRegistry( registry.GetValue(), tree ) );
    std::map<std::string, fs::path> byKind;
    const std::vector<fs::path>     corpus = OneOfEveryKindAndEveryOtherExtension( tree, byKind );
    ASSERT_EQ( byKind.size(), CONTENT_KIND_COUNT - kCookOnlyKindCount );

    const std::string materialKey = Common::AssetHandle::StableKeyForPath( byKind.at( "Material" ) );
    ChunkScheme       scheme;
    scheme.Chunks.emplace_back( ChunkRule{ "Region", { materialKey } } );
    const auto plan = BuildChunkPlan( registry.GetValue(), scheme );
    ASSERT_TRUE( plan ) << plan.GetError();

    std::vector<std::pair<std::string, fs::path>> files;
    files.reserve( files.size() + corpus.size() );
    for ( const fs::path& file : corpus )
        files.emplace_back( ArchiveKey( file ), file );

    const fs::path dir     = MakeTempDir( "roundtrip" );
    const auto     written = WriteChunkedPaks( dir / "Content.dpak", plan.GetValue(), files );
    ASSERT_TRUE( written ) << written.GetError();

    VFS::Unmount();
    for ( const fs::path& archive : written.GetValue().Archives )
        ASSERT_TRUE( VFS::MountPak( archive ) );

    // THE ROUND TRIP, PER KIND — the codec is chosen per entry from the DATA, so a text-shaped kind
    // and a compressed-binary kind take different paths through the writer and the reader.
    for ( const auto& [kind, file] : byKind )
    {
        const std::string key         = ArchiveKey( file );
        const std::string onDisk      = ReadBytes( file );
        const auto        fromArchive = VFS::ReadFile( dir / fs::path( key ) );
        ASSERT_TRUE( fromArchive.has_value() ) << kind << ": " << key << " did not read back";
        EXPECT_EQ( *fromArchive, onDisk ) << kind << ": " << key << " changed in the archive";
    }

    // The base's own declaration of what else has to be mounted, read back out of the archive.
    const auto manifest = VFS::ReadFile( dir / fs::path( std::string( CHUNK_MANIFEST_KEY ) ) );
    ASSERT_TRUE( manifest.has_value() );
    const auto names = ParseChunkManifest( *manifest );
    ASSERT_TRUE( names ) << names.GetError();
    ASSERT_EQ( names.GetValue().size(), 1u );
    EXPECT_EQ( names.GetValue().front(), "Region" );

    VFS::Unmount();
}

TEST( PakChunks, ADeclaredChunkThatReceivesNoFileIsARefusalAndNotAnEmptyArchive )
{
    const AssetRegistry registry = RegistryOf( { Row( "assets:A.demat", {} ) } );
    ChunkScheme         scheme;
    scheme.Chunks.emplace_back( ChunkRule{ "Region", { "assets:A.demat" } } );
    const auto plan = BuildChunkPlan( registry, scheme );
    ASSERT_TRUE( plan ) << plan.GetError();

    const fs::path dir    = MakeTempDir( "empty" );
    const fs::path source = dir / "unrelated.txt";
    {
        std::ofstream out( source, std::ios::binary );
        out << "content the chunk's roots do not reach";
    }

    // The chunk's root asset is not among the packed files, so the chunk gets nothing. An empty
    // archive here reads exactly like a correct small region, which is why it is refused.
    const auto written =
         WriteChunkedPaks( dir / "Content.dpak", plan.GetValue(), { { "Assets/unrelated.txt", source } } );
    EXPECT_FALSE( written.IsSuccess() );
    EXPECT_NE( written.GetError().find( "Region" ), std::string::npos ) << written.GetError();
    // AND IT SAYS WHY, which is the whole reason this refusal exists separately. `Finalize()` already
    // returns 0 for an archive with no entries, so a writer WITHOUT this check still fails here — with
    // "failed to finalize Chunk_Region.dpak", which reads as a disk problem and sends the reader to
    // the wrong place. Asserting only the chunk's name passed against both, i.e. proved nothing:
    // deleting the check left this test green (mutation, 2026-09-22).
    EXPECT_NE( written.GetError().find( "received no files" ), std::string::npos ) << written.GetError();
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

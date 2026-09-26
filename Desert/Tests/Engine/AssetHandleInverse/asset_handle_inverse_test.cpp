// A HANDLE MUST BE ABLE TO NAME ITS OWN PATH — asserted as a RELATION over the shipped corpus, not
// demonstrated on one example.
//
// `AssetHandle::FromCookedPath` is a one-way hash. Until `Common::AssetPathIndex` existed the only route
// back from a number to a file in this repository was `Core::MakeAssetResolver`'s `ToPath`, which asks the
// ASSET (`AssetManager::FindByHandle<T>`) through twelve hand-written per-type branches — so the answer
// needed the payload to be loaded and a branch for the type, and a type with no branch got "" with no
// error. This suite is the assertion that the new route has neither property.
//
// ── THE PREMISE THIS SUITE RETIRES ────────────────────────────────────────────────────────────────────
//
// `AssetHandle.hpp` carried, for a long time, the sentence "nothing in the repository referenced a
// path-derived handle by number", and that sentence was the licence under which the derivation was once
// re-stamped project-relative with no migration. It is FALSE, and it is falsifiable by arithmetic:
// scenes once stored `"MeshPath":"Cooked/Meshes/IKProbe.skmesh"` beside `"MeshGuid":556331627295699705`,
// and that number was exactly `FromCookedPath` of that path (ten such pairs, retired by SCNE 28, which names
// a mesh by its header GUID text). Still committed: 95 `TextureHandle` numbers, and 113 adopted-id entries
// in `.demat` files (bridged through the legacy register, since MATL 2 moved material identity to the
// header GUID).
//
// So the derivation is load-bearing for committed content, and `TestB` below is what makes moving it a RED
// BUILD naming the file rather than a silent emptying discovered a session later. (`AssetReferenceCensus`
// asserts the same shape for `.demat` texture slots; this suite covers what a scene writes, which that one
// does not read.)
//
// ── WHY THE PAIRS AND NOT A LIST OF NUMBERS ───────────────────────────────────────────────────────────
//
// A pinned list of known-good handles is satisfied by editing the list. Every assertion here pins a NAMED
// ROW that shipped content already contains — a path and a number written side by side for one reference —
// and derives the count from the corpus. There is no number typed into this file.
//
// `MaterialPaths` + `MaterialGuids` names the material by its header GUID TEXT (SCNE 27; before, an
// adopted legacy id), and breaks if the path and the GUID stop naming one file. The former mesh half
// (`MeshPath` + a path-derived `MeshGuid` number) is gone with SCNE 28: a scene names a mesh by its header
// GUID text, so no scene row depends on the path derivation any more.

#include <Common/Json/Document.hpp>
#include <Common/Json/Json.hpp>
#include <gtest/gtest.h>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Constants.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
#include <set>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // Walks up from the working directory looking for a file only the repository has. Same shape as
    // AssetReferenceCensus and MaterialIdentity, for the same reason: the runner's working directory is
    // not fixed, and the suites share no header.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Blanks out `//` and block comments, keeping every byte offset and newline so a reported line
    // number still means something.
    //
    // WHY THE CENSUSES BELOW NEED IT, MEASURED: the first run of this suite reported
    // `CloudLayoutAsset.cpp assigns m_Metadata.Handle directly`, and that file contains no such
    // assignment — its comment QUOTES the line it used to have, to explain why it no longer does. A
    // census that fires on prose is one somebody switches off, and it would have taken the real finding
    // in the same run down with it.
    std::string WithoutComments( std::string text )
    {
        for ( size_t at = 0; at + 1 < text.size(); ++at )
        {
            if ( text[at] == '"' )
            {
                // Skip a string literal whole, so a `//` inside one is not mistaken for a comment.
                for ( ++at; at < text.size() && text[at] != '"'; ++at )
                {
                    if ( text[at] == '\\' )
                        ++at;
                }
                continue;
            }
            if ( text[at] != '/' )
                continue;
            if ( text[at + 1] == '/' )
            {
                for ( ; at < text.size() && text[at] != '\n'; ++at )
                    text[at] = ' ';
                continue;
            }
            if ( text[at + 1] == '*' )
            {
                const size_t end  = text.find( "*/", at + 2 );
                const size_t last = end == std::string::npos ? text.size() : end + 2;
                for ( ; at < last; ++at )
                {
                    if ( text[at] != '\n' )
                        text[at] = ' ';
                }
                --at;
            }
        }
        return text;
    }

    // One offence per line. A FREE FUNCTION and not the `<< [&]{...}()` it replaces: a parameter-less
    // multi-line lambda inside a stream chain is one of the constructs clang-format 18 (the CI gate) and
    // clang-format 22 (what this machine has) wrap differently, so the local check passes work the gate
    // rejects. The rule is cheaper to obey than to rediscover.
    std::string Joined( const std::vector<std::string>& lines )
    {
        std::string out;
        for ( const std::string& line : lines )
            out += line + "\n";
        return out;
    }

    // Saves and restores the process-wide project root AND the index that is keyed behind it. The two
    // belong together: re-rooting mints the same relative keys behind different absolute roots, so an
    // index left over from the previous root answers the next test's questions with the previous test's
    // table.
    class ProjectRootGuard
    {
    public:
        ProjectRootGuard() : m_Saved( Common::Constants::Path::CurrentProjectRoot() )
        {
            Common::AssetPathIndex::Clear();
        }

        ~ProjectRootGuard()
        {
            Common::Constants::Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
            Common::AssetPathIndex::Clear();
        }

        ProjectRootGuard( const ProjectRootGuard& )            = delete;
        ProjectRootGuard& operator=( const ProjectRootGuard& ) = delete;

    private:
        Common::Constants::Path::ProjectRootState m_Saved;
    };

    // Reads every file under `dir` whose extension is `ext`, as text. Used by the two source-text censuses
    // at the bottom, which check a rule about the CODE rather than about the content.
    std::vector<fs::path> SourcesUnder( const fs::path& dir )
    {
        std::vector<fs::path> out;
        if ( !fs::exists( dir ) )
            return out;
        for ( const auto& entry : fs::recursive_directory_iterator( dir ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const std::string ext = entry.path().extension().string();
            if ( ext == ".cpp" || ext == ".hpp" )
                out.push_back( entry.path() );
        }
        return out;
    }

    // ── The corpus rows ───────────────────────────────────────────────────────────────────────────────

    // One (path, GUID) pair a shipped scene wrote for ONE reference.
    struct PathAndGuid
    {
        std::string Scene;
        std::string Field; // which pair of fields it came from, for the failure message
        std::string Path;
        std::string Guid; // the header GUID text the scene states (SCNE 27)
    };

    // Pulls every such pair out of one parsed scene document. The walk is recursive because a component
    // block sits at an unknown depth inside the entity list, and hard-coding the depth is how the next
    // format change makes this census pass over nothing.
    //
    // Every string element of an array node, in order; an element that is not a string reads as nullopt so
    // the zip below still lines paths up against guids by position.
    std::vector<std::optional<std::string>> CollectStrings( const Common::Json::Node& array )
    {
        std::vector<std::optional<std::string>> out;
        array.ForEachElement(
             [&]( std::size_t, const Common::Json::Node& element )
             {
                 const auto text = element.AsString();
                 out.push_back( text ? std::optional<std::string>( text.GetValue() ) : std::nullopt );
             } );
        return out;
    }

    // The recursion is silenced rather than removed: the document IS recursive, and an explicit worklist
    // would still have to hold one Node per pending subtree, which recursion already does for free. The
    // directive has to be the LAST comment line before the statement — one more line of prose under it and
    // clang-tidy does not see it, which is how the first attempt at this went red with the comment already
    // written.
    // NOLINTNEXTLINE(misc-no-recursion)
    void CollectPairs( const Common::Json::Node& node, const std::string& scene,
                       std::vector<PathAndGuid>& materials )
    {
        if ( node.GetKind() == Common::Json::Kind::Array )
        {
            node.ForEachElement( [&]( std::size_t, const Common::Json::Node& element )
                                 { CollectPairs( element, scene, materials ); } );
            return;
        }

        if ( node.GetKind() != Common::Json::Kind::Object )
            return;

        const auto paths = node.Find( "MaterialPaths" );
        const auto guids = node.Find( "MaterialGuids" );
        if ( paths && guids && paths->GetKind() == Common::Json::Kind::Array &&
             guids->GetKind() == Common::Json::Kind::Array )
        {
            const auto p = CollectStrings( *paths );
            const auto g = CollectStrings( *guids );
            for ( size_t at = 0; at < p.size() && at < g.size(); ++at )
            {
                if ( !p[at] || !g[at] || p[at]->empty() || g[at]->empty() )
                    continue;
                materials.push_back( { scene, "MaterialPaths/MaterialGuids", *p[at], *g[at] } );
            }
        }

        node.ForEachMember( [&]( std::string_view, const Common::Json::Node& value )
                            { CollectPairs( value, scene, materials ); } );
    }

    void CollectPairsInScenes( const fs::path& scenesRoot, std::vector<PathAndGuid>& materials,
                               std::string* parseError )
    {
        for ( const auto& entry : fs::directory_iterator( scenesRoot ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
                continue;
            const auto parsed = Common::Json::Parse( ReadAll( entry.path() ) );
            if ( !parsed )
            {
                if ( parseError != nullptr && parseError->empty() )
                    *parseError = entry.path().filename().string() + ": " + parsed.GetError();
                continue;
            }
            CollectPairs( Common::Json::Root( parsed.GetValue() ), entry.path().filename().string(), materials );
        }
    }

    // The quoted string value that follows `key` in `text`, starting the search at `from` — tolerant of
    // the whitespace both `LegacyMaterialIds.json` and a `.demat` pretty-print between the colon and the
    // opening quote (`"Guid": "..."`, not `"Guid":"..."`). Returns {} and leaves `at` at npos on failure.
    std::string QuotedValueAfter( const std::string& text, const std::string& key, size_t from, size_t& at )
    {
        at = text.find( key, from );
        if ( at == std::string::npos )
            return {};
        size_t cursor = at + key.size();
        while ( cursor < text.size() && std::isspace( static_cast<unsigned char>( text[cursor] ) ) )
            ++cursor;
        if ( cursor >= text.size() || text[cursor] != '"' )
        {
            at = std::string::npos;
            return {};
        }
        const size_t end = text.find( '"', cursor + 1 );
        if ( end == std::string::npos )
        {
            at = std::string::npos;
            return {};
        }
        return text.substr( cursor + 1, end - cursor - 1 );
    }

    // The header GUID text a `.demat` states, read as raw text (not through a struct) so a schema change to
    // MaterialData cannot make this census quietly stop finding the field.
    std::string GuidOfDemat( const fs::path& demat )
    {
        size_t at = 0;
        return QuotedValueAfter( ReadAll( demat ), R"("Guid":)", 0, at );
    }
} // namespace

// ── A. The relation itself, over every file the project ships ─────────────────────────────────────────

TEST( AssetHandleInverse, EveryContentFileIsNamedBackByItsOwnHandle )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const fs::path content = fs::path( root ) / "Editor/Resources/Assets";
    ASSERT_TRUE( fs::exists( content ) ) << content.string() << " is missing";

    const ProjectRootGuard guard;
    Common::Constants::Path::SetProjectRoot( root + "Editor", "Resources/Assets" );

    std::vector<std::string> offences;
    size_t                   files = 0;

    for ( const auto& entry : fs::recursive_directory_iterator( content ) )
    {
        if ( !entry.is_regular_file() )
            continue;
        ++files;

        const auto     handle = static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( entry.path() ) );
        const fs::path named  = Common::AssetPathIndex::PathFor( handle );

        if ( named.empty() )
        {
            offences.push_back( "  " + entry.path().string() + " derives " + std::to_string( handle ) +
                                " and the index names nothing for it" );
            continue;
        }
        // lexically_normal on both sides: the walk yields `./Editor/...` while the index expands the key
        // against the root as it stands, so the two spellings differ while naming one file. Comparing
        // spellings rather than files is how a green test would mean nothing.
        std::error_code ec;
        if ( !fs::equivalent( named, entry.path(), ec ) || ec )
        {
            offences.push_back( "  " + entry.path().string() + " derives " + std::to_string( handle ) +
                                " but the index names " + named.string() );
        }
    }

    EXPECT_TRUE( offences.empty() ) << offences.size() << " of " << files
                                    << " content files are not named back by their own handle:\n"
                                    << Joined( offences );

    // Printed rather than pinned. A census that asserts a COUNT is satisfied by editing the count; the
    // number is here so a reader of the sweep can see the corpus shrink, and the assertion below is only
    // the floor that says the walk found the project at all.
    std::cout << "[  COUNT   ] " << files << " content files, index holds " << Common::AssetPathIndex::Size()
              << " handles\n";
    EXPECT_GT( files, 100u ) << "only " << files
                             << " content files were walked — the sweep is not looking at the project";
}

// ── B. The number has not moved, proved from content rather than from a list ───────────────────────────

TEST( AssetHandleInverse, EveryPathAndHandleAShippedSceneWritesForOneReferenceAgree )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const fs::path scenes = fs::path( root ) / "Editor/Resources/Assets/Scenes";
    ASSERT_TRUE( fs::exists( scenes ) ) << scenes.string() << " is missing";

    const ProjectRootGuard guard;
    Common::Constants::Path::SetProjectRoot( root + "Editor", "Resources/Assets" );

    std::vector<PathAndGuid> materials;
    std::string              parseError;
    CollectPairsInScenes( scenes, materials, &parseError );
    ASSERT_TRUE( parseError.empty() ) << "a shipped scene did not parse: " << parseError;

    // `MaterialPaths` is written as `Materials/M.demat` — relative to the ASSETS root, and
    // `MakeAssetResolver`'s MaterialAsset branch joins it to `ASSETS_PATH` on the way in. Joining it to
    // the project directory instead is what made this census check ZERO rows on its first run.
    const auto expandFromAssets = []( const std::string& stored ) -> fs::path
    {
        const fs::path asWritten( stored );
        return asWritten.is_absolute() ? asWritten
                                       : ( Common::Constants::Path::ASSETS_PATH / stored ).lexically_normal();
    };

    ASSERT_FALSE( materials.empty() ) << "no MaterialPaths/MaterialGuids pair was found in any shipped scene";

    size_t checked = 0;
    for ( const auto& row : materials )
    {
        const fs::path demat = expandFromAssets( row.Path );
        if ( !fs::exists( demat ) )
            continue; // a scene naming a material this checkout does not have is AssetReferenceCensus's job
        ++checked;

        // SCNE 27: the scene names the material by the header GUID of the file its path locates.
        EXPECT_EQ( GuidOfDemat( demat ), row.Guid )
             << row.Scene << " writes '" << row.Path << "' beside GUID " << row.Guid << ", but that file's header "
             << "states " << GuidOfDemat( demat ) << ". The GUID is the identity and the path its locator, so "
             << "they must name one file.";
    }
    std::cout << "[  COUNT   ] " << checked << " of " << materials.size()
              << " MaterialPaths/MaterialGuids pairs resolvable in this checkout\n";
    EXPECT_GT( checked, 0u ) << "no named `.demat` exists in this checkout — the material half checked nothing";
}

// ── C-E. The index's own contract ──────────────────────────────────────────────────────────────────────

TEST( AssetHandleInverse, MintingAPathDerivedHandleRecordsItsInverse )
{
    const ProjectRootGuard guard;
    Common::Constants::Path::SetProjectRoot( "/tmp/desert-b5-probe", "Content" );

    const fs::path asset  = fs::path( "/tmp/desert-b5-probe/Content" ) / "Textures" / "T_Probe.png";
    const auto     handle = static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( asset ) );

    EXPECT_EQ( Common::AssetPathIndex::KeyFor( handle ), "assets:Textures/T_Probe.png" );
    EXPECT_EQ( Common::AssetPathIndex::PathFor( handle ).lexically_normal(), asset.lexically_normal() );
}

TEST( AssetHandleInverse, AKeyThatIsNotAPathIsNotIndexedAsOne )
{
    const ProjectRootGuard guard;

    // FromKey is the raw hash and has callers that mint identities rather than locations — the thumbnail
    // key, an imported sub-mesh's key, a `procedural://` clip. Indexing those would put entries in a table
    // whose whole meaning is "which FILE this number names", and every consumer would then have to ask
    // whether the answer is a path at all.
    const auto synthetic = static_cast<uint64_t>( Common::AssetHandle::FromKey( "procedural://Spin" ) );
    EXPECT_TRUE( Common::AssetPathIndex::KeyFor( synthetic ).empty() );
    EXPECT_TRUE( Common::AssetPathIndex::PathFor( synthetic ).empty() );
}

TEST( AssetHandleInverse, ACollisionIsRefusedAndTheFirstBindingStands )
{
    const ProjectRootGuard guard;

    const uint64_t handle = 0x5DE5E27B5u;

    EXPECT_TRUE( Common::AssetPathIndex::Record( handle, "assets:A.png" ).IsSuccess() );
    // Idempotent: the same pair again is a success and changes nothing.
    EXPECT_TRUE( Common::AssetPathIndex::Record( handle, "assets:A.png" ).IsSuccess() );

    const auto refused = Common::AssetPathIndex::Record( handle, "assets:B.png" );
    EXPECT_FALSE( refused.IsSuccess() )
         << "two different keys were allowed to claim one number. Whichever registered last would then own "
            "the identity, so which file a reference resolves to would depend on scan order.";
    EXPECT_EQ( Common::AssetPathIndex::KeyFor( handle ), "assets:A.png" )
         << "the refusal replaced the binding instead of keeping it";

    // Neither a null handle nor an empty key names anything, so neither may enter the table.
    EXPECT_FALSE( Common::AssetPathIndex::Record( 0, "assets:A.png" ).IsSuccess() );
    EXPECT_FALSE( Common::AssetPathIndex::Record( handle + 1, "" ).IsSuccess() );
    EXPECT_TRUE( Common::AssetPathIndex::KeyFor( 0 ).empty() );
}

TEST( AssetHandleInverse, PathForIsTheAssertedInverseAndNotASecondSpellingOfIt )
{
    const ProjectRootGuard guard;
    Common::Constants::Path::SetProjectRoot( "/tmp/desert-b5-probe", "Content" );

    // Every root, so a root added to the table reaches this assertion without an edit here.
    for ( const auto& candidate : Common::AssetHandle::ContentRoots() )
    {
        const fs::path asset  = *candidate.Root / "Nested" / "Thing.bin";
        const auto     handle = static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( asset ) );

        const std::string key = Common::AssetPathIndex::KeyFor( handle );
        ASSERT_FALSE( key.empty() ) << "no inverse recorded for a file under root '" << candidate.Tag << "'";
        EXPECT_EQ( key, std::string( candidate.Tag ) + ":Nested/Thing.bin" );
        EXPECT_EQ( Common::AssetPathIndex::PathFor( handle ), Common::AssetHandle::PathForStableKey( key ) )
             << "PathFor stopped being KeyFor composed with the tested key->path edge";
    }
}

// ── F-G. The two rules that keep the inverse TOTAL, checked over the source text ───────────────────────
//
// These read the code rather than run it, for the reason SettingConsumers does: the failure they guard
// against is a NEW call site written a month from now, and no run-time test can see a line nobody added
// yet. A census over the tree can.

TEST( AssetHandleInverse, EveryIdentityAdoptedFromAFileGoesThroughTheRecordingHelper )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    // `m_Metadata.Handle = <something>` is how a subclass installs an identity that came out of its FILE,
    // over the path-derived one AssetBase's constructor put there. Every such line must also record the
    // inverse, or that asset becomes the one the index cannot name — silently, because the asset itself
    // works perfectly.
    //
    // Two spellings are legitimate and are named here rather than pattern-matched, because each is a
    // deliberate exception with a reason:
    //   * AssetBase.hpp itself, which is where the helper assigns the field;
    //   * SurfaceMaterialAsset::CreateWorkingCopy, which mints a RANDOM id for a copy that is deliberately
    //     in no map at all — binding it to the subject's path would put a wrong answer in the index.
    const std::set<std::string> allowed = { "AssetBase.hpp", "SurfaceMaterialAsset.cpp" };

    std::vector<std::string> offences;
    size_t                   scanned = 0;

    for ( const fs::path& base : { fs::path( root ) / "Desert/Desert/Source", fs::path( root ) / "Editor/Source",
                                   fs::path( root ) / "Runtime/Source" } )
    {
        for ( const fs::path& source : SourcesUnder( base ) )
        {
            ++scanned;
            if ( allowed.contains( source.filename().string() ) )
                continue;

            const std::string text = WithoutComments( ReadAll( source ) );
            for ( size_t at = text.find( "m_Metadata.Handle" ); at != std::string::npos;
                  at        = text.find( "m_Metadata.Handle", at + 1 ) )
            {
                const size_t after = text.find_first_not_of( " \t", at + std::strlen( "m_Metadata.Handle" ) );
                if ( after == std::string::npos || text[after] != '=' || text[after + 1] == '=' )
                    continue;
                offences.push_back( "  " + source.string() + " assigns m_Metadata.Handle directly" );
            }
        }
    }

    ASSERT_GT( scanned, 100u ) << "only " << scanned << " sources were read — the census walked nothing";
    EXPECT_TRUE( offences.empty() )
         << "an identity is installed without recording its inverse, so that asset's handle will name "
            "nothing:\n"
         << Joined( offences )
         << "Use AssetBase::AdoptHandleFromFile instead, and pass the key of the file the NUMBER is a "
            "statement about.";
}

TEST( AssetHandleInverse, NothingInProductionClearsTheIndex )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    // AN ASSET'S IDENTITY OUTLIVES ITS PAYLOAD. That is the whole reason this index is not a cache: a
    // lookup key that dies with the load cut the mesh->rig edge for an entire session once, and
    // `TextureAsset::Unload` already has to say in prose that "an evicted asset keeps its identity". A
    // production caller of Clear() would reintroduce exactly that, and it would look like a tidy-up.
    std::vector<std::string> offences;
    size_t                   scanned = 0;

    for ( const fs::path& base :
          { fs::path( root ) / "Desert/Desert/Source", fs::path( root ) / "Desert/Common/Source",
            fs::path( root ) / "Editor/Source", fs::path( root ) / "Runtime/Source", fs::path( root ) / "Tools" } )
    {
        for ( const fs::path& source : SourcesUnder( base ) )
        {
            ++scanned;
            const std::string text = WithoutComments( ReadAll( source ) );
            if ( text.find( "AssetPathIndex::Clear" ) != std::string::npos )
                offences.push_back( "  " + source.string() );
        }
    }

    ASSERT_GT( scanned, 100u ) << "only " << scanned << " sources were read — the census walked nothing";
    EXPECT_TRUE( offences.empty() )
         << "production code clears the handle->path index:\n"
         << Joined( offences )
         << "Clear() exists for tests that move the project root. An asset that is unloaded must still be "
            "able to say which file it came from, or the reference that reloads it is a different asset.";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

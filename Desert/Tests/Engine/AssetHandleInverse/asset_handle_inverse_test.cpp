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
// `ANIM_RigWitness_NoRig.desce` stores `"MeshPath":"Cooked/Meshes/IKProbe.skmesh"` beside
// `"MeshGuid":556331627295699705`, and that number is exactly `FromCookedPath` of that path. Ten such
// pairs are committed, plus 95 `TextureHandle` and 113 `MaterialId` numbers in `.demat` files.
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
// And the two kinds of row are deliberately both here, because they fail in opposite directions:
//   * `MeshPath` + `MeshGuid` is a PATH-DERIVED handle. It breaks if the derivation moves.
//   * `MaterialPaths` + `MaterialGuids` is an ADOPTED handle — the `MaterialId` written inside the named
//     `.demat`, a random authored id that no derivation produces. It breaks if the adoption is dropped.
// An index that only recorded derivations would be empty for exactly the references that exist most.

#include <gtest/gtest.h>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Constants.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>
#include <rflcpp/rfl/json.hpp>

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
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
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

    // One (path, handle) pair a shipped scene wrote for ONE reference.
    struct PathAndHandle
    {
        std::string Scene;
        std::string Field; // which pair of fields it came from, for the failure message
        std::string Path;
        uint64_t    Handle = 0;
    };

    // Pulls every such pair out of one parsed scene document. The walk is recursive because a component
    // block sits at an unknown depth inside the entity list, and hard-coding the depth is how the next
    // format change makes this census pass over nothing.
    void CollectPairs( const rfl::Generic& node, const std::string& scene, std::vector<PathAndHandle>& meshes,
                       std::vector<PathAndHandle>& materials )
    {
        if ( const auto array = node.to_array() )
        {
            for ( const auto& element : array.value() )
                CollectPairs( element, scene, meshes, materials );
            return;
        }

        const auto object = node.to_object();
        if ( !object )
            return;

        const auto& fields = object.value();

        // `rfl::Object::find` returns an INDEX, not an iterator, and `Generic::to_int` is 32-bit — both
        // of which silently produce a wrong answer here. The pairs are walked instead, and the integer is
        // read through `to_int64` and reinterpreted, which is exact for the full uint64 range because the
        // writer emits the same bit pattern.
        const auto valueOf = [&fields]( const char* name ) -> const rfl::Generic*
        {
            for ( const auto& [key, value] : fields )
            {
                if ( key == name )
                    return &value;
            }
            return nullptr;
        };
        const auto stringAt = [&valueOf]( const char* name ) -> std::optional<std::string>
        {
            const rfl::Generic* found = valueOf( name );
            if ( !found )
                return std::nullopt;
            const auto text = found->to_string();
            return text ? std::optional<std::string>( text.value() ) : std::nullopt;
        };
        const auto intAt = [&valueOf]( const char* name ) -> std::optional<uint64_t>
        {
            const rfl::Generic* found = valueOf( name );
            if ( !found )
                return std::nullopt;
            if ( const auto value = found->to_int64() )
                return static_cast<uint64_t>( value.value() );
            return std::nullopt;
        };

        if ( const auto meshPath = stringAt( "MeshPath" ) )
        {
            if ( const auto guid = intAt( "MeshGuid" ); guid && *guid != 0 && !meshPath->empty() )
                meshes.push_back( { scene, "MeshPath/MeshGuid", *meshPath, *guid } );
        }

        const rfl::Generic* paths = valueOf( "MaterialPaths" );
        const rfl::Generic* guids = valueOf( "MaterialGuids" );
        if ( paths && guids )
        {
            const auto pathArray = paths->to_array();
            const auto guidArray = guids->to_array();
            if ( pathArray && guidArray )
            {
                const auto& p = pathArray.value();
                const auto& g = guidArray.value();
                for ( size_t at = 0; at < p.size() && at < g.size(); ++at )
                {
                    const auto text  = p[at].to_string();
                    const auto value = g[at].to_int64();
                    if ( !text || !value || text->empty() || *value == 0 )
                        continue;
                    materials.push_back(
                         { scene, "MaterialPaths/MaterialGuids", *text, static_cast<uint64_t>( *value ) } );
                }
            }
        }

        for ( const auto& [name, value] : fields )
            CollectPairs( value, scene, meshes, materials );
    }

    void CollectPairsInScenes( const fs::path& scenesRoot, std::vector<PathAndHandle>& meshes,
                               std::vector<PathAndHandle>& materials, std::string* parseError )
    {
        for ( const auto& entry : fs::directory_iterator( scenesRoot ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".desce" )
                continue;
            const auto parsed = rfl::json::read<rfl::Generic>( ReadAll( entry.path() ) );
            if ( !parsed )
            {
                if ( parseError && parseError->empty() )
                    *parseError = entry.path().filename().string() + ": " + parsed.error().what();
                continue;
            }
            CollectPairs( parsed.value(), entry.path().filename().string(), meshes, materials );
        }
    }

    // The `MaterialId` a `.demat` carries, or 0. Read as raw text rather than through MaterialData so a
    // schema change to that struct cannot make this census quietly stop finding the field.
    uint64_t MaterialIdIn( const fs::path& demat )
    {
        const std::string text = ReadAll( demat );
        const std::string key  = "\"MaterialId\":";
        const size_t      at   = text.rfind( key );
        if ( at == std::string::npos )
            return 0;
        return std::strtoull( text.c_str() + at + key.size(), nullptr, 10 );
    }
} // namespace

// ── A. The relation itself, over every file the project ships ─────────────────────────────────────────

TEST( AssetHandleInverse, EveryContentFileIsNamedBackByItsOwnHandle )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    const fs::path content = fs::path( root ) / "Editor/Resources/Assets";
    ASSERT_TRUE( fs::exists( content ) ) << content.string() << " is missing";

    ProjectRootGuard guard;
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

    ProjectRootGuard guard;
    Common::Constants::Path::SetProjectRoot( root + "Editor", "Resources/Assets" );

    std::vector<PathAndHandle> meshes;
    std::vector<PathAndHandle> materials;
    std::string                parseError;
    CollectPairsInScenes( scenes, meshes, materials, &parseError );
    ASSERT_TRUE( parseError.empty() ) << "a shipped scene did not parse: " << parseError;

    // THE TWO KINDS OF STORED PATH ARE JOINED TO DIFFERENT ROOTS, and each of the two spellings is read
    // back by the engine exactly this way:
    //   * `MeshPath` is written as `Cooked/Meshes/X.skmesh` — relative to the PROJECT directory, because
    //     the cooked tree is a sibling of the assets root and a path relative to the assets root would
    //     come out as `../Cooked/...`;
    //   * `MaterialPaths` is written as `Materials/M.demat` — relative to the ASSETS root, and
    //     `MakeAssetResolver`'s MaterialAsset branch joins it to `ASSETS_PATH` on the way in, saying in
    //     its own comment that deleting the join breaks the reference.
    // Using one base for both is what made the material half of this census check ZERO rows on its first
    // run while still reporting green on the mesh half.
    const auto expandFromProject = []( const std::string& stored ) -> fs::path
    {
        const fs::path asWritten( stored );
        return asWritten.is_absolute() ? asWritten
                                       : ( Common::Constants::Path::CurrentProjectRoot().ProjectDir / stored );
    };
    const auto expandFromAssets = []( const std::string& stored ) -> fs::path
    {
        const fs::path asWritten( stored );
        return asWritten.is_absolute() ? asWritten
                                       : ( Common::Constants::Path::ASSETS_PATH / stored ).lexically_normal();
    };

    ASSERT_FALSE( meshes.empty() ) << "no MeshPath/MeshGuid pair was found in any shipped scene — the "
                                      "census is looking at nothing, which is how it stays green forever";

    for ( const auto& row : meshes )
    {
        const auto derived =
             static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( expandFromProject( row.Path ) ) );
        EXPECT_EQ( derived, row.Handle )
             << row.Scene << " writes '" << row.Path << "' beside " << row.Handle << ", but that path now "
             << "derives " << derived << ". The stored number is what the scene resolves against, so "
             << "moving the derivation orphans this reference: the mesh loads as unset with no filename "
             << "anywhere in the log. Migrate the corpus in the same change.";

        // And the inverse names the file back. This is the half `MakeAssetResolver::ToPath` could not do
        // without the asset being loaded first.
        EXPECT_EQ( Common::AssetPathIndex::KeyFor( row.Handle ),
                   Common::AssetHandle::StableKeyForPath( expandFromProject( row.Path ) ) )
             << row.Scene << ": handle " << row.Handle << " does not name '" << row.Path << "' back";
    }

    ASSERT_FALSE( materials.empty() ) << "no MaterialPaths/MaterialGuids pair was found in any shipped scene";

    size_t checked = 0;
    for ( const auto& row : materials )
    {
        const fs::path demat = expandFromAssets( row.Path );
        if ( !fs::exists( demat ) )
            continue; // a scene naming a material this checkout does not have is AssetReferenceCensus's job
        ++checked;

        // An ADOPTED id: the number in the scene is the `MaterialId` inside the named `.demat`, not a
        // derivation of its path. Asserting it against FromCookedPath would be asserting the wrong thing
        // and would go red on correct content.
        EXPECT_EQ( MaterialIdIn( demat ), row.Handle )
             << row.Scene << " writes '" << row.Path << "' beside " << row.Handle
             << ", but that file carries MaterialId " << MaterialIdIn( demat )
             << ". A material's handle IS its in-file id, so these two must be one number.";

        EXPECT_NE( static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( demat ) ), row.Handle )
             << row.Path << " happens to derive its own MaterialId. That is not a defect, but this "
             << "assertion exists to keep the two kinds of identity distinguishable — if it ever fires, "
             << "the adopted case above has stopped being tested by this row.";
    }
    std::cout << "[  COUNT   ] " << meshes.size() << " MeshPath/MeshGuid pairs, " << checked << " of "
              << materials.size() << " MaterialPaths/MaterialGuids pairs resolvable in this checkout\n";
    EXPECT_GT( checked, 0u ) << "no named `.demat` exists in this checkout — the material half checked nothing";
}

// ── C-E. The index's own contract ──────────────────────────────────────────────────────────────────────

TEST( AssetHandleInverse, MintingAPathDerivedHandleRecordsItsInverse )
{
    ProjectRootGuard guard;
    Common::Constants::Path::SetProjectRoot( "/tmp/desert-b5-probe", "Content" );

    const fs::path asset  = fs::path( "/tmp/desert-b5-probe/Content" ) / "Textures" / "T_Probe.png";
    const auto     handle = static_cast<uint64_t>( Common::AssetHandle::FromCookedPath( asset ) );

    EXPECT_EQ( Common::AssetPathIndex::KeyFor( handle ), "assets:Textures/T_Probe.png" );
    EXPECT_EQ( Common::AssetPathIndex::PathFor( handle ).lexically_normal(), asset.lexically_normal() );
}

TEST( AssetHandleInverse, AKeyThatIsNotAPathIsNotIndexedAsOne )
{
    ProjectRootGuard guard;

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
    ProjectRootGuard guard;

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
    ProjectRootGuard guard;
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
            if ( allowed.count( source.filename().string() ) )
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

// THE RELATION THIS GUARDS, stated once and then asserted from both sides:
//
//     a thumbnail's cache key names the ASSET, not the string the caller was holding.
//
// From one side: every spelling of one asset's location must produce ONE key, or the same picture is
// captured twice and neither copy can be invalidated by a panel that only knows the other name. From the
// other side: two different assets must never produce the same key, or one of them shows the other's
// picture and nothing can tell them apart. Those are the two halves of a single statement, and testing
// only the first would accept `return "thumb.png";`.
//
// WHY IT NEEDED GUARDING. The key used to be the caller's own path with every non-alphanumeric byte turned
// into '_'. Two measured consequences, both in the live editor:
//
//   * The key was ABSOLUTE, so it carried the machine and the checkout — the file found in the tree while
//     this was written was really named
//     `_Users_daniilsavcenko_..._claude_worktrees_u5_editor_redesign_Editor_Resources_Assets_Materials_Starter_Prop_demat.png`.
//     Opening ONE project through two equivalent spellings of its own path (a directory and a symlink to
//     it) captured the same three materials twice: three assets, SIX files, in one Cooked/Thumbnails
//     folder. Renaming the project folder does the same to every thumbnail at once.
//   * The flattening was not injective: `Materials/Wood_Oak.demat` and `Materials/Wood/Oak.demat` both
//     became `..._Materials_Wood_Oak_demat`, so two assets shared one cache entry.
//
// The fix is not a new notion of sameness — it is the engine's existing one.
// `Common::AssetHandle::StableKeyForPath` is what the AssetManager already deduplicates its registry on
// ("which file is this, whatever the spelling"), and a thumbnail is a picture OF an asset, so the two must
// not be able to disagree. Several assertions below are therefore about AGREEMENT WITH THE ASSET HANDLE
// rather than about the key's own text: a key that were merely self-consistent could still drift away from
// what the rest of the engine calls one asset, which is the defect shape this project keeps paying for.
//
// WHAT THIS FILE DELIBERATELY DOES NOT ASSERT. It never pins the literal text of a key. A test that spelt
// out `assets_Materials_M_Wood_demat_123...` would fail the day the readable half is formatted differently
// while every property that matters still holds — and would say nothing about the day the readable half
// stays and the identity underneath it changes. Only relations are asserted.

#include <gtest/gtest.h>

#include <Editor/Import/CookPaths.hpp>
#include <Editor/Widgets/ThumbnailKey.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace
{
    namespace TK = Desert::Editor::ThumbnailKey;
    namespace fs = std::filesystem;

    // Every test runs with a project open, because that is the only state the editor ever captures a
    // thumbnail in (ProjectContext::Open remaps the content paths before the engine starts). The sandbox
    // is restored afterwards so no test can leak a root into the next one.
    class ThumbnailKeyTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // The project root is the WORKING DIRECTORY, so that a working-directory-relative spelling —
            // the form CollectionsPanel carries verbatim out of a collection.json manifest — is genuinely
            // the same file as the absolute one, and the equivalence below is a fact about the filesystem
            // rather than an assumption about it.
            m_ProjectDir = fs::current_path();
            Common::Constants::Path::SetProjectRoot( m_ProjectDir, fs::path( "Resources/Assets" ) );
        }

        void TearDown() override
        {
            Common::Constants::Path::ResetToSandbox();
        }

        // The absolute spelling of a path under the project's assets root.
        fs::path UnderAssets( const std::string& relative ) const
        {
            return ( Common::Constants::Path::ASSETS_PATH / relative ).lexically_normal();
        }

        fs::path m_ProjectDir;
    };

    // ─── One asset, many spellings ────────────────────────────────────────────────────────────────────

    // The four divergences the key has to survive, each written the way a caller actually produces it:
    // the absolute form (the asset browser's directory walk, and every AssetManager record), the
    // working-directory-relative form (a collection manifest), a `./` form, and a form with a `..`
    // detour through a sibling directory. All four open the same file.
    TEST_F( ThumbnailKeyTest, EverySpellingOfOneAssetGivesOneKey )
    {
        const std::string           relative  = "Resources/Assets/Materials/M_Wood.demat";
        const std::vector<fs::path> spellings = {
             m_ProjectDir / relative,                                              // absolute
             fs::path( relative ),                                                 // working-dir relative
             fs::path( "./" ) / relative,                                          // leading ./
             m_ProjectDir / "Resources/Assets/Textures/../Materials/M_Wood.demat", // a .. detour
             m_ProjectDir / "./Resources/./Assets/Materials/M_Wood.demat",         // interior ./
        };

        std::set<std::string> keys;
        for ( const fs::path& spelling : spellings )
        {
            keys.insert( TK::FileName( spelling.string() ) );
        }

        EXPECT_EQ( keys.size(), 1u ) << "one asset produced " << keys.size()
                                     << " cache keys; each extra one is a picture captured again and "
                                        "invalidated never";
    }

    // The defect measured in the editor: the project itself reached by a second, equivalent spelling of
    // its own directory. Nothing about the asset changed, so nothing about its key may change.
    TEST_F( ThumbnailKeyTest, MovingTheProjectDoesNotRenameItsThumbnails )
    {
        const std::string relative = "Materials/M_Wood.demat";
        const std::string here     = TK::FileName( UnderAssets( relative ).string() );

        Common::Constants::Path::SetProjectRoot( m_ProjectDir / "somewhere" / "else",
                                                 fs::path( "Resources/Assets" ) );
        const std::string there = TK::FileName( UnderAssets( relative ).string() );

        EXPECT_EQ( here, there ) << "the cache key carries the project's location, so moving, renaming or "
                                    "symlinking the project re-renders every thumbnail in it";
    }

    // The key must not contain the developer's home directory at all — that is the property that makes
    // the previous test true for roots nobody thought to enumerate, and it is what a committed or shared
    // Cooked/ tree depends on.
    TEST_F( ThumbnailKeyTest, KeyDoesNotCarryTheProjectRoot )
    {
        const std::string key = TK::FileName( UnderAssets( "Materials/M_Wood.demat" ).string() );

        // The root's own directory names, flattened the way the key flattens: none may appear in it.
        for ( const fs::path& part : m_ProjectDir )
        {
            const std::string name = part.string();
            if ( name.size() < 2 || name == "/" )
                continue; // separators and single letters are not evidence of anything
            EXPECT_EQ( key.find( name ), std::string::npos )
                 << "key '" << key << "' contains the project-root component '" << name << "'";
        }
    }

    // ─── Two assets, never one key ────────────────────────────────────────────────────────────────────

    // The pair the old flattening merged: '/' and '_' both became '_', so a folder boundary and an
    // underscore in a file name were the same byte.
    TEST_F( ThumbnailKeyTest, ASeparatorIsNotAnUnderscore )
    {
        const std::string flat   = TK::FileName( UnderAssets( "Materials/Wood_Oak.demat" ).string() );
        const std::string nested = TK::FileName( UnderAssets( "Materials/Wood/Oak.demat" ).string() );

        EXPECT_NE( flat, nested ) << "two different materials share one cache entry, so one of them "
                                     "displays the other's render";
    }

    // A census rather than a pair: every distinct asset in a small tree must get its own key. This is the
    // half that a degenerate implementation (a constant, or a hash of the file NAME only) fails.
    TEST_F( ThumbnailKeyTest, DistinctAssetsGetDistinctKeys )
    {
        const std::vector<std::string> assets = {
             "Materials/M_Wood.demat",        // the baseline
             "Materials/M_Wood_Dark.demat",   // a longer name sharing the baseline's prefix
             "Materials/Wood_Oak.demat",      // an underscore where the next row has a folder
             "Materials/Wood/Oak.demat",      // a folder where the previous row has an underscore
             "Materials/Nested/M_Wood.demat", // same file NAME as the baseline, different asset
             "Meshes/M_Wood.demat",           // same relative name under another folder
             "Materials/M_wood.demat",        // case differs: two files on a case-sensitive volume
        };

        std::set<std::string> keys;
        for ( const std::string& relative : assets )
        {
            keys.insert( TK::FileName( UnderAssets( relative ).string() ) );
        }

        EXPECT_EQ( keys.size(), assets.size() )
             << "only " << keys.size() << " keys for " << assets.size() << " assets";
    }

    // Two roots are SIBLINGS in the layout, so `Cooked/Textures/T.tex` and `Resources/Assets/Textures/T.tex`
    // both reduce to `Textures/T.tex`. The identity key separates them with a root tag; the thumbnail key
    // inherits that separation, and a key derived from a bare relative path would not.
    TEST_F( ThumbnailKeyTest, MirroredPathsUnderDifferentRootsStayDistinct )
    {
        const std::string content =
             TK::FileName( ( Common::Constants::Path::ASSETS_PATH / "Textures/T.tex" ).string() );
        const std::string cooked =
             TK::FileName( ( Common::Constants::Path::COOKED_PATH / "Textures/T.tex" ).string() );

        EXPECT_NE( content, cooked );
    }

    // ─── Agreement with the engine's own notion of one asset ──────────────────────────────────────────

    // The load-bearing one. The thumbnail key is derived from StableKeyForPath, which is also what the
    // asset's path-derived handle is hashed from, so "same asset" means the same thing to the cache and to
    // the registry. If someone ever gives the thumbnail cache its own normalization — case folding, say —
    // this fails, and it fails for the right reason: the two would then disagree about identity for a
    // whole class of paths, silently.
    TEST_F( ThumbnailKeyTest, IdentityIsTheSameOneTheAssetHandleUses )
    {
        const std::vector<std::string> assets = { "Materials/M_Wood.demat", "Materials/Wood/Oak.demat",
                                                  "Meshes/Rock.stmesh" };

        for ( const std::string& relative : assets )
        {
            const fs::path absolute = UnderAssets( relative );

            EXPECT_EQ( TK::Identity( absolute.string() ), Common::AssetHandle::StableKeyForPath( absolute ) );

            // ...and the number appended to the file name is that same identity's handle, not a second
            // hash of a second string.
            const std::uint64_t handle =
                 static_cast<std::uint64_t>( Common::AssetHandle::FromCookedPath( absolute ) );
            EXPECT_NE( TK::FileName( absolute.string() ).find( std::to_string( handle ) ), std::string::npos );
        }
    }

    // ─── The two derivations of a mesh's key must land on the same file ───────────────────────────────

    // THREE PANELS, TWO WAYS OF GETTING THERE, ONE PICTURE.
    //
    // The Details 3D Model row keys a mesh thumbnail on the REGISTERED asset's path, because a scene holds
    // a cooked handle and can reach nothing else: `mesh->GetMetadata().Filepath`, the `.stmesh` asset
    // beside its source (AF4d). The asset browser and the Collections grid start from a SOURCE the user is looking at —
    // an `.fbx` — and reach the same picture through `CookPaths::MeshAsset`. Two derivations, and until
    // 2026-09-08 they produced two different cache files for one mesh: the same 370 ms render, stored
    // twice under two names, neither able to satisfy the other panel.
    //
    // They are unified on the cooked side, and this is the assertion that keeps them there. It is worth a
    // test rather than a click precisely because both halves keep working when they disagree — each panel
    // shows a picture, so the only symptom is a second capture nobody counts. That is this project's most
    // repeated defect shape stated as a relation: assert that the two ends AGREE, not that each end runs.
    //
    // It fails if the asset's place changes on one side only (a static mesh under Cooked/ again), if the
    // extension changes on one side only, or if any panel goes back to keying a mesh on its source.
    TEST_F( ThumbnailKeyTest, TheBrowsersCookedMappingAndTheScenesRegisteredPathGiveOneKey )
    {
        // What the browser holds: the source the user clicked.
        const fs::path source = UnderAssets( "Meshes/Rock.fbx" );

        // What the browser derives from it, and what it now asks the thumbnail service for.
        const fs::path viaCookPaths = Desert::Editor::CookPaths::MeshAsset( source );

        // What the Details row holds instead: the registered cooked asset's own recorded path. Spelled
        // independently here rather than reused from the line above, or the test would compare a value
        // with itself and pass over any divergence at all.
        const fs::path viaRegistry = UnderAssets( "Meshes/Rock.stmesh" ).lexically_normal();

        // A STATIC MESH IS NEVER COOKED UNDER Cooked/ ANY MORE: that root holds only the skinned outputs.
        const fs::path cookedRoot = Common::Constants::Path::MESH_PATH_COOKED;
        EXPECT_TRUE( fs::relative( viaCookPaths, cookedRoot ).begin()->string() == ".." )
             << viaCookPaths.string() << " lies under the skinned cook root " << cookedRoot.string();

        EXPECT_EQ( TK::Identity( viaCookPaths.string() ), TK::Identity( viaRegistry.string() ) )
             << "the browser's cooked mapping and the scene's registered path no longer name one asset:\n"
             << "  browser  -> " << viaCookPaths.string() << "\n"
             << "  registry -> " << viaRegistry.string()
             << "\nEach panel still draws a thumbnail, so nothing looks broken — the mesh is simply "
                "photographed twice, at ~370 ms and ~200 KB per copy, and invalidating one leaves the "
                "other standing.";

        EXPECT_EQ( TK::FileName( viaCookPaths.string() ), TK::FileName( viaRegistry.string() ) )
             << "same identity, different cache file name — the key and the file name have come apart.";

        // And the identity really is the cooked one, not the source's: keying on the source is the state
        // this change left behind, and it must not be reachable by accident again.
        EXPECT_NE( TK::Identity( viaCookPaths.string() ), TK::Identity( source.string() ) )
             << "the cooked mesh and its source .fbx now share one identity, so the freshness rule can no "
                "longer tell which file it is comparing the picture against.";
    }

    // An asset genuinely outside the project has no project-relative identity, and StableKeyForPath says so
    // by returning its normalized spelling. The thumbnail key must inherit that rather than inventing a
    // relative form, or two projects' strays with matching tails would collide — while spellings of the
    // SAME stray must still agree.
    TEST_F( ThumbnailKeyTest, PathsOutsideTheProjectStayDistinctAndStayStable )
    {
        const fs::path outside = m_ProjectDir.parent_path() / "Elsewhere" / "Stray.demat";

        EXPECT_EQ( TK::FileName( outside.string() ),
                   TK::FileName( ( outside.parent_path() / "." / "Stray.demat" ).string() ) );
        EXPECT_NE( TK::FileName( outside.string() ), TK::FileName( UnderAssets( "Stray.demat" ).string() ) );
    }

    // Every key has to be a legal file name: the versioned folder is joined to it directly, so a surviving
    // separator would silently scatter the cache into subdirectories that PurgeOldVersions never sweeps.
    TEST_F( ThumbnailKeyTest, KeyIsASingleFileNameComponent )
    {
        const std::vector<fs::path> spellings = {
             UnderAssets( "Materials/M_Wood.demat" ),
             fs::path( "Resources/Assets/Materials/M_Wood.demat" ),
             m_ProjectDir.parent_path() / "Elsewhere" / "Stray.demat",
        };

        for ( const fs::path& spelling : spellings )
        {
            const std::string name = TK::FileName( spelling.string() );
            EXPECT_EQ( fs::path( name ).filename().string(), name ) << name;
            EXPECT_EQ( name.find( ':' ), std::string::npos ) << name; // the identity's root tag separator
        }
    }
} // namespace

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
